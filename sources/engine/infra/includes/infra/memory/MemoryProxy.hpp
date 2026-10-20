//
// Created by Lzorn on 2026/9/26.
//

#ifndef STABLEARRAY_MEMORYPROXY_H
#define STABLEARRAY_MEMORYPROXY_H


#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory_resource>
#include <mutex>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

#if !defined(_WIN32) && (defined(__linux__) || defined(__APPLE__))
#include <execinfo.h>
#endif

#include <infra/memory/ThreadCache.hpp>
#include <infra/containers/MPMCQueue.hpp>

namespace infra::mem
{

// dedicated 元数据分片上限。
inline constexpr std::size_t MemoryFacadeMaxDedicatedShardCount = 64;
// quarantine 分片上限。
inline constexpr std::size_t MemoryFacadeMaxQuarantineShardCount = 64;
// facade 分片最小值。
inline constexpr std::size_t MemoryFacadeMinShardCount = 8;
// facade 分片按硬件线程缩放倍数。
inline constexpr std::size_t MemoryFacadeShardScale = 2;
// 每个 quarantine 分片的无锁环容量（需为 2 的幂）。
inline constexpr std::size_t MemoryFacadeQuarantineRingCapacity = 1024;
// 大对象缓存桶数量。
inline constexpr std::size_t MemoryFacadeDedicatedCacheBucketCount = 64;
// 每个大对象缓存桶最大缓存条目数。
inline constexpr std::size_t MemoryFacadeDedicatedCacheBucketCapacity = 64;
// 大对象缓存查找线性探测次数（冲突时最多探测 N 个桶）。
inline constexpr std::size_t MemoryFacadeDedicatedCacheMaxProbe = 4;
// dedicated 元数据节点池每次扩容的节点数。
inline constexpr std::size_t MemoryFacadeDedicatedNodeChunkSize = 256;

/**
 * @brief 分配管线编排器：串联校验/布局/分配/初始化/事件通知各阶段。
 * @note 维护全局单例（Self()），并持有 PageAllocator/CentralPool/EventBus 与
 *       dedicated/quarantine 分片结构。
 */
class MemoryAllocatorEngine
{
public:
    using RuntimeStats = AllocatorRuntimeStatsSnapshot;

    explicit MemoryAllocatorEngine(const AllocatorConfig &InConfig)
        : Config(InConfig),
          PageAllocator(Config),
          CentralPool(PageAllocator),
          ActiveDedicatedShardCount(determineShardCount(MaxDedicatedShardCount)),
          ActiveQuarantineShardCount(determineShardCount(MaxQuarantineShardCount))
    {
        ThreadCacheRegistry::bind(CentralPool, Config);
    }

    ~MemoryAllocatorEngine()
    {
        releaseDedicatedNodeChunks();
    }

    MemoryAllocatorEngine(const MemoryAllocatorEngine &) = delete;

    MemoryAllocatorEngine &operator=(const MemoryAllocatorEngine &) = delete;

    [[nodiscard]] static MemoryAllocatorEngine &Self()
    {
        // 构造一次性完成；SelfConstructed 只在首次构造时置位，避免在每次
        // allocate/deallocate 热路径上对共享原子做 release 写造成缓存行失效。
        static MemoryAllocatorEngine *engine = []()
        {
            auto *Instance = new MemoryAllocatorEngine(getGlobalConfig());
            SelfConstructed().store(true, std::memory_order_release);
            return Instance;
        }();
        return *engine;
    }

    static void configureBeforeFirstUse(const AllocatorConfig &InConfig)
    {
        if (SelfConstructed().load(std::memory_order_acquire))
        {
            return;
        }

        getGlobalConfig() = InConfig;
    }

    [[nodiscard]] void *allocate(const AllocationDescriptor &Descriptor)
    {
        return allocateWithLifetime<AllocationLifetime::Default>(Descriptor);
    }

    void deallocate(void *Ptr, const AllocationDescriptor &Descriptor) noexcept
    {
        deallocateWithLifetime<AllocationLifetime::Default>(Ptr, Descriptor);
    }

    template<AllocationLifetime Lifetime>
    [[nodiscard]] void *allocateWithLifetime(const AllocationDescriptor &Descriptor)
    {
        CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::EngineAllocate);

        if (Descriptor.Size == 0) [[unlikely]]
        {
            return getZeroSizeAllocationPointer();
        }

        AllocationContext Context;
        Context.Descriptor    = Descriptor;
        Context.Runtime.Owner = this;

        const bool HasObservers = EventBus.hasObservers();
        prepareContext(Context, HasObservers);
        if (!validateRequest(Context))
        {
            emitValidationFailure(Context);
            return nullptr;
        }

        if (!buildLayout(Context))
        {
            emitValidationFailure(Context);
            return nullptr;
        }

        if (!allocateRawBlock<Lifetime>(Context))
        {
            return nullptr;
        }

        initializeLayout(Context);
        emitAllocationEvent(Context);
        Context.Stage            = AllocationStage::Return;
        Context.Result.IsSuccess = true;
        return Context.Runtime.UserPtr;
    }

    template<AllocationLifetime Lifetime>
    void deallocateWithLifetime(void *Ptr, const AllocationDescriptor &Descriptor) noexcept
    {
        CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::EngineDeallocate);

        if (!Ptr || isZeroSizeAllocationPointer(Ptr))
        {
            return;
        }

        AllocationContext Context;
        Context.Descriptor      = Descriptor;
        Context.Runtime.Owner   = this;
        Context.Runtime.UserPtr = static_cast<std::byte *>(Ptr);

        const bool HasObservers = EventBus.hasObservers();
        prepareContext(Context, HasObservers);
        if (!validateRequest(Context))
        {
            emitValidationFailure(Context);
            return;
        }

        if (!buildLayout(Context))
        {
            emitValidationFailure(Context);
            return;
        }

        // 小对象走 ThreadCache/CentralPool，大对象/高对齐走 dedicated 路径。
        // 依据调用方传入的 size/alignment 即可判定路径，避免每次释放都去
        // dedicated 表做一次共享锁查询（这是多线程释放路径的主要竞争点）。
        if (!Context.Layout.IsSmallAllocation)
        {
            if (releaseDedicatedBlock(Context))
            {
                emitDeallocationEvent(Context);
                return;
            }

            // 大对象指针不在 dedicated 表中：无效释放。
            emitValidationFailure(Context);
            return;
        }

