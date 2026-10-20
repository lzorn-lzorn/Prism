#pragma once
// ============================================================================
// MPMCQueue.hpp — 无锁多生产者多消费者 (MPMC) 有界队列
// ============================================================================
//
// 基于 Dmitry Vyukov 的经典 Bounded MPMC Queue 算法实现.
// 适用于多线程间高效, 无锁地传递消息.
//
// ■ 核心特性:
//   - 完全无锁 (lock-free), 基于 CAS (Compare-And-Swap) 原子操作实现同步
//   - 支持任意数量的生产者和消费者线程并发访问
//   - 有界队列: 容量在编译期确定, 必须为 2 的幂
//   - Cache Line 对齐: 关键成员分布在不同 Cache Line 上, 避免伪共享
//
// ■ 算法原理:
//   队列由固定大小的 cell 数组 (环形缓冲区) 组成.
//   每个 cell 维护一个原子序列号 (Sequence):
//     - 入队时: Sequence == 入队位置 -> cell 可写入
//     - 出队时: Sequence == 出队位置 + 1 -> cell 可读取
//   通过 CAS 在入队/出队位置的原子计数器上竞争, 实现无锁同步.
//
// ■ 用法:
//  infra::MPMCQueue<int, 1024> queue;
//
//   // 生产者线程
//   if (queue.tryPush(42))
//       std::cout << "入队成功" << std::endl;
//
//   // 消费者线程
//   if (auto val = queue.tryPop())
//       std::cout << "出队: " << *val << std::endl;
//
// ■ 线程安全: 所有公开方法均可从任意线程安全调用, 无需额外同步.
// ============================================================================

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>
#include <infra/common/Common.hpp>

namespace infra
{

// ============================================================================
// MPMCQueue<Ty, Capacity>
// ============================================================================
//
// 模板参数:
//   Ty        — 队列元素类型, 必须满足 MoveConstructible
//   Capacity — 队列容量, 必须为 2 的幂 (如 256, 512, 1024, 4096)
//              用位掩码取模, 因此必须为 2 的幂以获得最佳性能
//
template<typename Ty, size_t Capacity = 1024>
    requires (Capacity > 0 && (Capacity & (Capacity - 1)) == 0)
class MPMCQueue
{
    static_assert(
        std::is_nothrow_move_constructible_v<Ty> ||
        std::is_nothrow_move_assignable_v<Ty>,
        "MPMCQueue: Ty must be nothrow move constructible or nothrow move assignable"
    );
public:
    using value_type = Ty;
    using reference = value_type&;
    using const_reference = const value_type&;
    using difference_type = std::ptrdiff_t;
    using size_type = std::size_t;
    
private:
    // 环形缓冲区中的每个存储单元
    struct Cell_T
    {
        std::atomic<size_t> Sequence; // 序列号, 控制读写时序
        Ty Data;                       // 实际存储的数据
    };

    static constexpr size_t Mask = Capacity - 1; // 位掩码, 替代 % 取模运算

    // 三个关键成员分别对齐到独立的 Cache Line, 避免伪共享:
    //   - Buffer:     共享环形缓冲区 (所有线程访问)
    //   - EnqueuePos: 仅生产者竞争
    //   - DequeuePos: 仅消费者竞争
    alignas(CacheLineSize) Cell_T                Buffer[Capacity];
    alignas(CacheLineSize) std::atomic<size_t>   EnqueuePos{0};
    alignas(CacheLineSize) std::atomic<size_t>   DequeuePos{0};

public:
    MPMCQueue()
    {
        // 初始化: 每个 cell 的序列号设为其索引值, 表示 "可写入" 状态
        for (size_t i = 0; i < Capacity; ++i)
            Buffer[i].Sequence.store(i, std::memory_order_relaxed);
    }

    // 不可复制, 不可移动 (内含原子变量, 语义上不允许)
    MPMCQueue(const MPMCQueue&)            = delete;
    MPMCQueue& operator=(const MPMCQueue&) = delete;
    MPMCQueue(MPMCQueue&&)                 = delete;
    MPMCQueue& operator=(MPMCQueue&&)      = delete;

    /** 
     * @brief 尝试入队一个元素 (生产者调用)
     * @return: true = 入队成功, false = 队列已满
     *
     * 流程:
     *   1. load(EnqueuePos)     -> 获取当前想要写入的位置 pos
     *   2. load(cell.Sequence)  -> 读取该 cell 的序列号 seq
     *   3. 如果 seq == pos      -> 表示该 cell 空闲可写,
     *      CAS(EnqueuePos, pos, pos+1) 抢占此位置
     *   4. 写入数据, 更新 cell.Sequence = pos + 1 (标记为 "可读取")
     */
    [[nodiscard]] bool tryPush(Ty item)
    {
        Cell_T* cell;
        size_t pos = EnqueuePos.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &Buffer[pos & Mask];
            size_t seq = cell->Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);

