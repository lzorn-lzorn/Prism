//
// Created by Lzorn on 2026/9/24.
//

#ifndef STABLEARRAY_MEMORYVISIBILITY_H
#define STABLEARRAY_MEMORYVISIBILITY_H
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <ostream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>

#include <infra/memory/MemoryCommon.hpp>

#if CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING
#   define CORE_MEM_BENCH_SCOPE(PointValue) ::infra::mem::MemoryBenchScope CoreMemBenchScope_##__LINE__(PointValue)
#   define CORE_MEM_BENCH_SCOPE_SC(PointValue, SizeClassValue) \
	::infra::mem::MemoryBenchScope CoreMemBenchScope_##__LINE__(PointValue, static_cast<std::uint16_t>(SizeClassValue))
#else
#   define CORE_MEM_BENCH_SCOPE(PointValue) ((void)0)
#   define CORE_MEM_BENCH_SCOPE_SC(PointValue, SizeClassValue) ((void)0)
#endif


#if defined(__GNUC__) || defined(__clang__)
#   define CORE_MEM_HOOK_RETURN_ADDRESS __builtin_extract_return_addr(__builtin_return_address(0))
#elif defined(_MSC_VER)
#   include <intrin.h>
#   pragma intrinsic(_ReturnAddress)
#   define CORE_MEM_HOOK_RETURN_ADDRESS _ReturnAddress()
#   else
#   define CORE_MEM_HOOK_RETURN_ADDRESS ((void*)0)
#endif


namespace infra::mem
{
// 内部计时统计每累积多少次采样打印一次摘要。
inline constexpr std::size_t MemoryBenchPrintEverySamples         = 200000;
// 线程分组统计桶数（按线程哈希分桶）。
inline constexpr std::size_t MemoryBenchThreadGroupBucketCount    = 64;
// size-class 分组统计桶数（覆盖当前 56 个 class 并预留扩展）。
inline constexpr std::size_t MemoryBenchSizeClassGroupBucketCount = 64;
// 每个点位打印的线程分组 TopN（按调用次数）。
inline constexpr std::size_t MemoryBenchPrintTopThreadGroups      = 4;


[[nodiscard]] inline uint64_t getTimestampNs() noexcept
{
    using clock = std::chrono::steady_clock;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count());
}

enum class MemoryBenchPoint : std::uint8_t
{
    EngineAllocate = 0,
    EngineDeallocate,
    QuarantineEnqueue,
    QuarantineDrain,
    QuarantineReleaseEntry,
    DedicatedCacheAcquireHit,
    DedicatedCacheAcquireMiss,
    DedicatedCacheRecycle,
    ThreadCacheAllocate,
    ThreadCacheDeallocate,
    ThreadCacheRefillDeferred,
    ThreadCacheSpillDeferred,
    ThreadCacheDrainDeferred,
    ThreadCacheTrimToBudget,
    CentralPoolFetchBatch,
    CentralPoolReturnBatch,
    CentralSpanRegisterPages,
    CentralSpanUnregisterPages,
    PageAllocatorAcquireSpan,
    PageAllocatorReleaseSpan,
    Count
};

#if CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING
struct MemoryBenchCounter
{
    std::atomic<uint64_t> Calls   = 0;
    std::atomic<uint64_t> TotalNs = 0;
    std::atomic<uint64_t> MaxNs   = 0;
};

[[nodiscard]] inline std::array<MemoryBenchCounter, static_cast<std::size_t>(MemoryBenchPoint::Count)> &
memoryBenchCounters() noexcept
{
    static std::array<MemoryBenchCounter, static_cast<std::size_t>(MemoryBenchPoint::Count)> Counters{};
    return Counters;
}

[[nodiscard]] inline std::array<
    std::array<MemoryBenchCounter, MemoryBenchThreadGroupBucketCount>,
    static_cast<std::size_t>(MemoryBenchPoint::Count)> &
memoryBenchThreadGroupCounters() noexcept
{
    static std::array<
        std::array<MemoryBenchCounter, MemoryBenchThreadGroupBucketCount>,
        static_cast<std::size_t>(MemoryBenchPoint::Count)> Counters{};
    return Counters;
}

[[nodiscard]] inline std::array<
    std::array<MemoryBenchCounter, MemoryBenchSizeClassGroupBucketCount>,
    static_cast<std::size_t>(MemoryBenchPoint::Count)> &
memoryBenchSizeClassGroupCounters() noexcept
{
    static std::array<
        std::array<MemoryBenchCounter, MemoryBenchSizeClassGroupBucketCount>,
        static_cast<std::size_t>(MemoryBenchPoint::Count)> Counters{};
    return Counters;
}

[[nodiscard]] inline std::atomic<uint64_t> &memoryBenchSampleCounter() noexcept
{
    static std::atomic<uint64_t> Counter = 0;
    return Counter;
}

[[nodiscard]] inline std::size_t memoryBenchPointIndex(const MemoryBenchPoint Point) noexcept
{
    return static_cast<std::size_t>(Point);
}

[[nodiscard]] inline std::size_t memoryBenchThreadBucketForCurrentThread() noexcept
{
    thread_local const std::size_t ThreadHash =
        static_cast<std::size_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    return ThreadHash % MemoryBenchThreadGroupBucketCount;
}

inline void memoryBenchUpdateCounter(MemoryBenchCounter &Counter, const uint64_t ElapsedNs) noexcept
{
    Counter.Calls.fetch_add(1, std::memory_order_relaxed);
    Counter.TotalNs.fetch_add(ElapsedNs, std::memory_order_relaxed);

    uint64_t Max = Counter.MaxNs.load(std::memory_order_relaxed);
    while (Max < ElapsedNs &&
           !Counter.MaxNs.compare_exchange_weak(Max, ElapsedNs, std::memory_order_relaxed, std::memory_order_relaxed))
    {
    }
}

inline void memoryBenchPrintCounterLine(
    const char *Prefix,
    const char *Name,
    const uint64_t Calls,
    const uint64_t TotalNs,
    const uint64_t MaxNs) noexcept
{
    if (Calls == 0)
    {
        return;
    }

    const double AvgNs   = static_cast<double>(TotalNs) / static_cast<double>(Calls);
    const double TotalMs = static_cast<double>(TotalNs) / 1000000.0;
    std::printf(
        "[MEM-BENCH] %s %-30s calls=%llu total_ms=%.3f avg_ns=%.1f max_ns=%llu\n",
        Prefix,
        Name,
        static_cast<unsigned long long>(Calls),
        TotalMs,
        AvgNs,
        static_cast<unsigned long long>(MaxNs));
}

[[nodiscard]] inline const char *memoryBenchPointName(const MemoryBenchPoint Point) noexcept
{
    switch (Point)
    {
        case MemoryBenchPoint::EngineAllocate:
            return "engine.allocate";
        case MemoryBenchPoint::EngineDeallocate:
            return "engine.deallocate";
        case MemoryBenchPoint::QuarantineEnqueue:
            return "quarantine.enqueue";
        case MemoryBenchPoint::QuarantineDrain:
            return "quarantine.drain";
        case MemoryBenchPoint::QuarantineReleaseEntry:
            return "quarantine.release_entry";
        case MemoryBenchPoint::DedicatedCacheAcquireHit:
            return "dedicated_cache.acquire_hit";
        case MemoryBenchPoint::DedicatedCacheAcquireMiss:
            return "dedicated_cache.acquire_miss";
        case MemoryBenchPoint::DedicatedCacheRecycle:
            return "dedicated_cache.recycle";
        case MemoryBenchPoint::ThreadCacheAllocate:
            return "thread_cache.allocate";
        case MemoryBenchPoint::ThreadCacheDeallocate:
            return "thread_cache.deallocate";
        case MemoryBenchPoint::ThreadCacheRefillDeferred:
            return "thread_cache.refill_deferred";
        case MemoryBenchPoint::ThreadCacheSpillDeferred:
            return "thread_cache.spill_deferred";
        case MemoryBenchPoint::ThreadCacheDrainDeferred:
            return "thread_cache.drain_deferred";
        case MemoryBenchPoint::ThreadCacheTrimToBudget:
            return "thread_cache.trim_to_budget";
        case MemoryBenchPoint::CentralPoolFetchBatch:
            return "central_pool.fetch_batch";
        case MemoryBenchPoint::CentralPoolReturnBatch:
            return "central_pool.return_batch";
        case MemoryBenchPoint::CentralSpanRegisterPages:
            return "central_pool.register_span_pages";
        case MemoryBenchPoint::CentralSpanUnregisterPages:
            return "central_pool.unregister_span_pages";
        case MemoryBenchPoint::PageAllocatorAcquireSpan:
            return "page_allocator.acquire_span";
        case MemoryBenchPoint::PageAllocatorReleaseSpan:
            return "page_allocator.release_span";
        default:
            return "unknown";
    }
}

