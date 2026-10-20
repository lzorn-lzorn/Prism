#pragma once

#include <atomic>
#include <memory>
#include <type_traits>
#include <utility>
#include <span>
#include <iterator>
#include <optional>
#include <memory>
#include <stdexcept>
#include <array>
#include <algorithm>
// #include <thread>
// #include <iostream>

#include "infra/common/Common.hpp"

namespace infra
{
constexpr inline bool isPowerOf2(size_t N)
{
    return N != 0 && (N & (N - 1)) == 0;
}

enum class ERingBufferPolicy
{
    SingleThread,
    SPSC,
    MPSC,
    MPMC,
};

template <typename T, typename Allocator, ERingBufferPolicy Policy>
class RingBuffer;


template <typename T, typename Allocator>
class RingBuffer<T, Allocator, ERingBufferPolicy::SingleThread>
{
public:
    using value_type = T;
    using allocator_type = Allocator;
    using allocator_traits = std::allocator_traits<Allocator>;
    using size_type = std::size_t;
    using difference_type = allocator_traits::difference_type;
    using pointer = typename allocator_traits::pointer;
    using const_pointer = typename allocator_traits::const_pointer;
    using reference = value_type&;
    using const_reference = const value_type&;

private:
    template <bool IsConst>
    class iterator_impl 
    {
        friend class RingBuffer;
        using pointer = std::conditional_t<IsConst, const RingBuffer*, RingBuffer*>;
        using reference = std::conditional_t<IsConst, const T&, T&>;
        using buffer_type = std::conditional_t<IsConst, const RingBuffer, RingBuffer>;
        using value_type = std::conditional_t<IsConst, const T, T>;

        buffer_type* Parent;
        size_type Offset;  // 从 head 开始的偏移

        explicit iterator_impl(buffer_type* InParent, size_type InOffset) 
            : Parent(InParent)
            , Offset(InOffset)
        {
            
        }

    public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;

        iterator_impl() = default;
        iterator_impl(const iterator_impl<false>& Other)
            requires (!IsConst)
            : Parent(Other.Parent), Offset(Other.Offset)
        {
            
        }
        
        value_type& operator*() const noexcept
        {
            const size_type idx = (Parent->Head + Offset) & Parent->Mask;
            return Parent->InnerBuffer[idx];
        }

        iterator_impl& operator++() { ++Offset; return *this; }
        iterator_impl operator++(int) { auto tmp = *this; ++Offset; return tmp; }
        iterator_impl& operator--() { --Offset; return *this; }
        iterator_impl operator--(int) { auto tmp = *this; --Offset; return tmp; }

        iterator_impl& operator+=(difference_type N) { Offset += N; return *this; }
        iterator_impl& operator-=(difference_type N) { Offset -= N; return *this; }
        friend iterator_impl operator+(iterator_impl It, difference_type N) { It += N; return It; }
        friend iterator_impl operator-(iterator_impl It, difference_type N) { It -= N; return It; }
        friend difference_type operator-(const iterator_impl& A, const iterator_impl& B)
        {
            return static_cast<difference_type>(A.Offset) - B.Offset;
        }

        value_type* operator->() const { return &**this; }
        value_type& operator[](difference_type N) const { return *(*this + N); }

        std::strong_ordering operator<=>(const iterator_impl&) const = default;
        bool operator==(const iterator_impl&) const noexcept = default;
    };

public:
    using iterator       = iterator_impl<false>;
    using const_iterator = iterator_impl<true>;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

public:

    explicit RingBuffer(size_type InCapacity, const Allocator& InAlloc = Allocator{})
        : Capacity(InCapacity), Mask(InCapacity - 1), Alloc(InAlloc)
    {
        // 要求容量 > 0 且为 2 的幂（可选，也可用 % 代替）
        if (Capacity == 0 || !isPowerOf2(Capacity))
        {
        	throw std::invalid_argument("Capacity must be A power of 2 and > 0");
        }

        InnerBuffer = allocator_traits::allocate(Alloc, Capacity);
    }

    ~RingBuffer()
    {
        clear();  // 析构所有存活的元素
        if (InnerBuffer != nullptr)
        {
        	allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
        }
    }

    // 禁止拷贝（原子成员不可拷贝）
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 允许移动（但移动后源对象原子仍有效，需谨慎）
    RingBuffer(RingBuffer&& Other)
        : Capacity(Other.Capacity)
        , Head(Other.Head)
        , Tail(Other.Tail)
        , Mask(Other.Mask)
        , InnerBuffer(Other.InnerBuffer)
    {
        // 根据标准，移动构造应使用 allocator_traits::select_on_container_copy_construction，
        // 但这里没有源分配器可传播，我们采用以下策略：
        if constexpr (std::allocator_traits<Allocator>::propagate_on_container_move_assignment::value) 
        {
            Alloc = std::move(Other.Alloc);
        } 
        else 
        {
            // 不传播分配器，要求分配器必须相等，否则无法安全移动
            if (Other.Alloc != Allocator{})
            {
                throw std::logic_error("Cannot move-construct RingBuffer with non-equal, non-propagating allocators");
            }
            // Alloc 保持默认构造状态（与 Other 的分配器相等）
        }
        Other.Capacity = 0;
        Other.InnerBuffer = nullptr;
    	Other.Head = 0;
    	Other.Tail = 0;
    	Other.Mask = 0;
    }

