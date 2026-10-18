//
// Created by Lzorn on 2026/9/23.
//

#ifndef STABLEARRAY_PAGEALLOCATOR_H
#define STABLEARRAY_PAGEALLOCATOR_H

#include <bit>

#include <infra/memory/MemoryCommon.hpp>
#include <infra/memory/VirtualMemoryBackend.hpp>
#include <infra/memory/BootstrapAllocator.hpp>
#include <infra/memory/RadixTreeMap.hpp>
#include <infra/memory/MemoryVisibility.hpp>

namespace infra::mem
{


/**
 * @brief 页分配器：从 VirtualMemoryBackend 切出连续的 PageSpan，并按线程哈希分片。
 * @note 每个 shard 独立维护 Region 链表、RegionIndex 与 FreeSpanCache，降低页级竞争。
 */
class PageAllocator
{
    static constexpr size_t MaxShardCount = PageAllocatorMaxShardCount;
    static constexpr std::uint32_t DecommitSampleMask = PageAllocatorDecommitSampleMask;

    struct RegionCursor
    {
        VirtualRegion Region;
        uint64_t RegionId = 0;
        size_t ReservedPages = 0;
        size_t NextPage = 0;
        RegionCursor* Next = nullptr;
    };
    
    // RadixTreeMap 配置：L0=8,L1=8,L2=8 -> 24 位键空间(1600w)
    // 池容量：每 shard 最多 ~16 个 L1 节点、32 个 L2 节点
    using RegionIndexType = RadixTreeMap<RegionCursor*, 0, 8, 8, 8, 16, 32>;

    struct Shard
    {
        SpinLock Mutex;
        RegionCursor* RegionsHead = nullptr;
        RegionIndexType RegionIndex;
        using SpanVector = std::vector<PageSpan, BootstrapAllocator<PageSpan>>;
        std::array<SpanVector, PageAllocatorCacheBucketCount> FreeSpanCache;
        SpanVector OversizedFreeSpanCache;
        uint64_t NextRegionId = 1;
        std::uint32_t DecommitCounter = 0;
    };
public:
    struct RegionIndexPoolStats
    {
        size_t RegionCount = 0;
        size_t EntryCount = 0;
        size_t Level1NodesUsed = 0;
        size_t Level1NodesCapacity = 0;
        size_t Level2NodesUsed = 0;
        size_t Level2NodesCapacity = 0;
        double Level1UsageRatio = 0.0;
        double Level2UsageRatio = 0.0;
        size_t PoolExhaustionWarningCount = 0;
    };

    explicit PageAllocator(const AllocatorConfig& InConfig);
    ~PageAllocator();

    PageAllocator(const PageAllocator&) = delete;
    PageAllocator& operator=(const PageAllocator&) = delete;

    [[nodiscard]] size_t getPageSize() const noexcept { return PageSize; }

    /**
     * @brief 读取 RegionIndex 对象池的容量/用量统计。
     * @return RegionIndexPoolStats 各 shard 聚合后的池状态
     */
    [[nodiscard]] RegionIndexPoolStats getRegionIndexPoolStats();

    /**
     * @brief 获取一块连续的 PageCount 页 span。
     * @param PageCount 请求页数（>0）
     * @return 有效的 PageSpan；失败返回无效 span（isValid()==false）
     */
    [[nodiscard]] PageSpan acquireSpan(size_t PageCount);

    /**
     * @brief 归还 span（进入 FreeSpanCache，懒提交下按采样 decommit）。
     * @param Span 之前由 acquireSpan 返回的 span
     * @return void
     */
    void releaseSpan(const PageSpan& Span);

private:
    [[nodiscard]] static double toUsageRatio(size_t Used, size_t Capacity) noexcept;
    [[nodiscard]] static size_t determineShardCount() noexcept;
    [[nodiscard]] static size_t getCacheBucket(size_t PageCount) noexcept;
    [[nodiscard]] size_t decommitThresholdBytes() const noexcept;
    [[nodiscard]] size_t selectShardIndex() const noexcept;
    [[nodiscard]] Shard& getShardForCurrentThread() const noexcept { return Shards[selectShardIndex()]; }
    [[nodiscard]] Shard& getShard(uint64_t RegionId) const noexcept;
    [[nodiscard]] static bool tryPopCachedSpanLocked(Shard& ShardState, size_t PageCount, PageSpan& OutSpan);
    static void pushCachedSpanLocked(Shard& ShardState, const PageSpan& Span);

    AllocatorConfig Config;
    size_t PageSize = 0;
    size_t ActiveShardCount = 0;
    Shard* Shards = nullptr;
};


};


#endif //STABLEARRAY_PAGEALLOCATOR_H