inline void memoryBenchPrintSummary() noexcept
{
    static std::mutex PrintMutex;
    std::unique_lock<std::mutex> Lock(PrintMutex, std::try_to_lock);
    if (!Lock.owns_lock())
    {
        return;
    }

    auto &Counters          = memoryBenchCounters();
    auto &ThreadCounters    = memoryBenchThreadGroupCounters();
    auto &SizeClassCounters = memoryBenchSizeClassGroupCounters();
    std::printf("[MEM-BENCH] -------- allocator timing summary --------\n");
    for (std::size_t I = 0; I < Counters.size(); ++I)
    {
        const uint64_t Calls = Counters[I].Calls.load(std::memory_order_relaxed);
        if (Calls == 0)
        {
            continue;
        }

        const uint64_t TotalNs = Counters[I].TotalNs.load(std::memory_order_relaxed);
        const uint64_t MaxNs   = Counters[I].MaxNs.load(std::memory_order_relaxed);
        memoryBenchPrintCounterLine(
            "[all]",
            memoryBenchPointName(static_cast<MemoryBenchPoint>(I)),
            Calls,
            TotalNs,
            MaxNs);

        std::array<std::size_t, MemoryBenchPrintTopThreadGroups> TopThreadBuckets{};
        TopThreadBuckets.fill(std::numeric_limits<std::size_t>::max());
        for (std::size_t Rank = 0; Rank < TopThreadBuckets.size(); ++Rank)
        {
            std::size_t BestBucket  = std::numeric_limits<std::size_t>::max();
            uint64_t BestCalls = 0;
            for (std::size_t Bucket = 0; Bucket < MemoryBenchThreadGroupBucketCount; ++Bucket)
            {
                bool AlreadySelected = false;
                for (std::size_t Prev = 0; Prev < Rank; ++Prev)
                {
                    if (TopThreadBuckets[Prev] == Bucket)
                    {
                        AlreadySelected = true;
                        break;
                    }
                }

                if (AlreadySelected)
                {
                    continue;
                }

                const uint64_t BucketCalls =
                    ThreadCounters[I][Bucket].Calls.load(std::memory_order_relaxed);
                if (BucketCalls > BestCalls)
                {
                    BestCalls  = BucketCalls;
                    BestBucket = Bucket;
                }
            }

            if (BestBucket == std::numeric_limits<std::size_t>::max() || BestCalls == 0)
            {
                break;
            }

            TopThreadBuckets[Rank]           = BestBucket;
            const auto &Counter              = ThreadCounters[I][BestBucket];
            const uint64_t GroupCalls   = Counter.Calls.load(std::memory_order_relaxed);
            const uint64_t GroupTotalNs = Counter.TotalNs.load(std::memory_order_relaxed);
            const uint64_t GroupMaxNs   = Counter.MaxNs.load(std::memory_order_relaxed);
            const double GroupAvgNs          = static_cast<double>(GroupTotalNs) / static_cast<double>(GroupCalls);
            std::printf(
                "[MEM-BENCH] [thread-bucket] %-30s bucket=%llu calls=%llu total_ms=%.3f avg_ns=%.1f max_ns=%llu\n",
                memoryBenchPointName(static_cast<MemoryBenchPoint>(I)),
                static_cast<unsigned long long>(BestBucket),
                static_cast<unsigned long long>(GroupCalls),
                static_cast<double>(GroupTotalNs) / 1000000.0,
                GroupAvgNs,
                static_cast<unsigned long long>(GroupMaxNs));
        }

        for (std::size_t SizeClass = 0; SizeClass < MemoryBenchSizeClassGroupBucketCount; ++SizeClass)
        {
            const auto &Counter            = SizeClassCounters[I][SizeClass];
            const uint64_t GroupCalls = Counter.Calls.load(std::memory_order_relaxed);
            if (GroupCalls == 0)
            {
                continue;
            }

            const uint64_t GroupTotalNs = Counter.TotalNs.load(std::memory_order_relaxed);
            const uint64_t GroupMaxNs   = Counter.MaxNs.load(std::memory_order_relaxed);
            const double GroupAvgNs          = static_cast<double>(GroupTotalNs) / static_cast<double>(GroupCalls);
            std::printf(
                "[MEM-BENCH] [size-class] %-30s class=%llu calls=%llu total_ms=%.3f avg_ns=%.1f max_ns=%llu\n",
                memoryBenchPointName(static_cast<MemoryBenchPoint>(I)),
                static_cast<unsigned long long>(SizeClass),
                static_cast<unsigned long long>(GroupCalls),
                static_cast<double>(GroupTotalNs) / 1000000.0,
                GroupAvgNs,
                static_cast<unsigned long long>(GroupMaxNs));
        }
    }
    std::fflush(stdout);
}

inline void memoryBenchRecord(
    const MemoryBenchPoint Point,
    const uint64_t ElapsedNs,
    const std::uint16_t SizeClass = InvalidSizeClass) noexcept
{
    auto &Counters               = memoryBenchCounters();
    auto &ThreadCounters         = memoryBenchThreadGroupCounters();
    auto &SizeClassCounters      = memoryBenchSizeClassGroupCounters();
    const std::size_t PointIndex = memoryBenchPointIndex(Point);

    memoryBenchUpdateCounter(Counters[PointIndex], ElapsedNs);
    memoryBenchUpdateCounter(ThreadCounters[PointIndex][memoryBenchThreadBucketForCurrentThread()], ElapsedNs);
    if (SizeClass < MemoryBenchSizeClassGroupBucketCount)
    {
        memoryBenchUpdateCounter(SizeClassCounters[PointIndex][SizeClass], ElapsedNs);
    }

    const uint64_t Sample = memoryBenchSampleCounter().fetch_add(1, std::memory_order_relaxed) + 1;
    if (Sample % MemoryBenchPrintEverySamples == 0)
    {
        memoryBenchPrintSummary();
    }
}

class MemoryBenchScope
{
public:
    explicit MemoryBenchScope(
        const MemoryBenchPoint Point,
        const std::uint16_t SizeClass = InvalidSizeClass) noexcept
        : Point(Point),
          SizeClass(SizeClass),
          StartNs(getTimestampNs())
    {
    }

    ~MemoryBenchScope() noexcept
    {
        memoryBenchRecord(Point, getTimestampNs() - StartNs, SizeClass);
    }

private:
    MemoryBenchPoint Point;
    std::uint16_t SizeClass = InvalidSizeClass;
    uint64_t StartNs   = 0;
};
#else
class MemoryBenchScope
{
public:
    constexpr explicit MemoryBenchScope(
        const MemoryBenchPoint,
        const std::uint16_t = InvalidSizeClass) noexcept
    {
    }
};

inline void memoryBenchPrintSummary() noexcept
{
}
#endif

} // namespace infra::mem

// ---------------------------------------------------------------------------
// 观测与分析模块（公共 API，位于 infra）
// ---------------------------------------------------------------------------
namespace infra
{

struct LeakRecord
{
    std::size_t Size           = 0;
    std::size_t Alignment      = 0;
    uint64_t AllocationId = 0;
    uint64_t Timestamp    = 0;
    std::uint32_t FrameIndex   = 0;
    std::uint32_t ResourceId   = 0;
    std::thread::id ThreadId   = std::this_thread::get_id();
};

class MemoryLeakDetectorObserver
{
public:
    /**
     * @brief Consume allocator events and maintain live-allocation map.
     * @param
     *     - Event  const MemoryEvent&  Event emitted by allocator pipeline.
     * @usage
     *     - Bind this observer when leak-tracking is required.
     * @return
     *     - void
     */
    void onMemoryEvent(const MemoryEvent &Event)
    {
        if (Event.Type == MemoryEventType::Allocate)
        {
            LeakRecord Record;
            Record.Size         = Event.Size;
            Record.Alignment    = Event.Alignment;
            Record.AllocationId = Event.AllocationId;
            Record.Timestamp    = Event.Timestamp;
            Record.FrameIndex   = Event.FrameIndex;
            Record.ResourceId   = Event.ResourceId;
            Record.ThreadId     = Event.ThreadId;

            std::lock_guard Lock(Mutex);
            LiveAllocations[Event.UserPtr] = Record;
            return;
        }

        if (Event.Type == MemoryEventType::Deallocate)
        {
            std::lock_guard Lock(Mutex);
            LiveAllocations.erase(Event.UserPtr);
        }
    }

    /**
     * @brief Check whether there are remaining live allocations.
     * @param
     *     - none
     * @usage
     *     - Useful before shutdown assertion.
     * @return
     *     - bool  true if no leak Record exists.
     */
    [[nodiscard]] bool isEmpty() const
    {
        std::lock_guard Lock(Mutex);
        return LiveAllocations.empty();
    }

    /**
     * @brief Get number of tracked live allocations.
     * @param
     *     - none
     * @usage
     *     - Use in leak report summary output.
     * @return
     *     - std::size_t  Live allocation Record count.
     */
    [[nodiscard]] std::size_t getLeakCount() const
    {
        std::lock_guard Lock(Mutex);
        return LiveAllocations.size();
    }

    /**
     * @brief Build a stable leak snapshot for reporting.
     * @param
     *     - none
     * @usage
     *     - Call at frame end or process shutdown to print unresolved allocations.
     * @return
     *     - std::vector<std::pair<void*, LeakRecord>>  Copy of current leak records.
     */
    [[nodiscard]] std::vector<std::pair<void *, LeakRecord> > getLeakSnapshot() const
    {
        std::lock_guard Lock(Mutex);
        std::vector<std::pair<void *, LeakRecord> > Leaks;
        Leaks.reserve(LiveAllocations.size());
        for (const auto &[ptr, Record]: LiveAllocations)
        {
            Leaks.emplace_back(ptr, Record);
        }
        return Leaks;
    }

    /**
     * @brief Print current unresolved leaks into output stream.
     * @param
     *     - Output  std::ostream&  Target stream for textual leak report.
     * @usage
     *     - Call explicitly on shutdown boundary instead of relying on destructors.
     * @return
     *     - bool  true when leaks are present and printed.
     */
    [[nodiscard]] bool reportLeaks(std::ostream &Output) const
    {
        const auto Leaks = getLeakSnapshot();
        if (Leaks.empty())
        {
            Output << "[MemoryLeakDetector] no live allocations.\n";
            return false;
        }

        Output << "[MemoryLeakDetector] unresolved allocations=" << Leaks.size() << "\n";
        for (const auto &[Ptr, Record]: Leaks)
        {
            Output << "  ptr=" << Ptr
                << " size=" << Record.Size
                << " align=" << Record.Alignment
                << " allocId=" << Record.AllocationId
                << " frame=" << Record.FrameIndex
                << " resource=" << Record.ResourceId
                << "\n";
        }

        return true;
    }

private:
    mutable std::mutex Mutex;
    std::unordered_map<void *, LeakRecord> LiveAllocations;
};


struct MemoryStatisticsSnapshot
{
    uint64_t AllocateCount        = 0;
    uint64_t DeallocateCount      = 0;
    uint64_t ThreadCacheHitCount  = 0;
    uint64_t ThreadCacheMissCount = 0;
    uint64_t CentralFetchCount    = 0;
    uint64_t CentralReturnCount   = 0;
    uint64_t OsAllocateCount      = 0;
    uint64_t OsReleaseCount       = 0;
    uint64_t GuardCorruptionCount = 0;
    uint64_t UseAfterFreeCount    = 0;
    std::size_t CurrentBytes           = 0;
    std::size_t PeakBytes              = 0;
};

struct AllocatorRuntimeStatsSnapshot
{
    std::size_t DedicatedAllocationCount  = 0;
    std::size_t DedicatedCacheBucketCount = 0;
    std::size_t DedicatedCacheBytes       = 0;
    std::size_t QuarantineEntryCount      = 0;
    std::size_t QuarantineBytes           = 0;

