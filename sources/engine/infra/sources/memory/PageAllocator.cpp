//
// Created by Lzorn on 2026/9/23.
//
#include <infra/memory/PageAllocator.hpp>

namespace infra::mem
{

PageAllocator::PageAllocator(const AllocatorConfig& InConfig)
    : Config(InConfig)
    , PageSize(VirtualMemoryBackend::getSystemPageSize())
    , ActiveShardCount(determineShardCount())
{
    if (ActiveShardCount == 0)
    {
        return;
    }

    // 从 bootstrap arena 分配 Shard 数组
    void* Mem = BootstrapArena::self().allocate(
        sizeof(Shard) * ActiveShardCount, alignof(Shard));
    if (!Mem)
    {
        ActiveShardCount = 0;
        return;
    }

    Shards = static_cast<Shard*>(Mem);
    for (size_t I = 0; I < ActiveShardCount; ++I)
    {
        new (Shards + I) Shard{};
        // 交错分配 RegionId，使不同 shard 的 ID 分布均匀
        Shards[I].NextRegionId = (I == 0) ? ActiveShardCount : I;
    }
}

PageAllocator::~PageAllocator()
{
    if (!Shards)
    {
        return;
    }

    for (size_t I = 0; I < ActiveShardCount; ++I)
    {
        Shard& ShardState = Shards[I];
        std::lock_guard<SpinLock> Lock(ShardState.Mutex);

        // 释放所有 VM 区域；RegionCursor 元数据来自 arena，不回收
        for (RegionCursor* Cursor = ShardState.RegionsHead;
             Cursor != nullptr;
             Cursor = Cursor->Next)
        {
            VirtualMemoryBackend::release(Cursor->Region);
        }
        ShardState.RegionsHead = nullptr;

        // 手动调用析构（bootstrap 内存不会自动析构）
        ShardState.~Shard();
    }

    // Shards 数组本身来自 arena，deallocate 是 no-op，无需显式释放
    Shards = nullptr;
    ActiveShardCount = 0;
}

PageAllocator::RegionIndexPoolStats PageAllocator::getRegionIndexPoolStats()
{
    RegionIndexPoolStats Stats;

    for (size_t I = 0; I < ActiveShardCount; ++I)
    {
        Shard& ShardState = Shards[I];
        std::lock_guard<SpinLock> Lock(ShardState.Mutex);

        // Region 数量：遍历 intrusive 链表（比 deque 遍历更缓存友好）
        for (RegionCursor* Cursor = ShardState.RegionsHead;
             Cursor != nullptr;
             Cursor = Cursor->Next)
        {
            ++Stats.RegionCount;
        }

        Stats.EntryCount += ShardState.RegionIndex.size();
        Stats.Level1NodesUsed += ShardState.RegionIndex.getLevel1NodesUsed();
        Stats.Level1NodesCapacity += ShardState.RegionIndex.getLevel1NodesCapacity();
        Stats.Level2NodesUsed += ShardState.RegionIndex.getLevel2NodesUsed();
        Stats.Level2NodesCapacity += ShardState.RegionIndex.getLevel2NodesCapacity();
        Stats.PoolExhaustionWarningCount +=
            ShardState.RegionIndex.getPoolExhaustionFailureCount();
    }

    Stats.Level1UsageRatio = toUsageRatio(Stats.Level1NodesUsed, Stats.Level1NodesCapacity);
    Stats.Level2UsageRatio = toUsageRatio(Stats.Level2NodesUsed, Stats.Level2NodesCapacity);
    return Stats;
}

PageSpan PageAllocator::acquireSpan(size_t PageCount)
{
    CORE_MEM_BENCH_SCOPE(mem::MemoryBenchPoint::PageAllocatorAcquireSpan);

    if (PageCount == 0 || ActiveShardCount == 0) [[unlikely]]
    {
        return {};
    }

    Shard& ShardState = getShardForCurrentThread();
    std::lock_guard<SpinLock> Lock(ShardState.Mutex);

    // -------- 路径 1：FreeSpanCache 命中 --------
    PageSpan CachedSpan;
    if (tryPopCachedSpanLocked(ShardState, PageCount, CachedSpan))
    {
        RegionCursor* Owner = ShardState.RegionIndex.find(CachedSpan.OwnerRegionId);
        if (!Owner || !Owner->Region.isValid()) [[unlikely]]
        {
            return {};
        }

        const size_t Offset =
            static_cast<size_t>(CachedSpan.Base - Owner->Region.Base);
        const size_t SpanBytes = CachedSpan.getBytes();
        if (Offset > Owner->Region.ReservedSize ||
            SpanBytes > Owner->Region.ReservedSize - Offset)
        {
            return {};
        }

        if (!VirtualMemoryBackend::commit(Owner->Region, Offset, SpanBytes)) [[unlikely]]
        {
            // commit 失败：放回缓存避免丢失
            pushCachedSpanLocked(ShardState, CachedSpan);
            return {};
        }

        return CachedSpan;
    }

    // -------- 路径 2：从已有 Region 的 bump 指针分配 --------
    for (RegionCursor* Cursor = ShardState.RegionsHead;
         Cursor != nullptr;
         Cursor = Cursor->Next)
    {
        const size_t AvailablePages = Cursor->ReservedPages - Cursor->NextPage;
        if (AvailablePages < PageCount)
        {
            continue;
        }

        PageSpan Span;
        Span.Base = Cursor->Region.Base + Cursor->NextPage * PageSize;
        Span.PageCount = PageCount;
        Span.PageSize = PageSize;
        Span.OwnerRegionId = Cursor->RegionId;

        const size_t Offset = Cursor->NextPage * PageSize;
        Cursor->NextPage += PageCount;

        if (!VirtualMemoryBackend::commit(Cursor->Region, Offset, Span.getBytes()))
        {
            Cursor->NextPage -= PageCount;
            return {};
        }

        return Span;
    }

    // -------- 路径 3：分配新的 VirtualRegion --------
    const size_t RequestedBytes = PageCount * PageSize;
    const size_t ReserveBytes = alignUp(
        std::max(Config.SmallRegionReserveBytes, RequestedBytes), PageSize);

    VirtualRegion Region = VirtualMemoryBackend::reserve(
        ReserveBytes, Config.EnableHugePage, Config.PreferredNumaNode);
    if (!Region.isValid())
    {
        return {};
    }

    // 从 bootstrap arena 切出 RegionCursor（placement new）
    void* NodeMem = BootstrapArena::self().allocate(
        sizeof(RegionCursor), alignof(RegionCursor));
    if (!NodeMem) [[unlikely]]
    {
        VirtualMemoryBackend::release(Region);
        return {};
    }

    RegionCursor* NewCursor = new (NodeMem) RegionCursor{};
    NewCursor->Region = Region;
    NewCursor->RegionId = ShardState.NextRegionId;
    ShardState.NextRegionId += ActiveShardCount;
    NewCursor->ReservedPages = ReserveBytes / PageSize;
    NewCursor->NextPage = 0;

    // 插入索引（先插入再链接链表，失败时无需回滚链表）
    const auto InsertResult = ShardState.RegionIndex.insertOrAssign(
        NewCursor->RegionId, NewCursor);
    if (InsertResult != RadixTreeMap<
            RegionCursor*, 0, 8, 8, 8, 16, 32>::InsertResult::Inserted &&
        InsertResult != RadixTreeMap<
            RegionCursor*, 0, 8, 8, 8, 16, 32>::InsertResult::Replaced)
    {
        VirtualMemoryBackend::release(Region);
        // NewCursor 内存来自 arena，不回收
        return {};
    }

    // 头插链表
    NewCursor->Next = ShardState.RegionsHead;
    ShardState.RegionsHead = NewCursor;

    // 从新 Region 分配
    PageSpan Span;
    Span.Base = Region.Base;
    Span.PageCount = PageCount;
    Span.PageSize = PageSize;
    Span.OwnerRegionId = NewCursor->RegionId;
    NewCursor->NextPage = PageCount;

    if (!VirtualMemoryBackend::commit(Region, 0, Span.getBytes())) [[unlikely]]
    {
        // 回滚链表与索引
        ShardState.RegionsHead = NewCursor->Next;
        ShardState.RegionIndex.erase(NewCursor->RegionId);
        VirtualMemoryBackend::release(Region);
        return {};
    }

    return Span;
}

void PageAllocator::releaseSpan(const PageSpan& Span)
{
    CORE_MEM_BENCH_SCOPE(mem::MemoryBenchPoint::PageAllocatorReleaseSpan);

    if (!Span.isValid() || ActiveShardCount == 0) [[unlikely]]
    {
        return;
    }

    Shard& ShardState = getShard(Span.OwnerRegionId);
    std::lock_guard<SpinLock> Lock(ShardState.Mutex);

    if constexpr (MemoryCompileLazyCommit)
    {
        RegionCursor* Owner = ShardState.RegionIndex.find(Span.OwnerRegionId);
        if (Owner && Owner->Region.isValid())
        {
            const size_t Offset = static_cast<size_t>(
                Span.Base - Owner->Region.Base);
            const size_t SpanBytes = Span.getBytes();

            if (Offset <= Owner->Region.ReservedSize &&
                SpanBytes <= Owner->Region.ReservedSize - Offset)
            {
                const bool LargeSpan = SpanBytes >= decommitThresholdBytes();
                const bool Sampled =
                    (ShardState.DecommitCounter++ & DecommitSampleMask) == 0;

                if (LargeSpan || Sampled)
                {
                    VirtualMemoryBackend::decommit(Owner->Region, Offset, SpanBytes);
                }
            }
        }
    }

    pushCachedSpanLocked(ShardState, Span);
}

double PageAllocator::toUsageRatio(const size_t Used, const size_t Capacity) noexcept
{
    if (Capacity == 0)
    {
        return 0.0;
    }
    return static_cast<double>(Used) / static_cast<double>(Capacity);
}

size_t PageAllocator::determineShardCount() noexcept
{
    const size_t Hardware = std::thread::hardware_concurrency();
    const size_t Desired = Hardware == 0
        ? PageAllocatorMinShardCount
        : Hardware * PageAllocatorShardScale;
    return std::max<size_t>(1, std::min<size_t>(Desired, MaxShardCount));
}

size_t PageAllocator::getCacheBucket(const size_t PageCount) noexcept
{
    if (PageCount == 0)
    {
        return 0;
    }
    if (PageCount <= 4)
    {
        return PageCount - 1;
    }
    if (PageCount <= 16)
    {
        return 4 + (PageCount / 2);
    }

    const size_t FloorLog2 = std::bit_width(PageCount) - 1;
    return std::min<size_t>(
        PageAllocatorCacheBucketCount - 1,
        12 + FloorLog2);
}

size_t PageAllocator::decommitThresholdBytes() const noexcept
{
    const size_t DynamicThreshold = std::max<size_t>(
        PageAllocatorDecommitMinBytes,
        Config.SmallRegionReserveBytes / PageAllocatorDecommitRegionDivisor);
    return std::max<size_t>(
        DynamicThreshold,
        PageSize * PageAllocatorDecommitMinPages);
}

size_t PageAllocator::selectShardIndex() const noexcept
{
    if (ActiveShardCount <= 1)
    {
        return 0;
    }

    thread_local const size_t Hash =
        std::hash<std::thread::id>{}(std::this_thread::get_id());
    return Hash % ActiveShardCount;
}

PageAllocator::Shard& PageAllocator::getShard(uint64_t RegionId) const noexcept
{
    if (ActiveShardCount <= 1)
    {
        return Shards[0];
    }
    return Shards[static_cast<size_t>(RegionId % ActiveShardCount)];
}

bool PageAllocator::tryPopCachedSpanLocked(Shard& ShardState, size_t PageCount, PageSpan& OutSpan)
{
    auto& Bucket = ShardState.FreeSpanCache[getCacheBucket(PageCount)];
    for (size_t I = Bucket.size(); I > 0; --I)
    {
        const PageSpan& Candidate = Bucket[I - 1];
        if (Candidate.PageCount != PageCount)
        {
            continue;
        }

        OutSpan = Candidate;
        Bucket[I - 1] = Bucket.back();
        Bucket.pop_back();
        return true;
    }

    auto& Oversized = ShardState.OversizedFreeSpanCache;
    for (size_t I = 0; I < Oversized.size(); ++I)
    {
        if (Oversized[I].PageCount != PageCount)
        {
            continue;
        }

        OutSpan = Oversized[I];
        Oversized[I] = Oversized.back();
        Oversized.pop_back();
        return true;
    }

    return false;
}

void PageAllocator::pushCachedSpanLocked(Shard& ShardState, const PageSpan& Span)
{
    if (Span.PageCount <= PageAllocatorCachedMaxPageCount)
    {
        ShardState.FreeSpanCache[getCacheBucket(Span.PageCount)].push_back(Span);
        return;
    }

    ShardState.OversizedFreeSpanCache.push_back(Span);
}
}