        Context.Runtime.RawPtr = Context.Runtime.UserPtr - Context.Layout.UserOffset;
        destroyLayout(Context);
        freeSmallBlock<Lifetime>(Context);
        emitDeallocationEvent(Context);
    }

    MemoryEventBus &getEventBus() noexcept
    {
        return EventBus;
    }

    [[nodiscard]] std::uint32_t getCurrentFrame() const noexcept
    {
        return CurrentFrame.load(std::memory_order_relaxed);
    }

    void setCurrentFrame(const std::uint32_t Frame) noexcept
    {
        CurrentFrame.store(Frame, std::memory_order_relaxed);
        ThreadCacheRegistry::onThreadIdle();
    }

    void onThreadExit() noexcept
    {
        flushQuarantine();
        ThreadCacheRegistry::onThreadExit();
        if constexpr (MemoryCompileEnableInternalBenchTiming)
        {
            memoryBenchPrintSummary();
        }
    }

    [[nodiscard]] std::size_t prewarm(const std::vector<MemoryPrewarmRequest> &Requests)
    {
        std::size_t warmed = 0;

        for (const MemoryPrewarmRequest &Request: Requests)
        {
            if (Request.Size == 0 || Request.Count == 0)
            {
                continue;
            }

            AllocationDescriptor Descriptor;
            Descriptor.Size         = Request.Size;
            Descriptor.Alignment    = Request.Alignment;
            Descriptor.ResourceId   = Request.ResourceId;
            Descriptor.IsZeroMemory = Request.ZeroInitialize;

            std::vector<void *> blocks;
            blocks.reserve(Request.Count);

            for (std::uint32_t I = 0; I < Request.Count; ++I)
            {
                void *Ptr = nullptr;
                switch (Request.Lifetime)
                {
                    case AllocationLifetime::Persistent:
                        Ptr = allocateWithLifetime<AllocationLifetime::Persistent>(Descriptor);
                        break;
                    case AllocationLifetime::Transient:
                        Ptr = allocateWithLifetime<AllocationLifetime::Transient>(Descriptor);
                        break;
                    default:
                        Ptr = allocateWithLifetime<AllocationLifetime::Default>(Descriptor);
                        break;
                }

                if (!Ptr)
                {
                    break;
                }

                blocks.push_back(Ptr);
                ++warmed;
            }

            for (void *Ptr: blocks)
            {
                switch (Request.Lifetime)
                {
                    case AllocationLifetime::Persistent:
                        deallocateWithLifetime<AllocationLifetime::Persistent>(Ptr, Descriptor);
                        break;
                    case AllocationLifetime::Transient:
                        deallocateWithLifetime<AllocationLifetime::Transient>(Ptr, Descriptor);
                        break;
                    default:
                        deallocateWithLifetime<AllocationLifetime::Default>(Ptr, Descriptor);
                        break;
                }
            }
        }

        flushQuarantine();
        return warmed;
    }

    void flushDeferredFrees() noexcept
    {
        flushQuarantine();
        ThreadCacheRegistry::onThreadIdle();
    }

    [[nodiscard]] RuntimeStats getRuntimeStats() noexcept
    {
        AllocatorRuntimeStatsSnapshot Stats;
        const auto pageToSpanStats      = CentralPool.getPageToSpanPoolStats();
        Stats.PageToSpanEntryCount     = pageToSpanStats.EntryCount;
        Stats.SpanObjectPoolAllocated  = pageToSpanStats.SpanObjectsAllocated;
        Stats.SpanObjectPoolInUse      = pageToSpanStats.SpanObjectsInUse;
        Stats.SpanObjectPoolUsageRatio = pageToSpanStats.SpanObjectPoolUsageRatio;

        const auto regionIndexStats                  = PageAllocator.getRegionIndexPoolStats();
        Stats.RegionIndexRegionCount                = regionIndexStats.RegionCount;
        Stats.RegionIndexEntryCount                 = regionIndexStats.EntryCount;
        Stats.RegionIndexLevel1NodesUsed            = regionIndexStats.Level1NodesUsed;
        Stats.RegionIndexLevel1NodesCapacity        = regionIndexStats.Level1NodesCapacity;
        Stats.RegionIndexLevel1UsageRatio           = regionIndexStats.Level1UsageRatio;
        Stats.RegionIndexLevel2NodesUsed            = regionIndexStats.Level2NodesUsed;
        Stats.RegionIndexLevel2NodesCapacity        = regionIndexStats.Level2NodesCapacity;
        Stats.RegionIndexLevel2UsageRatio           = regionIndexStats.Level2UsageRatio;
        Stats.RegionIndexPoolExhaustionWarningCount = regionIndexStats.PoolExhaustionWarningCount;

        {
            for (std::size_t I = 0; I < ActiveDedicatedShardCount; ++I)
            {
                DedicatedAllocationShard &Shard = DedicatedAllocationShards[I];
                std::lock_guard<SpinLock> Lock(Shard.Mutex);
                Stats.DedicatedAllocationCount += Shard.Allocations.size();
            }
        }

        {
            for (std::size_t I = 0; I < DedicatedCacheBucketCount; ++I)
            {
                DedicatedRegionCacheBucket &Bucket = DedicatedRegionCache[I];
                std::lock_guard<SpinLock> Lock(Bucket.Mutex);
                if (Bucket.SizeKey != 0 && Bucket.Count > 0)
                {
                    ++Stats.DedicatedCacheBucketCount;
                }
            }
            Stats.DedicatedCacheBytes = DedicatedCacheBytes.load(std::memory_order_relaxed);
        }

        {
            for (std::size_t I = 0; I < ActiveQuarantineShardCount; ++I)
            {
                QuarantineShard &Shard      = QuarantineShards[I];
                Stats.QuarantineEntryCount += Shard.Entries.getSize();
                Stats.QuarantineBytes      += Shard.Bytes.load(std::memory_order_relaxed);
            }
        }

        return Stats;
    }

