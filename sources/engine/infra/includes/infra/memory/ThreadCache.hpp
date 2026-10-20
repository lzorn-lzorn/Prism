//
// Created by Lzorn on 2026/9/25.
//

#ifndef STABLEARRAY_THREADCACHE_H
#define STABLEARRAY_THREADCACHE_H

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>

#include <infra/memory/CentralPool.hpp>
#include <infra/memory/FreeList.hpp>

namespace infra::mem
{
// 每个 size-class 的最小预算字节数，避免小类被饿死。
inline constexpr size_t ThreadCacheMinBytesPerClass   = 256;
// 每个 size-class 的最小对象数。
inline constexpr size_t ThreadCacheMinObjectsPerClass = 8;
// spill 到 deferred 的最小批量。
inline constexpr uint16_t ThreadCacheSpillMinCount   = 2;
// trim 时每次 drain 的最小块数。
inline constexpr uint16_t ThreadCacheTrimMinDrainChunk = 8;
// trim 时每次 drain 的最大块数。
inline constexpr uint16_t ThreadCacheTrimMaxDrainChunk = 32;
// 触发 trim 的阈值倍率：MaxBytes * 5 / 4（即超 25%）。
inline constexpr size_t ThreadCacheTrimTriggerNumerator   = 5;
inline constexpr size_t ThreadCacheTrimTriggerDenominator = 4;
// 非强制 trim 的回落目标：MaxBytes * 9 / 8（即超 12.5% 即继续回收）。
inline constexpr size_t ThreadCacheTrimTargetNumerator  = 9;
inline constexpr size_t ThreadCacheTrimTargetDenominator = 8;
// 单次 trim 最多执行的批次数因子：NumSizeClasses * Factor。
inline constexpr uint32_t ThreadCacheTrimMaxBatchesFactor = 2;
// DeferredList 单次向 central 回流的最小批量。
inline constexpr uint16_t ThreadCacheDeferredDrainMinBatch = 16;
// DeferredList 单次向 central 回流的最大批量。
inline constexpr uint16_t ThreadCacheDeferredDrainMaxBatch = 128;
// Deferred 总预算占 ThreadCache 总预算的分母（1/2）。
inline constexpr size_t ThreadCacheDeferredBudgetDivisor  = 2;

// 线程私有的次级溢出缓冲：freelist 超预算时先 spill 到这里，
// 避免每次溢出都直接打中央池锁。
struct DeferredFreeList
{
    void *Head        = nullptr;
    uint16_t Count    = 0;
    MemSpan *SpanHint = nullptr;
};

struct ThreadCacheAllocation
{
    void *Block      = nullptr;
    bool Hit         = false;
    bool FromCentral = false;
};

/**
 * @brief 线程私有缓存：命中时零共享竞争。
 * @note 释放路径始终先放入本线程 freelist（无共享查询），超预算时 spill 到
 *       deferred 次级缓冲，最后才按批量回流中央池。跨 shard 的块由
 *       CentralPool::returnBatch 按 span 分组回收到所属 shard。
 */
class ThreadCache
{
public:
    explicit ThreadCache(CentralPool &CentralPool, const AllocatorConfig &Config);
    ThreadCache(const ThreadCache &) = delete;
    ThreadCache &operator=(const ThreadCache &) = delete;

    [[nodiscard]] ThreadCacheAllocation allocate(uint16_t SizeClassIndex, size_t BlockSize);
    void deallocate(uint16_t SizeClassIndex, size_t BlockSize, void *Ptr);

    void onIdle();
    void flushAll();

    [[nodiscard]] size_t getCurrentBytes() const noexcept
    {
        return CurrentBytes;
    }

    [[nodiscard]] size_t getDeferredBytes() const noexcept
    {
        return DeferredBytes;
    }

private:
    [[nodiscard]] size_t getTotalRetainedBytes() const noexcept
    {
        return CurrentBytes + DeferredBytes;
    }

    void refillFromDeferred(uint16_t SizeClassIndex, size_t BlockSize);
    void spillToDeferred(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount);
    void drainDeferredClassToCentral(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount);
    void drainDeferredToCentral(bool ForceAll);
    void drainToCentral(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount, MemSpan *SpanHint);
    void trimToBudget(bool ForceAll);

    CentralPool *CentralPool = nullptr;
    std::array<FreeList, NumSizeClasses> FreeLists{};
    std::array<DeferredFreeList, NumSizeClasses> DeferredLists{};
    size_t CurrentBytes        = 0;
    size_t DeferredBytes       = 0;
    size_t MaxBytes            = 0;
    size_t DeferredBudgetBytes = 0;
    uint16_t RefillBatch       = 0;
    size_t LastDrainClass         = 0;
    size_t LastDeferredDrainClass = 0;
};

class ThreadCacheRegistry
{
public:
    /**
     * @brief 绑定进程级中央组件，用于 TLS 缓存创建。
     * @param
     *     - CentralPool  CentralPool&           共享的中央池实例。
     *     - Config       const AllocatorConfig&  运行时分配器配置。
     * @usage
     *     - 必须在线程上首次调用 get() 之前调用。
     * @return
     *     - void
     */
    static void bind(CentralPool &CentralPool, const AllocatorConfig &Config)
    {
        CentralPoolRef.store(&CentralPool, std::memory_order_release);
        ConfigRef.store(&Config, std::memory_order_release);
    }

    /**
     * @brief 获取当前线程的线程本地缓存实例。
     * @param
     *     - none
     * @usage
     *     - 热分配路径应调用一次，然后保留本地引用。
     * @return
     *     - ThreadCache&  当前线程缓存引用。
     */
    [[nodiscard]] static ThreadCache &self()
    {
        CentralPool *CentralPool      = CentralPoolRef.load(std::memory_order_acquire);
        const AllocatorConfig *Config = ConfigRef.load(std::memory_order_acquire);
        assert(CentralPool && "ThreadCacheRegistry is not bound to a CentralPool");
        assert(Config && "ThreadCacheRegistry is not bound to an AllocatorConfig");

        if (!Slot.Cache)
        {
            Slot.Cache = new ThreadCache(*CentralPool, *Config);
        }

        return *Slot.Cache;
    }

    /**
     * @brief 显式冲刷并销毁当前线程缓存。
     * @param
     *     - none
     * @usage
     *     - 在已知的工作线程关闭边界处调用。
     * @return
     *     - void
     */
    static void onThreadExit()
    {
        if (Slot.Cache)
        {
            Slot.Cache->flushAll();
            delete Slot.Cache;
            Slot.Cache = nullptr;
        }
    }

    static void onThreadIdle()
    {
        if (Slot.Cache)
        {
            Slot.Cache->onIdle();
        }
    }

private:
    struct ThreadLocalSlot
    {
        ThreadCache *Cache = nullptr;

        ~ThreadLocalSlot()
        {
            if (Cache)
            {
                Cache->flushAll();
                delete Cache;
                Cache = nullptr;
            }
        }
    };

    static std::atomic<CentralPool *> CentralPoolRef;
    static std::atomic<const AllocatorConfig *> ConfigRef;
    static thread_local ThreadLocalSlot Slot;
};

inline std::atomic<CentralPool *> ThreadCacheRegistry::CentralPoolRef      = nullptr;
inline std::atomic<const AllocatorConfig *> ThreadCacheRegistry::ConfigRef = nullptr;
inline thread_local ThreadCacheRegistry::ThreadLocalSlot ThreadCacheRegistry::Slot{};
} // namespace infra::mem


#endif //STABLEARRAY_THREADCACHE_H
