//
// Created by Lzorn on 2026/9/25.
//

#include <infra/memory/ThreadCache.hpp>

namespace infra::mem
{

ThreadCache::ThreadCache(infra::mem::CentralPool &CentralPool, const AllocatorConfig &Config)
    : CentralPool(&CentralPool),
      MaxBytes(Config.ThreadCacheMaxBytes),
      DeferredBudgetBytes(std::max<size_t>(
          1,
          Config.ThreadCacheMaxBytes / ThreadCacheDeferredBudgetDivisor)),
      RefillBatch(Config.RefillBatchSize)
{
    const size_t BytesPerClass = std::max<size_t>(
        ThreadCacheMinBytesPerClass,
        Config.ThreadCacheMaxBytes / NumSizeClasses);

    for (size_t I = 0; I < NumSizeClasses; ++I)
    {
        const size_t BlockSize      = classIndexToSize(static_cast<uint16_t>(I));
        const size_t CountFromBytes = std::max<size_t>(
            ThreadCacheMinObjectsPerClass,
            BytesPerClass / BlockSize);
        const size_t Capped = std::min<size_t>(CountFromBytes, Config.ThreadCacheMaxPerClass);
        FreeLists[I].setMaxCount(static_cast<uint16_t>(Capped));
    }
}

ThreadCacheAllocation ThreadCache::allocate(uint16_t SizeClassIndex, size_t BlockSize)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::ThreadCacheAllocate, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses) [[unlikely]]
    {
        return {};
    }

    FreeList &List = FreeLists[SizeClassIndex];
    if (List.isEmpty())
    {
        refillFromDeferred(SizeClassIndex, BlockSize);
    }

    if (!List.isEmpty()) [[likely]]
    {
        void *Block  = List.pop();
        CurrentBytes -= BlockSize;

        ThreadCacheAllocation Result;
        Result.Block = Block;
        Result.Hit   = true;
        return Result;
    }

    constexpr uint32_t MaxBatch = 128;
    void *Batch[MaxBatch]{};
    MemSpan *FetchedSpanHint = nullptr;
    const uint32_t Target    = std::min<uint32_t>(RefillBatch, MaxBatch);
    const uint32_t Fetched   = CentralPool->fetchBatch(SizeClassIndex, Batch, Target, &FetchedSpanHint);
    if (Fetched == 0)
    {
        return {};
    }

    if (FetchedSpanHint)
    {
        List.setSpanHint(FetchedSpanHint);
    }

    for (uint32_t I = 1; I < Fetched; ++I)
    {
        List.push(Batch[I]);
        CurrentBytes += BlockSize;
    }

    ThreadCacheAllocation Result;
    Result.Block       = Batch[0];
    Result.FromCentral = true;
    return Result;
}

void ThreadCache::deallocate(uint16_t SizeClassIndex, size_t BlockSize, void *Ptr)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::ThreadCacheDeallocate, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses || !Ptr)
    {
        return;
    }

    FreeList &List = FreeLists[SizeClassIndex];
    List.push(Ptr);
    CurrentBytes += BlockSize;

    // freelist 超预算：spill 到 deferred 次级缓冲，避免直接打中央池锁。
    if (List.getCount() > List.getMaxCount() || CurrentBytes > MaxBytes)
    {
        const uint16_t SpillCount = List.getMaxCount() > 0
            ? static_cast<uint16_t>(
                std::max<uint32_t>(
                    ThreadCacheSpillMinCount,
                    std::min<uint32_t>(
                        RefillBatch,
                        static_cast<uint32_t>(List.getMaxCount() / 4 + 1))))
            : 1;
        spillToDeferred(SizeClassIndex, BlockSize, SpillCount);
    }

    if (DeferredBytes > DeferredBudgetBytes)
    {
        drainDeferredToCentral(false);
    }

    const size_t TriggerBytes =
        (MaxBytes * ThreadCacheTrimTriggerNumerator) /
        ThreadCacheTrimTriggerDenominator;
    if (getTotalRetainedBytes() > TriggerBytes)
    {
        trimToBudget(false);
    }
}

void ThreadCache::onIdle()
{
    drainDeferredToCentral(true);
    trimToBudget(true);
}

