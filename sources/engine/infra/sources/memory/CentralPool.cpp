//
// Created by Lzorn on 2026/9/24.
//

#include <infra/memory/CentralPool.hpp>

namespace infra::mem
{
CentralPool::CentralPool(infra::mem::PageAllocator& InPageAllocator)
    : PageAllocator(&InPageAllocator)
    , PageSize(InPageAllocator.getPageSize())
    , ActiveBucketShardCount(determineBucketShardCount())
{
    for (size_t i = 0; i < NumSizeClasses; ++i)
    {
        Classes[i] = buildSizeClass(InPageAllocator.getPageSize(), SizeClasses[i]);
    }
}

uint32_t CentralPool::fetchBatch(uint16_t SizeClassIndex, void **OutBlocks, uint32_t MaxBlocks, MemSpan **OutSpanHint)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::CentralPoolFetchBatch, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses || !OutBlocks || MaxBlocks == 0)
    {
        return 0;
    }

    const size_t shardIndex = getThreadShardIndex();
    SizeClassBucketShard &Shard  = Buckets[SizeClassIndex].Shards[shardIndex];
    std::lock_guard<SpinLock> Lock(Shard.Lock);

    MemSpan *SpanObj = Shard.PartialSpans;
    if (!SpanObj)
    {
        SpanObj = Shard.EmptySpans;
    }

    if (!SpanObj)
    {
        SpanObj = allocateSpanForClass(SizeClassIndex, static_cast<uint16_t>(shardIndex), Shard);
        if (!SpanObj)
        {
            return 0;
        }
    }

    if (SpanObj->isInEmptyList())
    {
        removeSpan(Shard.EmptySpans, SpanObj);
        SpanObj->setEmpty(false);
        if (Shard.EmptySpanCount > 0)
        {
            --Shard.EmptySpanCount;
        }
    }

    if (!SpanObj->isInPartialList())
    {
        insertSpan(Shard.PartialSpans, SpanObj);
        SpanObj->setPartial(true);
    }

    uint32_t Allocated = 0;
    while (Allocated < MaxBlocks)
    {
        void *Block = SpanObj->popBlock();
        if (!Block)
        {
            break;
        }

        OutBlocks[Allocated] = Block;
        ++Allocated;
    }

    if (SpanObj->isFull() && SpanObj->isInPartialList())
    {
        removeSpan(Shard.PartialSpans, SpanObj);
        SpanObj->setPartial(false);
    }

    if (OutSpanHint)
    {
        *OutSpanHint = SpanObj;
    }

    return Allocated;
}

