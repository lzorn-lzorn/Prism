//
// Created by Lzorn on 2026/9/23.
//

#ifndef STABLEARRAY_RADIXTREEMAP_H
#define STABLEARRAY_RADIXTREEMAP_H

#pragma once

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace infra::mem
{
/**
 * @note: RadixTreeMap 使用键的比特位直接进行分层索引,
 * 把 uint64_t 键的比特位直接当作树的分支路径. 每一层取固定宽度的比特位作为数组下标
 *
 * 键(归一化后)的 36 位布局: x86 架构的 CPU 只利用了虚拟内存后 48 位, 高位均是 0;
 * 同时现代的内存页基本上都是以 4kb (2^12) 为单位, 所以该索引结构就是 48 - 12 = 36
 * 来索引一个内存页面
 *
 * [ 16 bits ][ 10 bits ][ 10 bits ]
 *     L0         L1         L2
 *
 * L0 选中 Root 里的一个槽
 * L1 选中该 Level1 节点的一个子槽
 * L2 选中该 Level2 节点的一个 value 槽
 */
template <typename ValueType,
    size_t KeyShift = 0,
    size_t Level0Bits = 16,
    size_t Level1Bits = 10,
    size_t Level2Bits = 10,
    size_t Level1NodeCapacity = 64,
    size_t Level2NodeCapacity = 512>
class RadixTreeMap
{
public:
    static_assert(std::is_pointer_v<ValueType>, "RadixTreeMap supports pointer value types only.");
    static_assert(Level0Bits > 0 && Level1Bits > 0 && Level2Bits > 0, "All radix levels must be non-zero.");
    static_assert(KeyShift < 64, "KeyShift must be less than 64.");
    static_assert(Level0Bits + Level1Bits + Level2Bits <= (64 - KeyShift), "Radix layout exceeds key width after shift.");
    static_assert(Level1NodeCapacity > 0 && Level2NodeCapacity > 0, "Pool capacities must be non-zero.");

    static constexpr size_t IndexedBits = Level0Bits + Level1Bits + Level2Bits;
    static constexpr size_t Level0Size = size_t{1} << Level0Bits;
    static constexpr size_t Level1Size = size_t{1} << Level1Bits;
    static constexpr size_t Level2Size = size_t{1} << Level2Bits;

    static constexpr uint64_t Level0Mask = (uint64_t{1} << Level0Bits) - 1;
    static constexpr uint64_t Level1Mask = (uint64_t{1} << Level1Bits) - 1;
    static constexpr uint64_t Level2Mask = (uint64_t{1} << Level2Bits) - 1;

    enum class InsertResult : uint8_t
    {
        Inserted,
        Replaced,
        Erased,
        ErasedMissing,
        OutOfRange,
        PoolExhausted,
    };

    RadixTreeMap() = default;
    RadixTreeMap(const RadixTreeMap&) = delete;
    RadixTreeMap& operator=(const RadixTreeMap&) = delete;

    [[nodiscard]] InsertResult insertOrAssign(const uint64_t Key, const ValueType Value) noexcept
    {
        if (Value == nullptr)
        {
            return erase(Key) ? InsertResult::Erased : InsertResult::ErasedMissing;
        }

        size_t L0 = 0, L1 = 0, L2 = 0;
        if (!decodeKey(Key, L0, L1, L2))
        {
            ++KeyOutOfRangeFailureCount;
            return InsertResult::OutOfRange;
        }

        bool AllocatedLevel1 = false;
        Level1Node*& Level1Slot = Root[L0];
        if (!Level1Slot)
        {
            Level1Slot = Level1Pool.acquire();
            if (!Level1Slot)
            {
                ++PoolExhaustionFailureCount;
                return InsertResult::PoolExhausted;
            }
            AllocatedLevel1 = true;
        }

        Level2Node*& Level2Slot = Level1Slot->Children[L1];
        if (!Level1Slot->ChildPresent[L1])
        {
            Level2Slot = Level2Pool.acquire();
            if (!Level2Slot)
            {
                ++PoolExhaustionFailureCount;
                // 回滚刚分配的 Level1（如果它还是空的）
                if (AllocatedLevel1 && Level1Slot->UsedCount == 0)
                {
                    Level1Pool.release(Level1Slot);
                    Level1Slot = nullptr;
                }
                return InsertResult::PoolExhausted;
            }
            Level1Slot->ChildPresent[L1] = true;
            ++Level1Slot->UsedCount;
        }

        if (!Level2Slot->ValuePresent[L2])
        {
            Level2Slot->Values[L2] = Value;
            Level2Slot->ValuePresent[L2] = true;
            ++Level2Slot->UsedCount;
            ++Size;
            return InsertResult::Inserted;
        }

        Level2Slot->Values[L2] = Value;
        return InsertResult::Replaced;
    }

    [[nodiscard]] ValueType find(const uint64_t Key) const noexcept
    {
        size_t L0 = 0, L1 = 0, L2 = 0;
        if (!decodeKey(Key, L0, L1, L2))
        {
            return nullptr;
        }

        const Level1Node* Level1Slot = Root[L0];
        if (!Level1Slot || !Level1Slot->ChildPresent[L1])
        {
            return nullptr;
        }

        const Level2Node* Level2Slot = Level1Slot->Children[L1];
        if (!Level2Slot || !Level2Slot->ValuePresent[L2])
        {
            return nullptr;
        }

        return Level2Slot->Values[L2];
    }

    [[nodiscard]] bool contains(const uint64_t Key) const noexcept
    {
        return find(Key) != nullptr;
    }

    // 批量场景可选：提前把三级路径拉进缓存
    void prefetch(const uint64_t Key) const noexcept
    {
        size_t L0 = 0, L1 = 0, L2 = 0;
        if (!decodeKey(Key, L0, L1, L2))
        {
            return;
        }

        const Level1Node* Level1Slot = Root[L0];
        if (!Level1Slot)
        {
            return;
        }

#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&Level1Slot->Children[L1], 0, 1);
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_prefetch(reinterpret_cast<const char*>(&Level1Slot->Children[L1]), _MM_HINT_T1);
#endif

        if (!Level1Slot->ChildPresent[L1])
        {
            return;
        }

        const Level2Node* Level2Slot = Level1Slot->Children[L1];
        if (!Level2Slot)
        {
            return;
        }

#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(&Level2Slot->Values[L2], 0, 1);
#elif defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
        _mm_prefetch(reinterpret_cast<const char*>(&Level2Slot->Values[L2]), _MM_HINT_T1);
#endif
    }

    bool erase(const uint64_t Key) noexcept
    {
        size_t L0 = 0, L1 = 0, L2 = 0;
        if (!decodeKey(Key, L0, L1, L2))
        {
            return false;
        }

        Level1Node*& Level1Slot = Root[L0];
        if (!Level1Slot || !Level1Slot->ChildPresent[L1])
        {
            return false;
        }

        Level2Node*& Level2Slot = Level1Slot->Children[L1];
        if (!Level2Slot || !Level2Slot->ValuePresent[L2])
        {
            return false;
        }

        Level2Slot->Values[L2] = nullptr;
        Level2Slot->ValuePresent[L2] = false;
        --Level2Slot->UsedCount;
        --Size;

        if (Level2Slot->UsedCount == 0)
        {
            Level2Pool.release(Level2Slot);
            Level2Slot = nullptr;
            Level1Slot->ChildPresent[L1] = false;
            --Level1Slot->UsedCount;
        }

        if (Level1Slot->UsedCount == 0)
        {
            Level1Pool.release(Level1Slot);
            Level1Slot = nullptr;
        }

        return true;
    }

    // -------- 清空 --------
    void clear() noexcept
    {
        for (Level1Node*& Level1Slot : Root)
        {
            if (!Level1Slot)
            {
                continue;
            }

            // 必须用位图遍历，因为 Children 数组里可能残留脏指针
            for (size_t I = 0; I < Level1Size; ++I)
            {
                if (Level1Slot->ChildPresent[I])
                {
                    Level2Pool.release(Level1Slot->Children[I]);
                    Level1Slot->Children[I] = nullptr;
                }
            }

            Level1Pool.release(Level1Slot);
            Level1Slot = nullptr;
        }

        Size = 0;
    }

    void resetStatistics() noexcept
    {
        PoolExhaustionFailureCount = 0;
        KeyOutOfRangeFailureCount = 0;
    }

    // -------- 观测 --------
    [[nodiscard]] size_t size() const noexcept { return Size; }
    [[nodiscard]] bool empty() const noexcept { return Size == 0; }

    [[nodiscard]] size_t getLevel1NodesCapacity() const noexcept { return Level1Pool.capacity(); }
    [[nodiscard]] size_t getLevel1NodesUsed() const noexcept { return Level1Pool.used(); }
    [[nodiscard]] size_t getLevel2NodesCapacity() const noexcept { return Level2Pool.capacity(); }
    [[nodiscard]] size_t getLevel2NodesUsed() const noexcept { return Level2Pool.used(); }

    [[nodiscard]] size_t getPoolExhaustionFailureCount() const noexcept { return PoolExhaustionFailureCount; }
    [[nodiscard]] size_t getKeyOutOfRangeFailureCount() const noexcept { return KeyOutOfRangeFailureCount; }

private:
    struct Level2Node
    {
        Level2Node* PoolNext = nullptr;
        std::array<ValueType, Level2Size> Values{};   // 脏数据由 ValuePresent 门控
        std::bitset<Level2Size> ValuePresent{};
        size_t UsedCount = 0;

        void reset() noexcept
        {
            ValuePresent.reset();   // 128 B memset，替代 8 KB
            UsedCount = 0;
        }
    };

    struct Level1Node
    {
        Level1Node* PoolNext = nullptr;
        std::array<Level2Node*, Level1Size> Children{};
        std::bitset<Level1Size> ChildPresent{};
        size_t UsedCount = 0;

        void reset() noexcept
        {
            ChildPresent.reset();   // 128 B memset
            UsedCount = 0;
        }
    };

    template <typename NodeType,size_t Capacity>
    class FixedNodePool
    {
    public:
        static_assert(Capacity > 0, "Pool capacity must be non-zero.");

        FixedNodePool() { reset(); }

        FixedNodePool(const FixedNodePool&) = delete;
        FixedNodePool& operator=(const FixedNodePool&) = delete;

        [[nodiscard]] NodeType* acquire() noexcept
        {
            if (!FreeList)
            {
                return nullptr;
            }

            NodeType* Node = FreeList;
            FreeList = Node->PoolNext;
            Node->PoolNext = nullptr;
            --FreeCount;
            // 节点在 release 时已经 reset 过，这里不再 reset
            return Node;
        }

        void release(NodeType* Node) noexcept
        {
            if (!Node)
            {
                return;
            }

            Node->reset();          // 唯一的 reset 点
            Node->PoolNext = FreeList;
            FreeList = Node;
            ++FreeCount;
        }

        [[nodiscard]] constexpr size_t capacity() const noexcept { return Capacity; }
        [[nodiscard]]size_t freeCount() const noexcept { return FreeCount; }
        [[nodiscard]]size_t used() const noexcept { return Capacity - FreeCount; }

    private:
        void reset() noexcept
        {
            FreeList = nullptr;
            FreeCount = 0;
            for (NodeType& Node : Storage)
            {
                Node.reset();
                Node.PoolNext = FreeList;
                FreeList = &Node;
                ++FreeCount;
            }
        }

        std::array<NodeType, Capacity> Storage{};
        NodeType* FreeList = nullptr;
        size_t FreeCount = 0;
    };

    [[nodiscard]] static bool decodeKey(
        const uint64_t Key,
        size_t& OutLevel0Index,
        size_t& OutLevel1Index,
        size_t& OutLevel2Index) noexcept
    {
        const uint64_t Normalized = Key >> KeyShift;
        if ((Normalized >> IndexedBits) != 0)
        {
            return false;
        }

        OutLevel2Index = static_cast<size_t>(Normalized & Level2Mask);
        OutLevel1Index = static_cast<size_t>((Normalized >> Level2Bits) & Level1Mask);
        OutLevel0Index = static_cast<size_t>((Normalized >> (Level2Bits + Level1Bits)) & Level0Mask);
        return true;
    }

    std::array<Level1Node*, Level0Size> Root{};
    FixedNodePool<Level1Node, Level1NodeCapacity> Level1Pool;
    FixedNodePool<Level2Node, Level2NodeCapacity> Level2Pool;
    size_t Size = 0;
    size_t PoolExhaustionFailureCount = 0;
    size_t KeyOutOfRangeFailureCount = 0;
};

} // namespace infra::containers


#endif //STABLEARRAY_RADIXTREEMAP_H
