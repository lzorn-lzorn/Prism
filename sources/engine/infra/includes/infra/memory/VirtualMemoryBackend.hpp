//
// Created by Lzorn on 2026/9/22.
//

#ifndef STABLEARRAY_VIRTUALMEMORYBACKEND_H
#define STABLEARRAY_VIRTUALMEMORYBACKEND_H

#include <algorithm>
#include <array>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

#include <infra/memory/MemoryCommon.hpp>
#include <infra/memory/RadixTreeMap.hpp>
#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <Windows.h>
#else
#   include <sys/mman.h>
#   include <unistd.h>
#endif

namespace infra::mem
{

/**
 * @note: 虚拟地址空间, PageAllocator 在其基础上进行分配, 并从中切片出若干个 PageSpan
 * 默认值 SmallRegionReserveBytes = 64MB
 */
struct VirtualRegion
{
    std::byte* Base {nullptr}; // 区间起始地址
    size_t ReservedSize { 0 }; // 已 reserve 的总大小
    size_t CommittedSize { 0 };// 已 commit 的大小
    size_t PageSize { 0 };     // 该区间使用的页大小
    int32_t NumaNode { -1 };   // NUMA 偏好
    bool HugePage { false };   // 是否使用大页

    [[nodiscard]] bool isValid() const noexcept
    {
        return Base != nullptr;
    }

    void reset() noexcept;
};

/**
 * @note: PageSpan 不拥有内存, 其仅是 (Regin*, 起始页, 页数) 三元组
 */
struct PageSpan
{
    std::byte* Base = nullptr;
    std::size_t PageCount = 0;
    std::size_t PageSize = 0;
    void* OwnerRegion = nullptr;
    std::uint64_t OwnerRegionId = 0;

    [[nodiscard]] bool isValid() const noexcept
    {
        return Base != nullptr && PageCount > 0 && PageSize > 0;
    }

    [[nodiscard]] std::size_t getBytes() const noexcept
    {
        return PageCount * PageSize;
    }
};

class VirtualMemoryBackend
{
public:
    [[nodiscard]] static size_t getSystemPageSize() noexcept;
    [[nodiscard]] static size_t getHugePageSize() noexcept;

    /**
     * @brief 向操作系统提出内存申请, 但是 OS 不会真的分配内存, 只是标记虚拟内存地址
     * @param RequestedBytes 申请的内存大小
     * @param IsHugePage 是否申请大页面
     * @param NumaNode NumaNode 相关设置, 负数则使用默认配置
     * @return 失败则返回一个空的 VirtualRegion
     */
    [[nodiscard]] static VirtualRegion reserve(size_t RequestedBytes, bool IsHugePage, int32_t NumaNode = -1) noexcept;

    /**
     * @brief 向操作系统申请实际物理内存
     * @param Region
     * @param Offset Region Base 开始的偏移量
     * @param Bytes Commit 的实际上 Size
     * @return
     */
    static bool commit(VirtualRegion& Region, size_t Offset, size_t Bytes) noexcept;

    /**
     * @brief 撤回 commit, 即让操作系统收回物理页面, 但是地址空间依旧保留
     * @param Region
     * @param Offset
     * @param Bytes
     * @return
     */
    static bool decommit(VirtualRegion& Region, size_t Offset, size_t Bytes) noexcept;

    /**
     * @brief 释放虚拟地址空间,
     * @param Region
     */
    static void release(VirtualRegion& Region) noexcept;

private:
#if defined(_WIN32)
    [[nodiscard]] static void* allocateWindows(
        void* Hint,
        const std::size_t Size,
        const DWORD AllocType,
        const DWORD Protect,
        const std::int32_t NumaNode) noexcept;
#endif
};

}


#endif // STABLEARRAY_VIRTUALMEMORYBACKEND_H