void CentralPool::returnBatch(uint16_t SizeClassIndex, void *Head, uint32_t Count, MemSpan *InitialHint)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::CentralPoolReturnBatch, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses || !Head || Count == 0)
    {
        return;
    }

    SizeClassBucket &Bucket = Buckets[SizeClassIndex];

    struct SpanBatch
    {
        MemSpan *SpanObj    = nullptr;
        void *Head          = nullptr;
        void *Tail          = nullptr;
        uint32_t Count      = 0;
        uint16_t ShardIndex = 0;
    };

    std::array<SpanBatch, MaxReturnBatchGroups> Batches{};
    size_t BatchCount = 0;

    auto FindBatch = [&](MemSpan *SpanObj) -> SpanBatch *
    {
        for (size_t i = 0; i < BatchCount; ++i)
        {
            if (Batches[i].SpanObj == SpanObj)
            {
                return &Batches[i];
            }
        }

        return nullptr;
    };

    auto FlushSingleBlock = [&](MemSpan *SpanObj, void *Block)
    {
        if (!SpanObj || !Block)
        {
            return;
        }

        SizeClassBucketShard &Shard = Bucket.Shards[normalizeShardIndex(SpanObj->getBucketShardIndex())];
        std::lock_guard<SpinLock> Lock(Shard.Lock);
        applyReturnedBatchLocked(Shard, *SpanObj, Block, Block, 1);
        trimEmptySpansLocked(Shard);
    };

    void *Current  = Head;
    MemSpan *SpanHint = InitialHint;
    for (uint32_t i = 0; i < Count && Current; ++i)
    {
        void *Next    = *reinterpret_cast<void **>(Current);
        MemSpan *SpanObj = findSpanHinted(Current, SpanHint, SizeClassIndex);

        if (!SpanObj || SpanObj->isReleased())
        {
            Current = Next;
            continue;
        }

        SpanBatch *Batch = FindBatch(SpanObj);
        if (!Batch)
        {
            if (BatchCount < Batches.size())
            {
                Batch             = &Batches[BatchCount++];
                Batch->SpanObj    = SpanObj;
                Batch->ShardIndex = SpanObj->getBucketShardIndex();
            }
            else
            {
                FlushSingleBlock(SpanObj, Current);
                SpanHint = SpanObj;
                Current  = Next;
                continue;
            }
        }

        *reinterpret_cast<void **>(Current) = Batch->Head;
        Batch->Head                         = Current;
        if (!Batch->Tail)
        {
            Batch->Tail = Current;
        }
        ++Batch->Count;
        SpanHint = SpanObj;

        Current = Next;
    }

    for (size_t i = 0; i < BatchCount; ++i)
    {
        SpanBatch &Batch = Batches[i];
        if (!Batch.SpanObj || Batch.Count == 0)
        {
            continue;
        }

        SizeClassBucketShard &Shard = Bucket.Shards[normalizeShardIndex(Batch.ShardIndex)];
        std::lock_guard<SpinLock> Lock(Shard.Lock);
        applyReturnedBatchLocked(Shard, *Batch.SpanObj, Batch.Head, Batch.Tail, Batch.Count);
        trimEmptySpansLocked(Shard);
    }
}
size_t CentralPool::determineBucketShardCount() noexcept
{
    const size_t Hardware = std::thread::hardware_concurrency();
    const size_t Desired  = Hardware == 0
                                     ? CentralPoolMinShardCount
                                     : Hardware * CentralPoolShardScale;
    return std::max<size_t>(
        CentralPoolMinShardCount,
        std::min<size_t>(Desired, CentralPoolBucketMaxShardCount));
}


CentralPool::PageToSpanPoolStats CentralPool::getPageToSpanPoolStats()
{
    PageToSpanPoolStats stats;

    for (size_t I = 0; I < CentralPoolPageMapShardCount; ++I)
    {
        const PageMapShard &Shard = PageToSpanShards[I];
        std::shared_lock Lock(Shard.Mutex);
        stats.EntryCount += Shard.Map.size();
    }

    {
        std::lock_guard storageLock(SpanStorageMutex);
        const size_t Allocated = SpanPoolAllocatedCount.load(std::memory_order_relaxed);
        const size_t Free      = SpanPoolFreeCount.load(std::memory_order_relaxed);
        stats.SpanObjectsAllocated = Allocated;
        stats.SpanObjectsInUse     = Allocated >= Free ? Allocated - Free : 0;
    }

    stats.SpanObjectPoolUsageRatio = toUsageRatio(stats.SpanObjectsInUse, stats.SpanObjectsAllocated);
    return stats;
}