void ThreadCache::flushAll()
{
    drainDeferredToCentral(true);

    for (size_t I = 0; I < NumSizeClasses; ++I)
    {
        const size_t BlockSize = classIndexToSize(static_cast<uint16_t>(I));
        drainToCentral(
            static_cast<uint16_t>(I),
            BlockSize,
            FreeLists[I].getCount(),
            FreeLists[I].getSpanHint());
    }
}

void ThreadCache::refillFromDeferred(uint16_t SizeClassIndex, size_t BlockSize)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::ThreadCacheRefillDeferred, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses)
    {
        return;
    }

    DeferredFreeList &Deferred = DeferredLists[SizeClassIndex];
    if (!Deferred.Head)
    {
        return;
    }

    FreeList &List        = FreeLists[SizeClassIndex];
    const uint16_t Target = std::max<uint16_t>(1, RefillBatch);
    uint16_t Moved        = 0;
    while (Moved < Target && Deferred.Head)
    {
        void *Block = Deferred.Head;
        Deferred.Head = *reinterpret_cast<void **>(Block);
        --Deferred.Count;
        DeferredBytes -= BlockSize;
        List.push(Block);
        CurrentBytes += BlockSize;
        ++Moved;
    }

    if (!Deferred.Head)
    {
        Deferred.SpanHint = nullptr;
    }
}

void ThreadCache::spillToDeferred(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount)
{
    CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::ThreadCacheSpillDeferred, SizeClassIndex);

    if (SizeClassIndex >= NumSizeClasses || RequestedCount == 0)
    {
        return;
    }

    FreeList &List             = FreeLists[SizeClassIndex];
    DeferredFreeList &Deferred = DeferredLists[SizeClassIndex];
    uint16_t Spilled           = 0;
    while (Spilled < RequestedCount && !List.isEmpty())
    {
        void *Block  = List.pop();
        CurrentBytes -= BlockSize;
        *reinterpret_cast<void **>(Block) = Deferred.Head;
        Deferred.Head                     = Block;
        ++Deferred.Count;
        DeferredBytes += BlockSize;
        ++Spilled;
    }

    if (Deferred.SpanHint == nullptr)
    {
        Deferred.SpanHint = List.getSpanHint();
    }

    if (List.isEmpty())
    {
        List.setSpanHint(nullptr);
    }
}

void ThreadCache::drainDeferredClassToCentral(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount)
{
    if (SizeClassIndex >= NumSizeClasses || RequestedCount == 0)
    {
        return;
    }

    DeferredFreeList &Deferred = DeferredLists[SizeClassIndex];
    if (!Deferred.Head)
    {
        return;
    }

    void *Chain      = nullptr;
    uint32_t Drained = 0;
    while (Drained < RequestedCount && Deferred.Head)
    {
        void *Block                       = Deferred.Head;
        Deferred.Head                     = *reinterpret_cast<void **>(Block);
        *reinterpret_cast<void **>(Block) = Chain;
        Chain                             = Block;
        --Deferred.Count;
        DeferredBytes -= BlockSize;
        ++Drained;
    }

    if (Chain && Drained > 0)
    {
        CentralPool->returnBatch(SizeClassIndex, Chain, Drained, Deferred.SpanHint);
    }

    if (!Deferred.Head)
    {
        Deferred.SpanHint = nullptr;
    }
}

void ThreadCache::drainDeferredToCentral(bool ForceAll)
{
    CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::ThreadCacheDrainDeferred);

    if (!ForceAll && DeferredBytes <= DeferredBudgetBytes)
    {
        return;
    }

    if (ForceAll)
    {
        for (size_t I = 0; I < NumSizeClasses; ++I)
        {
            const size_t BlockSize = classIndexToSize(static_cast<uint16_t>(I));
            drainDeferredClassToCentral(
                static_cast<uint16_t>(I),
                BlockSize,
                DeferredLists[I].Count);
        }
        return;
    }

    const uint16_t DrainBatch = static_cast<uint16_t>(
        std::max<uint32_t>(
            ThreadCacheDeferredDrainMinBatch,
            std::min<uint32_t>(
                ThreadCacheDeferredDrainMaxBatch,
                RefillBatch)));
    const uint32_t MaxBatches = static_cast<uint32_t>(NumSizeClasses) *
                                ThreadCacheTrimMaxBatchesFactor;

    for (uint32_t Batch = 0; Batch < MaxBatches && DeferredBytes > DeferredBudgetBytes; ++Batch)
    {
        bool Drained = false;
        for (size_t Offset = 0; Offset < NumSizeClasses; ++Offset)
        {
            const size_t Index = (LastDeferredDrainClass + Offset) % NumSizeClasses;
            if (!DeferredLists[Index].Head)
            {
                continue;
            }

            const size_t BlockSize   = classIndexToSize(static_cast<uint16_t>(Index));
            const uint16_t Requested = std::min<uint16_t>(
                DeferredLists[Index].Count,
                DrainBatch);
            drainDeferredClassToCentral(
                static_cast<uint16_t>(Index),
                BlockSize,
                Requested);
            LastDeferredDrainClass = (Index + 1) % NumSizeClasses;
            Drained                = true;
            break;
        }

        if (!Drained)
        {
            break;
        }
    }
}