    RingBuffer& operator=(RingBuffer&& Other) 
        noexcept (allocator_traits::is_always_equal::value && std::is_nothrow_move_assignable_v<Allocator>)
    {
        if (this != &Other)
        {
            clear();
            if (InnerBuffer != nullptr)
            {
                allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
            }

            if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
                Head = Other.Head;
                Tail = Other.Tail;
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
            } else {
                if (Alloc != Other.Alloc)
                {
                    throw std::runtime_error("Cannot move-assign RingBuffer with different allocators");
                }
                Head = Other.Head;
                Tail = Other.Tail;
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
            }

            Other.Capacity = 0;
            Other.Mask = 0;
            Other.InnerBuffer = nullptr;
            Other.Head = 0;
            Other.Tail = 0;
        }
        return *this;
    }

    reference front() noexcept { return InnerBuffer[Head & Mask]; }
    const_reference front() const noexcept { return InnerBuffer[Head & Mask]; }
    reference back() noexcept { return InnerBuffer[(Tail - 1) & Mask]; }
    const_reference back() const noexcept { return InnerBuffer[(Tail - 1) & Mask]; }

    void pushBack(const value_type& Value)
    {
        if (isFull())
        {
            throw std::out_of_range("RingBuffer: buffer is full");
        }

    	const auto idx = Tail & Mask;
    	allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), Value);
    	++Tail;
    }

    void pushBack(value_type&& Value)
    {
    	if (isFull())
    	{
    		throw std::out_of_range("RingBuffer: buffer is full");
    	}

    	const auto idx = Tail & Mask;
    	allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::move(Value));
    	++Tail;
    }

    template <typename... Args>
    T& emplaceBack(Args&&... InArgs)
    {
    	if (isFull())
    	{
    		throw std::out_of_range("RingBuffer: buffer is full");
    	}

    	const auto idx = Tail & Mask;
    	allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::forward<Args>(InArgs)...);
    	++Tail;
    	return InnerBuffer[idx];
    }

    void pop_front()
    {
    	if (isEmpty())
    	{
    		throw std::out_of_range("RingBuffer: buffer is empty");
    	}

    	const auto idx = Head & Mask;
    	allocator_traits::destroy(Alloc, std::addressof(InnerBuffer[idx]));
    	++Head;
    }

    void clear() noexcept
    {
        while (!isEmpty())
        {
        	pop_front();
        }
    }

    [[nodiscard]] bool isEmpty() const noexcept { return Head == Tail; }
    [[nodiscard]] bool isFull() const noexcept { return getSize() == Capacity; }
    [[nodiscard]] size_type getSize() const noexcept { return Tail - Head; }
    [[nodiscard]] size_type getCapacity() const noexcept { return Capacity; }
    [[nodiscard]] size_type getMaxSize() const noexcept { return allocator_traits::max_size(Alloc); }

    // ==================== 迭代器接口 ====================
    [[nodiscard]] iterator begin() noexcept { return iterator(this, 0); }
    [[nodiscard]] iterator end() noexcept { return iterator(this, getSize()); }

    [[nodiscard]] const_iterator begin() const noexcept { return const_iterator(this, 0); }
    [[nodiscard]] const_iterator end() const noexcept { return const_iterator(this, getSize()); }

    [[nodiscard]] const_iterator cbegin() const noexcept { return begin(); }
    [[nodiscard]] const_iterator cend() const noexcept { return end(); }

    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(begin()); }

    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }

    [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    [[nodiscard]] const_reverse_iterator crend() const noexcept { return rend(); }

    /// 将环形数据转为两个连续 span（无拷贝，高效访问）
    [[nodiscard]] std::pair<std::span<T>, std::span<T>> getLinearizeViews() noexcept
    {
    	const auto sz = getSize();
    	if (sz == 0) return {};

    	const auto h_idx = Head & Mask;
    	const auto first_len = std::min(Capacity - h_idx, sz);
    	return {
    		std::span<T>(InnerBuffer + h_idx, first_len),
            std::span<T>(InnerBuffer, sz - first_len)
        };
    }

    [[nodiscard]] std::pair<std::span<const T>, std::span<const T>> getLinearizeViews() const noexcept
    {
    	const auto sz = getSize();
    	if (sz == 0) return {};

    	const auto h_idx = Head & Mask;
    	const auto first_len = std::min(Capacity - h_idx, sz);
    	return {
    		std::span<const T>(InnerBuffer + h_idx, first_len),
            std::span<const T>(InnerBuffer, sz - first_len)
        };
    }

    [[nodiscard]] allocator_type getAllocator() const noexcept { return Alloc; }
private:
    alignas(CacheLineSize) size_type Head {0};
    alignas(CacheLineSize) size_type Tail {0};

    size_type Capacity;
    size_type Mask;
    value_type* InnerBuffer;
    Allocator Alloc;

};