private:
    struct DedicatedAllocation
    {
        VirtualRegion Region;
        std::size_t UserOffset         = 0;
        std::size_t RequestedSize      = 0;
        std::size_t RequestedAlignment = 0;
        AllocationLifetime Lifetime    = AllocationLifetime::Default;
    };

    struct SmallBlockQuarantineEntry
    {
        std::byte *RawPtr            = nullptr;
        std::size_t UserOffset       = 0;
        std::size_t UserSize         = 0;
        std::size_t BlockSize        = 0;
        std::uint16_t SizeClassIndex = InvalidSizeClass;
        uint64_t AllocationId   = 0;
        std::uint32_t FrameIndex     = 0;
        std::uint32_t ResourceId     = 0;
        AllocationLifetime Lifetime  = AllocationLifetime::Default;
    };

    static_assert(std::is_nothrow_move_constructible_v<SmallBlockQuarantineEntry>);
    static_assert(std::is_nothrow_move_assignable_v<SmallBlockQuarantineEntry>);

    static constexpr std::size_t MaxDedicatedShardCount       = MemoryFacadeMaxDedicatedShardCount;
    static constexpr std::size_t MaxQuarantineShardCount      = MemoryFacadeMaxQuarantineShardCount;
    static constexpr std::size_t DedicatedCacheBucketCount    = MemoryFacadeDedicatedCacheBucketCount;
    static constexpr std::size_t DedicatedCacheBucketCapacity =
        MemoryFacadeDedicatedCacheBucketCapacity;
    static constexpr std::size_t QuarantineRingCapacity = MemoryFacadeQuarantineRingCapacity;

    struct DedicatedAllocationNode
    {
        DedicatedAllocation Record;
        DedicatedAllocationNode *NextFree = nullptr;
    };

    struct DedicatedAllocationNodeChunk
    {
        DedicatedAllocationNode *Nodes     = nullptr;
        DedicatedAllocationNodeChunk *Next = nullptr;
    };

    struct DedicatedAllocationShard
    {
        SpinLock Mutex;
        // 大对象 user 指针 -> 元数据节点，访问均在 Shard.Mutex 保护下。
        std::unordered_map<std::uintptr_t, DedicatedAllocationNode *> Allocations;
    };

    struct QuarantineShard
    {
        MPMCQueue<SmallBlockQuarantineEntry, QuarantineRingCapacity> Entries;
        std::atomic<std::size_t> Bytes = 0;
    };

    struct DedicatedRegionCacheBucket
    {
        SpinLock Mutex;
        std::size_t SizeKey = 0;
        std::uint16_t Count = 0;
        std::array<VirtualRegion, DedicatedCacheBucketCapacity> Regions{};
    };

    static AllocatorConfig &getGlobalConfig()
    {
        static AllocatorConfig Config;
        return Config;
    }

    static std::atomic<bool> &SelfConstructed()
    {
        static std::atomic<bool> Value = false;
        return Value;
    }

    [[nodiscard]] static void *getZeroSizeAllocationPointer() noexcept
    {
        alignas(std::max_align_t) static std::byte Sentinel = std::byte{0};
        return &Sentinel;
    }

    [[nodiscard]] static bool isZeroSizeAllocationPointer(const void *Ptr) noexcept
    {
        return Ptr == getZeroSizeAllocationPointer();
    }

    void prepareContext(AllocationContext &Context, const bool FillObserverMetadata)
    {
        Context.Stage               = AllocationStage::Prepare;
        Context.Metadata.ResourceId = Context.Descriptor.ResourceId;
        Context.Metadata.FrameIndex = Context.Descriptor.FrameIndex != 0
                                          ? Context.Descriptor.FrameIndex
                                          : CurrentFrame.load(std::memory_order_relaxed);

        // AllocationId 仅在观测器或 Debug 守卫需要时才生成；Release 无观测器时
        // 跳过，省去 thread_local 访问（macOS 上 TLV 查找有可观的固定开销）。
        if (FillObserverMetadata || MemoryCompileEnableDebugGuards)
        {
            Context.Metadata.AllocationId = generateAllocationId();
        }
        else
        {
            Context.Metadata.AllocationId = 0;
        }

        if (!FillObserverMetadata)
        {
            Context.Metadata.ThreadId  = {};
            Context.Metadata.Timestamp = 0;
            Context.Metadata.StackId   = 0;
            return;
        }

        Context.Metadata.ThreadId = std::this_thread::get_id();
        if constexpr (MemoryCompileEnableDebugGuards)
        {
            Context.Metadata.Timestamp = getTimestampNs();
        }
        else
        {
            Context.Metadata.Timestamp = 0;
        }
        Context.Metadata.StackId = captureStackId();
    }

    [[nodiscard]] bool validateRequest(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Validate;

        if (Context.Descriptor.Size == 0)
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        if (!isPowerOfTwo(Context.Descriptor.Alignment))
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::invalid_argument);
            return false;
        }

        if (Context.Descriptor.Alignment < alignof(void *))
        {
            Context.Descriptor.Alignment = alignof(void *);
        }

        return true;
    }

    [[nodiscard]] bool buildLayout(AllocationContext &Context)
    {
        Context.Stage                     = AllocationStage::Layout;
        Context.Layout                    = {};
        Context.Layout.RequestedSize      = Context.Descriptor.Size;
        Context.Layout.RequestedAlignment = Context.Descriptor.Alignment;

        if constexpr (MemoryCompileEnableDebugGuards)
        {
            Context.Layout.HeaderSize = sizeof(DebugHeader);
            Context.Layout.FrontGuard = 16;
            Context.Layout.BackGuard  = 16;
        }

        const std::size_t Prefix  = Context.Layout.HeaderSize + Context.Layout.FrontGuard;
        Context.Layout.UserOffset = alignUp(Prefix, Context.Descriptor.Alignment);

        std::size_t Total = 0;
        if (!checkedAdd(Context.Layout.UserOffset, Context.Descriptor.Size, Total))
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        if (!checkedAdd(Total, Context.Layout.BackGuard, Total))
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        Context.Layout.TotalSize = Total;

        const bool IsSmallAlignment = Context.Descriptor.Alignment <= alignof(std::max_align_t);
        if (IsSmallAlignment)
        {
            const std::uint16_t ClassIndex = sizeToClassIndex(Total);
            if (ClassIndex != InvalidSizeClass)
            {
                Context.Layout.SizeClassIndex    = ClassIndex;
                Context.Layout.BlockSize         = classIndexToSize(ClassIndex);
                Context.Layout.TotalSize         = Context.Layout.BlockSize;
                Context.Layout.IsSmallAllocation = true;
                return true;
            }
        }

        Context.Layout.IsSmallAllocation = false;
        const std::size_t pageSize       = PageAllocator.getPageSize();
        std::size_t ReserveBytes         = 0;
        if (!checkedAdd(Context.Layout.TotalSize, Context.Descriptor.Alignment, ReserveBytes))
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        Context.Layout.TotalSize = alignUp(ReserveBytes, pageSize);
        return true;
    }

    template<AllocationLifetime Lifetime>
    [[nodiscard]] bool allocateRawBlock(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Allocate;

        if (Context.Layout.IsSmallAllocation)
        {
            if constexpr (Lifetime == AllocationLifetime::Persistent)
            {
                void *Block                 = nullptr;
                const std::uint32_t fetched = CentralPool.fetchBatch(
                    Context.Layout.SizeClassIndex,
                    &Block,
                    1);

                if (fetched == 0 || !Block)
                {
                    Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
                    return false;
                }

                Context.Runtime.RawPtr            = static_cast<std::byte *>(Block);
                Context.Runtime.UserPtr           = Context.Runtime.RawPtr + Context.Layout.UserOffset;
                Context.Runtime.IsFromThreadCache = false;
                Context.Runtime.IsFromCentralPool = true;
                Context.Result.IsSuccess          = true;
                return true;
            }
            else
            {
                ThreadCache &Cache                   = ThreadCacheRegistry::self();
                const ThreadCacheAllocation Decision = Cache.allocate(
                    Context.Layout.SizeClassIndex,
                    Context.Layout.BlockSize);

                if (!Decision.Block)
                {
                    Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
                    return false;
                }

                Context.Runtime.RawPtr            = static_cast<std::byte *>(Decision.Block);
                Context.Runtime.UserPtr           = Context.Runtime.RawPtr + Context.Layout.UserOffset;
                Context.Runtime.IsFromThreadCache = Decision.Hit;
                Context.Runtime.IsFromCentralPool = Decision.FromCentral;
                Context.Result.IsSuccess          = true;
                return true;
            }
        }

        VirtualRegion Region = acquireDedicatedRegion<Lifetime>(Context.Layout.TotalSize);

        if (!Region.isValid())
        {
            Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
            return false;
        }

        if (!VirtualMemoryBackend::commit(Region, 0, Region.ReservedSize))
        {
            VirtualMemoryBackend::release(Region);
            Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
            return false;
        }

        const std::uintptr_t RawAddress     = reinterpret_cast<std::uintptr_t>(Region.Base);
        const std::uintptr_t MinUserAddress = RawAddress + Context.Layout.HeaderSize + Context.Layout.FrontGuard;
        const std::uintptr_t UserAddress    = alignUp(MinUserAddress, Context.Descriptor.Alignment);
        const std::size_t UserOffset        = static_cast<std::size_t>(UserAddress - RawAddress);

        std::size_t RequiredTail = 0;
        if (!checkedAdd(UserOffset, Context.Descriptor.Size, RequiredTail) ||
            !checkedAdd(RequiredTail, Context.Layout.BackGuard, RequiredTail) ||
            RequiredTail > Region.ReservedSize)
        {
            VirtualMemoryBackend::release(Region);
            Context.Result.ErrorCode = std::make_error_code(std::errc::value_too_large);
            return false;
        }

        Context.Layout.UserOffset = UserOffset;
        Context.Layout.TotalSize  = Region.ReservedSize;
        Context.Runtime.RawPtr    = Region.Base;
        Context.Runtime.UserPtr   = reinterpret_cast<std::byte *>(UserAddress);
        Context.Runtime.IsFromOS  = true;

        DedicatedAllocation Record;
        Record.Region             = Region;
        Record.UserOffset         = UserOffset;
        Record.RequestedSize      = Context.Descriptor.Size;
        Record.RequestedAlignment = Context.Descriptor.Alignment;
        Record.Lifetime           = Lifetime;

        DedicatedAllocationNode *Node = acquireDedicatedAllocationNode();
        if (!Node)
        {
            VirtualMemoryBackend::release(Region);
            Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
            return false;
        }
        Node->Record = Record;

        {
            DedicatedAllocationShard &Shard = getDedicatedAllocationShard(Context.Runtime.UserPtr);
            std::lock_guard<SpinLock> Lock(Shard.Mutex);
            const uint64_t Key = dedicatedAllocationKey(Context.Runtime.UserPtr);
            const auto [It, Inserted] = Shard.Allocations.emplace(Key, Node);
            if (!Inserted)
            {
                releaseDedicatedAllocationNode(Node);
                VirtualMemoryBackend::release(Region);
                Context.Result.ErrorCode = std::make_error_code(std::errc::not_enough_memory);
                return false;
            }
        }

        Context.Result.IsSuccess = true;
        return true;
    }

    void initializeLayout(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Initialize;

        if (Context.Descriptor.IsZeroMemory)
        {
            std::memset(Context.Runtime.UserPtr, 0, Context.Descriptor.Size);
        }
        else if constexpr (MemoryCompileEnableDebugGuards)
        {
            std::memset(Context.Runtime.UserPtr, AllocatedPattern, Context.Descriptor.Size);
        }

        if constexpr (!MemoryCompileEnableDebugGuards)
        {
            return;
        }

        auto *Header               = reinterpret_cast<DebugHeader *>(Context.Runtime.RawPtr);
        Header->Magic              = DebugHeaderMagic;
        Header->Version            = 1;
        Header->AllocationId       = Context.Metadata.AllocationId;
        Header->RequestedSize      = Context.Descriptor.Size;
        Header->RequestedAlignment = static_cast<std::uint32_t>(Context.Descriptor.Alignment);
        Header->SizeClassIndex     = Context.Layout.SizeClassIndex;
        Header->FrontGuardBytes    = static_cast<std::uint32_t>(Context.Layout.FrontGuard);
        Header->BackGuardBytes     = static_cast<std::uint32_t>(Context.Layout.BackGuard);

        if (Context.Layout.FrontGuard > 0)
        {
            std::memset(
                Context.Runtime.RawPtr + Context.Layout.HeaderSize,
                FrontGuardPattern,
                Context.Layout.FrontGuard);
        }

        if (Context.Layout.BackGuard > 0)
        {
            std::memset(
                Context.Runtime.UserPtr + Context.Descriptor.Size,
                BackGuardPattern,
                Context.Layout.BackGuard);
        }
    }

    void destroyLayout(AllocationContext &Context) noexcept
    {
        Context.Stage = AllocationStage::Destroy;
        if constexpr (!MemoryCompileEnableDebugGuards)
        {
            return;
        }

        auto *Header          = reinterpret_cast<DebugHeader *>(Context.Runtime.RawPtr);
        const bool MagicOk    = Header->Magic == DebugHeaderMagic;
        const bool IsHeaderOk = MagicOk && Header->RequestedSize == Context.Descriptor.Size;

        bool IsGuardsOk = true;
        if (IsHeaderOk)
        {
            const std::byte *front_guard = Context.Runtime.RawPtr + Context.Layout.HeaderSize;
            for (std::size_t I = 0; I < Context.Layout.FrontGuard; ++I)
            {
                if (static_cast<std::uint8_t>(front_guard[I]) != FrontGuardPattern)
                {
                    IsGuardsOk = false;
                    break;
                }
            }

            if (IsGuardsOk)
            {
                const std::byte *BackGuard = Context.Runtime.UserPtr + Context.Descriptor.Size;
                for (std::size_t I = 0; I < Context.Layout.BackGuard; ++I)
                {
                    if (static_cast<std::uint8_t>(BackGuard[I]) != BackGuardPattern)
                    {
                        IsGuardsOk = false;
                        break;
                    }
                }
            }
        }

        if (!IsHeaderOk || !IsGuardsOk)
        {
            emitGuardCorruption(Context);
        }

        Header->Magic = 0;

        // 使用 header 中记录的真实分配大小做 poison：即使调用方传入错误的 size，
        // 也不会越界写坏相邻 block 的 freelist 头指针。
        const std::size_t PoisonBytes = MagicOk ? Header->RequestedSize : 0;
        if (PoisonBytes > 0)
        {
            std::memset(Context.Runtime.UserPtr, FreedPattern, PoisonBytes);
        }
    }

    template<AllocationLifetime Lifetime>
    void freeSmallBlock(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Free;
        if (shouldQuarantineSmallBlock(Context))
        {
            enqueueSmallBlockQuarantine<Lifetime>(Context);
            return;
        }

        releaseSmallBlock<Lifetime>(
            Context.Layout.SizeClassIndex,
            Context.Layout.BlockSize,
            Context.Runtime.RawPtr);
    }

    [[nodiscard]] bool releaseDedicatedBlock(AllocationContext &Context) noexcept
    {
        DedicatedAllocation Record;
        {
            DedicatedAllocationShard &Shard = getDedicatedAllocationShard(Context.Runtime.UserPtr);
            std::lock_guard<SpinLock> Lock(Shard.Mutex);
            const uint64_t Key = dedicatedAllocationKey(Context.Runtime.UserPtr);
            const auto It      = Shard.Allocations.find(Key);
            if (It == Shard.Allocations.end())
            {
                return false;
            }

            DedicatedAllocationNode *Node = It->second;
            Record                       = Node->Record;
            Shard.Allocations.erase(It);
            releaseDedicatedAllocationNode(Node);
        }

        Context.Layout.UserOffset    = Record.UserOffset;
        Context.Layout.TotalSize     = Record.Region.ReservedSize;
        Context.Descriptor.Size      = Record.RequestedSize;
        Context.Descriptor.Alignment = Record.RequestedAlignment;
        Context.Runtime.RawPtr       = Record.Region.Base;
        Context.Runtime.IsFromOS     = true;

        destroyLayout(Context);

        VirtualRegion Region = Record.Region;
        releaseDedicatedRegion(Region, Record.Lifetime);
        return true;
    }

    template<AllocationLifetime Lifetime>
    void releaseSmallBlock(
        const std::uint16_t SizeClassIndex,
        const std::size_t BlockSize,
        std::byte *RawPtr)
    {
        if (!RawPtr || SizeClassIndex == InvalidSizeClass)
        {
            return;
        }

        if constexpr (Lifetime == AllocationLifetime::Persistent)
        {
            *reinterpret_cast<void **>(RawPtr) = nullptr;
            CentralPool.returnBatch(SizeClassIndex, RawPtr, 1);
            return;
        }
        else
        {
            ThreadCacheRegistry::self().deallocate(SizeClassIndex, BlockSize, RawPtr);
        }
    }

    [[nodiscard]] bool shouldQuarantineSmallBlock(const AllocationContext &Context) const noexcept
    {
        if constexpr (!MemoryCompileEnableUseAfterFreeDetection || !MemoryCompileEnableDebugGuards)
        {
            return false;
        }

        return Context.Layout.IsSmallAllocation && Context.Runtime.RawPtr != nullptr;
    }

    template<AllocationLifetime Lifetime>
    void enqueueSmallBlockQuarantine(const AllocationContext &Context)
    {
        CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::QuarantineEnqueue, Context.Layout.SizeClassIndex);

        SmallBlockQuarantineEntry Entry;
        Entry.RawPtr           = Context.Runtime.RawPtr;
        Entry.UserOffset       = Context.Layout.UserOffset;
        Entry.UserSize         = Context.Descriptor.Size;
        Entry.BlockSize        = Context.Layout.BlockSize;
        Entry.SizeClassIndex   = Context.Layout.SizeClassIndex;
        Entry.AllocationId     = Context.Metadata.AllocationId;
        Entry.FrameIndex       = Context.Metadata.FrameIndex;
        Entry.ResourceId       = Context.Metadata.ResourceId;
        Entry.Lifetime         = Lifetime;
        QuarantineShard &Shard = getQuarantineShardForCurrentThread();
        if (!Shard.Entries.tryPush(Entry))
        {
            const auto Popped = Shard.Entries.tryPop();
            if (Popped)
            {
                const SmallBlockQuarantineEntry Evicted = *Popped;
                Shard.Bytes.fetch_sub(Evicted.BlockSize, std::memory_order_relaxed);
                releaseSingleQuarantineEntry(Evicted);
            }

            if (!Shard.Entries.tryPush(Entry))
            {
                releaseSmallBlock<Lifetime>(
                    Entry.SizeClassIndex,
                    Entry.BlockSize,
                    Entry.RawPtr);
                return;
            }
        }

        Shard.Bytes.fetch_add(Entry.BlockSize, std::memory_order_relaxed);

        if constexpr (MemoryCompileQuarantineReleaseOnlyOnFlush)
        {
            return;
        }

        drainQuarantineShard(Shard, false);
    }

    void drainQuarantineShard(QuarantineShard &Shard, const bool ForceAll)
    {
        CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::QuarantineDrain);

        const std::size_t ByteLimit  = quarantineByteLimitPerShard();
        const std::size_t EntryLimit = quarantineEntryLimitPerShard();

        for (;;)
        {
            if (!ForceAll)
            {
                const std::size_t Bytes = Shard.Bytes.load(std::memory_order_relaxed);
                const bool overBytes    = Bytes > ByteLimit;
                const bool overEntries  = Shard.Entries.getSize() > EntryLimit;

                if (!overBytes && !overEntries)
                {
                    break;
                }
            }

            const auto Popped = Shard.Entries.tryPop();
            if (!Popped)
            {
                break;
            }

            const SmallBlockQuarantineEntry Entry = *Popped;
            Shard.Bytes.fetch_sub(Entry.BlockSize, std::memory_order_relaxed);
            releaseSingleQuarantineEntry(Entry);
        }
    }

    void releaseSingleQuarantineEntry(const SmallBlockQuarantineEntry &Entry)
    {
        CORE_MEM_BENCH_SCOPE_SC(MemoryBenchPoint::QuarantineReleaseEntry, Entry.SizeClassIndex);

        if (!isQuarantinePatternIntact(Entry))
        {
            emitUseAfterFree(Entry);
        }

        switch (Entry.Lifetime)
        {
            case AllocationLifetime::Persistent:
                releaseSmallBlock<AllocationLifetime::Persistent>(
                    Entry.SizeClassIndex,
                    Entry.BlockSize,
                    Entry.RawPtr);
                break;
            case AllocationLifetime::Transient:
                releaseSmallBlock<AllocationLifetime::Transient>(
                    Entry.SizeClassIndex,
                    Entry.BlockSize,
                    Entry.RawPtr);
                break;
            default:
                releaseSmallBlock<AllocationLifetime::Default>(
                    Entry.SizeClassIndex,
                    Entry.BlockSize,
                    Entry.RawPtr);
                break;
        }
    }

    [[nodiscard]] bool isQuarantinePatternIntact(const SmallBlockQuarantineEntry &Entry) const noexcept
    {
        const std::uint8_t *Bytes = reinterpret_cast<const std::uint8_t *>(Entry.RawPtr + Entry.UserOffset);
        for (std::size_t I = 0; I < Entry.UserSize; ++I)
        {
            if (Bytes[I] != FreedPattern)
            {
                return false;
            }
        }

        return true;
    }

    void flushQuarantine() noexcept
    {
        for (std::size_t I = 0; I < ActiveQuarantineShardCount; ++I)
        {
            QuarantineShard &Shard = QuarantineShards[I];
            drainQuarantineShard(Shard, true);
        }
    }

    template<AllocationLifetime Lifetime>
    [[nodiscard]] VirtualRegion acquireDedicatedRegion(const std::size_t Bytes)
    {
        if constexpr (Lifetime == AllocationLifetime::Transient)
        {
            if (Config.EnableDedicatedRegionCache)
            {
                constexpr std::size_t MaxProbe = MemoryFacadeDedicatedCacheMaxProbe;
                for (std::size_t Probe = 0; Probe < MaxProbe; ++Probe)
                {
                    DedicatedRegionCacheBucket &Bucket =
                        DedicatedRegionCache[(dedicatedRegionCacheBucketIndex(Bytes) + Probe) %
                                             DedicatedCacheBucketCount];
                    std::lock_guard<SpinLock> Lock(Bucket.Mutex);

                    if (Bucket.Count == 0 && Bucket.SizeKey != 0 && Bucket.SizeKey != Bytes)
                    {
                        Bucket.SizeKey = Bytes;
                    }

                    if (Bucket.SizeKey == Bytes && Bucket.Count > 0)
                    {
                        CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::DedicatedCacheAcquireHit);

                        VirtualRegion Region             = Bucket.Regions[Bucket.Count - 1];
                        Bucket.Regions[Bucket.Count - 1] = {};
                        --Bucket.Count;
                        DedicatedCacheBytes.fetch_sub(Region.ReservedSize, std::memory_order_relaxed);
                        return Region;
                    }

                    if (Bucket.SizeKey == 0)
                    {
                        break;
                    }
                }
            }
        }

        CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::DedicatedCacheAcquireMiss);
        return VirtualMemoryBackend::reserve(Bytes, Config.EnableHugePage, Config.PreferredNumaNode);
    }

    void releaseDedicatedRegion(VirtualRegion &Region, const AllocationLifetime Lifetime) noexcept
    {
        if (!Region.isValid())
        {
            return;
        }

        if (Config.EnableDedicatedRegionCache && Lifetime == AllocationLifetime::Transient)
        {
            constexpr std::size_t MaxProbe = MemoryFacadeDedicatedCacheMaxProbe;
            for (std::size_t Probe = 0; Probe < MaxProbe; ++Probe)
            {
                DedicatedRegionCacheBucket &Bucket = DedicatedRegionCache[
                    (dedicatedRegionCacheBucketIndex(Region.ReservedSize) + Probe) % DedicatedCacheBucketCount];
                std::lock_guard<SpinLock> Lock(Bucket.Mutex);

                if (Bucket.Count == 0 && Bucket.SizeKey != 0 && Bucket.SizeKey != Region.ReservedSize)
                {
                    Bucket.SizeKey = Region.ReservedSize;
                }

                if (Bucket.SizeKey != 0 && Bucket.SizeKey != Region.ReservedSize)
                {
                    continue;
                }

                if (Bucket.SizeKey == 0)
                {
                    Bucket.SizeKey = Region.ReservedSize;
                }

                const std::size_t BucketLimit = std::min<std::size_t>(
                    DedicatedCacheBucketCapacity,
                    Config.DedicatedRegionCacheMaxEntriesPerSize);
                if (Bucket.Count >= BucketLimit)
                {
                    break;
                }

                const std::size_t CurrentCachedBytes = DedicatedCacheBytes.load(std::memory_order_relaxed);
                if (CurrentCachedBytes + Region.ReservedSize > Config.DedicatedRegionCacheLimitBytes)
                {
                    break;
                }

                CORE_MEM_BENCH_SCOPE(MemoryBenchPoint::DedicatedCacheRecycle);

                VirtualMemoryBackend::decommit(Region, 0, Region.ReservedSize);
                Bucket.Regions[Bucket.Count++] = Region;
                DedicatedCacheBytes.fetch_add(Region.ReservedSize, std::memory_order_relaxed);
                Region = {};
                return;
            }
        }

        VirtualMemoryBackend::release(Region);
    }

    void emitAllocationEvent(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Notify;
        if (!EventBus.hasObservers())
        {
            return;
        }

        MemoryEvent Event;
        Event.Type            = MemoryEventType::Allocate;
        Event.UserPtr         = Context.Runtime.UserPtr;
        Event.Size            = Context.Descriptor.Size;
        Event.Alignment       = Context.Descriptor.Alignment;
        Event.AllocationId    = Context.Metadata.AllocationId;
        Event.Timestamp       = Context.Metadata.Timestamp;
        Event.ThreadId        = Context.Metadata.ThreadId;
        Event.FrameIndex      = Context.Metadata.FrameIndex;
        Event.ResourceId      = Context.Metadata.ResourceId;
        Event.FromThreadCache = Context.Runtime.IsFromThreadCache;
        Event.FromCentralPool = Context.Runtime.IsFromCentralPool;
        Event.FromOS          = Context.Runtime.IsFromOS;
        Event.Context         = &Context;
        EventBus.emit(Event);

        if (Context.Runtime.IsFromThreadCache)
        {
            Event.Type = MemoryEventType::ThreadCacheHit;
            EventBus.emit(Event);
        }
        else
        {
            Event.Type = MemoryEventType::ThreadCacheMiss;
            EventBus.emit(Event);
        }

        if (Context.Runtime.IsFromCentralPool)
        {
            Event.Type = MemoryEventType::CentralFetch;
            EventBus.emit(Event);
        }
    }

    void emitDeallocationEvent(AllocationContext &Context)
    {
        Context.Stage = AllocationStage::Notify;
        if (!EventBus.hasObservers())
        {
            return;
        }

        MemoryEvent Event;
        Event.Type            = MemoryEventType::Deallocate;
        Event.UserPtr         = Context.Runtime.UserPtr;
        Event.Size            = Context.Descriptor.Size;
        Event.Alignment       = Context.Descriptor.Alignment;
        Event.AllocationId    = Context.Metadata.AllocationId;
        Event.Timestamp       = Context.Metadata.Timestamp;
        Event.ThreadId        = Context.Metadata.ThreadId;
        Event.FrameIndex      = Context.Metadata.FrameIndex;
        Event.ResourceId      = Context.Metadata.ResourceId;
        Event.FromThreadCache = !Context.Runtime.IsFromOS;
        Event.FromCentralPool = !Context.Runtime.IsFromOS;
        Event.FromOS          = Context.Runtime.IsFromOS;
        Event.Context         = &Context;
        EventBus.emit(Event);

        if (!Context.Runtime.IsFromOS)
        {
            Event.Type = MemoryEventType::CentralReturn;
            EventBus.emit(Event);
        }
    }

    void emitValidationFailure(AllocationContext &Context)
    {
        if (!EventBus.hasObservers())
        {
            return;
        }

        MemoryEvent Event;
        Event.Type         = MemoryEventType::ValidationError;
        Event.UserPtr      = Context.Runtime.UserPtr;
        Event.Size         = Context.Descriptor.Size;
        Event.Alignment    = Context.Descriptor.Alignment;
        Event.AllocationId = Context.Metadata.AllocationId;
        Event.Timestamp    = Context.Metadata.Timestamp;
        Event.ThreadId     = Context.Metadata.ThreadId;
        Event.FrameIndex   = Context.Metadata.FrameIndex;
        Event.ResourceId   = Context.Metadata.ResourceId;
        Event.Context      = &Context;
        EventBus.emit(Event);
    }

    void emitGuardCorruption(AllocationContext &Context) noexcept
    {
        if (!EventBus.hasObservers())
        {
            return;
        }

        MemoryEvent Event;
        Event.Type         = MemoryEventType::GuardCorruption;
        Event.UserPtr      = Context.Runtime.UserPtr;
        Event.Size         = Context.Descriptor.Size;
        Event.Alignment    = Context.Descriptor.Alignment;
        Event.AllocationId = Context.Metadata.AllocationId;
        Event.Timestamp    = Context.Metadata.Timestamp;
        Event.ThreadId     = Context.Metadata.ThreadId;
        Event.FrameIndex   = Context.Metadata.FrameIndex;
        Event.ResourceId   = Context.Metadata.ResourceId;
        Event.Context      = &Context;
        EventBus.emit(Event);
    }

    void emitUseAfterFree(const SmallBlockQuarantineEntry &Entry) noexcept
    {
        if (!EventBus.hasObservers())
        {
            return;
        }

        MemoryEvent Event;
        Event.Type         = MemoryEventType::UseAfterFree;
        Event.UserPtr      = Entry.RawPtr + Entry.UserOffset;
        Event.Size         = Entry.UserSize;
        Event.Alignment    = 0;
        Event.AllocationId = Entry.AllocationId;
        if constexpr (MemoryCompileEnableDebugGuards)
        {
            Event.Timestamp = getTimestampNs();
        }
        else
        {
            Event.Timestamp = 0;
        }
        Event.ThreadId   = std::this_thread::get_id();
        Event.FrameIndex = Entry.FrameIndex;
        Event.ResourceId = Entry.ResourceId;
        Event.Context    = nullptr;
        EventBus.emit(Event);
    }

    [[nodiscard]] std::uint32_t captureStackId() const noexcept
    {
        if constexpr (!MemoryCompileCaptureStack)
        {
            return 0;
        }

#if defined(_WIN32)
        void *Frames[16]{};
        const USHORT count = ::RtlCaptureStackBackTrace(2, 16, Frames, nullptr);
        uint64_t Hash = 1469598103934665603ull;
        for (USHORT I = 0; I < count; ++I)
        {
            Hash ^= static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(Frames[I]));
            Hash *= 1099511628211ull;
        }
        return static_cast<std::uint32_t>(Hash ^ (Hash >> 32));