SizeClass CentralPool::buildSizeClass(size_t PageSize, uint32_t BlockSize) noexcept
{
    const uint32_t AlignedBlockSize =
        static_cast<uint32_t>(alignUp(static_cast<size_t>(BlockSize), alignof(std::max_align_t)));

    const size_t TargetBytes = AlignedBlockSize <= 4096 ? 128u * 1024u : 256u * 1024u;
    size_t Blocks            = std::max<size_t>(32, TargetBytes / AlignedBlockSize);

    size_t SpanBytes = alignUp(Blocks * static_cast<size_t>(AlignedBlockSize), PageSize);
    size_t Pages     = std::max<size_t>(1, SpanBytes / PageSize);
    SpanBytes        = Pages * PageSize;
    Blocks           = SpanBytes / AlignedBlockSize;

    if (Blocks > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
    {
        Blocks = std::numeric_limits<uint16_t>::max();
    }
    if (Pages > static_cast<size_t>(std::numeric_limits<uint16_t>::max()))
    {
        Pages = std::numeric_limits<uint16_t>::max();
    }

    SizeClass Info;
    Info.BlockSize     = AlignedBlockSize;
    Info.BlocksPerSpan = static_cast<uint16_t>(Blocks);
    Info.PagesPerSpan  = static_cast<uint16_t>(Pages);
    return Info;
}

MemSpan *CentralPool::allocateSpanForClass(
    uint16_t ClassIndex,
    uint16_t ShardIndex,
    SizeClassBucketShard &Shard)
{
    const SizeClass Info = Classes[ClassIndex];
    PageSpan PageSpan    = PageAllocator->acquireSpan(Info.PagesPerSpan);
    if (!PageSpan.isValid())
    {
        return nullptr;
    }

    MemSpan *SpanRawPtr = acquireSpanObject(ClassIndex);
    if (!SpanRawPtr)
    {
        PageAllocator->releaseSpan(PageSpan);
        return nullptr;
    }

    SpanRawPtr->setup(
        Info.BlockSize,
        Info.BlocksPerSpan,
        ClassIndex,
        ShardIndex,
        PageSpan.Base,
        PageSpan);

    if (!registerSpanPages(*SpanRawPtr))
    {
        PageAllocator->releaseSpan(SpanRawPtr->getBackingSpan());
        releaseSpanObject(SpanRawPtr);
        return nullptr;
    }
    insertSpan(Shard.EmptySpans, SpanRawPtr);
    SpanRawPtr->setEmpty(true);
    ++Shard.EmptySpanCount;

    return SpanRawPtr;
}

void CentralPool::releaseEmptySpan(SizeClassBucketShard &Shard, MemSpan *InSpan)
{
    if (!InSpan || InSpan->isReleased())
    {
        return;
    }

    if (InSpan->isInPartialList())
    {
        removeSpan(Shard.PartialSpans, InSpan);
        InSpan->setPartial(false);
    }

    if (InSpan->isInEmptyList())
    {
        removeSpan(Shard.EmptySpans, InSpan);
        InSpan->setEmpty(false);
        if (Shard.EmptySpanCount > 0)
        {
            --Shard.EmptySpanCount;
        }
    }

    unregisterSpanPages(*InSpan);
    PageAllocator->releaseSpan(InSpan->getBackingSpan());

    InSpan->setReleased(true);
    releaseSpanObject(InSpan);
}

void CentralPool::trimEmptySpansLocked(SizeClassBucketShard &Shard)
{
    while (Shard.EmptySpanCount > MaxRetainedEmptySpansPerClass)
    {
        MemSpan *SpanObj = Shard.EmptySpans;
        if (!SpanObj)
        {
            break;
        }

        releaseEmptySpan(Shard, SpanObj);
    }
}

MemSpan *CentralPool::acquireSpanObject(uint16_t ClassIndex)
{
    // span 对象池的分配/回收频率较低，用 SpanStorageMutex 串行化，
    // 避免无锁栈的 ABA 问题（同一 MemSpan 指针被并发 pop 后重新 push）。
    std::lock_guard storageLock(SpanStorageMutex);

    auto popHeadLocked = [](std::atomic<MemSpan *> &Head) -> MemSpan *
    {
        MemSpan *Node = Head.load(std::memory_order_relaxed);
        if (Node)
        {
            Head.store(Node->PoolNext, std::memory_order_relaxed);
            Node->PoolNext = nullptr;
        }
        return Node;
    };

    auto acquireLocked = [&](std::atomic<MemSpan *> &Head) -> MemSpan *
    {
        MemSpan *Node = popHeadLocked(Head);
        if (Node)
        {
            SpanPoolFreeCount.fetch_sub(1, std::memory_order_relaxed);
            Node->reset();
        }
        return Node;
    };

    if (ClassIndex < NumSizeClasses)
    {
        if (MemSpan *ClassSpan = acquireLocked(SpanClassFreeLists[ClassIndex]))
        {
            return ClassSpan;
        }
    }

    if (MemSpan *GenericSpan = acquireLocked(SpanFreeList))
    {
        return GenericSpan;
    }

    if (!growSpanPoolLocked())
    {
        return nullptr;
    }

    if (ClassIndex < NumSizeClasses)
    {
        if (MemSpan *ClassSpan = acquireLocked(SpanClassFreeLists[ClassIndex]))
        {
            return ClassSpan;
        }
    }

    if (MemSpan *GenericSpan = acquireLocked(SpanFreeList))
    {
        return GenericSpan;
    }

    return nullptr;
}

void CentralPool::releaseSpanObject(MemSpan *InSpan)
{
    if (!InSpan)
    {
        return;
    }

    const uint16_t ClassIndex = InSpan->getSizeClassIndex();
    InSpan->reset();

    std::lock_guard storageLock(SpanStorageMutex);

    std::atomic<MemSpan *> &Head =
        ClassIndex < NumSizeClasses ? SpanClassFreeLists[ClassIndex] : SpanFreeList;
    InSpan->PoolNext = Head.load(std::memory_order_relaxed);
    Head.store(InSpan, std::memory_order_relaxed);

    SpanPoolFreeCount.fetch_add(1, std::memory_order_relaxed);
}

bool CentralPool::growSpanPoolLocked()
{
    // 调用方必须已持有 SpanStorageMutex。
    auto NewChunk = std::unique_ptr<MemSpan[]>(new(std::nothrow) MemSpan[SpanPoolChunkSize]);
    if (!NewChunk)
    {
        return false;
    }

    MemSpan *chunkBase = NewChunk.get();
    MemSpan *LocalHead = nullptr;
    MemSpan *LocalTail = nullptr;
    for (size_t i = 0; i < SpanPoolChunkSize; ++i)
    {
        chunkBase[i].reset();
        chunkBase[i].PoolNext = LocalHead;
        LocalHead             = &chunkBase[i];
        if (!LocalTail)
        {
            LocalTail = &chunkBase[i];
        }
    }

    // 持有锁，直接串接新旧链表（无需 CAS）。
    if (LocalTail)
    {
        LocalTail->PoolNext = SpanFreeList.load(std::memory_order_relaxed);
    }
    SpanFreeList.store(LocalHead, std::memory_order_relaxed);

    SpanPoolChunks.push_back(std::move(NewChunk));
    SpanPoolAllocatedCount.fetch_add(SpanPoolChunkSize, std::memory_order_relaxed);
    SpanPoolFreeCount.fetch_add(SpanPoolChunkSize, std::memory_order_relaxed);
    return true;
}

bool CentralPool::registerSpanPages(MemSpan &InSpan)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::CentralSpanRegisterPages, InSpan.getSizeClassIndex());

    if (!InSpan.getBackingSpan().isValid())
    {
        return false;
    }

    const uintptr_t Base     = reinterpret_cast<uintptr_t>(InSpan.getSpanBase());
    const size_t PageCount   = InSpan.getBackingSpan().PageCount;

    // 按 shard 分桶，使每个 shard 只做一次 copy-on-write，降低写路径拷贝成本。
    std::array<std::vector<uint64_t>, CentralPoolPageMapShardCount> Buckets;
    for (size_t I = 0; I < PageCount; ++I)
    {
        const uint64_t PageBase = static_cast<uint64_t>(Base + I * PageSize);
        Buckets[pageMapShardIndex(PageBase)].push_back(PageBase);
    }

    for (size_t ShardIndex = 0; ShardIndex < CentralPoolPageMapShardCount; ++ShardIndex)
    {
        const auto &Pages = Buckets[ShardIndex];
        if (Pages.empty())
        {
            continue;
        }

        PageMapShard &Shard = PageToSpanShards[ShardIndex];
        std::unique_lock Lock(Shard.Mutex);
        for (const uint64_t PageBase : Pages)
        {
            Shard.Map.insert_or_assign(PageBase, &InSpan);
        }
    }

    return true;
}


