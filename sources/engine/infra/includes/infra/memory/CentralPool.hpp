//
// Created by Lzorn on 2026/9/24.
//

#ifndef STABLEARRAY_CENTRALPOOL_H
#define STABLEARRAY_CENTRALPOOL_H

#include <array>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <infra/memory/MemoryCommon.hpp>
#include <infra/memory/MemSpan.hpp>
#include <infra/memory/VirtualMemoryBackend.hpp>
#include <infra/memory/MemoryVisibility.hpp>
#include <infra/memory/PageAllocator.hpp>

namespace infra::mem
{
inline constexpr std::array<uint32_t, 56> SizeClasses = {
    8, 16, 24, 32, 40, 48, 56, 64,
    80, 96, 112, 128, 160, 192, 224, 256,
    320, 384, 448, 512, 640, 768, 896, 1024,
    1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384,
    20480, 24576, 28672, 32768, 40960, 49152, 57344, 65536,
    81920, 98304, 114688, 131072, 163840, 196608, 229376, 262144
};
inline constexpr size_t NumSizeClasses     = SizeClasses.size();
inline constexpr size_t MaxSmallAllocation = SizeClasses.back();
inline constexpr size_t ClassQuantum       = 8;
static_assert((ClassQuantum & (ClassQuantum - 1)) == 0, "ClassQuantum must be power-of-two");
inline constexpr size_t ClassQuantumShift = []
{
    size_t Shift = 0;
    size_t Value = ClassQuantum;
    while (Value > 1)
    {
        Value >>= 1;
        ++Shift;
    }
    return Shift;
}();

// 中央池每个 size-class 的最大分片数（按硬件并发自适应，不超过该上限）。
inline constexpr size_t CentralPoolBucketMaxShardCount = 64;
// 中央池 page->span 映射分片数，降低注册/查询的锁竞争。
inline constexpr size_t CentralPoolPageMapShardCount = 64;
// returnBatch 批处理分组上限，避免热路径动态分配。
inline constexpr size_t CentralPoolMaxReturnBatchGroups = 8;
// 每个分片最多保留空 span 数，超过后归还给页分配器。
inline constexpr size_t CentralPoolMaxRetainedEmptySpansPerClass = 2;
// span 元数据对象池每次扩容块大小。
inline constexpr size_t CentralPoolSpanPoolChunkSize = 256;
// 中央池分片数的最小值。
inline constexpr size_t CentralPoolMinShardCount = 8;
// 中央池分片数相对硬件线程数的缩放倍数。
inline constexpr size_t CentralPoolShardScale = 2;
// page-base 分片哈希右移位数（4KB 页对齐）。
inline constexpr size_t CentralPoolPageShift = 12;

inline constexpr size_t ClassLookupCount               = (MaxSmallAllocation / ClassQuantum) + 1;
inline constexpr size_t MaxRetainedEmptySpansPerClass  = CentralPoolMaxRetainedEmptySpansPerClass;
inline constexpr size_t MaxReturnBatchGroups           = CentralPoolMaxReturnBatchGroups;

inline std::array<uint16_t, ClassLookupCount> buildSizeClassLookup()
{
    std::array<uint16_t, ClassLookupCount> lookup{};

    for (size_t Bucket = 0; Bucket < lookup.size(); ++Bucket)
    {
        const size_t Size    = Bucket * ClassQuantum;
        uint16_t class_index = 0;

        while (class_index + 1 < NumSizeClasses && SizeClasses[class_index] < Size)
        {
            ++class_index;
        }

        lookup[Bucket] = class_index;
    }

    return lookup;
}

inline const std::array<uint16_t, ClassLookupCount> SizeClassLookup = buildSizeClassLookup();

[[nodiscard]] inline uint16_t sizeToClassIndex(const size_t Size) noexcept
{
    if (Size == 0 || Size > MaxSmallAllocation) [[unlikely]]
    {
        return InvalidSizeClass;
    }

    const size_t Bucket = (Size + ClassQuantum - 1) >> ClassQuantumShift;
    return SizeClassLookup[Bucket];
}

[[nodiscard]] inline size_t classIndexToSize(const uint16_t ClassIndex) noexcept
{
    if (ClassIndex >= NumSizeClasses) [[unlikely]]
    {
        return 0;
    }

    return alignUp(static_cast<size_t>(SizeClasses[ClassIndex]), alignof(std::max_align_t));
}

struct SizeClass
{
    uint32_t BlockSize     = 0;
    uint16_t BlocksPerSpan = 0;
    uint16_t PagesPerSpan  = 0;
};

class CentralPool
{
public:
    struct PageToSpanPoolStats
    {
        size_t EntryCount            = 0;
        size_t SpanObjectsAllocated  = 0;
        size_t SpanObjectsInUse      = 0;
        double SpanObjectPoolUsageRatio = 0.0;
    };

    explicit CentralPool(PageAllocator &);
    CentralPool(const CentralPool &) = delete;
    CentralPool &operator=(const CentralPool &) = delete;

    /**
     * @brief Fetch a Block Batch from one Size class into thread-local cache.
     * @param
     *     - SizeClassIndex  uint16_t  Target Size-class index.
     *     - OutBlocks       void**         Caller-provided output Block array.
     *     - MaxBlocks       uint32_t  Maximum Blocks to fetch.
     * @usage
     *     - Called on ThreadCache miss to amortize synchronization cost.
     * @return
     *     - uint32_t  Number of Blocks returned.
     */
    [[nodiscard]] uint32_t fetchBatch(uint16_t SizeClassIndex, void **OutBlocks, uint32_t MaxBlocks, MemSpan **OutSpanHint = nullptr);