#elif defined(__linux__) || defined(__APPLE__)
        void *Frames[16]{};
        const int count    = ::backtrace(Frames, 16);
        uint64_t Hash = 1469598103934665603ull;
        for (int I = 0; I < count; ++I)
        {
            Hash ^= static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(Frames[I]));
            Hash *= 1099511628211ull;
        }
        return static_cast<std::uint32_t>(Hash ^ (Hash >> 32));
#else
        return 0;
#endif
    }

    [[nodiscard]] static uint64_t generateAllocationId() noexcept
    {
        thread_local std::uint32_t LocalCounter       = 0;
        thread_local const std::uint32_t ThreadIdHash =
            static_cast<std::uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));

        const std::uint32_t Counter = LocalCounter++;
        return (static_cast<uint64_t>(ThreadIdHash) << 32) |
               static_cast<uint64_t>(Counter);
    }

    [[nodiscard]] static std::size_t determineShardCount(const std::size_t MaxShardCount) noexcept
    {
        const std::size_t Hardware = std::thread::hardware_concurrency();
        const std::size_t Desired  = Hardware == 0
                                         ? MemoryFacadeMinShardCount
                                         : Hardware * MemoryFacadeShardScale;
        return std::max<std::size_t>(
            MemoryFacadeMinShardCount,
            std::min<std::size_t>(Desired, MaxShardCount));
    }

    [[nodiscard]] static uint64_t dedicatedAllocationKey(const void *Ptr) noexcept
    {
        return static_cast<uint64_t>(reinterpret_cast<std::uintptr_t>(Ptr));
    }

    [[nodiscard]] DedicatedAllocationNode *acquireDedicatedAllocationNode() noexcept
    {
        std::lock_guard Lock(DedicatedNodePoolMutex);
        if (!DedicatedNodeFreeList && !growDedicatedNodePoolLocked())
        {
            return nullptr;
        }

        DedicatedAllocationNode *Node = DedicatedNodeFreeList;
        DedicatedNodeFreeList         = Node ? Node->NextFree : nullptr;
        if (Node)
        {
            Node->NextFree = nullptr;
        }
        return Node;
    }

    void releaseDedicatedAllocationNode(DedicatedAllocationNode *Node) noexcept
    {
        if (!Node)
        {
            return;
        }

        std::lock_guard Lock(DedicatedNodePoolMutex);
        Node->Record          = {};
        Node->NextFree        = DedicatedNodeFreeList;
        DedicatedNodeFreeList = Node;
    }

    [[nodiscard]] bool growDedicatedNodePoolLocked()
    {
        constexpr std::size_t ChunkNodeCount = MemoryFacadeDedicatedNodeChunkSize;
        DedicatedAllocationNodeChunk *Chunk  =
            new(std::nothrow) DedicatedAllocationNodeChunk();
        if (!Chunk)
        {
            return false;
        }

        Chunk->Nodes = new(std::nothrow) DedicatedAllocationNode[ChunkNodeCount]();
        if (!Chunk->Nodes)
        {
            delete Chunk;
            return false;
        }

        for (std::size_t I = 0; I < ChunkNodeCount; ++I)
        {
            Chunk->Nodes[I].Record   = {};
            Chunk->Nodes[I].NextFree = DedicatedNodeFreeList;
            DedicatedNodeFreeList    = &Chunk->Nodes[I];
        }

        Chunk->Next         = DedicatedNodeChunks;
        DedicatedNodeChunks = Chunk;
        return true;
    }

    void releaseDedicatedNodeChunks() noexcept
    {
        std::lock_guard Lock(DedicatedNodePoolMutex);
        DedicatedAllocationNodeChunk *Chunk = DedicatedNodeChunks;
        while (Chunk)
        {
            DedicatedAllocationNodeChunk *Next = Chunk->Next;
            delete[] Chunk->Nodes;
            delete Chunk;
            Chunk = Next;
        }

        DedicatedNodeChunks   = nullptr;
        DedicatedNodeFreeList = nullptr;
    }

    [[nodiscard]] static std::size_t dedicatedRegionCacheBucketIndex(const std::size_t Size) noexcept
    {
        const std::size_t Mixed = Size ^ (Size >> 7) ^ (Size >> 13);
        return Mixed % DedicatedCacheBucketCount;
    }

    [[nodiscard]] std::size_t dedicatedShardIndex(const void *Ptr) const noexcept
    {
        if (ActiveDedicatedShardCount <= 1)
        {
            return 0;
        }

        const std::uintptr_t Address = reinterpret_cast<std::uintptr_t>(Ptr);
        const std::size_t Mixed      = static_cast<std::size_t>((Address >> 4) ^ (Address >> 16));
        return Mixed % ActiveDedicatedShardCount;
    }

    [[nodiscard]] DedicatedAllocationShard &getDedicatedAllocationShard(const void *Ptr) noexcept
    {
        return DedicatedAllocationShards[dedicatedShardIndex(Ptr)];
    }

    [[nodiscard]] std::size_t quarantineShardIndexForCurrentThread() const noexcept
    {
        if (ActiveQuarantineShardCount <= 1)
        {
            return 0;
        }

        thread_local const std::size_t ThreadHash =
            static_cast<std::size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
        return ThreadHash % ActiveQuarantineShardCount;
    }

    [[nodiscard]] QuarantineShard &getQuarantineShardForCurrentThread() noexcept
    {
        return QuarantineShards[quarantineShardIndexForCurrentThread()];
    }

    [[nodiscard]] std::size_t quarantineByteLimitPerShard() const noexcept
    {
        const std::size_t ShardCount = std::max<std::size_t>(1, ActiveQuarantineShardCount);
        return std::max<std::size_t>(1, Config.UseAfterFreeQuarantineBytes / ShardCount);
    }

    [[nodiscard]] std::size_t quarantineEntryLimitPerShard() const noexcept
    {
        const std::size_t ShardCount = std::max<std::size_t>(1, ActiveQuarantineShardCount);
        return std::max<std::size_t>(1, Config.UseAfterFreeQuarantineMaxEntries / ShardCount);
    }

    [[nodiscard]] static bool checkedAdd(
        const std::size_t Left,
        const std::size_t Right,
        std::size_t &Out) noexcept
    {
        if (Left > std::numeric_limits<std::size_t>::max() - Right)
        {
            Out = 0;
            return false;
        }

        Out = Left + Right;
        return true;
    }

    AllocatorConfig Config;
    PageAllocator PageAllocator;
    CentralPool CentralPool;
    MemoryEventBus EventBus;

    std::atomic<std::uint32_t> CurrentFrame = 0;
    std::size_t ActiveDedicatedShardCount   = MemoryFacadeMinShardCount;
    std::array<DedicatedAllocationShard, MaxDedicatedShardCount> DedicatedAllocationShards{};
    std::mutex DedicatedNodePoolMutex;
    DedicatedAllocationNode *DedicatedNodeFreeList    = nullptr;
    DedicatedAllocationNodeChunk *DedicatedNodeChunks = nullptr;

    std::array<DedicatedRegionCacheBucket, DedicatedCacheBucketCount> DedicatedRegionCache{};
    std::atomic<std::size_t> DedicatedCacheBytes = 0;

    std::size_t ActiveQuarantineShardCount = MemoryFacadeMinShardCount;
    std::array<QuarantineShard, MaxQuarantineShardCount> QuarantineShards{};
};

} // namespace infra::mem