    std::size_t PageToSpanEntryCount = 0;

    std::size_t SpanObjectPoolAllocated = 0;
    std::size_t SpanObjectPoolInUse     = 0;
    double SpanObjectPoolUsageRatio     = 0.0;

    std::size_t RegionIndexRegionCount                = 0;
    std::size_t RegionIndexEntryCount                 = 0;
    std::size_t RegionIndexLevel1NodesUsed            = 0;
    std::size_t RegionIndexLevel1NodesCapacity        = 0;
    double RegionIndexLevel1UsageRatio                = 0.0;
    std::size_t RegionIndexLevel2NodesUsed            = 0;
    std::size_t RegionIndexLevel2NodesCapacity        = 0;
    double RegionIndexLevel2UsageRatio                = 0.0;
    std::size_t RegionIndexPoolExhaustionWarningCount = 0;
};

class MemoryStatisticsObserver
{
public:
    /**
     * @brief Consume one memory event and update aggregated counters.
     * @param
     *     - event  const MemoryEvent&  Event emitted by allocator pipeline.
     * @usage
     *     - Bind this observer to AllocatorFacade event bus for runtime metrics.
     * @return
     *     - void
     */
    void onMemoryEvent(const MemoryEvent &event) noexcept
    {
        switch (event.Type)
        {
            case MemoryEventType::Allocate:
                AllocateCount.fetch_add(1, std::memory_order_relaxed);
                CurrentBytes.fetch_add(event.Size, std::memory_order_relaxed);
                updatePeak();
                if (event.FromOS)
                {
                    OsAllocateCount.fetch_add(1, std::memory_order_relaxed);
                }
                break;

            case MemoryEventType::Deallocate:
                DeallocateCount.fetch_add(1, std::memory_order_relaxed);
                subtractCurrentBytes(event.Size);
                if (event.FromOS)
                {
                    OsReleaseCount.fetch_add(1, std::memory_order_relaxed);
                }
                break;

            case MemoryEventType::ThreadCacheHit:
                ThreadCacheHitCount.fetch_add(1, std::memory_order_relaxed);
                break;

            case MemoryEventType::ThreadCacheMiss:
                ThreadCacheMissCount.fetch_add(1, std::memory_order_relaxed);
                break;

            case MemoryEventType::CentralFetch:
                CentralFetchCount.fetch_add(1, std::memory_order_relaxed);
                break;

            case MemoryEventType::CentralReturn:
                CentralReturnCount.fetch_add(1, std::memory_order_relaxed);
                break;

            case MemoryEventType::GuardCorruption:
                GuardCorruptionCount.fetch_add(1, std::memory_order_relaxed);
                break;

            case MemoryEventType::UseAfterFree:
                UseAfterFreeCount.fetch_add(1, std::memory_order_relaxed);
                break;

            default:
                break;
        }
    }

    /**
     * @brief Build an immutable statistics snapshot.
     * @param
     *     - none
     * @usage
     *     - Poll this periodically for HUD/telemetry output.
     * @return
     *     - MemoryStatisticsSnapshot  Current accumulated counters.
     */
    [[nodiscard]] MemoryStatisticsSnapshot getSnapshot() const noexcept
    {
        MemoryStatisticsSnapshot Value;
        Value.AllocateCount        = AllocateCount.load(std::memory_order_relaxed);
        Value.DeallocateCount      = DeallocateCount.load(std::memory_order_relaxed);
        Value.ThreadCacheHitCount  = ThreadCacheHitCount.load(std::memory_order_relaxed);
        Value.ThreadCacheMissCount = ThreadCacheMissCount.load(std::memory_order_relaxed);
        Value.CentralFetchCount    = CentralFetchCount.load(std::memory_order_relaxed);
        Value.CentralReturnCount   = CentralReturnCount.load(std::memory_order_relaxed);
        Value.OsAllocateCount      = OsAllocateCount.load(std::memory_order_relaxed);
        Value.OsReleaseCount       = OsReleaseCount.load(std::memory_order_relaxed);
        Value.GuardCorruptionCount = GuardCorruptionCount.load(std::memory_order_relaxed);
        Value.UseAfterFreeCount    = UseAfterFreeCount.load(std::memory_order_relaxed);
        Value.CurrentBytes         = CurrentBytes.load(std::memory_order_relaxed);
        Value.PeakBytes            = PeakBytes.load(std::memory_order_relaxed);
        return Value;
    }

private:
    void updatePeak() noexcept
    {
        std::size_t Current = CurrentBytes.load(std::memory_order_relaxed);
        std::size_t peak    = PeakBytes.load(std::memory_order_relaxed);
        while (Current > peak && !PeakBytes.compare_exchange_weak(peak, Current, std::memory_order_relaxed))
        {
        }
    }

    // 带下溢钳制的减法：避免调用方传入错误 size 导致无符号回绕。
    void subtractCurrentBytes(const std::size_t Delta) noexcept
    {
        std::size_t Current = CurrentBytes.load(std::memory_order_relaxed);
        for (;;)
        {
            const std::size_t Next = Current >= Delta ? Current - Delta : 0;
            if (CurrentBytes.compare_exchange_weak(
                    Current, Next, std::memory_order_relaxed, std::memory_order_relaxed))
            {
                break;
            }
        }
    }

    std::atomic<uint64_t> AllocateCount        = 0;
    std::atomic<uint64_t> DeallocateCount      = 0;
    std::atomic<uint64_t> ThreadCacheHitCount  = 0;
    std::atomic<uint64_t> ThreadCacheMissCount = 0;
    std::atomic<uint64_t> CentralFetchCount    = 0;
    std::atomic<uint64_t> CentralReturnCount   = 0;
    std::atomic<uint64_t> OsAllocateCount      = 0;
    std::atomic<uint64_t> OsReleaseCount       = 0;
    std::atomic<uint64_t> GuardCorruptionCount = 0;
    std::atomic<uint64_t> UseAfterFreeCount    = 0;
    std::atomic<std::size_t> CurrentBytes           = 0;
    std::atomic<std::size_t> PeakBytes              = 0;
};


inline constexpr std::size_t MemoryTrackerUnknownSize               = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t MemoryTrackerDefaultMaxTimelineEntries = 10000;

enum class HookAllocationOp : std::uint8_t
{
    New,
    Delete,
    NewArray,
    DeleteArray,
    Malloc,
    Free,
    Calloc,
    Realloc,
    AlignedNew,
    AlignedDelete,
    Unknown,
};

using LegacyAllocOp                            = HookAllocationOp;
inline constexpr std::size_t LegacyUnknownSize = MemoryTrackerUnknownSize;

/**
 * @brief 将 Hook 操作枚举转换为可读名称（供报告与时间线复用）。
 * @param Op HookAllocationOp 操作类型
 * @return 静态字符串名称
 */
[[nodiscard]] inline const char *hookOpName(const HookAllocationOp Op) noexcept
{
    switch (Op)
    {
        case HookAllocationOp::New:          return "new";
        case HookAllocationOp::Delete:       return "delete";
        case HookAllocationOp::NewArray:     return "new[]";
        case HookAllocationOp::DeleteArray:  return "delete[]";
        case HookAllocationOp::Malloc:       return "malloc";
        case HookAllocationOp::Free:         return "free";
        case HookAllocationOp::Calloc:       return "calloc";
        case HookAllocationOp::Realloc:      return "realloc";
        case HookAllocationOp::AlignedNew:   return "aligned_new";
        case HookAllocationOp::AlignedDelete: return "aligned_delete";
        default:                             return "unknown";
    }
}

enum class MemoryIssueType : std::uint8_t
{
    DoubleAllocation,
    DoubleFree,
    InvalidFree,
    MismatchedFreeFunction,
    SizeMismatch,
    AlignmentMismatch,
    GuardCorruption,
    UseAfterFree,
    ValidationError,
    AllocationFailure,
    ReallocFailure,
};

struct MemoryPayloadPreview
{
    std::size_t CapturedBytes = 0;
    std::size_t OriginalBytes = 0;
    std::string Hex;
};

struct TraceRecord
{
    std::string Name;
    std::string Source;
    uint64_t TimestampUs  = 0;
    std::uint32_t ThreadId     = 0;
    std::uintptr_t Address     = 0;
    std::size_t Size           = 0;
    std::size_t Alignment      = 0;
    uint64_t AllocationId = 0;
    std::uint32_t ResourceId   = 0;
};

struct MemoryIssueRecord
{
    MemoryIssueType Type       = MemoryIssueType::InvalidFree;
    HookAllocationOp Operation = HookAllocationOp::Unknown;
    std::uintptr_t Address     = 0;
    std::size_t Size           = 0;
    std::size_t Alignment      = 0;
    uint64_t TimestampNs  = 0;
    std::uint32_t ThreadId     = 0;
    std::string Message;
    MemoryPayloadPreview Payload;
};

struct LiveAllocationRecord
{
    HookAllocationOp Operation = HookAllocationOp::Unknown;
    std::uintptr_t Address     = 0;
    std::size_t Size           = 0;
    std::size_t Alignment      = 0;
    uint64_t AllocationId = 0;
    uint64_t TimestampNs  = 0;
    std::uint32_t ThreadId     = 0;
    std::uint32_t FrameIndex   = 0;
    std::uint32_t ResourceId   = 0;
    bool FromEngineEvent       = false;
    bool FromThreadCache       = false;
    bool FromCentralPool       = false;
    bool FromOS                = false;
    MemoryPayloadPreview LastPayload;
};

struct MemoryTrackerStatistics
{
    uint64_t EngineEventCount = 0;
    uint64_t HookEventCount   = 0;

    uint64_t AllocateCount        = 0;
    uint64_t DeallocateCount      = 0;
    uint64_t ThreadCacheHitCount  = 0;
    uint64_t ThreadCacheMissCount = 0;
    uint64_t CentralFetchCount    = 0;
    uint64_t CentralReturnCount   = 0;
    uint64_t OsAllocateCount      = 0;
    uint64_t OsReleaseCount       = 0;
    uint64_t GuardCorruptionCount = 0;
    uint64_t UseAfterFreeCount    = 0;
    uint64_t ValidationErrorCount = 0;

