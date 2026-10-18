//
// Created by Lzorn on 2026/9/24.
//

#ifndef STABLEARRAY_MEMSPAN_H
#define STABLEARRAY_MEMSPAN_H
#include <cstddef>
#include <bit>
#include <infra/memory/MemoryCommon.hpp>
#include <infra/memory/VirtualMemoryBackend.hpp>

namespace infra::mem
{
/**
 * @brief span 元数据：一个 span 仅服务一个 size class，内部块通过侵入式单链表组织。
 * @note 通过 FreeBlocks 驱动 Full / Partial / Empty 状态机；页级内存由 PageSpan 描述。
 */
class MemSpan
{
public:
    static constexpr std::uint8_t StatePartial  = 1u << 0;
    static constexpr std::uint8_t StateEmpty    = 1u << 1;
    static constexpr std::uint8_t StateReleased = 1u << 2;

    /**
     * @brief 清空 span 所有字段，使其可被对象池复用。
     * @return void
     */
    void reset() noexcept;

    /**
     * @brief 初始化 span 并按块大小构建侵入式空闲链表。
     * @param InBlockSize       块大小（字节）
     * @param InTotalBlocks     块总数
     * @param InSizeClassIndex  所属 size class 索引
     * @param InBucketShardIndex 所属中央池 shard 索引
     * @param InSpanBase        span 起始地址
     * @param InBackingSpan     底层 PageSpan（页级内存描述）
     * @return void
     */
    void setup(
        uint32_t InBlockSize, 
        uint16_t InTotalBlocks, 
        uint16_t InSizeClassIndex, 
        uint16_t InBucketShardIndex,
        std::byte *InSpanBase,
        const PageSpan &InBackingSpan) noexcept;

    [[nodiscard]] void *popBlock() noexcept;
    [[nodiscard]] void* popBatch(uint16_t MaxCount, void*& Tail) noexcept;
    void pushBlock(void *Block) noexcept;
    void pushBatch(void *BatchHead, void *BatchTail, uint32_t BatchCount) noexcept;
    void setPartial(bool Value) noexcept;
    void setEmpty(bool Value) noexcept;
    void setReleased(bool Value) noexcept;

    [[nodiscard]] bool isFull() const noexcept { return FreeBlocks == 0; }
    [[nodiscard]] bool isEmpty() const noexcept { return FreeBlocks == TotalBlocks; }
    [[nodiscard]] bool isReleased() const noexcept      { return (StateFlags & StateReleased) != 0; }
    [[nodiscard]] bool isInPartialList() const noexcept { return (StateFlags & StatePartial)  != 0; }
    [[nodiscard]] bool isInEmptyList() const noexcept   { return (StateFlags & StateEmpty)    != 0; }
    [[nodiscard]] bool isPartial() const noexcept { return FreeBlocks > 0 && FreeBlocks < TotalBlocks; }
    [[nodiscard]] uint16_t getSizeClassIndex() const noexcept { return SizeClassIndex; }
    [[nodiscard]] uint16_t getBucketShardIndex() const noexcept { return BucketShardIndex; }
    [[nodiscard]] uint32_t getBlockSize() const noexcept { return BlockSize; }
    [[nodiscard]] uint16_t getTotalBlocks() const noexcept { return TotalBlocks; }
    [[nodiscard]] uint16_t getFreeBlocks() const noexcept { return FreeBlocks; }
    [[nodiscard]] void *getFreeListHead() const noexcept { return FreeList; }
    [[nodiscard]] std::byte *getSpanBase() const noexcept { return SpanBase; }
    [[nodiscard]] const PageSpan &getBackingSpan() const noexcept { return BackingSpan; }

    [[nodiscard]] std::size_t getBytes() const noexcept
    {
        return static_cast<std::size_t>(TotalBlocks) * static_cast<std::size_t>(BlockSize);
    }

public:
    MemSpan *Next     = nullptr;
    MemSpan *Prev     = nullptr;
    MemSpan *PoolNext = nullptr;

private:
    // 热字段 pop/push 首访, 尽量在第一条 cache line
    void* FreeList = nullptr;                 // 8
    uint16_t FreeBlocks = 0;                  // 2
    uint16_t TotalBlocks = 0;                 // 2
    uint32_t BlockSize = 0;                   // 4
    // 16 bytes 对齐到 cache line

    // 中频字段
    uint16_t SizeClassIndex = InvalidSizeClass;   // 2
    uint16_t BucketShardIndex = 0;                // 2
    uint8_t StateFlags = 0;                       // 1

    // 冷字段
    std::byte* SpanBase = nullptr;
    PageSpan BackingSpan{};
};
};



#endif //STABLEARRAY_MEMSPAN_H
