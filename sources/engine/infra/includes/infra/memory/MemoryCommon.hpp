//
// Created by Lzorn on 2026/9/22.
//

#ifndef STABLEARRAY_MEMORYCOMMON_H
#define STABLEARRAY_MEMORYCOMMON_H
#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <memory>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <system_error>
#include <infra/common/Platform.hpp>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#   include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#   include <immintrin.h>
#elif defined(__aarch64__) || defined(__arm__)
#   include <arm_acle.h>
#else
#endif

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

// 通过编译器注册宏来导入编译期设置:
#ifndef CORE_MEM_ENABLE_LAZY_COMMIT
#define CORE_MEM_ENABLE_LAZY_COMMIT 1
#endif

#ifndef CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING
#define CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING 0
#endif

#ifndef CORE_MEM_ENABLE_DEBUG_GUARDS
#define CORE_MEM_ENABLE_DEBUG_GUARDS 0
#endif

#ifndef CORE_MEM_ENABLE_UAF_DETECTION
#define CORE_MEM_ENABLE_UAF_DETECTION 0
#endif

#ifndef CORE_MEM_CAPTURE_STACK
#define CORE_MEM_CAPTURE_STACK 0
#endif

#ifndef CORE_MEM_QUARANTINE_RELEASE_ONLY_ON_FLUSH
#define CORE_MEM_QUARANTINE_RELEASE_ONLY_ON_FLUSH 0
#endif


inline constexpr bool MemoryCompileEnableDebugGuards = CORE_MEM_ENABLE_DEBUG_GUARDS != 0;
inline constexpr bool MemoryCompileEnableUseAfterFreeDetection = CORE_MEM_ENABLE_UAF_DETECTION != 0;
inline constexpr bool MemoryCompileCaptureStack = CORE_MEM_CAPTURE_STACK != 0;
inline constexpr bool MemoryCompileQuarantineReleaseOnlyOnFlush =
    CORE_MEM_QUARANTINE_RELEASE_ONLY_ON_FLUSH != 0;
inline constexpr bool MemoryCompileLazyCommit = CORE_MEM_ENABLE_LAZY_COMMIT != 0;
inline constexpr bool MemoryCompileEnableInternalBenchTiming =
    CORE_MEM_ENABLE_INTERNAL_BENCH_TIMING != 0;

inline constexpr uint8_t AllocatedPattern = 0xCD;
inline constexpr uint8_t FreedPattern = 0xDD;
inline constexpr uint8_t FrontGuardPattern = 0xCD;
inline constexpr uint8_t BackGuardPattern = 0xFD;
inline constexpr uint32_t DebugHeaderMagic = 0x504D454D; // "PMEM"
inline constexpr uint16_t InvalidSizeClass = std::numeric_limits<uint16_t>::max();