void CentralPool::unregisterSpanPages(const MemSpan &InSpan)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::CentralSpanUnregisterPages, InSpan.getSizeClassIndex());

    if (!InSpan.getBackingSpan().isValid())
    {
        return;
    }

    const uintptr_t Base   = reinterpret_cast<uintptr_t>(InSpan.getSpanBase());
    const size_t PageCount = InSpan.getBackingSpan().PageCount;

    std::array<std::vector<uint64_t>, CentralPoolPageMapShardCount> Buckets;
    for (size_t I = 0; I < PageCount; ++I)
    {
        const uint64_t PageBase = static_cast<uint64_t>(Base + I * PageSize);
        Buckets[pageMapShardIndex(PageBase)].push_back(PageBase);
    }

    for (size_t ShardIndex = 0; ShardIndex < CentralPoolPageMapShardCount; ++ShardIndex)
    {
        const auto &Pages = Buckets[ShardIndex];
        if (Pages.empty())
        {
            continue;
        }

        PageMapShard &Shard = PageToSpanShards[ShardIndex];
        std::unique_lock Lock(Shard.Mutex);
        for (const uint64_t PageBase : Pages)
        {
            Shard.Map.erase(PageBase);
        }
    }
}

MemSpan *CentralPool::findSpanSnapshot(void *Ptr) const
{
    const uintptr_t Address = reinterpret_cast<uintptr_t>(Ptr);
    const uint64_t PageBase = static_cast<uint64_t>(Address & ~(PageSize - 1));
    const PageMapShard &Shard = getPageMapShard(PageBase);

    // 共享锁读：读者之间不互斥，只有稀疏的注册/反注册写路径才需要独占锁。
    std::shared_lock Lock(Shard.Mutex);
    const auto It = Shard.Map.find(PageBase);
    return It != Shard.Map.end() ? It->second : nullptr;
}