    uint64_t HookAllocationCount        = 0;
    uint64_t HookFreeCount              = 0;
    uint64_t HookReallocCount           = 0;
    uint64_t HookNullFreeCount          = 0;
    uint64_t HookAllocationFailureCount = 0;

    uint64_t HookNewCount           = 0;
    uint64_t HookDeleteCount        = 0;
    uint64_t HookNewArrayCount      = 0;
    uint64_t HookDeleteArrayCount   = 0;
    uint64_t HookMallocCount        = 0;
    uint64_t HookFreeFunctionCount  = 0;
    uint64_t HookCallocCount        = 0;
    uint64_t HookAlignedNewCount    = 0;
    uint64_t HookAlignedDeleteCount = 0;

    uint64_t DoubleAllocationCount       = 0;
    uint64_t DoubleFreeCount             = 0;
    uint64_t InvalidFreeCount            = 0;
    uint64_t MismatchedFreeFunctionCount = 0;
    uint64_t SizeMismatchCount           = 0;
    uint64_t AlignmentMismatchCount      = 0;
    uint64_t IssueCount                  = 0;

    std::size_t TotalAllocatedBytes = 0;
    std::size_t TotalFreedBytes     = 0;
    std::size_t CurrentLiveBytes    = 0;
    std::size_t PeakLiveBytes       = 0;
    std::size_t CurrentLiveBlocks   = 0;
    std::size_t PeakLiveBlocks      = 0;
};

struct MemoryTrackerReport
{
    MemoryTrackerStatistics Statistics;
    std::vector<TraceRecord> Timeline;
    std::vector<MemoryIssueRecord> Issues;
    std::vector<LiveAllocationRecord> LiveAllocations;
};

class IMemoryTrackerReportBackend
{
public:
    virtual ~IMemoryTrackerReportBackend() = default;

    [[nodiscard]] virtual bool write(
        const MemoryTrackerReport &Report,
        const std::filesystem::path &Path) const = 0;
};

class JsonMemoryTrackerReportBackend final : public IMemoryTrackerReportBackend
{
public:
    [[nodiscard]] bool write(
        const MemoryTrackerReport &Report,
        const std::filesystem::path &Path) const override
    {
        std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
        if (!Output.is_open())
        {
            return false;
        }

        Output << "{\n";
        writeStatistics(Report.Statistics, Output);
        Output << ",\n";
        writeIssues(Report.Issues, Output);
        Output << ",\n";
        writeLiveAllocations(Report.LiveAllocations, Output);
        Output << ",\n";
        writeTimeline(Report.Timeline, Output);
        Output << "\n}\n";
        return Output.good();
    }

private:
    [[nodiscard]] static std::string escapeJson(const std::string &InText)
    {
        std::string Escaped;
        Escaped.reserve(InText.size() + 16);
        for (char Character: InText)
        {
            switch (Character)
            {
                case '"':
                    Escaped += "\\\"";
                    break;
                case '\\':
                    Escaped += "\\\\";
                    break;
                case '\b':
                    Escaped += "\\b";
                    break;
                case '\f':
                    Escaped += "\\f";
                    break;
                case '\n':
                    Escaped += "\\n";
                    break;
                case '\r':
                    Escaped += "\\r";
                    break;
                case '\t':
                    Escaped += "\\t";
                    break;
                default:
                    Escaped += Character;
                    break;
            }
        }
        return Escaped;
    }

    [[nodiscard]] static std::string pointerToString(const std::uintptr_t Address)
    {
        std::ostringstream Stream;
        Stream << "0x" << std::hex << std::uppercase << Address;
        return Stream.str();
    }

    [[nodiscard]] static const char *issueTypeName(const MemoryIssueType Type)
    {
        switch (Type)
        {
            case MemoryIssueType::DoubleAllocation:
                return "double_allocation";
            case MemoryIssueType::DoubleFree:
                return "double_free";
            case MemoryIssueType::InvalidFree:
                return "invalid_free";
            case MemoryIssueType::MismatchedFreeFunction:
                return "mismatched_free_function";
            case MemoryIssueType::SizeMismatch:
                return "size_mismatch";
            case MemoryIssueType::AlignmentMismatch:
                return "alignment_mismatch";
            case MemoryIssueType::GuardCorruption:
                return "guard_corruption";
            case MemoryIssueType::UseAfterFree:
                return "use_after_free";
            case MemoryIssueType::ValidationError:
                return "validation_error";
            case MemoryIssueType::AllocationFailure:
                return "allocation_failure";
            case MemoryIssueType::ReallocFailure:
                return "realloc_failure";
            default:
                return "unknown";
        }
    }

    static void writeStatistics(const MemoryTrackerStatistics &Stats, std::ofstream &Output)
    {
        Output << "  \"statistics\": {\n";
        Output << "    \"engineEventCount\": " << Stats.EngineEventCount << ",\n";
        Output << "    \"hookEventCount\": " << Stats.HookEventCount << ",\n";
        Output << "    \"allocateCount\": " << Stats.AllocateCount << ",\n";
        Output << "    \"deallocateCount\": " << Stats.DeallocateCount << ",\n";
        Output << "    \"threadCacheHitCount\": " << Stats.ThreadCacheHitCount << ",\n";
        Output << "    \"threadCacheMissCount\": " << Stats.ThreadCacheMissCount << ",\n";
        Output << "    \"centralFetchCount\": " << Stats.CentralFetchCount << ",\n";
        Output << "    \"centralReturnCount\": " << Stats.CentralReturnCount << ",\n";
        Output << "    \"osAllocateCount\": " << Stats.OsAllocateCount << ",\n";
        Output << "    \"osReleaseCount\": " << Stats.OsReleaseCount << ",\n";
        Output << "    \"guardCorruptionCount\": " << Stats.GuardCorruptionCount << ",\n";
        Output << "    \"useAfterFreeCount\": " << Stats.UseAfterFreeCount << ",\n";
        Output << "    \"validationErrorCount\": " << Stats.ValidationErrorCount << ",\n";
        Output << "    \"hookAllocationCount\": " << Stats.HookAllocationCount << ",\n";
        Output << "    \"hookFreeCount\": " << Stats.HookFreeCount << ",\n";
        Output << "    \"hookReallocCount\": " << Stats.HookReallocCount << ",\n";
        Output << "    \"hookNullFreeCount\": " << Stats.HookNullFreeCount << ",\n";
        Output << "    \"hookAllocationFailureCount\": " << Stats.HookAllocationFailureCount << ",\n";
        Output << "    \"doubleAllocationCount\": " << Stats.DoubleAllocationCount << ",\n";
        Output << "    \"doubleFreeCount\": " << Stats.DoubleFreeCount << ",\n";
        Output << "    \"invalidFreeCount\": " << Stats.InvalidFreeCount << ",\n";
        Output << "    \"mismatchedFreeFunctionCount\": " << Stats.MismatchedFreeFunctionCount << ",\n";
        Output << "    \"sizeMismatchCount\": " << Stats.SizeMismatchCount << ",\n";
        Output << "    \"alignmentMismatchCount\": " << Stats.AlignmentMismatchCount << ",\n";
        Output << "    \"issueCount\": " << Stats.IssueCount << ",\n";
        Output << "    \"totalAllocatedBytes\": " << Stats.TotalAllocatedBytes << ",\n";
        Output << "    \"totalFreedBytes\": " << Stats.TotalFreedBytes << ",\n";
        Output << "    \"currentLiveBytes\": " << Stats.CurrentLiveBytes << ",\n";
        Output << "    \"peakLiveBytes\": " << Stats.PeakLiveBytes << ",\n";
        Output << "    \"currentLiveBlocks\": " << Stats.CurrentLiveBlocks << ",\n";
        Output << "    \"peakLiveBlocks\": " << Stats.PeakLiveBlocks << "\n";
        Output << "  }";
    }

    static void writeIssues(const std::vector<MemoryIssueRecord> &Issues, std::ofstream &Output)
    {
        Output << "  \"issues\": [\n";
        for (std::size_t Index = 0; Index < Issues.size(); ++Index)
        {
            const MemoryIssueRecord &Issue = Issues[Index];
            Output << "    {\"type\":\"" << issueTypeName(Issue.Type)
                << "\",\"operation\":\"" << hookOpName(Issue.Operation)
                << "\",\"address\":\"" << pointerToString(Issue.Address)
                << "\",\"size\":" << Issue.Size
                << ",\"alignment\":" << Issue.Alignment
                << ",\"timestampNs\":" << Issue.TimestampNs
                << ",\"threadId\":" << Issue.ThreadId
                << ",\"message\":\"" << escapeJson(Issue.Message)
                << "\",\"payload\":{\"capturedBytes\":" << Issue.Payload.CapturedBytes
                << ",\"originalBytes\":" << Issue.Payload.OriginalBytes
                << ",\"hex\":\"" << escapeJson(Issue.Payload.Hex) << "\"}}";
            if (Index + 1 < Issues.size())
            {
                Output << ",";
            }
            Output << "\n";
        }
        Output << "  ]";
    }

    static void writeLiveAllocations(
        const std::vector<LiveAllocationRecord> &LiveAllocations,
        std::ofstream &Output)
    {
        Output << "  \"liveAllocations\": [\n";
        for (std::size_t Index = 0; Index < LiveAllocations.size(); ++Index)
        {
            const LiveAllocationRecord &Record = LiveAllocations[Index];
            Output << "    {\"operation\":\"" << hookOpName(Record.Operation)
                << "\",\"address\":\"" << pointerToString(Record.Address)
                << "\",\"size\":" << Record.Size
                << ",\"alignment\":" << Record.Alignment
                << ",\"allocationId\":" << Record.AllocationId
                << ",\"timestampNs\":" << Record.TimestampNs
                << ",\"threadId\":" << Record.ThreadId
                << ",\"frameIndex\":" << Record.FrameIndex
                << ",\"resourceId\":" << Record.ResourceId
                << ",\"fromEngineEvent\":" << (Record.FromEngineEvent ? "true" : "false")
                << ",\"fromThreadCache\":" << (Record.FromThreadCache ? "true" : "false")
                << ",\"fromCentralPool\":" << (Record.FromCentralPool ? "true" : "false")
                << ",\"fromOS\":" << (Record.FromOS ? "true" : "false")
                << ",\"payload\":{\"capturedBytes\":" << Record.LastPayload.CapturedBytes
                << ",\"originalBytes\":" << Record.LastPayload.OriginalBytes
                << ",\"hex\":\"" << escapeJson(Record.LastPayload.Hex) << "\"}}";
            if (Index + 1 < LiveAllocations.size())
            {
                Output << ",";
            }
            Output << "\n";
        }
        Output << "  ]";
    }