/*
int test_signal_thread()
{
    // 创建容量为 8（2的幂）的环形缓冲区
    RingBuffer<int, std::allocator<int>, ERingBufferPolicy::SingleThread> buf(8);

    // 插入元素
    for (int i = 0; i < 5; ++i) {
        buf.pushBack(i);
    }

    // 原地构造
    buf.emplaceBack(99);

    // 遍历输出
    std::cout << "遍历元素：";
    for (int val : buf) {
        std::cout << val << " ";
    }
    std::cout << "\n大小：" << buf.getSize() << "\N";

    // 弹出队首
    buf.pop_front();
    std::cout << "弹出后首元素：" << buf.front() << "\N";

    // C++20 span 视图访问
    auto [first, second] = buf.getLinearizeViews();
    std::cout << "Span 访问：";
    for (int val : first) std::cout << val << " ";
    for (int val : second) std::cout << val << " ";
    std::cout << "\N";

    // 清空
    buf.clear();
    std::cout << "清空后是否为空：" << std::boolalpha << buf.isEmpty() << "\N";

    return 0;
}
*/

template <typename T, typename Allocator>
class RingBuffer<T, Allocator, ERingBufferPolicy::SPSC> 
{
public:
    using value_type       = T;
    using allocator_type   = Allocator;
    using allocator_traits = std::allocator_traits<Allocator>;
    using size_type        = std::size_t;
    using difference_type  = allocator_traits::difference_type;
    using pointer          = typename allocator_traits::pointer;
    using const_pointer    = typename allocator_traits::const_pointer;
    using reference        = value_type&;
    using const_reference  = const value_type&;

private:
    template <bool IsConst>
    class iterator_impl 
    {
    };

public:
    using iterator               = iterator_impl<false>;
    using const_iterator         = iterator_impl<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

public:
    explicit RingBuffer(size_type InCapacity, const Allocator& InAlloc = Allocator{})
        : Capacity(InCapacity), Mask(InCapacity - 1), Alloc(InAlloc)
    {
        if (Capacity == 0 || !isPowerOf2(Capacity))
        {
            throw std::invalid_argument("Capacity must be A power of 2 and > 0");
        }

        InnerBuffer = allocator_traits::allocate(Alloc, Capacity);
    }

    ~RingBuffer()
    {
        clear();
        if (InnerBuffer != nullptr)
        {
            allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 移动构造/赋值 noexcept
    RingBuffer(RingBuffer&& Other)
        : Capacity(Other.Capacity),
          Mask(Other.Mask),
          InnerBuffer(Other.InnerBuffer)
    {
        if constexpr (std::allocator_traits<Allocator>::propagate_on_container_move_assignment::value) 
        {
            Alloc = std::move(Other.Alloc);
        } 
        else 
        {
            // 不传播分配器，要求分配器必须相等，否则无法安全移动
            if (Other.Alloc != Allocator{})
            {
                throw std::logic_error("Cannot move-construct RingBuffer with non-equal, non-propagating allocators");
            }
            // Alloc 保持默认构造状态（与 Other 的分配器相等）
        }
        Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Other.Capacity = 0;
        Other.Mask = 0;
        Other.InnerBuffer = nullptr;
        Other.Head.store(0, std::memory_order_relaxed);
        Other.Tail.store(0, std::memory_order_relaxed);
    }

    RingBuffer& operator=(RingBuffer&& Other) 
        noexcept (allocator_traits::is_always_equal::value && std::is_nothrow_move_assignable_v<Allocator>)
    {
        if (this != &Other)
        {
            clear();
            if (InnerBuffer != nullptr)
            {
                allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
            }

            if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
                Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
            } else {
                if (Alloc != Other.Alloc)
                {
                    throw std::runtime_error("Cannot move-assign RingBuffer with different allocators");
                }
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
                Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);
            }

            Other.Capacity = 0;
            Other.Mask = 0;
            Other.InnerBuffer = nullptr;
            Other.Head.store(0, std::memory_order_relaxed);
            Other.Tail.store(0, std::memory_order_relaxed);
        }
        return *this;
    }

