//
// Created by Lzorn on 2026/9/25.
//

#include <infra/memory/FreeList.hpp>

namespace infra::mem
{

void FreeList::push(void* Block) noexcept
{
    if (!Block) [[unlikely]]
    {
        return;
    }

    *reinterpret_cast<void**>(Block) = Head;
    Head = Block;
    ++Count;
}

void* FreeList::pop() noexcept
{
    if (!Head) [[unlikely]]
    {
        return nullptr;
    }

    // @note: load-load 依赖: 后一条 load 的地址依赖于前一条 load 的数据
    // 其生成:
    // mov rax, QWORD PTR [rdi]   ; load 1: 读 Head
    // mov rdx, QWORD PTR [rax]   ; load 2: 读 *Block
    // 换句话说 load Head -> 得到 Block 地址 -> load *Block -> 得到 next
    void* Block = Head;
    void* Next = *reinterpret_cast<void**>(Block);
    Head = Next; // load-load 依赖

    --Count;

    // 预取 Next 的 cache line，让下一次 pop 的 *Next load 命中
    if (Next) [[likely]]
    {
        prefetchRead(Next);
    }
    return Block;
}

void FreeList::pushBatch(void* BatchHead, void* BatchTail, uint16_t BatchCount) noexcept
{
    if (!BatchHead || !BatchTail || BatchCount == 0)
    {
        return;
    }

    *reinterpret_cast<void**>(BatchTail) = Head;
    Head = BatchHead;
    const std::uint32_t Sum = static_cast<std::uint32_t>(Count) + BatchCount;
    Count = static_cast<uint16_t>(Sum > 0xFFFFu ? 0xFFFFu : Sum);
}


void* FreeList::popBatch(uint16_t MaxCount, void*& Tail) noexcept
{
    if (!Head || MaxCount == 0) [[unlikely]]
    {
        Tail = nullptr;
        return nullptr;
    }
    void* BatchHead = Head;
    void* Current = BatchHead;
    uint16_t Popped = 0;
    while (Current && Popped < MaxCount)
    {
        Tail = Current;
        Current = *reinterpret_cast<void**>(Current);
        ++Popped;
    }
    *reinterpret_cast<void**>(Tail) = nullptr;
    Head = Current;
    Count -= Popped;
    return BatchHead;
}

void FreeList::clear() noexcept
{
    Head = nullptr;
    SpanHint = nullptr;
    Count = 0;
}


}