    static void writeTimeline(const std::vector<TraceRecord> &Timeline, std::ofstream &Output)
    {
        Output << "  \"timeline\": [\n";
        for (std::size_t Index = 0; Index < Timeline.size(); ++Index)
        {
            const TraceRecord &Record = Timeline[Index];
            Output << "    {\"name\":\"" << escapeJson(Record.Name)
                << "\",\"source\":\"" << escapeJson(Record.Source)
                << "\",\"timestampUs\":" << Record.TimestampUs
                << ",\"threadId\":" << Record.ThreadId
                << ",\"address\":\"" << pointerToString(Record.Address)
                << "\",\"size\":" << Record.Size
                << ",\"alignment\":" << Record.Alignment
                << ",\"allocationId\":" << Record.AllocationId
                << ",\"resourceId\":" << Record.ResourceId << "}";
            if (Index + 1 < Timeline.size())
            {
                Output << ",";
            }
            Output << "\n";
        }
        Output << "  ]";
    }
};

class MemoryTracker
{
public:
    [[nodiscard]] static constexpr bool isCompiledEnabled() noexcept
    {
        return MemoryCompileEnableDebugGuards;
    }

    /**
     * @brief Convert memory events into tracker records and statistics.
     * @param
     *     - Event  const MemoryEvent&  Event emitted by allocator pipeline.
     * @usage
     *     - Bind one tracker instance to AllocatorFacade event bus.
     * @return
     *     - void
     */
    void onMemoryEvent(const MemoryEvent &Event)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) Event;
            return;
        }

        global().recordEngineEvent(Event);
    }

    /**
     * @brief Notify tracker about an allocation captured by hook wrappers.
     * @param
     *     - Operation  HookAllocationOp  Hook operation kind.
     *     - Ptr        void*             Returned allocation pointer.
     *     - Size       std::size_t       Requested size.
     *     - Alignment  std::size_t       Requested alignment or unknown marker.
     *     - Caller     void*             Caller return address if available.
     * @usage
     *     - Called by new/malloc hook wrappers.
     * @return
     *     - void
     */
    static void onHookAllocation(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) Operation;
            (void) Ptr;
            (void) Size;
            (void) Alignment;
            (void) Caller;
            return;
        }

        global().recordHookAllocation(Operation, Ptr, Size, Alignment, Caller);
    }

    /**
     * @brief Notify tracker about a deallocation captured by hook wrappers.
     * @param
     *     - Operation  HookAllocationOp  Hook operation kind.
     *     - Ptr        void*             Pointer passed to free/delete.
     *     - Size       std::size_t       Optional size (or unknown marker).
     *     - Alignment  std::size_t       Optional alignment (or unknown marker).
     *     - Caller     void*             Caller return address if available.
     * @usage
     *     - Called by new/malloc hook wrappers before real free.
     * @return
     *     - void
     */
    static void onHookFree(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) Operation;
            (void) Ptr;
            (void) Size;
            (void) Alignment;
            (void) Caller;
            return;
        }

        global().recordHookFree(Operation, Ptr, Size, Alignment, Caller);
    }

    /**
     * @brief Notify tracker about realloc behavior captured by hook wrappers.
     * @param
     *     - OldPtr     void*        Old pointer passed to realloc.
     *     - NewPtr     void*        Returned pointer from realloc.
     *     - NewSize    std::size_t  Requested new size.
     *     - Caller     void*        Caller return address if available.
     * @usage
     *     - Called by realloc hook wrapper after real realloc returns.
     * @return
     *     - void
     */
    static void onHookReallocate(
        void *OldPtr,
        void *NewPtr,
        const std::size_t NewSize,
        void *Caller)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) OldPtr;
            (void) NewPtr;
            (void) NewSize;
            (void) Caller;
            return;
        }

        global().recordHookReallocate(OldPtr, NewPtr, NewSize, Caller);
    }

    /**
     * @brief Notify tracker about allocation failure captured by hook wrappers.
     * @param
     *     - Operation  HookAllocationOp  Hook operation kind.
     *     - Size       std::size_t       Requested size.
     *     - Alignment  std::size_t       Requested alignment or unknown marker.
     *     - Caller     void*             Caller return address if available.
     * @usage
     *     - Called when hook wrapper receives null from allocator function.
     * @return
     *     - void
     */
    static void onHookAllocationFailure(
        const HookAllocationOp Operation,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) Operation;
            (void) Size;
            (void) Alignment;
            (void) Caller;
            return;
        }

        global().recordHookAllocationFailure(Operation, Size, Alignment, Caller);
    }

    [[nodiscard]] static MemoryTracker &global()
    {
        struct StorageBlock
        {
            alignas(MemoryTracker) std::byte Data[sizeof(MemoryTracker)];
        };

        static StorageBlock Storage;
        static MemoryTracker *Instance = []() -> MemoryTracker *
        {
            struct BootstrapSuppressionScope
            {
                BootstrapSuppressionScope()
                {
                    ++getHookSuppressionDepth();
                }

                ~BootstrapSuppressionScope()
                {
                    --getHookSuppressionDepth();
                }
            } Scope;

            (void) Scope;
            return new(Storage.Data) MemoryTracker();
        }();
        return *Instance;
    }

    static void setHookEnabled(const bool Enabled)
    {
        if constexpr (!isCompiledEnabled())
        {
            global().HookEnabled.store(false, std::memory_order_release);
            (void) Enabled;
            return;
        }

        global().HookEnabled.store(Enabled, std::memory_order_release);
    }

    [[nodiscard]] static bool isHookEnabled()
    {
        if constexpr (!isCompiledEnabled())
        {
            return false;
        }

        return global().HookEnabled.load(std::memory_order_acquire);
    }

    [[nodiscard]] static bool isTrackingSuppressedForCurrentThread() noexcept
    {
        return getHookSuppressionDepth() > 0;
    }

    static void setPayloadPreviewBytes(const std::size_t Bytes)
    {
        std::lock_guard Lock(global().Mutex);
        global().PayloadPreviewBytes = Bytes;
    }

    [[nodiscard]] static std::size_t getPayloadPreviewBytes()
    {
        std::lock_guard Lock(global().Mutex);
        return global().PayloadPreviewBytes;
    }

    static void setFreedHistoryLimit(const std::size_t Limit)
    {
        std::lock_guard Lock(global().Mutex);
        global().FreedHistoryLimit = std::max<std::size_t>(1, Limit);
        global().trimFreedHistoryLocked();
    }

    [[nodiscard]] static std::size_t getFreedHistoryLimit()
    {
        std::lock_guard Lock(global().Mutex);
        return global().FreedHistoryLimit;
    }

    static void setMaxTimelineEntries(const std::size_t Limit)
    {
        std::lock_guard Lock(global().Mutex);
        global().MaxTimelineEntries = Limit;
        while (global().Timeline.size() > global().MaxTimelineEntries)
        {
            global().Timeline.pop_front();
        }
    }

    [[nodiscard]] static std::size_t getMaxTimelineEntries()
    {
        std::lock_guard Lock(global().Mutex);
        return global().MaxTimelineEntries;
    }

    /**
     * @brief Flush the full tracker report by using JSON backend.
     * @param
     *     - InPath  const std::filesystem::path&  Output path.
     * @usage
     *     - Use when a persistent report file is required after test/runtime.
     * @return
     *     - bool  true when report is written successfully.
     */
    [[nodiscard]] bool flushToFile(const std::filesystem::path &InPath) const
    {
        JsonMemoryTrackerReportBackend Backend;
        return flushReport(InPath, Backend);
    }

    /**
     * @brief Flush tracker report through custom report backend.
     * @param
     *     - InPath    const std::filesystem::path&  Output path.
     *     - Backend   const IMemoryTrackerReportBackend&  Backend implementation.
     * @usage
     *     - Use to extend report format (json/yml/etc.) while sharing infra snapshot.
     * @return
     *     - bool  true when backend successfully writes report.
     */
    [[nodiscard]] bool flushReport(
        const std::filesystem::path &InPath,
        const IMemoryTrackerReportBackend &Backend) const
    {
        return Backend.write(getReport(), InPath);
    }

    /**
     * @brief Get immutable report snapshot.
     * @param
     *     - none
     * @usage
     *     - Read detailed statistics, issues and live allocations in-memory.
     * @return
     *     - MemoryTrackerReport  Snapshot copy.
     */
    [[nodiscard]] MemoryTrackerReport getReport() const
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(global().Mutex);
        MemoryTrackerReport Report;
        Report.Statistics = global().Statistics;
        Report.Timeline.assign(global().Timeline.begin(), global().Timeline.end());
        Report.Issues          = global().Issues;
        Report.LiveAllocations = global().buildLiveAllocationSnapshotLocked();
        return Report;
    }

    [[nodiscard]] MemoryTrackerStatistics getStatistics() const
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(global().Mutex);
        return global().Statistics;
    }

    /**
     * @brief Query current buffered event count.
     * @param
     *     - none
     * @usage
     *     - Keep compatibility with existing tests checking tracker activity.
     * @return
     *     - std::size_t  Number of timeline records.
     */
    [[nodiscard]] std::size_t getEventCount() const
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(global().Mutex);
        return global().Timeline.size();
    }

    void clear()
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(global().Mutex);
        global().Timeline.clear();
        global().Issues.clear();
        global().ActiveAllocations.clear();
        global().FreedAllocations.clear();
        global().FreedOrder.clear();
        global().Statistics        = {};
        global().NextFreedSequence = 0;
    }