    bool tryPush(const value_type& Value)
        noexcept(std::is_nothrow_copy_constructible_v<value_type>)
    {
        const size_type head = Head.load(std::memory_order_acquire);
        const size_type tail = Tail.load(std::memory_order_relaxed);

        if ((tail - head) >= Capacity)
        {
            return false;
        }

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), Value);
        Tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool tryPush(value_type&& Value)
        noexcept(std::is_nothrow_move_constructible_v<value_type>)
    {
        const size_type head = Head.load(std::memory_order_acquire);
        const size_type tail = Tail.load(std::memory_order_relaxed);

        if ((tail - head) >= Capacity)
        {
            return false;
        }

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::move(Value));
        Tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    bool try_emplace(Args&&... InArgs)
        noexcept(std::is_nothrow_constructible_v<T, Args&&...>)
    {
        const size_type head = Head.load(std::memory_order_acquire);
        const size_type tail = Tail.load(std::memory_order_relaxed);

        if ((tail - head) >= Capacity)
        {
            return false;
        }

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::forward<Args>(InArgs)...);
        Tail.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::optional<T> tryPop() noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_acquire);
        if (head == tail)
        {
            return std::nullopt;
        }

        const size_type idx = head & Mask;
        T ret = std::move(InnerBuffer[idx]);
        allocator_traits::destroy(Alloc, std::addressof(InnerBuffer[idx]));
        Head.store(head + 1, std::memory_order_release);
        return ret;
    }

    // ====================== 容量查询(仅快照，值瞬时失效) ======================
    [[nodiscard]] bool isEmpty() const noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_relaxed);
        return head == tail;
    }

    [[nodiscard]] bool isFull() const noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_relaxed);
        return tail - head == Capacity;
    }

    [[nodiscard]] size_type getSize() const noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_relaxed);
        return tail - head;
    }

    [[nodiscard]] size_type getCapacity() const noexcept { return Capacity; }
    [[nodiscard]] size_type getMaxSize() const noexcept { return allocator_traits::max_size(Alloc); }

    void clear() noexcept
    {
        while (!isEmpty())
        {
            [[maybe_unused]] auto val = tryPop();
        }
    }


    [[nodiscard]] std::pair<std::span<T>, std::span<T>> getLinearizeViews()
    {
        static_assert([]{
            constexpr bool ok = []{
                // 提示使用者该接口仅静态单线程快照可用
                return true;
            }();
            return ok;
        }(), "getLinearizeViews() only safe when producer & consumer threads are stopped entirely");

        const size_type h = Head.load(std::memory_order_relaxed);
        const size_type t = Tail.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<T>(InnerBuffer + h_idx, first_len),
            std::span<T>(InnerBuffer, sz - first_len)
        };
    }

    [[nodiscard]] std::pair<std::span<const T>, std::span<const T>> getLinearizeViews() const
    {
        static_assert([]{
            constexpr bool ok = []{ return true; }();
            return ok;
        }(), "getLinearizeViews() only safe when producer & consumer threads are stopped entirely");

        const size_type h = Head.load(std::memory_order_relaxed);
        const size_type t = Tail.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<const T>(InnerBuffer + h_idx, first_len),
            std::span<const T>(InnerBuffer, sz - first_len)
        };
    }

    // 分配器访问
    [[nodiscard]] allocator_type getAllocator() const noexcept { return Alloc; }
private:
    alignas(CacheLineSize) std::atomic<size_type> Head{0};
    alignas(CacheLineSize) std::atomic<size_type> Tail{0};

    size_type Capacity;
    size_type Mask;
    pointer InnerBuffer;
    Allocator Alloc;
};

template <typename T, typename Allocator>
class RingBuffer<T, Allocator, ERingBufferPolicy::MPSC>
{
public:
    using value_type       = T;
    using allocator_type   = Allocator;
    using allocator_traits = std::allocator_traits<Allocator>;
    using size_type        = std::size_t;
    using difference_type  = allocator_traits::difference_type;
    using pointer          = typename allocator_traits::pointer;
    using const_pointer    = typename allocator_traits::const_pointer;
    using reference        = value_type&;
    using const_reference  = const value_type&;

private:
    template <bool IsConst>
    class iterator_impl 
    {
    };

public:
    using iterator               = iterator_impl<false>;
    using const_iterator         = iterator_impl<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    explicit RingBuffer(size_type InCapacity, const Allocator& InAlloc = Allocator{})
        : Capacity(InCapacity)
        , Mask(InCapacity - 1)
        , Alloc(InAlloc)
        , m_Ready(new std::atomic<unsigned char>[InCapacity]())  // 全零初始化(空闲)
    {
        if (Capacity == 0 || !isPowerOf2(Capacity))
        {
            throw std::invalid_argument("Capacity must be A power of 2 and > 0");
        }

        InnerBuffer = allocator_traits::allocate(Alloc, Capacity);
    }