            if (diff == 0)
            {
                // cell 可写, 通过 CAS 抢占入队位置
                if (EnqueuePos.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
            {
                // seq < pos -> 队列已满
                return false;
            }
            else
            {
                // diff > 0 -> 被其他生产者抢占, 重新读取位置
                CPU_RELAX;
                pos = EnqueuePos.load(std::memory_order_relaxed);
            }
        }

        cell->Data = std::move(item);
        // 更新序列号 -> 通知消费者此 cell 可读
        cell->Sequence.store(pos + 1, std::memory_order_release);
        return true;
    }

    /** 
     * @brief 尝试出队一个元素 (消费者调用)
     * @return: std::optional<Ty>, 有值 = 出队成功, 空 = 队列为空
     *
     * 流程:
     *   1. load(DequeuePos)     -> 获取当前想要读取的位置 pos
     *   2. load(cell.Sequence)  -> 读取该 cell 的序列号 seq
     *   3. 如果 seq == pos + 1  -> 表示该 cell 已写入数据可读,
     *      CAS(DequeuePos, pos, pos+1) 抢占此位置
     *   4. 移动数据, 更新 cell.Sequence = pos + Capacity (标记为 "可写入")
     */ 
    [[nodiscard]] std::optional<Ty> tryPop()
    {
        Cell_T* cell;
        size_t pos = DequeuePos.load(std::memory_order_relaxed);

        for (;;)
        {
            cell = &Buffer[pos & Mask];
            size_t seq = cell->Sequence.load(std::memory_order_acquire);
            auto diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);

            if (diff == 0)
            {
                // cell 可读, 通过 CAS 抢占出队位置
                if (DequeuePos.compare_exchange_weak(
                        pos, pos + 1, std::memory_order_relaxed))
                    break;
            }
            else if (diff < 0)
            {
                // seq < pos + 1 -> 队列为空
                return std::nullopt;
            }
            else
            {
                // diff > 0 -> 被其他消费者抢占, 重新读取位置
                CPU_RELAX;
                pos = DequeuePos.load(std::memory_order_relaxed);
            }
        }

        Ty result = std::move(cell->Data);
        // 更新序列号 -> 标记此 cell 可被生产者重新写入
        // +Capacity 使得序列号回绕到下一轮
        cell->Sequence.store(pos + Capacity, std::memory_order_release);
        return result;
    }


    template <typename OutputIt>
        requires std::output_iterator<OutputIt, Ty>
    size_t tryPopBulk(OutputIt out, size_t max_count)
    {
        size_t popped = 0;
        while (popped < max_count)
        {
            auto item = tryPop();
            if (!item) break;
            *out++ = std::move(*item);
            ++popped;
        }
        return popped;
    }
    /** 
     * @brief isEmpty — 检查队列是否为空 (近似值)
     * @note 返回值可能在读取后立即过时, 仅用于调试/监控
     */
    [[nodiscard]] bool isEmpty() const noexcept
    {
        return EnqueuePos.load(std::memory_order_relaxed)
            == DequeuePos.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::vector<Ty> getSnapshot() const
    {
        std::vector<Ty> result;
        size_t deq = DequeuePos.load(std::memory_order_acquire);
        size_t enq = EnqueuePos.load(std::memory_order_acquire);
        size_t count = enq >= deq ? enq - deq : 0;
        result.reserve(count);
        for (size_t i = 0; i < count; ++i)
        {
            size_t idx = (deq + i) & Mask;
            // 注意：数据读取必须发生在序列号检查之后，这里简单取快照，
            // 可能读到正在写入的数据，但这是“近似”快照，可接受. 
            // 如果要求严格一致性，需加额外逻辑. 
            result.push_back(Buffer[idx].Data);
        }
        return result;
    }

    /** 
     * @brief getSize — 获取队列近似元素数量
     * @note 返回值可能在读取后立即过时, 仅用于调试/监控
     */
    [[nodiscard]] size_t getSize() const noexcept
    {
        size_t enq = EnqueuePos.load(std::memory_order_relaxed);
        size_t deq = DequeuePos.load(std::memory_order_relaxed);
        return enq >= deq ? enq - deq : 0;
    }

    /** 
     * @brief isFull — 检查队列是否已满 (近似值)
     * @note 返回值可能在读取后立即过时, 仅用于调试/监控
     */
    [[nodiscard]] bool isFull() const noexcept {
        return (EnqueuePos.load(std::memory_order_relaxed) -
                DequeuePos.load(std::memory_order_relaxed)) == Capacity;
    }

    /** 
     * @brief capacity — 获取队列容量 (编译期常量)
     */
    [[nodiscard]] static consteval size_t capacity() { return Capacity; }
};

} // namespace infra