// ---------------------------------------------------------------------------
// 对外公共 API（位于 infra）：AllocatorFacade / EngineAllocator / EngineMemoryResource
// ---------------------------------------------------------------------------
namespace infra
{

/**
 * @brief 统一分配器门面：对外暴露的静态分配/释放入口。
 */
class AllocatorFacade
{
public:
    using RuntimeStats = mem::MemoryAllocatorEngine::RuntimeStats;

    /**
     * @brief 在首次运行时使用之前配置分配器。
     * @param
     *     - Config  const AllocatorConfig&  运行时分配器选项。
     * @usage
     *     - 在引擎启动阶段、任何内存分配调用之前调用。
     * @return
     *     - void
     */
    static void configure(const AllocatorConfig &Config)
    {
        mem::MemoryAllocatorEngine::configureBeforeFirstUse(Config);
    }

    /**
     * @brief 从统一分配器管线中分配内存块。
     * @param
     *     - Size        std::size_t   请求的字节数。
     *     - Alignment   std::size_t   要求的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     *     - ZeroMemory  bool          用户数据是否应被零填充。
     * @usage
     *     - 作为需要跟踪分配的引擎系统的默认入口使用。
     * @return
     *     - void*  用户数据指针。除非 IsNoThrow 为 true，失败时抛出 std::bad_alloc。
     */
    [[nodiscard]] static void *allocate(
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0,
        const bool ZeroMemory          = false,
        const bool IsNoThrow           = false)
    {
        return allocateWithLifetime<AllocationLifetime::Default>(
            Size,
            Alignment,
            ResourceId,
            ZeroMemory,
            IsNoThrow);
    }