    ~RingBuffer()
    {
        clear();
        if (InnerBuffer != nullptr)
        {
        	allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    RingBuffer(RingBuffer&& Other)
        : Capacity(Other.Capacity)
        , Mask(Other.Mask)
        , InnerBuffer(Other.InnerBuffer)
        , m_Ready(std::move(Other.m_Ready))
    {
        if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
        {
            Alloc = std::move(Other.Alloc);
        }
        else
        {
            if (Other.Alloc != Allocator{})
            {
                throw std::logic_error("Cannot move-construct RingBuffer with non-equal, non-propagating allocators");
            }
        }

        Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Other.Capacity = 0;
        Other.Mask = 0;
        Other.InnerBuffer = nullptr;
        Other.Head.store(0, std::memory_order_relaxed);
        Other.Tail.store(0, std::memory_order_relaxed);
    }

     RingBuffer& operator=(RingBuffer&& Other) 
        noexcept (allocator_traits::is_always_equal::value && std::is_nothrow_move_assignable_v<Allocator>)
    {
        if (this != &Other)
        {
            clear();
            if (InnerBuffer != nullptr)
            {
            	allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
            }

            if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                m_Ready = std::move(Other.m_Ready);
            }
            else
            {
                if (Alloc != Other.Alloc)
                    throw std::runtime_error("Cannot move-assign RingBuffer with different allocators");
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                m_Ready = std::move(Other.m_Ready);
            }

            Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
            Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);

            Other.Capacity = 0;
            Other.Mask = 0;
            Other.InnerBuffer = nullptr;
            Other.Head.store(0, std::memory_order_relaxed);
            Other.Tail.store(0, std::memory_order_relaxed);
        }
        return *this;
    }

    bool tryPush(const value_type& value)
        noexcept(std::is_nothrow_copy_constructible_v<value_type>)
    {
        size_type head, tail;
        do 
        {
            head = Head.load(std::memory_order_acquire);
            tail = Tail.load(std::memory_order_relaxed);
            if ((tail - head) >= Capacity)
            {
                return false;
            }
        } while (!Tail.compare_exchange_weak(tail, tail + 1, std::memory_order_acq_rel, std::memory_order_relaxed));

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), value);
        m_Ready[idx].store(1, std::memory_order_release);
        return true;
    }

    bool tryPush(value_type&& Value)
        noexcept(std::is_nothrow_move_constructible_v<value_type>)
    {
        size_type head, tail;
        do
        {
            head = Head.load(std::memory_order_acquire);
            tail = Tail.load(std::memory_order_relaxed);
            if (tail - head >= Capacity)
                return false;
        } while (!Tail.compare_exchange_weak(tail, tail + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed));

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::move(Value));
        m_Ready[idx].store(1, std::memory_order_release);
        return true;
    }

    template <typename... Args>
    bool try_emplace(Args&&... InArgs)
        noexcept(std::is_nothrow_constructible_v<value_type, Args&&...>)
    {
        size_type head, tail;
        do
        {
            head = Head.load(std::memory_order_acquire);
            tail = Tail.load(std::memory_order_relaxed);
            if (tail - head >= Capacity)
            {
                return false;
            }
        } while (!Tail.compare_exchange_weak(tail, tail + 1,
                                                std::memory_order_acq_rel,
                                                std::memory_order_relaxed));

        const size_type idx = tail & Mask;
        allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]),
                                    std::forward<Args>(InArgs)...);
        m_Ready[idx].store(1, std::memory_order_release);
        return true;
    }

    std::optional<value_type> tryPop() noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type idx = head & Mask;

        if (m_Ready[idx].load(std::memory_order_acquire) == 0)
        {
            return std::nullopt;
        }

        value_type ret = std::move(InnerBuffer[idx]);
        allocator_traits::destroy(Alloc, std::addressof(InnerBuffer[idx]));
        m_Ready[idx].store(0, std::memory_order_release);
        Head.store(head + 1, std::memory_order_release);
        return ret;
    }

    void clear() noexcept
    {
        // 前提：调用 clear 前必须保证所有生产者已停止，否则可能漏删正在构造的元素
        while (auto val = tryPop())
        {
            // 弹出并析构所有已发布元素
        }
    }

    [[nodiscard]] bool isEmpty() const noexcept
    {
        return Head.load(std::memory_order_relaxed) == Tail.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isFull() const noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_relaxed);
        return tail - head == Capacity;
    }

    [[nodiscard]] size_type getSize() const noexcept
    {
        const size_type head = Head.load(std::memory_order_relaxed);
        const size_type tail = Tail.load(std::memory_order_relaxed);
        return tail - head;
    }

    [[nodiscard]] size_type getCapacity() const noexcept { return Capacity; }

    [[nodiscard]] std::pair<std::span<T>, std::span<T>> getLinearizeViews() noexcept
    {
        static_assert([]{
            return true;
        }(), "getLinearizeViews() only safe when all threads are stopped");

        const size_type h = Head.load(std::memory_order_relaxed);
        const size_type t = Tail.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<T>(InnerBuffer + h_idx, first_len),
            std::span<T>(InnerBuffer, sz - first_len)
        };
    }

    [[nodiscard]] std::pair<std::span<const T>, std::span<const T>> getLinearizeViews() const noexcept
    {
        static_assert([]{
            return true;
        }(), "getLinearizeViews() only safe when all threads are stopped");

        const size_type h = Head.load(std::memory_order_relaxed);
        const size_type t = Tail.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<const T>(InnerBuffer + h_idx, first_len),
            std::span<const T>(InnerBuffer, sz - first_len)
        };
    }
