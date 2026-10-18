//
// Created by Lzorn on 2026/9/24.
//

#include <infra/memory/MemSpan.hpp>

namespace infra::mem
{

void MemSpan::reset() noexcept
{
    Next     = nullptr;
    Prev     = nullptr;
    PoolNext = nullptr;

    BlockSize        = 0;
    TotalBlocks      = 0;
    FreeBlocks       = 0;
    SizeClassIndex   = InvalidSizeClass;
    BucketShardIndex = 0;
    StateFlags       = 0;

    FreeList    = nullptr;
    SpanBase    = nullptr;
    BackingSpan = {};
}

void MemSpan::setup(
    uint32_t InBlockSize,
    uint16_t InTotalBlocks,
    uint16_t InSizeClassIndex,
    uint16_t InBucketShardIndex,
    std::byte *InSpanBase,
    const PageSpan &InBackingSpan) noexcept
{
    BlockSize = InBlockSize;
    TotalBlocks = InTotalBlocks;
    FreeBlocks = InTotalBlocks;
    SizeClassIndex = InSizeClassIndex;
    BucketShardIndex = InBucketShardIndex;

    SpanBase = InSpanBase;
    BackingSpan = InBackingSpan;

    StateFlags = 0;

    if (InTotalBlocks == 0) [[unlikely]]
    {
        FreeList = nullptr;
        return;
    }

    std::byte* Block = InSpanBase;
    std::byte* NextBlock = Block + InBlockSize;
    for (std::uint16_t I = 1; I < InTotalBlocks; ++I)
    {
        *reinterpret_cast<void**>(Block) = NextBlock;
        prefetchWrite(NextBlock);
        Block = NextBlock;
        NextBlock = Block + InBlockSize;
    }
    *reinterpret_cast<void**>(Block) = nullptr;
    FreeList = InSpanBase;
}

void *MemSpan::popBlock() noexcept
{
    if (!FreeList) [[unlikely]]
    {
        return nullptr;
    }

    void* Block = FreeList;
    void* NextBlock = *reinterpret_cast<void**>(Block);
    FreeList = NextBlock;
    --FreeBlocks;

    if (NextBlock) [[likely]]
    {
        const void* NextNext = *reinterpret_cast<const void* const*>(NextBlock);
        prefetchRead(NextNext);
    }
    return Block;
}

void MemSpan::pushBlock(void *Block) noexcept
{
    if (!Block) [[unlikely]]
    {
        return;
    }

    *reinterpret_cast<void**>(Block) = FreeList;
    FreeList = Block;

    // 饱和加法：FreeBlocks + (FreeBlocks < TotalBlocks ? 1 : 0)
    // 编译为 cmp + setb + add，无分支
    const auto Inc = static_cast<uint16_t>(FreeBlocks < TotalBlocks);
    FreeBlocks = static_cast<uint16_t>(FreeBlocks + Inc);
}


void MemSpan::pushBatch(void *BatchHead, void *BatchTail, uint32_t BatchCount) noexcept
{
    if (!BatchHead || !BatchTail || BatchCount == 0)
    {
        return;
    }

    *reinterpret_cast<void **>(BatchTail) = FreeList;
    FreeList                              = BatchHead;
    const uint32_t Sum = static_cast<uint32_t>(FreeBlocks) + BatchCount;
    FreeBlocks = static_cast<uint16_t>(Sum < TotalBlocks ? Sum : TotalBlocks);
}

void* MemSpan::popBatch(std::uint16_t MaxCount, void*& Tail) noexcept
{
    Tail = nullptr;
    if (!FreeList || MaxCount == 0) [[unlikely]]
    {
        return nullptr;
    }

    void* BatchHead = FreeList;
    void* PrevTmp = BatchHead;
    void* Current = *reinterpret_cast<void**>(BatchHead);
    std::uint16_t Popped = 1;

    // 带预取的批量遍历
    while (Current && Popped < MaxCount)
    {
        const void* NextNext = *reinterpret_cast<const void* const*>(Current);
        prefetchRead(NextNext);

        PrevTmp = Current;
        Current = const_cast<void*>(NextNext);
        ++Popped;
    }

    *reinterpret_cast<void**>(PrevTmp) = nullptr;
    Tail = PrevTmp;
    FreeList = Current;
    FreeBlocks = static_cast<std::uint16_t>(FreeBlocks - Popped);
    return BatchHead;
}

void MemSpan::setPartial(bool Value) noexcept
{
    StateFlags = Value ? static_cast<std::uint8_t>(StateFlags | StatePartial)
                   : static_cast<std::uint8_t>(StateFlags & ~StatePartial);
}
void MemSpan::setEmpty(bool Value) noexcept
{
    StateFlags = Value ? static_cast<std::uint8_t>(StateFlags | StateEmpty)
                   : static_cast<std::uint8_t>(StateFlags & ~StateEmpty);
}
void MemSpan::setReleased(bool Value) noexcept
{
    StateFlags = Value ? static_cast<std::uint8_t>(StateFlags | StateReleased)
                   : static_cast<std::uint8_t>(StateFlags & ~StateReleased);
}
}