    /**
     * @brief 以编译期决定的 transient/persistent 生命周期分配内存。
     * @param
     *     - Size        std::size_t   请求的字节数。
     *     - Alignment   std::size_t   要求的对齐值。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     *     - ZeroMemory  bool          用户数据是否应被零填充。
     *     - IsNoThrow   bool          失败时返回 nullptr 而非抛出异常。
     * @usage
     *     - 供 default/transient/persistent 入口点使用的内部门面辅助函数。
     * @return
     *     - void*  用户数据指针。
     */
    template<AllocationLifetime Lifetime>
    [[nodiscard]] static void *allocateWithLifetime(
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0,
        const bool ZeroMemory          = false,
        const bool IsNoThrow           = false)
    {
        AllocationDescriptor Descriptor;
        Descriptor.Size         = Size;
        Descriptor.Alignment    = Alignment;
        Descriptor.IsZeroMemory = ZeroMemory;
        Descriptor.IsNoThrow    = IsNoThrow;
        Descriptor.ResourceId   = ResourceId;

        void *Ptr = mem::MemoryAllocatorEngine::Self().allocateWithLifetime<Lifetime>(Descriptor);
        if (!Ptr && !Descriptor.IsNoThrow)
        {
            throw std::bad_alloc();
        }

        return Ptr;
    }