void ThreadCache::drainToCentral(uint16_t SizeClassIndex, size_t BlockSize, uint16_t RequestedCount, MemSpan *SpanHint)
{
    if (SizeClassIndex >= NumSizeClasses || RequestedCount == 0)
    {
        return;
    }

    FreeList &List   = FreeLists[SizeClassIndex];
    void *Chain      = nullptr;
    uint32_t Drained = 0;

    while (Drained < RequestedCount && !List.isEmpty())
    {
        void *Block                       = List.pop();
        *reinterpret_cast<void **>(Block) = Chain;
        Chain                             = Block;
        CurrentBytes                      -= BlockSize;
        ++Drained;
    }

    if (Chain && Drained > 0)
    {
        CentralPool->returnBatch(SizeClassIndex, Chain, Drained, SpanHint);
    }

    if (List.isEmpty())
    {
        List.setSpanHint(nullptr);
    }
}

void ThreadCache::trimToBudget(bool ForceAll)
{
    CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::ThreadCacheTrimToBudget);

    if (!ForceAll && getTotalRetainedBytes() <= MaxBytes)
    {
        return;
    }

    drainDeferredToCentral(ForceAll);

    constexpr uint16_t MinDrainChunk = ThreadCacheTrimMinDrainChunk;
    constexpr uint16_t MaxDrainChunk = ThreadCacheTrimMaxDrainChunk;
    const uint16_t DrainChunk        = static_cast<uint16_t>(
        std::max<uint32_t>(
            1,
            std::min<uint32_t>(
                MaxDrainChunk,
                std::max<uint32_t>(MinDrainChunk, RefillBatch / 2u))));

    if (ForceAll)
    {
        for (size_t I = 0; I < NumSizeClasses; ++I)
        {
            FreeList &List = FreeLists[I];
            if (List.isEmpty() || List.getCount() == 0)
            {
                continue;
            }

            const size_t BlockSize = classIndexToSize(static_cast<uint16_t>(I));
            drainToCentral(
                static_cast<uint16_t>(I),
                BlockSize,
                List.getCount(),
                List.getSpanHint());
        }
        return;
    }

    const size_t TargetBytes =
        (MaxBytes * ThreadCacheTrimTargetNumerator) /
        ThreadCacheTrimTargetDenominator;
    constexpr uint32_t MaxBatchesPerPass =
        static_cast<uint32_t>(NumSizeClasses) * ThreadCacheTrimMaxBatchesFactor;
    for (uint32_t batch = 0; batch < MaxBatchesPerPass && CurrentBytes > TargetBytes; ++batch)
    {
        bool Drained = false;
        for (size_t offset = 0; offset < NumSizeClasses; ++offset)
        {
            const size_t index = (LastDrainClass + offset) % NumSizeClasses;
            FreeList &List     = FreeLists[index];
            if (List.isEmpty() || List.getCount() == 0)
            {
                continue;
            }

            const size_t BlockSize   = classIndexToSize(static_cast<uint16_t>(index));
            const uint16_t Requested = std::min<uint16_t>(List.getCount(), DrainChunk);
            drainToCentral(
                static_cast<uint16_t>(index),
                BlockSize,
                Requested,
                List.getSpanHint());
            LastDrainClass = (index + 1) % NumSizeClasses;
            Drained        = true;
            break;
        }

        if (!Drained)
        {
            break;
        }
    }
}

}