private:
    alignas(CacheLineSize) std::atomic<size_type> Head{0};
    alignas(CacheLineSize) std::atomic<size_type> Tail{0};

    size_type Capacity;
    size_type Mask;
    pointer InnerBuffer;
    Allocator Alloc;

    // 每个槽位的就绪标志：0 - 空闲, 1 - 已发布有效元素
    std::unique_ptr<std::atomic<unsigned char>[]> m_Ready;
};

template <typename T, typename Allocator>
class RingBuffer<T, Allocator, ERingBufferPolicy::MPMC>
{
public:
    using value_type       = T;
    using allocator_type   = Allocator;
    using allocator_traits = std::allocator_traits<Allocator>;
    using size_type        = std::size_t;
    using difference_type  = allocator_traits::difference_type;
    using pointer          = typename allocator_traits::pointer;
    using const_pointer    = typename allocator_traits::const_pointer;
    using reference        = value_type&;
    using const_reference  = const value_type&;

private:
    template <bool IsConst>
    class iterator_impl 
    {
    };

public:
    using iterator               = iterator_impl<false>;
    using const_iterator         = iterator_impl<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    explicit RingBuffer(size_type InCapacity, const Allocator& InAlloc = Allocator{})
        : Capacity(InCapacity)
        , Mask(InCapacity - 1)
        , Alloc(InAlloc)
        , Sequence(new std::atomic<size_type>[InCapacity])
    {
        if (Capacity == 0 || !isPowerOf2(Capacity))
        {
            throw std::invalid_argument("Capacity must be A power of 2 and > 0");
        }


        InnerBuffer = allocator_traits::allocate(Alloc, Capacity);

        // 序列号初始化为槽位索引：槽位 i 期望的写入序列号就是 i
        for (size_type i = 0; i < Capacity; ++i)
        {
            Sequence[i].store(i, std::memory_order_relaxed);
        }
    }

    ~RingBuffer()
    {
        clear();
        if (InnerBuffer != nullptr)
        {
        	allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
        }
    }

    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

        RingBuffer(RingBuffer&& Other)
        : Capacity(Other.Capacity)
        , Mask(Other.Mask)
        , InnerBuffer(Other.InnerBuffer)
        , Sequence(std::move(Other.Sequence))
    {
        if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
            Alloc = std::move(Other.Alloc);
        else
        {
            if (Other.Alloc != Allocator{})
                throw std::logic_error("Cannot move-construct RingBuffer with non-equal, non-propagating allocators");
        }

        EnqueuePos.store(Other.EnqueuePos.load(std::memory_order_relaxed), std::memory_order_relaxed);
        DequeuePos.store(Other.DequeuePos.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Other.Capacity = 0;
        Other.Mask = 0;
        Other.InnerBuffer = nullptr;
        Other.EnqueuePos.store(0, std::memory_order_relaxed);
        Other.DequeuePos.store(0, std::memory_order_relaxed);
    }

    // 移动赋值
    RingBuffer& operator=(RingBuffer&& Other) 
        noexcept (allocator_traits::is_always_equal::value && std::is_nothrow_move_assignable_v<Allocator>)
    {
        if (this != &Other)
        {
            clear();
            if (InnerBuffer != nullptr)
            {
            	allocator_traits::deallocate(Alloc, InnerBuffer, Capacity);
            }

            if constexpr (allocator_traits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                Sequence = std::move(Other.Sequence);
            }
            else
            {
                if (Alloc != Other.Alloc)
                {
                    throw std::runtime_error("Cannot move-assign RingBuffer with different allocators");
                }
    
                Capacity = Other.Capacity;
                Mask = Other.Mask;
                InnerBuffer = Other.InnerBuffer;
                Sequence = std::move(Other.Sequence);
            }

            EnqueuePos.store(Other.EnqueuePos.load(std::memory_order_relaxed), std::memory_order_relaxed);
            DequeuePos.store(Other.DequeuePos.load(std::memory_order_relaxed), std::memory_order_relaxed);

            Other.Capacity = 0;
            Other.Mask = 0;
            Other.InnerBuffer = nullptr;
            Other.EnqueuePos.store(0, std::memory_order_relaxed);
            Other.DequeuePos.store(0, std::memory_order_relaxed);
        }
        return *this;
    }

    bool tryPush(const value_type& value)
        noexcept(std::is_nothrow_copy_constructible_v<value_type>)
    {
        size_type pos = EnqueuePos.load(std::memory_order_relaxed);
        while(true)
        {
            const size_type idx = pos & Mask;
            const size_type seq = Sequence[idx].load(std::memory_order_acquire);

            if (seq != pos)                     // 槽位不空闲（满）
            {
                return false;
            }

            // 尝试原子申请该写入位置
            if (EnqueuePos.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            {
                // 成功占用槽位 idx
                allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), value);
                Sequence[idx].store(pos + 1, std::memory_order_release);
                return true;
            }
            // CAS 失败：pos 自动更新为最新值，重试
        }
    }