    /**
     * @brief 以 nothrow 语义分配内存。
     * @param
     *     - Size        std::size_t   请求的字节数。
     *     - Alignment   std::size_t   要求的对齐值。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     *     - ZeroMemory  bool          用户数据是否应被零填充。
     * @usage
     *     - 当调用方需要“失败返回 null”而非异常的行为时使用。
     * @return
     *     - void*  用户数据指针；分配失败时返回 nullptr。
     */
    [[nodiscard]] static void *allocateNoThrow(
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0,
        const bool ZeroMemory          = false) noexcept
    {
        return allocate(Size, Alignment, ResourceId, ZeroMemory, true);
    }

    /**
     * @brief 释放由 AllocatorFacade::allocate 分配的内存块。
     * @param
     *     - Ptr         void*         allocate 返回的指针。
     *     - Size        std::size_t   最初请求的 Size。
     *     - Alignment   std::size_t   最初的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     * @usage
     *     - 始终传入与分配时相同的 Size/Alignment 元数据。
     * @return
     *     - void
     */
    static void deallocate(
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0) noexcept
    {
        deallocateWithLifetime<AllocationLifetime::Default>(
            Ptr,
            Size,
            Alignment,
            ResourceId);
    }

    /**
     * @brief 使用编译期生命周期分派来释放内存。
     * @param
     *     - Ptr         void*         由对应分配路径返回的指针。
     *     - Size        std::size_t   最初请求的 Size。
     *     - Alignment   std::size_t   最初的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     * @usage
     *     - 供 default/transient/persistent 释放 API 使用的内部门面辅助函数。
     * @return
     *     - void
     */
    template<AllocationLifetime Lifetime>
    static void deallocateWithLifetime(
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0) noexcept
    {
        if (!Ptr)
        {
            return;
        }

        AllocationDescriptor Descriptor;
        Descriptor.Size       = Size;
        Descriptor.Alignment  = Alignment;
        Descriptor.ResourceId = ResourceId;
        mem::MemoryAllocatorEngine::Self().deallocateWithLifetime<Lifetime>(Ptr, Descriptor);
    }

    /**
     * @brief 分配标记为 transient/热生命周期的内存。
     * @param
     *     - Size        std::size_t   请求的字节数。
     *     - Alignment   std::size_t   要求的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     *     - ZeroMemory  bool          用户数据是否应被零填充。
     * @usage
     *     - 用于帧内或短生命周期对象，以优先走快速复用路径。
     * @return
     *     - void*  用户数据指针。
     */
    [[nodiscard]] static void *allocateTransient(
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0,
        const bool ZeroMemory          = false,
        const bool IsNoThrow           = false)
    {
        return allocateWithLifetime<AllocationLifetime::Transient>(
            Size,
            Alignment,
            ResourceId,
            ZeroMemory,
            IsNoThrow);
    }

