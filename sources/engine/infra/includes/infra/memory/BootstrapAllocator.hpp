//
// Created by Lzorn on 2026/9/23.
//

#ifndef STABLEARRAY_BOOSTSTRAPALLOCATOR_H
#define STABLEARRAY_BOOSTSTRAPALLOCATOR_H

#include <infra/memory/VirtualMemoryBackend.hpp>

#include <atomic>
namespace infra::mem
{
// 默认块大小：1 MiB. 既能摊销 syscall 成本,又不至于过大浪费. 
constexpr size_t DefaultChunkBytes = 1024 * 1024;
// 最小块：64 KiB. 避免小请求触发过小的块. 
constexpr size_t MinChunkBytes = 64 * 1024;
// allocate 的最大重试次数：防御性上限,正常 1~2 次即成功. 
constexpr int MaxRetries = 8;
class BootstrapArena
{
    struct Chunk
    {
        std::byte* Data;                 // 可分配区起始
        size_t Capacity;                 // 可分配字节数
        Chunk* Next;                     // 上一块（链表头插入）
        std::atomic<size_t> Offset{0};   // 已使用字节数（相对 Data）
    };
public:
    [[nodiscard]] static BootstrapArena& self() noexcept
    {
        static BootstrapArena Instance;
        return Instance;
    }

    BootstrapArena(const BootstrapArena&) = delete;
    BootstrapArena& operator=(const BootstrapArena&) = delete;

    [[nodiscard]] void* allocate(size_t Size, size_t Alignment) noexcept;

    void deallocate(void* Pointer, size_t Size) noexcept {}

    [[nodiscard]] size_t totalCapacity() const noexcept
    {
        return TotalCapacity.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t usedBytes() const noexcept
    {
        return UsedBytes.load(std::memory_order_relaxed);
    }
private:
    BootstrapArena() noexcept = default;
    ~BootstrapArena() noexcept = default;

    [[nodiscard]] bool growArena(size_t MinBytes) noexcept;


    std::mutex GrowMutex;                          // 仅保护 growArena
    std::atomic<Chunk*> CurrentChunk{nullptr};  // 当前用于 bump 的块
    Chunk* ChunkList{nullptr};                     // 所有已分配块（仅诊断用）
    std::atomic<size_t> TotalCapacity{0};
    std::atomic<size_t> UsedBytes{0};
};

// @note: 单调内存分配, 只分配不释放
// 低地址 ─────────────────────────────────────> 高地址
// [ 分配1 ][ 分配2 ][ 分配3 ][ 空闲 ][ 未提交 ]
//                                        ^
//                                      Offset
template <typename T>
class BootstrapAllocator
{
public:
    using value_type = T;

    BootstrapAllocator() noexcept = default;

    template <typename U>
    BootstrapAllocator(BootstrapAllocator<U> const&) noexcept {}

    [[nodiscard]] static T* allocate(std::size_t N)
    {
        void* Ptr = BootstrapArena::self().allocate(N * sizeof(T), alignof(T));
        if (!Ptr)
        {
            throw std::bad_alloc{};
        }
        return static_cast<T*>(Ptr);
    }
    static void deallocate(T*, std::size_t) noexcept {}

    template <typename U>
    [[nodiscard]] bool operator==(const BootstrapAllocator<U>&) const noexcept { return true; }
    template <typename U>
    [[nodiscard]] bool operator!=(const BootstrapAllocator<U>&) const noexcept { return false; }
};
}
#endif //STABLEARRAY_BOOSTSTRAPALLOCATOR_H