    bool tryPush(value_type&& Value)
        noexcept(std::is_nothrow_move_constructible_v<value_type>)
    {
        size_type pos = EnqueuePos.load(std::memory_order_relaxed);
        while(true)
        {
            const size_type idx = pos & Mask;
            const size_type seq = Sequence[idx].load(std::memory_order_acquire);

            if (seq != pos)
            {
                return false;
            }

            if (EnqueuePos.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            {
                allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]), std::move(Value));
                Sequence[idx].store(pos + 1, std::memory_order_release);
                return true;
            }
        }
    }

    template <typename... Args>
    bool try_emplace(Args&&... InArgs)
        noexcept(std::is_nothrow_constructible_v<value_type, Args&&...>)
    {
        size_type pos = EnqueuePos.load(std::memory_order_relaxed);
        while(true)
        {
            const size_type idx = pos & Mask;
            const size_type seq = Sequence[idx].load(std::memory_order_acquire);

            if (seq != pos)
            {
                return false;
            }

            if (EnqueuePos.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            {
                allocator_traits::construct(Alloc, std::addressof(InnerBuffer[idx]),
                                            std::forward<Args>(InArgs)...);
                Sequence[idx].store(pos + 1, std::memory_order_release);
                return true;
            }
        }
    }

    std::optional<value_type> tryPop() noexcept
    {
        size_type pos = DequeuePos.load(std::memory_order_relaxed);
        while(true)
        {
            const size_type idx = pos & Mask;
            const size_type seq = Sequence[idx].load(std::memory_order_acquire);

            if (seq != pos + 1)                  // 槽位未就绪（空）
            {
                return std::nullopt;
            }

            if (DequeuePos.compare_exchange_weak(pos, pos + 1,
                                                   std::memory_order_relaxed,
                                                   std::memory_order_relaxed))
            {
                // 成功占用消费权
                value_type ret = std::move(InnerBuffer[idx]);
                allocator_traits::destroy(Alloc, std::addressof(InnerBuffer[idx]));

                // 释放槽位：下一个写入该槽位的生产者将使用 pos + Capacity
                Sequence[idx].store(pos + Capacity, std::memory_order_release);
                return ret;
            }
        }
    }

    [[nodiscard]] bool isEmpty() const noexcept
    {
        return EnqueuePos.load(std::memory_order_relaxed) ==
               DequeuePos.load(std::memory_order_relaxed);
    }

    [[nodiscard]] bool isFull() const noexcept
    {
        const size_type enq = EnqueuePos.load(std::memory_order_relaxed);
        const size_type deq = DequeuePos.load(std::memory_order_relaxed);
        return (enq - deq) >= Capacity;
    }

    [[nodiscard]] size_type getSize() const noexcept
    {
        const size_type enq = EnqueuePos.load(std::memory_order_relaxed);
        const size_type deq = DequeuePos.load(std::memory_order_relaxed);
        return enq - deq;
    }

    [[nodiscard]] size_type getCapacity() const noexcept { return Capacity; }

    void clear() noexcept
    {
        // 要求所有生产者/消费者均停止
        while (auto val = tryPop()){ }
        
    }

    [[nodiscard]] std::pair<std::span<T>, std::span<T>> getLinearizeViews() noexcept
    {
        static_assert([]{ return true; }(), "getLinearizeViews() only safe when all threads are stopped");

        const size_type h = DequeuePos.load(std::memory_order_relaxed);
        const size_type t = EnqueuePos.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<T>(InnerBuffer + h_idx, first_len),
            std::span<T>(InnerBuffer, sz - first_len)
        };
    }

    [[nodiscard]] std::pair<std::span<const T>, std::span<const T>> getLinearizeViews() const noexcept
    {
        static_assert([]{ return true; }(), "getLinearizeViews() only safe when all threads are stopped");

        const size_type h = DequeuePos.load(std::memory_order_relaxed);
        const size_type t = EnqueuePos.load(std::memory_order_relaxed);
        const size_type sz = t - h;
        if (sz == 0) return {};

        const size_type h_idx = h & Mask;
        const size_type first_len = std::min(Capacity - h_idx, sz);
        return {
            std::span<const T>(InnerBuffer + h_idx, first_len),
            std::span<const T>(InnerBuffer, sz - first_len)
        };
    }
