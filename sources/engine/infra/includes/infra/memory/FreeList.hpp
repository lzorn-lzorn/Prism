//
// Created by Lzorn on 2026/9/25.
//

#ifndef STABLEARRAY_FREELIST_H
#define STABLEARRAY_FREELIST_H


#pragma once

#include <cstddef>
#include <cstdint>

#include <infra/memory/MemSpan.hpp>
namespace infra::mem
{

/**
 * @brief 线程私有侵入式单链表，作为 ThreadCache 每个 size class 的空闲块列表。
 * @note 节点指针复用块首 8 字节，无额外内存开销；Count/MaxCount 用于预算控制。
 */
class FreeList
{
public:
    /**
     * @brief 将一个块压入链表头。
     * @param Block 块指针，其首 8 字节会被改写为 next 指针
     * @return void
     */
    void push(void* Block) noexcept;

    /**
     * @brief 弹出链表头块。
     * @return 块指针；链表为空时返回 nullptr
     */
    void* pop() noexcept;

    /**
     * @brief 将一条已串联的块链压入链表头。
     * @param BatchHead  批链头
     * @param BatchTail  批链尾
     * @param BatchCount 批块数
     * @return void
     */
    void pushBatch(void* BatchHead, void* BatchTail, uint16_t BatchCount) noexcept;

    /**
     * @brief 弹出至多 MaxCount 个块的批链。
     * @param MaxCount 最大块数
     * @param Tail     输出批链尾
     * @return 批链头；链表为空时返回 nullptr
     */
    void* popBatch(uint16_t MaxCount, void*& Tail) noexcept;

    [[nodiscard]] bool isEmpty() const noexcept { return Head == nullptr; }

    [[nodiscard]] uint16_t getCount() const noexcept { return Count; }

    [[nodiscard]] uint16_t getMaxCount() const noexcept { return MaxCount; }

    void setMaxCount(const uint16_t Value) noexcept { MaxCount = Value; }

    [[nodiscard]] MemSpan* getSpanHint() const noexcept { return SpanHint; }

    void setSpanHint(MemSpan* SpanHint) noexcept { this->SpanHint = SpanHint; }

    void clear() noexcept;

private:
    void* Head { nullptr };
    uint16_t Count { 0 };
    uint16_t MaxCount { 0 };
    MemSpan* SpanHint { nullptr };   // 缓存最近使用的 MemSpan 对象
};

}
#endif //STABLEARRAY_FREELIST_H