    /**
     * @brief 分配标记为 persistent/长生命周期的内存。
     * @param
     *     - Size        std::size_t   请求的字节数。
     *     - Alignment   std::size_t   要求的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     *     - ZeroMemory  bool          用户数据是否应被零填充。
     * @usage
     *     - 用于长生命周期的资源，以减少 TLS 缓存污染。
     * @return
     *     - void*  用户数据指针。
     */
    [[nodiscard]] static void *allocatePersistent(
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0,
        const bool ZeroMemory          = false,
        const bool IsNoThrow           = false)
    {
        return allocateWithLifetime<AllocationLifetime::Persistent>(
            Size,
            Alignment,
            ResourceId,
            ZeroMemory,
            IsNoThrow);
    }

    /**
     * @brief 释放由 allocateTransient() 分配的内存。
     * @param
     *     - Ptr         void*         allocateTransient() 返回的指针。
     *     - Size        std::size_t   最初请求的 Size。
     *     - Alignment   std::size_t   最初的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     * @usage
     *     - 与 allocateTransient() 配对使用，以保持生命周期提示一致。
     * @return
     *     - void
     */
    static void deallocateTransient(
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0) noexcept
    {
        deallocateWithLifetime<AllocationLifetime::Transient>(
            Ptr,
            Size,
            Alignment,
            ResourceId);
    }

    /**
     * @brief 释放由 allocatePersistent() 分配的内存。
     * @param
     *     - Ptr         void*         allocatePersistent() 返回的指针。
     *     - Size        std::size_t   最初请求的 Size。
     *     - Alignment   std::size_t   最初的 Alignment。
     *     - ResourceId  std::uint32_t 可选的资源分组 id。
     * @usage
     *     - 与 allocatePersistent() 配对使用，以保持生命周期提示一致。
     * @return
     *     - void
     */
    static void deallocatePersistent(
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment    = alignof(std::max_align_t),
        const std::uint32_t ResourceId = 0) noexcept
    {
        deallocateWithLifetime<AllocationLifetime::Persistent>(
            Ptr,
            Size,
            Alignment,
            ResourceId);
    }

    template<typename ObserverType>
    /**
     * @brief 将 Observer 绑定到全局内存 Event 总线。
     * @param
     *     - Observer  ObserverType&  实现了 onMemoryEvent() 的 Observer 实例。
     * @usage
     *     - 无需修改分配器基础设施即可绑定统计/泄漏/性能分析模块。
     * @return
     *     - void
     */
    static void bindObserver(ObserverType &Observer)
    {
        mem::MemoryAllocatorEngine::Self().getEventBus().bind(Observer);
    }

    template<typename ObserverType>
    /**
     * @brief 将 Observer 从全局内存 Event 总线解绑。
     * @param
     *     - Observer  ObserverType&  先前绑定的 Observer 实例。
     * @usage
     *     - 在子系统关闭时调用，以避免悬空回调。
     * @return
     *     - void
     */
    static void unbindObserver(ObserverType &Observer)
    {
        mem::MemoryAllocatorEngine::Self().getEventBus().unbind(Observer);
    }

    /**
     * @brief 访问全局内存 Event 总线。
     * @param
     *     - none
     * @usage
     *     - 通过此 API 绑定统计、泄漏和性能分析观测器。
     * @return
     *     - MemoryEventBus&  共享的 Event 总线实例。
     */
    [[nodiscard]] static MemoryEventBus &getEventBus()
    {
        return mem::MemoryAllocatorEngine::Self().getEventBus();
    }

    /**
     * @brief 显式冲刷并释放当前线程的 Cache。
     * @param
     *     - none
     * @usage
     *     - 在长生命周期进程的工作线程关闭时调用。
     * @return
     *     - void
     */
    static void onThreadExit() noexcept
    {
        mem::MemoryAllocatorEngine::Self().onThreadExit();
    }

    /**
     * @brief 显式将延迟释放（UAF quarantine）冲刷到分配器后端。
     * @param
     *     - none
     * @usage
     *     - 在需要确定性释放的帧边界或测试检查点处很有用。
     * @return
     *     - void
     */
    static void flushDeferredFrees() noexcept
    {
        mem::MemoryAllocatorEngine::Self().flushDeferredFrees();
    }

    /**
     * @brief 设置注入到分配元数据中的当前 Frame id。
     * @param
     *     - Frame  std::uint32_t  帧号。
     * @usage
     *     - 在每帧游戏逻辑分配之前更新一次。
     * @return
     *     - void
     */
    static void setCurrentFrame(const std::uint32_t Frame) noexcept
    {
        mem::MemoryAllocatorEngine::Self().setCurrentFrame(Frame);
    }

    /**
     * @brief 为启动或关卡切换的尖峰批量预热分配器类。
     * @param
     *     - Requests  const std::vector<MemoryPrewarmRequest>&  一批预热请求。
     * @usage
     *     - 在加载阶段执行，以预触碰 span 并减少首次使用的卡顿。
     * @return
     *     - std::size_t  成功执行的预热分配数量。
     */
    [[nodiscard]] static std::size_t prewarm(const std::vector<MemoryPrewarmRequest> &Requests)
    {
        return mem::MemoryAllocatorEngine::Self().prewarm(Requests);
    }

    /**
     * @brief 读取对诊断面板有用的运行时分配器计数器。
     * @param
     *     - none
     * @usage
     *     - 定期轮询以观察 quarantine 压力与 dedicated Cache 占用。
     * @return
     *     - RuntimeStats  当前分配器运行时计数器。
     */
    [[nodiscard]] static RuntimeStats getStats()
    {
        return mem::MemoryAllocatorEngine::Self().getRuntimeStats();
    }
};

template<typename Ty>
class EngineAllocator
{
public:
    using value_type = Ty;

    /**
     * @brief 默认构造 STL 适配器分配器。
     * @param
     *     - none
     * @usage
     *     - 作为 std 容器的分配器实参使用。
     * @return
     *     - void
     */
    constexpr EngineAllocator() noexcept = default;

    template<typename Other>
    constexpr EngineAllocator(const EngineAllocator<Other> &) noexcept
    {
    }

    /**
     * @brief 为 STL 容器分配连续的带类型存储。
     * @param
     *     - count  std::size_t  元素数量。
     * @usage
     *     - 在增长过程中由 STL 容器内部调用。
     * @return
     *     - Ty*  带类型存储指针。
     */
    [[nodiscard]] Ty *allocate(const std::size_t count)
    {
        const std::size_t bytes = count * sizeof(Ty);
        void *Ptr               = AllocatorFacade::allocate(bytes, alignof(Ty));
        return static_cast<Ty *>(Ptr);
    }

    /**
     * @brief 释放由此分配器先前分配的带类型存储。
     * @param
     *     - Ptr    Ty*          存储指针。
     *     - count  std::size_t  元素数量。
     * @usage
     *     - 在收缩/析构过程中由 STL 容器内部调用。
     * @return
     *     - void
     */
    void deallocate(Ty *Ptr, const std::size_t count) noexcept
    {
        AllocatorFacade::deallocate(Ptr, count * sizeof(Ty), alignof(Ty));
    }

    template<typename Other>
    [[nodiscard]] constexpr bool operator==(const EngineAllocator<Other> &) const noexcept
    {
        return true;
    }

    template<typename Other>
    [[nodiscard]] constexpr bool operator!=(const EngineAllocator<Other> &) const noexcept
    {
        return false;
    }
};

// PMR 兼容层必须继承 std::pmr::memory_resource，此处虚函数仅存在于标准库边界。
class EngineMemoryResource final : public std::pmr::memory_resource
{
protected:
    [[nodiscard]] void *do_allocate(const std::size_t bytes, const std::size_t Alignment) override
    {
        return AllocatorFacade::allocate(bytes, Alignment);
    }

    void do_deallocate(void *Ptr, const std::size_t bytes, const std::size_t Alignment) override
    {
        AllocatorFacade::deallocate(Ptr, bytes, Alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override
    {
        return this == &other;
    }
};
} // namespace infra


#endif //STABLEARRAY_MEMORYPROXY_H