private:
    alignas(CacheLineSize) std::atomic<size_type> EnqueuePos{0};
    alignas(CacheLineSize) std::atomic<size_type> DequeuePos{0};

    size_type Capacity;
    size_type Mask;
    pointer InnerBuffer;
    Allocator Alloc;

    // 每个槽位的状态序列号，用于协调生产者与消费者
    std::unique_ptr<std::atomic<size_type>[]> Sequence;
};

// SPSC
template<typename T>
concept TriviallyCopyableType = std::is_trivially_copyable_v<T>;

template <TriviallyCopyableType Ty, size_t Capacity>
class DoubleBuffer
{
public:
    DoubleBuffer() = default;
    DoubleBuffer(const DoubleBuffer&) = delete;
    DoubleBuffer& operator=(const DoubleBuffer&) = delete;
    DoubleBuffer(DoubleBuffer&& Other) noexcept = delete;
    DoubleBuffer& operator=(DoubleBuffer&& Other) noexcept = delete;
    ~DoubleBuffer() = default;

    std::span<Ty, Capacity> getWritingBuffer()
    {
        return std::span { InnerBuffers[WriteIdx] };
    }

    [[nodiscard]] bool isCommitted() const noexcept
    {
        return SwapPending.load(std::memory_order_acquire);
    }

    // 生产者填充缓冲区, 提交交换请求
    bool commit() noexcept
    {
        if (isCommitted()) [[unlikely]]
        {
            return false;
        }
        const uint64_t new_id = FrameId.fetch_add(1, std::memory_order_relaxed) + 1;

        SwapPending.store(true, std::memory_order_release);
        LastWriteFrameId = new_id;
        return true;
    }
    
    uint64_t getCurrentWriteFrameId() const noexcept
    {
        return LastWriteFrameId;
    }

    void clearWriteBuffer() noexcept
    {
        std::memset(InnerBuffers[WriteIdx].data(), 0, sizeof(InnerBuffers[WriteIdx]));
    }
    // ============== Consumer 消费者接口 =============================
    bool trySwap() noexcept
    {
        if (!SwapPending.load(std::memory_order_acquire))
        {
            return false;
        }
        
        WriteIdx = 1 - WriteIdx;
        LastReadFrameId = LastWriteFrameId;
        SwapPending.store(false, std::memory_order_release);
        return true;
    }
    
    std::span<const Ty, Capacity> get_read_buffer() const noexcept
    {
        return std::span { InnerBuffers[1-WriteIdx] };
    }

    uint64_t current_read_frame_id() const noexcept
    {
        return LastReadFrameId;
    }


    bool has_new_frame() const noexcept
    {
        const uint64_t latest = FrameId.load(std::memory_order_acquire);
        return latest > LastReadFrameId;
    }

    void clear_read_buffer() noexcept
    {
        std::memset(InnerBuffers[1-WriteIdx].data(), 0, sizeof(InnerBuffers[1-WriteIdx]));
    }

private:
    std::array<Ty, Capacity> InnerBuffers[2];
    size_t WriteIdx = 0;
    alignas(CacheLineSize) std::atomic<bool> SwapPending { false };
    alignas(CacheLineSize) std::atomic<uint64_t> FrameId {};

    uint64_t LastWriteFrameId {0}, LastReadFrameId {0};
};
/*
void produce(DoubleBuffer<float, 64>& buffer)
{
    float val = 0.0;
    for (size_t frame = 0; frame < 100; ++frame)
    {
        auto write_buffer = buffer.getWritingBuffer();
        for (size_t index = 0; index < 64; ++index)
        {
            write_buffer[index] = val++;
        }
        buffer.commit();
        std::cout << "[Producer] 提交第 " << frame << " 帧\N";
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void consume(DoubleBuffer<float, 64>& buffer)
{
    size_t read_frames = 0;
    while (read_frames < 100)
    {
        if (buffer.trySwap())
        {
            auto read_span = buffer.get_read_buffer();
            float first = read_span[0];
            float last = read_span.back();
            std::cout << "[Consumer] 读取新帧, 首值=" << first << " 末值=" << last << "\N";
            read_frames++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main()
{
    DoubleBuffer<float, 64> double_buffer_;
    std::jthread prod (produce, std::ref(double_buffer_));
    std::jthread cons (consume, std::ref(double_buffer_));
    return 0;
}
*/
}