bool CentralPool::isBlockInSpan(const MemSpan &InSpan, const void *Ptr) noexcept
{
    if (!InSpan.getSpanBase() || InSpan.getBlockSize() == 0 || InSpan.getTotalBlocks() == 0)
    {
        return false;
    }

    const uintptr_t Address = reinterpret_cast<uintptr_t>(Ptr);
    const uintptr_t Begin   = reinterpret_cast<uintptr_t>(InSpan.getSpanBase());
    const uintptr_t End     = Begin + static_cast<uintptr_t>(InSpan.getBlockSize()) * static_cast<uintptr_t>(InSpan.getTotalBlocks());
    return Address >= Begin && Address < End;
}

MemSpan *CentralPool::findSpanHinted(void *Ptr, MemSpan *Hint, uint16_t ExpectedSizeClass) const
{
    if (Hint &&
        !Hint->isReleased() &&
        Hint->getSizeClassIndex() == ExpectedSizeClass &&
        isBlockInSpan(*Hint, Ptr))
    {
        return Hint;
    }

    MemSpan *SpanObj = findSpanSnapshot(Ptr);
    if (!SpanObj)
    {
        return nullptr;
    }

    if (SpanObj->isReleased() || SpanObj->getSizeClassIndex() != ExpectedSizeClass)
    {
        return nullptr;
    }

    return isBlockInSpan(*SpanObj, Ptr) ? SpanObj : nullptr;
}

void CentralPool::applyReturnedBatchLocked(SizeClassBucketShard &Shard, MemSpan &InSpan, void *BatchHead, void *BatchTail, uint32_t BatchCount)
{
    if (BatchCount == 0 || !BatchHead || !BatchTail || InSpan.isReleased())
    {
        return;
    }

    const bool WasFull = InSpan.isFull();
    InSpan.pushBatch(BatchHead, BatchTail, BatchCount);

    if (WasFull)
    {
        insertSpan(Shard.PartialSpans, &InSpan);
        InSpan.setPartial(true);
    }

    if (InSpan.isEmpty())
    {
        if (InSpan.isInPartialList())
        {
            removeSpan(Shard.PartialSpans, &InSpan);
            InSpan.setPartial(false);
        }

        if (!InSpan.isInEmptyList())
        {
            insertSpan(Shard.EmptySpans, &InSpan);
            InSpan.setEmpty(true);
            ++Shard.EmptySpanCount;
        }
    }
}

void CentralPool::insertSpan(MemSpan *&Head, MemSpan *InSpan)
{
    InSpan->Prev = nullptr;
    InSpan->Next = Head;
    if (Head)
    {
        Head->Prev = InSpan;
    }
    Head = InSpan;
}

void CentralPool::removeSpan(MemSpan *&Head, MemSpan *InSpan)
{
    if (InSpan->Prev)
    {
        InSpan->Prev->Next = InSpan->Next;
    }
    else
    {
        Head = InSpan->Next;
    }

    if (InSpan->Next)
    {
        InSpan->Next->Prev = InSpan->Prev;
    }

    InSpan->Next = nullptr;
    InSpan->Prev = nullptr;
}

void *CentralPool::popBlock(void *&Head) noexcept
{
    if (!Head)
    {
        return nullptr;
    }

    void *Block = Head;
    Head        = *reinterpret_cast<void **>(Block);
    return Block;
}

void CentralPool::pushBlock(void *&Head, void *Block) noexcept
{
    *reinterpret_cast<void **>(Block) = Head;
    Head                              = Block;
}

double CentralPool::toUsageRatio(size_t Used, size_t Capacity) noexcept
{
    if (Capacity == 0)
    {
        return 0.0;
    }

    return static_cast<double>(Used) / static_cast<double>(Capacity);
}
}