    /**
     * @brief Return a linked Block Batch back to one central Size class.
     * @param
     *     - SizeClassIndex  uint16_t  Target Size-class index.
     *     - Head            void*          Intrusive single-link Head pointer.
     *     - Count           uint32_t  Number of Blocks in chain.
     * @usage
     *     - Called by ThreadCache when local freelist is over target capacity.
     * @return
     *     - void
     */
    void returnBatch(uint16_t SizeClassIndex, void *Head, uint32_t Count, MemSpan *InitialHint = nullptr);

    /**
     * @brief Read immutable Size-class metadata.
     * @param
     *     - ClassIndex  uint16_t  Size-class index.
     * @usage
     *     - Used by ThreadCache and allocator layout stage.
     * @return
     *     - SizeClass  Metadata for this class or zero-initialized fallback.
     */
    [[nodiscard]] SizeClass getSizeClassInfo(const uint16_t ClassIndex) const noexcept
    {
        return ClassIndex < NumSizeClasses ? Classes[ClassIndex] : SizeClass{};
    }

    [[nodiscard]] PageToSpanPoolStats getPageToSpanPoolStats();
private:
    static constexpr size_t SpanPoolChunkSize = CentralPoolSpanPoolChunkSize;

    struct SizeClassBucketShard
    {
        SpinLock Lock;
        MemSpan *PartialSpans         = nullptr;
        MemSpan *EmptySpans           = nullptr;
        size_t EmptySpanCount = 0;
    };

    struct SizeClassBucket
    {
        std::array<SizeClassBucketShard, CentralPoolBucketMaxShardCount> Shards{};
    };

    // 页基址 -> MemSpan* 映射。
    // 读写自旋锁：注册/反注册为写路径（独占，频率低），returnBatch 查询为读路径
    // （读者并发），避免页映射锁成为多线程释放路径的瓶颈。
    using PageToSpanMap = std::unordered_map<std::uint64_t, MemSpan *>;

    struct PageMapShard
    {
        mutable RwSpinLock Mutex;
        PageToSpanMap Map;
    };

    [[nodiscard]] static size_t determineBucketShardCount() noexcept;

    [[nodiscard]] size_t getThreadShardIndex() const noexcept
    {
        thread_local const size_t Hash =
            std::hash<std::thread::id>{}(std::this_thread::get_id());
        return Hash % ActiveBucketShardCount;
    }

    [[nodiscard]] size_t normalizeShardIndex(const uint16_t ShardIndex) const noexcept
    {
        return static_cast<size_t>(ShardIndex) % ActiveBucketShardCount;
    }

    [[nodiscard]] static size_t pageMapShardIndex(const uint64_t PageBase) noexcept
    {
        return static_cast<size_t>((PageBase >> CentralPoolPageShift) % CentralPoolPageMapShardCount);
    }

    [[nodiscard]] PageMapShard &getPageMapShard(const uint64_t PageBase) noexcept
    {
        return PageToSpanShards[pageMapShardIndex(PageBase)];
    }

    [[nodiscard]] PageMapShard &getPageMapShard(const uint64_t PageBase) const noexcept
    {
        return const_cast<PageMapShard &>(PageToSpanShards[pageMapShardIndex(PageBase)]);
    }

    [[nodiscard]] static double toUsageRatio(size_t used, size_t capacity) noexcept;
    [[nodiscard]] static SizeClass buildSizeClass(size_t PageSize, uint32_t BlockSize) noexcept;
    [[nodiscard]] MemSpan *allocateSpanForClass(uint16_t ClassIndex, uint16_t ShardIndex, SizeClassBucketShard &Shard);
    [[nodiscard]] MemSpan *acquireSpanObject(uint16_t ClassIndex);
    [[nodiscard]] bool growSpanPoolLocked();
    [[nodiscard]] bool registerSpanPages(MemSpan &InSpan);
    [[nodiscard]] MemSpan *findSpanSnapshot(void *Ptr) const;
    [[nodiscard]] static bool isBlockInSpan(const MemSpan &InSpan, const void *Ptr) noexcept;
    [[nodiscard]] MemSpan *findSpanHinted(void *Ptr, MemSpan *Hint, uint16_t ExpectedSizeClass) const;

    void releaseEmptySpan(SizeClassBucketShard &Shard, MemSpan *InSpan);
    void trimEmptySpansLocked(SizeClassBucketShard &Shard);
    void releaseSpanObject(MemSpan *InSpan);
    void unregisterSpanPages(const MemSpan &InSpan);
    void applyReturnedBatchLocked(SizeClassBucketShard &Shard, MemSpan &InSpan, void *BatchHead, void *BatchTail, uint32_t BatchCount);

    static void insertSpan(MemSpan *&Head, MemSpan *InSpan);
    static void removeSpan(MemSpan *&Head, MemSpan *InSpan);
    [[nodiscard]] static void *popBlock(void *&Head) noexcept;
    void pushBlock(void *&Head, void *Block) noexcept;

    PageAllocator *PageAllocator  = nullptr;
    size_t PageSize               = 0;
    size_t ActiveBucketShardCount = 8;

    std::array<SizeClass, NumSizeClasses> Classes{};
    std::array<SizeClassBucket, NumSizeClasses> Buckets{};
    std::array<PageMapShard, CentralPoolPageMapShardCount> PageToSpanShards{};

    std::mutex SpanStorageMutex;
    std::atomic<MemSpan *> SpanFreeList = nullptr;
    std::array<std::atomic<MemSpan *>, NumSizeClasses> SpanClassFreeLists{};
    std::vector<std::unique_ptr<MemSpan[]> > SpanPoolChunks;
    std::atomic<size_t> SpanPoolAllocatedCount = 0;
    std::atomic<size_t> SpanPoolFreeCount      = 0;
};

} // namespace infra::mem


#endif //STABLEARRAY_CENTRALPOOL_H