namespace infra
{
inline constexpr uint64_t SmallRegionReserveBytes              = 64 * 1024 * 1024;
// 页分配器分片上限
inline constexpr uint64_t PageAllocatorMaxShardCount           = 64;
// 页分配器分片最小值
inline constexpr size_t PageAllocatorMinShardCount             = 8;
// decommit 采样掩码, 0x7 为 1/8 采样
inline constexpr uint32_t PageAllocatorDecommitSampleMask = 0x7;
// 页分配器分片相对硬件线程数的缩放倍数。
inline constexpr size_t PageAllocatorShardScale                = 2;
// decommit 的最小阈值字节。
inline constexpr size_t PageAllocatorDecommitMinBytes          = 256u * 1024u;
// decommit 动态阈值分母：SmallRegionReserveBytes / 128。
inline constexpr size_t PageAllocatorDecommitRegionDivisor     = 128;
// decommit 的最小页数阈值。
inline constexpr size_t PageAllocatorDecommitMinPages          = 64;
// FreeSpanCache 桶数量（压缩映射）。
inline constexpr size_t PageAllocatorCacheBucketCount          = 64;
// 进入压缩桶缓存的最大页数，超过该值进入 oversized 回退列表。
inline constexpr size_t PageAllocatorCachedMaxPageCount        = 1024;

struct AllocatorConfig
{
    size_t ThreadCacheMaxBytes                   = 16u * 1024u * 1024u;
    uint16_t ThreadCacheMaxPerClass         = 1024;
    uint16_t RefillBatchSize                = 64;
    size_t SmallRegionReserveBytes               = 64u * 1024u * 1024u;
    size_t DedicatedRegionReserveBytes           = 2u * 1024u * 1024u;
    bool EnableHugePage                          = false;
    bool EnableDedicatedRegionCache              = true;
    size_t DedicatedRegionCacheLimitBytes        = 128u * 1024u * 1024u;
    size_t DedicatedRegionCacheMaxEntriesPerSize = 8;
    size_t UseAfterFreeQuarantineBytes           = 512u * 1024u;
    size_t UseAfterFreeQuarantineMaxEntries      = 8192;
    int32_t PreferredNumaNode                    = -1;
};


[[nodiscard]] constexpr size_t alignUp(size_t Value, size_t Alignment) noexcept
{
    return (Value + Alignment - 1) & ~(Alignment - 1);
}

[[nodiscard]] constexpr bool isPowerOfTwo(size_t Value) noexcept
{
    return Value != 0 && (Value & (Value - 1)) == 0;
}

// @note: prefetch 在真正访问某块内存之前, 提前告诉 CPU 把对应缓存行加载到缓存中, 以减少后续读/写时的 cache miss 延迟
inline void prefetchRead(const void *Ptr) noexcept
{
    if (!Ptr)
    {
        return;
    }

#if (defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__))
    _mm_prefetch(reinterpret_cast<const char *>(Ptr), _MM_HINT_T0);
#elif defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(Ptr, 0, 3);
#endif
}

inline void prefetchWrite(const void *Ptr) noexcept
{
    if (!Ptr)
    {
        return;
    }

#if defined(__GNUC__) || defined(__clang__)
    __builtin_prefetch(Ptr, 1, 3);
#else
    prefetchRead(Ptr);
#endif
}

inline void relax() noexcept
{
#if (defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__))
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    __yield();
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

/**
 * @note: TTAS Test-Test-And-Set:
 * 朴素的 TAS 问题:
 *   while (Locked.test_and_set(std::memory_order_acquire)) {
 *       // 空转
 *   }
 *
 * test_and_set 是一种 RMW(read-modify-write), 即要拿到缓存行的独占权.
 * 当多个核同时循环时, 这个锁变量所在的缓存行会在各个核的 L1/L2 之间来回弹跳,
 * 产生大量 cache coherence 流量, 即 cache line ping-pong. 而临界区越短, 这样的弹跳则越亏
 *
 * TTAS 则是解决上述问题的一种算法: 先只读地 test, 发现可能空闲了, 再 test_and_set
 *
 *   void lock() {
 *       // 快路径：直接抢一次
 *       if (!Locked.test_and_set(std::memory_order_acquire)) {
 *           return;
 *       }
 *
 *      // 慢路径：先只读轮询
 *       while (Locked.test(std::memory_order_relaxed)) {
 *           cpuRelax();   // pause / yield
 *       }
 *
 *       // 看到可能空闲, 再尝试原子抢
 *       while (Locked.test_and_set(std::memory_order_acquire)) {
 *           // 失败后退避
 *           while (Locked.test(std::memory_order_relaxed)) {
 *               cpuRelax();
 *           }
 *      }
 *   }
 *
 * 而 TTAS 这种方法并非公平抢占, 可能出现饥饿问题. 如果竞争线程真的很多的情况下, 当锁释放时,
 * 其同时被多个竞争者同时看到, 然后他们同时 test_and_set 缓存行在此弹跳.
 *
 * 所以 TTAS 适合竞争频率不激烈, 临界区短的情况. 这也是以下 SpanLock 的实现方式.
 *
 * @note: MAC 锁: MCS 来自 Mellor-Crummey 和 Scott 的论文, 其是一个队列自旋锁
 * 其核心思路是不让所有人盯着同一个锁变量, 而是让每个等待线程在自己的本地节点上自旋。前驱释放锁时, 只写后继的节点, 唤醒它
 * 但是在内存分配器中, 相对更适合使用 TTAS, 临界区短, 竞争不算激烈的场景
 */
// 自旋锁初始暂停次数, 用于控制轻度竞争时忙等成本
inline constexpr uint32_t SpinLockInitialPauseCount = 4;
// 暂停次数上限, 超过之后让出时间片避免空转
inline constexpr uint32_t SpinLockMaxPauseCount     = 1024;
// 连续几轮之后吃满 relax 之后才 yield
inline constexpr uint32_t SpinLockYieldThreshold    = 4;

#if defined(__linux__) && defined(__GLIBC__)
struct SpinLock
{
public:
    SpinLock() noexcept
    {
        pthread_spin_init(&Locked, PTHREAD_PROCESS_PRIVATE);
    }

    ~SpinLock() noexcept
    {
        pthread_spin_destroy(&Locked);
    }

    SpinLock(const SpinLock &) noexcept = delete;

    SpinLock &operator=(const SpinLock &) noexcept = delete;

    void lock() noexcept
    {
        pthread_spin_lock(&Locked);
    }

    void unlock() noexcept
    {
        pthread_spin_unlock(&Locked);
    }

    [[nodiscard]] bool tryLock() noexcept
    {
        return pthread_spin_trylock(&Locked) == 0;
    }

private:
    alignas(64) pthread_spinlock_t Locked = {};
};
#else
struct alignas(64) SpinLock
{
    void lock() noexcept
    {
        // 快路径: 一次 RWM 直接拿到
        if (!Locked.test_and_set(std::memory_order_acquire))
        {
            return;
        }

        // 慢路径: TTAS + 指数退避
        uint32_t FailRounds  = 0;
        uint32_t PauseBudget = SpinLockInitialPauseCount;
        for (;;)
        {
            // 只读轮询, 避免 RMW 造成的缓存行弹跳
            while (Locked.test(std::memory_order_relaxed))
            {
                for (uint32_t i = 0; i < PauseBudget; ++i)
                {
                    relax();
                }
                if (PauseBudget < SpinLockMaxPauseCount)
                {
                    PauseBudget <<= 1;
                }
            }

            // 观察到可能空闲, 做一次 RMW 尝试
            if (!Locked.test_and_set(std::memory_order_acquire))
            {
                return;
            }

            // 又失败：继续退避, 极端情况下才让出时间片
            if (PauseBudget >= SpinLockMaxPauseCount)
            {
                if (++FailRounds >= SpinLockYieldThreshold)
                {
                    std::this_thread::yield();
                    FailRounds = 0;
                }
            }
        }
    }

    void unlock() noexcept
    {
        Locked.clear();
    }

    [[nodiscard]] bool tryLock() noexcept
    {
        return !Locked.test_and_set();
    }

private:
    std::atomic_flag Locked = ATOMIC_FLAG_INIT;
};
#endif


/**
 * @brief 读写自旋锁：读者并发、写者独占。
 * @note libc++ 的 std::shared_mutex 在部分平台上退化为普通 mutex（读者互斥），
 *       本实现用单个原子计数器实现真正的"读者共享"语义，适合页映射这类读多写少场景。
 */
struct alignas(64) RwSpinLock
{
    static constexpr std::uint32_t WriterBit  = 1u << 31;
    static constexpr std::uint32_t ReaderMask = WriterBit - 1;

    void lock_shared() noexcept
    {
        for (;;)
        {
            std::uint32_t S = State.load(std::memory_order_relaxed);
            if ((S & WriterBit) == 0 && S < ReaderMask)
            {
                if (State.compare_exchange_weak(
                        S, S + 1, std::memory_order_acquire, std::memory_order_relaxed))
                {
                    return;
                }
            }
            else
            {
                relax();
            }
        }
    }

    void unlock_shared() noexcept
    {
        State.fetch_sub(1, std::memory_order_release);
    }

    [[nodiscard]] bool try_lock_shared() noexcept
    {
        std::uint32_t S = State.load(std::memory_order_relaxed);
        if ((S & WriterBit) == 0 && S < ReaderMask)
        {
            return State.compare_exchange_weak(
                S, S + 1, std::memory_order_acquire, std::memory_order_relaxed);
        }
        return false;
    }

    void lock() noexcept
    {
        // 1) 抢占 writer 位（仅当无读者、无写者时）
        for (;;)
        {
            std::uint32_t Expected = 0;
            if (State.compare_exchange_weak(
                    Expected, WriterBit, std::memory_order_acquire, std::memory_order_relaxed))
            {
                break;
            }
            relax();
        }

        // 2) 等待在位的读者退出
        while ((State.load(std::memory_order_acquire) & ReaderMask) != 0)
        {
            relax();
        }
    }

    void unlock() noexcept
    {
        State.store(0, std::memory_order_release);
    }

    [[nodiscard]] bool try_lock() noexcept
    {
        std::uint32_t Expected = 0;
        return State.compare_exchange_weak(
            Expected, WriterBit, std::memory_order_acquire, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint32_t> State{0};
};

enum class AllocationStage : uint16_t
{
    Prepare    = 0,
    Validate   = 1,
    Layout     = 2,
    Allocate   = 3,
    Initialize = 4,
    Notify     = 5,
    Return     = 6,
    Free       = 7,
    Destroy    = 8,
};

enum class MemoryEventType : uint8_t
{
    Allocate,
    Deallocate,
    Reallocate,
    Reserve,
    Commit,
    Decommit,
    Release,
    ThreadCacheHit,
    ThreadCacheMiss,
    CentralFetch,
    CentralReturn,
    PageAllocate,
    PageFree,
    GuardCorruption,
    UseAfterFree,
    ValidationError,
};

enum class AllocationLifetime : uint8_t
{
    Default,
    Transient,
    Persistent,
};

struct AllocationDescriptor
{
    size_t Size         = 0;
    size_t Alignment    = alignof(std::max_align_t);
    bool IsZeroMemory        = false;
    bool IsNoThrow           = false;
    uint32_t FrameIndex = 0;
    uint32_t ResourceId = 0;
};

struct MemoryPrewarmRequest
{
    size_t Size            = 0;
    size_t Alignment       = alignof(std::max_align_t);
    uint32_t Count         = 0;
    uint32_t ResourceId    = 0;
    bool ZeroInitialize         = false;
    AllocationLifetime Lifetime = AllocationLifetime::Transient;
};

struct AllocationLayout
{
    size_t RequestedSize      = 0;
    size_t RequestedAlignment = alignof(std::max_align_t);

    size_t TotalSize        = 0;
    size_t HeaderSize       = 0;
    size_t FrontGuard       = 0;
    size_t BackGuard        = 0;
    size_t UserOffset       = 0;
    size_t BlockSize        = 0;
    uint16_t SizeClassIndex = InvalidSizeClass;
    bool IsSmallAllocation       = false;
};

struct AllocationMetadata
{
    std::uint64_t AllocationId = 0;
    uint32_t StackId      = 0;
    std::thread::id ThreadId{};
    uint32_t ResourceId = 0;
    uint32_t ArenaId    = 0;
    std::uint64_t Timestamp  = 0;
    uint32_t FrameIndex = 0;
};

struct AllocationRuntime
{
    std::byte *RawPtr      = nullptr;
    std::byte *UserPtr     = nullptr;
    void *Owner            = nullptr;
    void *InternalHandle   = nullptr;
    bool IsFromThreadCache = false;
    bool IsFromCentralPool = false;
    bool IsFromOS          = false;
};

struct AllocationResult
{
    bool IsSuccess = false;
    std::error_code ErrorCode;
};

struct AllocationContext
{
    AllocationDescriptor Descriptor;
    AllocationLayout Layout;
    AllocationMetadata Metadata;
    AllocationRuntime Runtime;
    AllocationResult Result;
    AllocationStage Stage = AllocationStage::Prepare;

    constexpr void reset() noexcept
    {
        Descriptor = {};
        Layout     = {};
        Metadata   = {};
        Runtime    = {};
        Result     = {};
        Stage      = AllocationStage::Prepare;
    }
};

struct DebugHeader
{
    uint32_t Magic              = 0;
    uint32_t Version            = 1;
    std::uint64_t AllocationId       = 0;
    std::uint64_t RequestedSize      = 0;
    uint32_t RequestedAlignment = 0;
    uint16_t SizeClassIndex     = InvalidSizeClass;
    uint16_t Reserved           = 0;
    uint32_t FrontGuardBytes    = 0;
    uint32_t BackGuardBytes     = 0;
};

template<typename Stage>
concept CAllocationStage = requires(AllocationContext &Context)
{
    { Stage::allocate(Context) } -> std::same_as<void>;
    { Stage::deallocate(Context) } -> std::same_as<void>;
};

struct MemoryEvent
{
    MemoryEventType Type       = MemoryEventType::Allocate;
    void *UserPtr              = nullptr;
    size_t Size           = 0;
    size_t Alignment      = 0;
    std::uint64_t AllocationId = 0;
    std::uint64_t Timestamp    = 0;
    std::thread::id ThreadId{};
    uint32_t FrameIndex         = 0;
    uint32_t ResourceId         = 0;
    bool FromThreadCache             = false;
    bool FromCentralPool             = false;
    bool FromOS                      = false;
    const AllocationContext *Context = nullptr;
};

template<typename Ty>
concept CMemoryObserver = requires(Ty &Observer, const MemoryEvent &Event)
{
    { Observer.onMemoryEvent(Event) } -> std::same_as<void>;
};

struct ObserverEntry
{
    void *Object = nullptr;

    void (*Callback)(void *, const MemoryEvent &) = nullptr;
};

class MemoryEventBus
{
public:
    MemoryEventBus()
        : Snapshot(std::make_shared<const std::vector<ObserverEntry>>())
    {
    }

    template<CMemoryObserver ObserverType>
    void bind(ObserverType &Observer)
    {
        ObserverEntry Entry;
        Entry.Object   = &Observer;
        Entry.Callback = [](void *object, const MemoryEvent &event)
        {
            static_cast<ObserverType *>(object)->onMemoryEvent(event);
        };

        std::lock_guard Lock(MutationMutex);
        auto Next = std::make_shared<std::vector<ObserverEntry> >(*std::atomic_load(&Snapshot));
        Next->push_back(Entry);
        std::atomic_store(&Snapshot, std::shared_ptr<const std::vector<ObserverEntry>>(Next));
        HasObservers.store(!Next->empty(), std::memory_order_relaxed);
    }

    template<CMemoryObserver ObserverType>
    void unbind(ObserverType &Observer)
    {
        std::lock_guard Lock(MutationMutex);
        auto Next = std::make_shared<std::vector<ObserverEntry> >(*std::atomic_load(&Snapshot));
        Next->erase(
            std::remove_if(Next->begin(), Next->end(), [&](const ObserverEntry &Entry)
            {
                return Entry.Object == &Observer;
            }),
            Next->end());
        std::atomic_store(&Snapshot, std::shared_ptr<const std::vector<ObserverEntry>>(Next));
        HasObservers.store(!Next->empty(), std::memory_order_relaxed);
    }

    /**
     * @brief 快速判断是否存在已绑定的观测器。
     * @note 热路径（每次 allocate/deallocate）会调用此接口，因此只做一次 relaxed
     *       原子读，避免对 shared_ptr 快照做 atomic_load 触发的全局自旋锁竞争。
     */
    [[nodiscard]] bool hasObservers() const noexcept
    {
        return HasObservers.load(std::memory_order_relaxed);
    }

    void emit(const MemoryEvent &Event) const
    {
        if (!HasObservers.load(std::memory_order_relaxed))
        {
            return;
        }

        const auto RetSnapshot = std::atomic_load(&Snapshot);
        if (!RetSnapshot)
        {
            return;
        }

        for (const ObserverEntry &Entry: *RetSnapshot)
        {
            Entry.Callback(Entry.Object, Event);
        }
    }

private:
    std::mutex MutationMutex;
    std::shared_ptr<const std::vector<ObserverEntry>> Snapshot;
    std::atomic<bool> HasObservers{false};
};
}


#endif // STABLEARRAY_MEMORYCOMMON_H