private:
    struct ActiveAllocation
    {
        HookAllocationOp Operation = HookAllocationOp::Unknown;
        std::size_t Size           = 0;
        std::size_t Alignment      = MemoryTrackerUnknownSize;
        uint64_t AllocationId = 0;
        uint64_t TimestampNs  = 0;
        std::uint32_t ThreadId     = 0;
        std::uint32_t FrameIndex   = 0;
        std::uint32_t ResourceId   = 0;
        bool FromEngineEvent       = false;
        bool FromThreadCache       = false;
        bool FromCentralPool       = false;
        bool FromOS                = false;
        MemoryPayloadPreview LastPayload;
    };

    struct FreedAllocation
    {
        uint64_t Sequence    = 0;
        HookAllocationOp FreedBy  = HookAllocationOp::Unknown;
        std::size_t Size          = 0;
        std::size_t Alignment     = MemoryTrackerUnknownSize;
        uint64_t TimestampNs = 0;
        std::uint32_t ThreadId    = 0;
        bool FromEngineEvent      = false;
        MemoryPayloadPreview Payload;
    };

    struct FreedOrderEntry
    {
        void *Ptr              = nullptr;
        uint64_t Sequence = 0;
    };

    [[nodiscard]] static int &getHookSuppressionDepth() noexcept
    {
        static thread_local int Depth = 0;
        return Depth;
    }

    class HookSuppressionScope
    {
    public:
        HookSuppressionScope()
        {
            ++getHookSuppressionDepth();
        }

        ~HookSuppressionScope()
        {
            --getHookSuppressionDepth();
        }
    };

    MemoryTracker() = default;

    static void updatePeak(std::size_t &Peak, const std::size_t Current)
    {
        if (Current > Peak)
        {
            Peak = Current;
        }
    }

    [[nodiscard]] static std::uint32_t threadIdFrom(const std::thread::id &Id)
    {
        return static_cast<std::uint32_t>(std::hash<std::thread::id>{}(Id));
    }

    [[nodiscard]] static std::uint32_t currentThreadId()
    {
        return threadIdFrom(std::this_thread::get_id());
    }

    [[nodiscard]] static HookAllocationOp expectedFreeOperation(const HookAllocationOp Operation)
    {
        switch (Operation)
        {
            case HookAllocationOp::New:
                return HookAllocationOp::Delete;
            case HookAllocationOp::NewArray:
                return HookAllocationOp::DeleteArray;
            case HookAllocationOp::AlignedNew:
                return HookAllocationOp::AlignedDelete;
            case HookAllocationOp::Malloc:
            case HookAllocationOp::Calloc:
            case HookAllocationOp::Realloc:
                return HookAllocationOp::Free;
            default:
                return HookAllocationOp::Unknown;
        }
    }

    [[nodiscard]] static const char *eventTypeName(const MemoryEventType Type)
    {
        switch (Type)
        {
            case MemoryEventType::Allocate:
                return "Allocate";
            case MemoryEventType::Deallocate:
                return "Deallocate";
            case MemoryEventType::ThreadCacheHit:
                return "ThreadCacheHit";
            case MemoryEventType::ThreadCacheMiss:
                return "ThreadCacheMiss";
            case MemoryEventType::CentralFetch:
                return "CentralFetch";
            case MemoryEventType::CentralReturn:
                return "CentralReturn";
            case MemoryEventType::GuardCorruption:
                return "GuardCorruption";
            case MemoryEventType::UseAfterFree:
                return "UseAfterFree";
            case MemoryEventType::ValidationError:
                return "ValidationError";
            case MemoryEventType::Reserve:
                return "Reserve";
            case MemoryEventType::Commit:
                return "Commit";
            case MemoryEventType::Decommit:
                return "Decommit";
            case MemoryEventType::Release:
                return "Release";
            case MemoryEventType::PageAllocate:
                return "PageAllocate";
            case MemoryEventType::PageFree:
                return "PageFree";
            case MemoryEventType::Reallocate:
                return "Reallocate";
            default:
                return "Unknown";
        }
    }

    [[nodiscard]] MemoryPayloadPreview capturePayloadPreview(const void *Ptr, const std::size_t Size) const
    {
        MemoryPayloadPreview Preview;
        Preview.OriginalBytes = Size;

        if constexpr (!MemoryCompileEnableDebugGuards)
        {
            return Preview;
        }

        if (!Ptr || Size == 0 || PayloadPreviewBytes == 0)
        {
            return Preview;
        }

        const std::size_t CaptureBytes = std::min(Size, PayloadPreviewBytes);
        Preview.CapturedBytes          = CaptureBytes;
        const auto *Bytes              = static_cast<const std::uint8_t *>(Ptr);

        std::ostringstream Stream;
        Stream << std::hex << std::setfill('0');
        for (std::size_t Index = 0; Index < CaptureBytes; ++Index)
        {
            Stream << std::setw(2) << static_cast<unsigned>(Bytes[Index]);
            if (Index + 1 < CaptureBytes)
            {
                Stream << ' ';
            }
        }

        Preview.Hex = Stream.str();
        return Preview;
    }

    void appendTimelineLocked(
        const std::string &Name,
        const std::string &Source,
        const uint64_t TimestampNs,
        const std::uint32_t ThreadId,
        const std::uintptr_t Address,
        const std::size_t Size,
        const std::size_t Alignment,
        const uint64_t AllocationId,
        const std::uint32_t ResourceId)
    {
        TraceRecord Record;
        Record.Name         = Name;
        Record.Source       = Source;
        Record.TimestampUs  = TimestampNs / 1000;
        Record.ThreadId     = ThreadId;
        Record.Address      = Address;
        Record.Size         = Size;
        Record.Alignment    = Alignment;
        Record.AllocationId = AllocationId;
        Record.ResourceId   = ResourceId;
        if (MaxTimelineEntries == 0)
        {
            return;
        }
        Timeline.push_back(std::move(Record));
        if (Timeline.size() > MaxTimelineEntries)
        {
            Timeline.pop_front();
        }
    }

    void addIssueLocked(
        const MemoryIssueType Type,
        const HookAllocationOp Operation,
        const std::uintptr_t Address,
        const std::size_t Size,
        const std::size_t Alignment,
        const std::string &Message,
        const MemoryPayloadPreview &Payload,
        const uint64_t TimestampNs,
        const std::uint32_t ThreadId)
    {
        MemoryIssueRecord Issue;
        Issue.Type        = Type;
        Issue.Operation   = Operation;
        Issue.Address     = Address;
        Issue.Size        = Size;
        Issue.Alignment   = Alignment;
        Issue.TimestampNs = TimestampNs;
        Issue.ThreadId    = ThreadId;
        Issue.Message     = Message;
        Issue.Payload     = Payload;
        Issues.push_back(std::move(Issue));
        ++Statistics.IssueCount;
    }

    void rememberFreedAllocationLocked(void *Ptr, const FreedAllocation &Record)
    {
        FreedAllocations[Ptr] = Record;
        FreedOrder.push_back(FreedOrderEntry{Ptr, Record.Sequence});
        trimFreedHistoryLocked();
    }

    void trimFreedHistoryLocked()
    {
        while (FreedOrder.size() > FreedHistoryLimit)
        {
            const FreedOrderEntry Entry = FreedOrder.front();
            FreedOrder.pop_front();

            auto Iter = FreedAllocations.find(Entry.Ptr);
            if (Iter != FreedAllocations.end() && Iter->second.Sequence == Entry.Sequence)
            {
                FreedAllocations.erase(Iter);
            }
        }
    }

    [[nodiscard]] std::vector<LiveAllocationRecord> buildLiveAllocationSnapshotLocked() const
    {
        std::vector<LiveAllocationRecord> Snapshot;
        Snapshot.reserve(ActiveAllocations.size());

        for (const auto &[Ptr, Allocation]: ActiveAllocations)
        {
            LiveAllocationRecord Record;
            Record.Operation       = Allocation.Operation;
            Record.Address         = reinterpret_cast<std::uintptr_t>(Ptr);
            Record.Size            = Allocation.Size;
            Record.Alignment       = Allocation.Alignment;
            Record.AllocationId    = Allocation.AllocationId;
            Record.TimestampNs     = Allocation.TimestampNs;
            Record.ThreadId        = Allocation.ThreadId;
            Record.FrameIndex      = Allocation.FrameIndex;
            Record.ResourceId      = Allocation.ResourceId;
            Record.FromEngineEvent = Allocation.FromEngineEvent;
            Record.FromThreadCache = Allocation.FromThreadCache;
            Record.FromCentralPool = Allocation.FromCentralPool;
            Record.FromOS          = Allocation.FromOS;
            Record.LastPayload     = Allocation.LastPayload;
            Snapshot.push_back(std::move(Record));
        }

        return Snapshot;
    }

    void updateHookOperationCountersLocked(const HookAllocationOp Operation)
    {
        switch (Operation)
        {
            case HookAllocationOp::New:
                ++Statistics.HookNewCount;
                break;
            case HookAllocationOp::Delete:
                ++Statistics.HookDeleteCount;
                break;
            case HookAllocationOp::NewArray:
                ++Statistics.HookNewArrayCount;
                break;
            case HookAllocationOp::DeleteArray:
                ++Statistics.HookDeleteArrayCount;
                break;
            case HookAllocationOp::Malloc:
                ++Statistics.HookMallocCount;
                break;
            case HookAllocationOp::Free:
                ++Statistics.HookFreeFunctionCount;
                break;
            case HookAllocationOp::Calloc:
                ++Statistics.HookCallocCount;
                break;
            case HookAllocationOp::AlignedNew:
                ++Statistics.HookAlignedNewCount;
                break;
            case HookAllocationOp::AlignedDelete:
                ++Statistics.HookAlignedDeleteCount;
                break;
            default:
                break;
        }
    }

    void recordEngineEvent(const MemoryEvent &Event)
    {
        if constexpr (!isCompiledEnabled())
        {
            (void) Event;
            return;
        }

        HookSuppressionScope Scope;
        std::lock_guard Lock(Mutex);
        ++Statistics.EngineEventCount;

        appendTimelineLocked(
            eventTypeName(Event.Type),
            "engine",
            Event.Timestamp,
            threadIdFrom(Event.ThreadId),
            reinterpret_cast<std::uintptr_t>(Event.UserPtr),
            Event.Size,
            Event.Alignment,
            Event.AllocationId,
            Event.ResourceId);

        switch (Event.Type)
        {
            case MemoryEventType::Allocate:
            {
                ++Statistics.AllocateCount;
                Statistics.TotalAllocatedBytes += Event.Size;
                Statistics.CurrentLiveBytes    += Event.Size;
                ++Statistics.CurrentLiveBlocks;
                updatePeak(Statistics.PeakLiveBytes, Statistics.CurrentLiveBytes);
                updatePeak(Statistics.PeakLiveBlocks, Statistics.CurrentLiveBlocks);

                if (Event.FromOS)
                {
                    ++Statistics.OsAllocateCount;
                }

                if (Event.UserPtr)
                {
                    auto Existing = ActiveAllocations.find(Event.UserPtr);
                    if (Existing != ActiveAllocations.end() && Existing->second.FromEngineEvent)
                    {
                        Existing->second.Size            = Event.Size;
                        Existing->second.Alignment       = Event.Alignment;
                        Existing->second.AllocationId    = Event.AllocationId;
                        Existing->second.TimestampNs     = Event.Timestamp;
                        Existing->second.ThreadId        = threadIdFrom(Event.ThreadId);
                        Existing->second.FrameIndex      = Event.FrameIndex;
                        Existing->second.ResourceId      = Event.ResourceId;
                        Existing->second.FromThreadCache = Event.FromThreadCache;
                        Existing->second.FromCentralPool = Event.FromCentralPool;
                        Existing->second.FromOS          = Event.FromOS;
                        FreedAllocations.erase(Event.UserPtr);
                        break;
                    }

                    ActiveAllocation Allocation;
                    Allocation.Operation             = HookAllocationOp::Unknown;
                    Allocation.Size                  = Event.Size;
                    Allocation.Alignment             = Event.Alignment;
                    Allocation.AllocationId          = Event.AllocationId;
                    Allocation.TimestampNs           = Event.Timestamp;
                    Allocation.ThreadId              = threadIdFrom(Event.ThreadId);
                    Allocation.FrameIndex            = Event.FrameIndex;
                    Allocation.ResourceId            = Event.ResourceId;
                    Allocation.FromEngineEvent       = true;
                    Allocation.FromThreadCache       = Event.FromThreadCache;
                    Allocation.FromCentralPool       = Event.FromCentralPool;
                    Allocation.FromOS                = Event.FromOS;
                    ActiveAllocations[Event.UserPtr] = std::move(Allocation);
                    FreedAllocations.erase(Event.UserPtr);
                }
                break;
            }

            case MemoryEventType::Deallocate:
            {
                std::size_t ReleasedSize = Event.Size;
                if (Event.UserPtr)
                {
                    auto ActiveIter = ActiveAllocations.find(Event.UserPtr);
                    if (ActiveIter != ActiveAllocations.end())
                    {
                        ReleasedSize = ActiveIter->second.Size;

                        FreedAllocation Freed;
                        Freed.Sequence        = ++NextFreedSequence;
                        Freed.FreedBy         = HookAllocationOp::Unknown;
                        Freed.Size            = ActiveIter->second.Size;
                        Freed.Alignment       = ActiveIter->second.Alignment;
                        Freed.TimestampNs     = Event.Timestamp;
                        Freed.ThreadId        = threadIdFrom(Event.ThreadId);
                        Freed.FromEngineEvent = true;
                        rememberFreedAllocationLocked(Event.UserPtr, Freed);
                        ActiveAllocations.erase(ActiveIter);
                    }
                    else
                    {
                        FreedAllocations.erase(Event.UserPtr);
                    }
                }

                ++Statistics.DeallocateCount;
                Statistics.TotalFreedBytes  += ReleasedSize;
                Statistics.CurrentLiveBytes = Statistics.CurrentLiveBytes >= ReleasedSize
                                                  ? Statistics.CurrentLiveBytes - ReleasedSize
                                                  : 0;
                if (Statistics.CurrentLiveBlocks > 0)
                {
                    --Statistics.CurrentLiveBlocks;
                }
                if (Event.FromOS)
                {
                    ++Statistics.OsReleaseCount;
                }
                break;
            }

            case MemoryEventType::ThreadCacheHit:
                ++Statistics.ThreadCacheHitCount;
                break;

            case MemoryEventType::ThreadCacheMiss:
                ++Statistics.ThreadCacheMissCount;
                break;

            case MemoryEventType::CentralFetch:
                ++Statistics.CentralFetchCount;
                break;

            case MemoryEventType::CentralReturn:
                ++Statistics.CentralReturnCount;
                break;

            case MemoryEventType::GuardCorruption:
                ++Statistics.GuardCorruptionCount;
                addIssueLocked(
                    MemoryIssueType::GuardCorruption,
                    HookAllocationOp::Unknown,
                    reinterpret_cast<std::uintptr_t>(Event.UserPtr),
                    Event.Size,
                    Event.Alignment,
                    "Guard bytes were corrupted before deallocation.",
                    {},
                    Event.Timestamp,
                    threadIdFrom(Event.ThreadId));
                break;

            case MemoryEventType::UseAfterFree:
                ++Statistics.UseAfterFreeCount;
                addIssueLocked(
                    MemoryIssueType::UseAfterFree,
                    HookAllocationOp::Unknown,
                    reinterpret_cast<std::uintptr_t>(Event.UserPtr),
                    Event.Size,
                    Event.Alignment,
                    "Use-after-free detected from allocator quarantine validation.",
                    {},
                    Event.Timestamp,
                    threadIdFrom(Event.ThreadId));
                break;

            case MemoryEventType::ValidationError:
                ++Statistics.ValidationErrorCount;
                addIssueLocked(
                    MemoryIssueType::ValidationError,
                    HookAllocationOp::Unknown,
                    reinterpret_cast<std::uintptr_t>(Event.UserPtr),
                    Event.Size,
                    Event.Alignment,
                    "Allocator validation failed for request/deallocation metadata.",
                    {},
                    Event.Timestamp,
                    threadIdFrom(Event.ThreadId));
                break;

            default:
                break;
        }
    }

    void recordHookAllocation(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(Mutex);
        if (!HookEnabled.load(std::memory_order_acquire))
        {
            return;
        }

        const uint64_t Now = mem::getTimestampNs();
        const std::uint32_t Tid = currentThreadId();

        ++Statistics.HookEventCount;
        ++Statistics.HookAllocationCount;
        updateHookOperationCountersLocked(Operation);

        appendTimelineLocked(
            hookOpName(Operation),
            "hook",
            Now,
            Tid,
            reinterpret_cast<std::uintptr_t>(Ptr),
            Size,
            Alignment,
            0,
            0);

        if (!Ptr)
        {
            ++Statistics.HookAllocationFailureCount;
            addIssueLocked(
                MemoryIssueType::AllocationFailure,
                Operation,
                reinterpret_cast<std::uintptr_t>(Caller),
                Size,
                Alignment,
                "Hook allocation returned null pointer.",
                {},
                Now,
                Tid);
            return;
        }

        auto Existing = ActiveAllocations.find(Ptr);
        if (Existing != ActiveAllocations.end())
        {
            if (Existing->second.FromEngineEvent)
            {
                if (Existing->second.Operation == HookAllocationOp::Unknown)
                {
                    Existing->second.Operation = Operation;
                }
                FreedAllocations.erase(Ptr);
                (void) Caller;
                return;
            }

            ++Statistics.DoubleAllocationCount;
            addIssueLocked(
                MemoryIssueType::DoubleAllocation,
                Operation,
                reinterpret_cast<std::uintptr_t>(Ptr),
                Size,
                Alignment,
                "Pointer was allocated again before being released.",
                {},
                Now,
                Tid);

            Statistics.CurrentLiveBytes = Statistics.CurrentLiveBytes >= Existing->second.Size
                                              ? Statistics.CurrentLiveBytes - Existing->second.Size
                                              : 0;
        }
        else
        {
            ++Statistics.CurrentLiveBlocks;
            updatePeak(Statistics.PeakLiveBlocks, Statistics.CurrentLiveBlocks);
        }

        Statistics.TotalAllocatedBytes += Size;
        Statistics.CurrentLiveBytes    += Size;
        updatePeak(Statistics.PeakLiveBytes, Statistics.CurrentLiveBytes);

        ActiveAllocation Allocation;
        Allocation.Operation       = Operation;
        Allocation.Size            = Size;
        Allocation.Alignment       = Alignment;
        Allocation.AllocationId    = 0;
        Allocation.TimestampNs     = Now;
        Allocation.ThreadId        = Tid;
        Allocation.FrameIndex      = 0;
        Allocation.ResourceId      = 0;
        Allocation.FromEngineEvent = false;
        ActiveAllocations[Ptr]     = std::move(Allocation);
        FreedAllocations.erase(Ptr);
        (void) Caller;
    }

    void recordHookFree(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(Mutex);
        if (!HookEnabled.load(std::memory_order_acquire))
        {
            return;
        }

        const uint64_t Now = mem::getTimestampNs();
        const std::uint32_t Tid = currentThreadId();

        ++Statistics.HookEventCount;
        ++Statistics.HookFreeCount;
        updateHookOperationCountersLocked(Operation);

        appendTimelineLocked(
            hookOpName(Operation),
            "hook",
            Now,
            Tid,
            reinterpret_cast<std::uintptr_t>(Ptr),
            Size,
            Alignment,
            0,
            0);

        recordHookFreeNoLock(Operation, Ptr, Size, Alignment, Caller, Now, Tid);
    }

    void recordHookReallocate(void *OldPtr, void *NewPtr, const std::size_t NewSize, void *Caller)
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(Mutex);
        if (!HookEnabled.load(std::memory_order_acquire))
        {
            return;
        }

        const uint64_t Now = mem::getTimestampNs();
        const std::uint32_t Tid = currentThreadId();

        ++Statistics.HookEventCount;
        ++Statistics.HookReallocCount;
        updateHookOperationCountersLocked(HookAllocationOp::Realloc);

        appendTimelineLocked(
            hookOpName(HookAllocationOp::Realloc),
            "hook",
            Now,
            Tid,
            reinterpret_cast<std::uintptr_t>(NewPtr),
            NewSize,
            MemoryTrackerUnknownSize,
            0,
            0);

        if (OldPtr == nullptr)
        {
            if (NewPtr)
            {
                recordHookAllocationNoLock(HookAllocationOp::Realloc, NewPtr, NewSize, MemoryTrackerUnknownSize);
            }
            else
            {
                ++Statistics.HookAllocationFailureCount;
                addIssueLocked(
                    MemoryIssueType::ReallocFailure,
                    HookAllocationOp::Realloc,
                    reinterpret_cast<std::uintptr_t>(Caller),
                    NewSize,
                    MemoryTrackerUnknownSize,
                    "realloc(nullptr, size) failed to allocate memory.",
                    {},
                    Now,
                    Tid);
            }
            return;
        }

        if (NewPtr == nullptr)
        {
            ++Statistics.HookAllocationFailureCount;
            addIssueLocked(
                MemoryIssueType::ReallocFailure,
                HookAllocationOp::Realloc,
                reinterpret_cast<std::uintptr_t>(OldPtr),
                NewSize,
                MemoryTrackerUnknownSize,
                "realloc failed and old allocation remains active.",
                {},
                Now,
                Tid);
            return;
        }

        if (OldPtr == NewPtr)
        {
            auto Iter = ActiveAllocations.find(OldPtr);
            if (Iter == ActiveAllocations.end())
            {
                ++Statistics.InvalidFreeCount;
                addIssueLocked(
                    MemoryIssueType::InvalidFree,
                    HookAllocationOp::Realloc,
                    reinterpret_cast<std::uintptr_t>(OldPtr),
                    NewSize,
                    MemoryTrackerUnknownSize,
                    "realloc in-place was called on unknown pointer.",
                    {},
                    Now,
                    Tid);
                return;
            }

            if (Iter->second.FromEngineEvent)
            {
                return;
            }

            const std::size_t OldSize = Iter->second.Size;
            Iter->second.Operation    = HookAllocationOp::Realloc;
            Iter->second.Size         = NewSize;
            Iter->second.TimestampNs  = Now;
            Iter->second.ThreadId     = Tid;

            if (NewSize >= OldSize)
            {
                const std::size_t Delta        = NewSize - OldSize;
                Statistics.TotalAllocatedBytes += Delta;
                Statistics.CurrentLiveBytes    += Delta;
                updatePeak(Statistics.PeakLiveBytes, Statistics.CurrentLiveBytes);
            }
            else
            {
                const std::size_t Delta     = OldSize - NewSize;
                Statistics.TotalFreedBytes  += Delta;
                Statistics.CurrentLiveBytes = Statistics.CurrentLiveBytes >= Delta
                                                  ? Statistics.CurrentLiveBytes - Delta
                                                  : 0;
            }

            return;
        }

        recordHookFreeNoLock(
            HookAllocationOp::Free,
            OldPtr,
            MemoryTrackerUnknownSize,
            MemoryTrackerUnknownSize,
            Caller,
            Now,
            Tid);
        recordHookAllocationNoLock(HookAllocationOp::Realloc, NewPtr, NewSize, MemoryTrackerUnknownSize);
    }

    void recordHookAllocationFailure(
        const HookAllocationOp Operation,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller)
    {
        HookSuppressionScope Scope;
        std::lock_guard Lock(Mutex);
        if (!HookEnabled.load(std::memory_order_acquire))
        {
            return;
        }

        const uint64_t Now = mem::getTimestampNs();
        const std::uint32_t Tid = currentThreadId();
        ++Statistics.HookEventCount;
        ++Statistics.HookAllocationFailureCount;
        appendTimelineLocked(
            hookOpName(Operation),
            "hook",
            Now,
            Tid,
            reinterpret_cast<std::uintptr_t>(Caller),
            Size,
            Alignment,
            0,
            0);
        addIssueLocked(
            MemoryIssueType::AllocationFailure,
            Operation,
            reinterpret_cast<std::uintptr_t>(Caller),
            Size,
            Alignment,
            "Hook allocation reported failure.",
            {},
            Now,
            Tid);
    }

    void recordHookAllocationNoLock(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment)
    {
        if (!Ptr)
        {
            ++Statistics.HookAllocationFailureCount;
            return;
        }

        auto Existing = ActiveAllocations.find(Ptr);
        if (Existing != ActiveAllocations.end())
        {
            if (Existing->second.FromEngineEvent)
            {
                if (Existing->second.Operation == HookAllocationOp::Unknown)
                {
                    Existing->second.Operation = Operation;
                }
                FreedAllocations.erase(Ptr);
                return;
            }

            ++Statistics.DoubleAllocationCount;
            addIssueLocked(
                MemoryIssueType::DoubleAllocation,
                Operation,
                reinterpret_cast<std::uintptr_t>(Ptr),
                Size,
                Alignment,
                "Pointer was allocated again before being released.",
                {},
                mem::getTimestampNs(),
                currentThreadId());
            Statistics.CurrentLiveBytes = Statistics.CurrentLiveBytes >= Existing->second.Size
                                              ? Statistics.CurrentLiveBytes - Existing->second.Size
                                              : 0;
        }
        else
        {
            ++Statistics.CurrentLiveBlocks;
            updatePeak(Statistics.PeakLiveBlocks, Statistics.CurrentLiveBlocks);
        }

        Statistics.TotalAllocatedBytes += Size;
        Statistics.CurrentLiveBytes    += Size;
        updatePeak(Statistics.PeakLiveBytes, Statistics.CurrentLiveBytes);

        ActiveAllocation Allocation;
        Allocation.Operation       = Operation;
        Allocation.Size            = Size;
        Allocation.Alignment       = Alignment;
        Allocation.AllocationId    = 0;
        Allocation.TimestampNs     = mem::getTimestampNs();
        Allocation.ThreadId        = currentThreadId();
        Allocation.FromEngineEvent = false;
        ActiveAllocations[Ptr]     = std::move(Allocation);
        FreedAllocations.erase(Ptr);
    }

    void recordHookFreeNoLock(
        const HookAllocationOp Operation,
        void *Ptr,
        const std::size_t Size,
        const std::size_t Alignment,
        void *Caller,
        const uint64_t TimestampNs,
        const std::uint32_t ThreadId)
    {
        if (!Ptr)
        {
            ++Statistics.HookNullFreeCount;
            return;
        }

        auto ActiveIter = ActiveAllocations.find(Ptr);
        if (ActiveIter == ActiveAllocations.end())
        {
            auto FreedIter = FreedAllocations.find(Ptr);
            if (FreedIter != FreedAllocations.end())
            {
                if (FreedIter->second.FromEngineEvent)
                {
                    FreedIter->second.FromEngineEvent = false;
                    return;
                }

                ++Statistics.DoubleFreeCount;
                addIssueLocked(
                    MemoryIssueType::DoubleFree,
                    Operation,
                    reinterpret_cast<std::uintptr_t>(Ptr),
                    FreedIter->second.Size,
                    FreedIter->second.Alignment,
                    "Double free detected on a previously released pointer.",
                    FreedIter->second.Payload,
                    TimestampNs,
                    ThreadId);
            }
            else
            {
                ++Statistics.InvalidFreeCount;
                addIssueLocked(
                    MemoryIssueType::InvalidFree,
                    Operation,
                    reinterpret_cast<std::uintptr_t>(Ptr),
                    Size,
                    Alignment,
                    "Free/delete called for an unknown pointer.",
                    {},
                    TimestampNs,
                    ThreadId);
            }
            return;
        }

        const ActiveAllocation Active = ActiveIter->second;
        if (Active.FromEngineEvent)
        {
            return;
        }

        const HookAllocationOp Expected = expectedFreeOperation(Active.Operation);
        if (Expected != HookAllocationOp::Unknown && Expected != Operation)
        {
            ++Statistics.MismatchedFreeFunctionCount;
            addIssueLocked(
                MemoryIssueType::MismatchedFreeFunction,
                Operation,
                reinterpret_cast<std::uintptr_t>(Ptr),
                Active.Size,
                Active.Alignment,
                "Mismatched deallocation function for pointer.",
                {},
                TimestampNs,
                ThreadId);
        }

        if (Size != MemoryTrackerUnknownSize && Active.Size != 0 && Size != Active.Size)
        {
            ++Statistics.SizeMismatchCount;
            addIssueLocked(
                MemoryIssueType::SizeMismatch,
                Operation,
                reinterpret_cast<std::uintptr_t>(Ptr),
                Size,
                Active.Alignment,
                "Sized delete/free size does not match allocation size.",
                {},
                TimestampNs,
                ThreadId);
        }

        if (Alignment != MemoryTrackerUnknownSize &&
            Active.Alignment != MemoryTrackerUnknownSize &&
            Alignment != Active.Alignment)
        {
            ++Statistics.AlignmentMismatchCount;
            addIssueLocked(
                MemoryIssueType::AlignmentMismatch,
                Operation,
                reinterpret_cast<std::uintptr_t>(Ptr),
                Active.Size,
                Alignment,
                "Aligned delete/free alignment does not match allocation alignment.",
                {},
                TimestampNs,
                ThreadId);
        }

        MemoryPayloadPreview Payload = capturePayloadPreview(Ptr, Active.Size);
        Statistics.TotalFreedBytes   += Active.Size;
        Statistics.CurrentLiveBytes  = Statistics.CurrentLiveBytes >= Active.Size
                                           ? Statistics.CurrentLiveBytes - Active.Size
                                           : 0;
        if (Statistics.CurrentLiveBlocks > 0)
        {
            --Statistics.CurrentLiveBlocks;
        }

        FreedAllocation Freed;
        Freed.Sequence        = ++NextFreedSequence;
        Freed.FreedBy         = Operation;
        Freed.Size            = Active.Size;
        Freed.Alignment       = Active.Alignment;
        Freed.TimestampNs     = TimestampNs;
        Freed.ThreadId        = ThreadId;
        Freed.FromEngineEvent = false;
        Freed.Payload         = Payload;
        rememberFreedAllocationLocked(Ptr, Freed);

        ActiveAllocations.erase(ActiveIter);
        (void) Caller;
    }

    mutable std::mutex Mutex;
    std::unordered_map<void *, ActiveAllocation> ActiveAllocations;
    std::unordered_map<void *, FreedAllocation> FreedAllocations;
    std::deque<FreedOrderEntry> FreedOrder;
    std::deque<TraceRecord> Timeline;
    std::vector<MemoryIssueRecord> Issues;
    MemoryTrackerStatistics Statistics;

    std::size_t PayloadPreviewBytes = 32;
    std::size_t FreedHistoryLimit   = 8192;
    std::size_t MaxTimelineEntries  = MemoryTrackerDefaultMaxTimelineEntries;
    uint64_t NextFreedSequence = 0;
    std::atomic<bool> HookEnabled{false};
};
} // namespace infra

#endif //STABLEARRAY_MEMORYVISIBILITY_H
