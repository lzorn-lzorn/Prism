//
// Created by Lzorn on 2026/9/23.
//

#include <infra/memory/BootstrapAllocator.hpp>

namespace infra::mem
{

void* BootstrapArena::allocate(size_t Size, size_t Alignment) noexcept
{
    // 归一化：Size 至少 1, Alignment 至少 max_align_t
    if (Size == 0)
    {
        Size = 1;
    }
    if (Alignment < alignof(std::max_align_t))
    {
        Alignment = alignof(std::max_align_t);
    }
    const size_t Aligned = alignUp(Size, Alignment);

    for (int Retry = 0; Retry < MaxRetries; ++Retry)
    {
        Chunk* Current = CurrentChunk.load(std::memory_order_acquire);
        if (Current) [[likely]]
        {
            size_t Cur = Current->Offset.load(std::memory_order_relaxed);
            if (Cur + Aligned <= Current->Capacity)
            {
                // 快路径：无锁 CAS 抢占块内空间
                if (Current->Offset.compare_exchange_weak(
                        Cur,
                        Cur + Aligned,
                        std::memory_order_relaxed,
                        std::memory_order_relaxed))
                {
                    UsedBytes.fetch_add(Aligned, std::memory_order_relaxed);
                    return Current->Data + Cur;
                }
                // CAS 失败：别的线程先抢到了, 重试
                continue;
            }
        }

        // 慢路径：当前块不可用或不存在, 进入 growArena
        if (!growArena(Aligned))
        {
            return nullptr;
        }
    }

    return nullptr;
}

bool BootstrapArena::growArena(size_t MinBytes) noexcept
{
    std::lock_guard<std::mutex> Lock(GrowMutex);

    // double-check：可能在等锁期间其他线程已经扩容了
    Chunk* Current = CurrentChunk.load(std::memory_order_acquire);
    if (Current)
    {
        size_t Cur = Current->Offset.load(std::memory_order_relaxed);
        if (Cur + MinBytes <= Current->Capacity)
        {
            return true;  // 其他线程已扩容, 直接用
        }
    }

    // 计算本次块的字节数：至少能装下 MinBytes, 且不小于 DefaultChunkBytes
    const size_t PageSize = VirtualMemoryBackend::getSystemPageSize();
    const size_t HeaderSize = alignUp(sizeof(Chunk), alignof(Chunk));
    size_t PayloadBytes = MinBytes > DefaultChunkBytes ? MinBytes : DefaultChunkBytes;
    if (PayloadBytes < MinChunkBytes)
    {
        PayloadBytes = MinChunkBytes;
    }
    size_t TotalBytes = alignUp(HeaderSize + PayloadBytes, PageSize);

    // 从 VM 拿地址空间
    VirtualRegion Region = VirtualMemoryBackend::reserve(TotalBytes, false, -1);
    if (!Region.isValid())
    {
        return false;
    }

    // @note: 先 commit, 再写 Chunk
    // reserve 得到的是 PROT_NONE 区域, 直接写会 SIGSEGV
    if (!VirtualMemoryBackend::commit(Region, 0, Region.ReservedSize))
    {
        VirtualMemoryBackend::release(Region);
        return false;
    }

    // 用区域头部存 Chunk 结构
    std::byte* Base = Region.Base;
    std::byte* Payload = Base + HeaderSize;
    size_t PayloadCapacity = Region.ReservedSize - HeaderSize;

    // placement new 构造 Chunk, 含 atomic 成员, 不能用 memset
    auto* NewChunk = new (Base) Chunk{};
    NewChunk->Data = Payload;
    NewChunk->Capacity = PayloadCapacity;
    NewChunk->Next = ChunkList;
    // Offset 已经由 Chunk{} 的成员初始化器置 0, 这里无需再 store

    // 链接到诊断链表
    ChunkList = NewChunk;
    TotalCapacity.fetch_add(PayloadCapacity, std::memory_order_relaxed);

    // release 语义：一旦其他线程通过 acquire 看到 NewChunk, 
    // 就能看到上面所有初始化
    CurrentChunk.store(NewChunk, std::memory_order_release);
    return true;
}

}