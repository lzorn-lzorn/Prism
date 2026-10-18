#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <type_traits>


constexpr size_t MaxSize = 8192;

template <typename T>
consteval bool ValidateCharType() {
    static_assert(std::is_object_v<T>, "Ty is not an object type. It may be void, a function type, or a reference.");
    static_assert(!std::is_array_v<T>, "Ty is an array type, which is not allowed.");
    static_assert(std::is_standard_layout_v<T>,
        "Ty is not standard layout. It may have virtual functions, "
        "mixed access specifiers, or non-standard-layout members.");
    static_assert(std::is_trivially_copyable_v<T>,
        "Ty is not trivially copyable. Maybe you have:\n"
        "  1. custom copy/move constructor or copy/move assignment\n"
        "  2. virtual function\n"
        "  3. non-trivial destructor");
    static_assert(std::is_trivially_default_constructible_v<T>,
        "Ty is not trivially default-constructible. Maybe you have:\n"
        "  1. custom default constructor\n"
        "  2. virtual function\n"
        "  3. reference member which deletes the default constructor\n"
        "  4. default member initializers");
    return true;
}

template <typename Ty>
concept CharType = ValidateCharType<Ty>();

template <CharType Ty, size_t Size, typename Traits = std::char_traits<Ty>>
class FixedString
{
    static_assert(Size <= MaxSize, "Size is too large");
    static_assert(Size > 0, "Size equals 0 is not allowed");
    static_assert(std::is_same_v<Ty, typename Traits::char_type>, "first template param Ty is not equal Traits::char_type");
public:
    constexpr static size_t max_size = Size;

    using value_type = Ty;
    using traits_type = Traits;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = Ty*;
    using const_pointer = const Ty*;
    using reference = Ty&;
    using const_reference = const Ty&;

    using iterator = std::array<Ty, max_size>::iterator;
    using const_iterator = std::array<Ty, max_size>::const_iterator;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    FixedString() = default;
    FixedString(const Ty* Chars, size_t Length) noexcept { append(Chars, Length); }
    FixedString(const std::basic_string<Ty>& String) noexcept { append(String.data(), String.size()); }
    FixedString(std::string_view StringView) noexcept { append(StringView.data(), StringView.size()); }
    ~FixedString() = default;
    FixedString(const FixedString& Other) = default;
    FixedString(FixedString&& Other) = default;

    FixedString& operator=(const FixedString& Other) = default;
    FixedString& operator=(FixedString&& Other) = default;

    bool operator==(const FixedString& Other) const noexcept;

    [[nodiscard]] constexpr iterator begin() noexcept { return Data.begin(); }
    [[nodiscard]] constexpr iterator end() noexcept { return Data.end(); }
    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept { return Data.cbegin(); }
    [[nodiscard]] constexpr const_iterator cend() const noexcept { return Data.cend(); }
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    [[nodiscard]] constexpr reference at(size_type Position);
    [[nodiscard]] constexpr const_reference at(size_type Position) const;
    [[nodiscard]] constexpr reference operator[](size_type Position) noexcept;
    [[nodiscard]] constexpr const_reference operator[](size_type Position) const noexcept;

    [[nodiscard]] constexpr reference front() noexcept { return Data.front(); }
    [[nodiscard]] constexpr const_reference front() const noexcept { return Data.front(); }
    [[nodiscard]] constexpr reference back() noexcept { return Data.back(); }
    [[nodiscard]] constexpr const_reference back() const noexcept { return Data.back(); }

    void clear() noexcept { Data.fill(Ty{}); }
public:
    FixedString& append(const Ty* Chars, size_t Length) noexcept 
    {
        if (Length + CurrentLength <= max_size)
        {
            std::copy_n(Chars, Length, Data.begin() + CurrentLength);
            CurrentLength += Length;
        }
        return *this;
    }
    FixedString& append(const std::basic_string<Ty>& String) noexcept
    {
        return append(String.data(), String.size());
    }
    FixedString& append(std::string_view StringView) noexcept
    {
        return append(StringView.data(), StringView.size());
    }

    template <class AllocTy = std::allocator<Ty>>
    [[nodiscard]] std::basic_string<Ty, traits_type, AllocTy> toString() const
    {
        return std::basic_string<Ty, traits_type, AllocTy>(Data.data(), CurrentLength);
    }

    // @param StatefulAllocator 有状态内存分配器
    template <class StatefulAllocTy>
    [[nodiscard]] std::basic_string<Ty, traits_type, StatefulAllocTy> toString(StatefulAllocTy StatefulAllocator) const;
private:
    size_t CurrentLength { 0 };
    std::array<Ty, max_size> Data;
};

template <CharType Ty, size_t Size, typename Traits = std::char_traits<Ty>>
class StackedString
{
    static_assert(Size <= MaxSize, "Size is too large");
    static_assert(Size > 0, "Size equals 0 is not allowed");
    static_assert(std::is_same_v<Ty, typename Traits::char_type>, "first template param Ty is not equal Traits::char_type");
public:
    constexpr static size_t max_size = Size;

    using value_type = Ty;
    using traits_type = Traits;
    using size_type = size_t;
    using difference_type = ptrdiff_t;
    using pointer = Ty*;
    using const_pointer = const Ty*;
    using reference = Ty&;
    using const_reference = const Ty&;

    using iterator = std::array<Ty, max_size>::iterator;
    using const_iterator = std::array<Ty, max_size>::const_iterator;
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    StackedString() = default;
    ~StackedString() = default;
    StackedString(const StackedString& Other) = default;
    StackedString(StackedString&& Other) = default;

    StackedString& operator=(const StackedString& Other) = default;
    StackedString& operator=(StackedString&& Other) = default;

    bool operator==(const StackedString& Other) const noexcept;

    [[nodiscard]] constexpr iterator begin() noexcept;
    [[nodiscard]] constexpr iterator end() noexcept;
    [[nodiscard]] constexpr reverse_iterator rbegin() noexcept;
    [[nodiscard]] constexpr reverse_iterator rend() noexcept;
    [[nodiscard]] constexpr const_iterator cbegin() const noexcept;
    [[nodiscard]] constexpr const_iterator cend() const noexcept;
    [[nodiscard]] constexpr const_reverse_iterator crbegin() const noexcept;
    [[nodiscard]] constexpr const_reverse_iterator crend() const noexcept;

    [[nodiscard]] constexpr reference at(size_type Position);
    [[nodiscard]] constexpr const_reference at(size_type Position) const;
    [[nodiscard]] constexpr reference operator[](size_type Position) noexcept;
    [[nodiscard]] constexpr const_reference operator[](size_type Position) const noexcept;

    [[nodiscard]] constexpr reference front() noexcept;
    [[nodiscard]] constexpr const_reference front() const noexcept;
    [[nodiscard]] constexpr reference back() noexcept;
    [[nodiscard]] constexpr const_reference back() const noexcept;

    void clear() noexcept;
public:
    StackedString& append(const Ty* Chars, size_t Length) noexcept;
    StackedString& append(const std::basic_string<Ty>& String) noexcept;
    StackedString& append(std::string_view StringView) noexcept;

    bool push(const Ty* Chars, size_t Length) noexcept;
    bool push(const std::basic_string<Ty>& String) noexcept;
    bool push(std::string_view StringView) noexcept;

    void pop() noexcept;
    [[nodiscard]] std::basic_string_view<Ty> top() const noexcept;

    template <class AllocTy = std::allocator<Ty>>
    [[nodiscard]] std::basic_string<Ty, traits_type, AllocTy> toString() const;

    // @param StatefulAllocator 有状态内存分配器
    template <class StatefulAllocTy>
    [[nodiscard]] std::basic_string<Ty, traits_type, StatefulAllocTy> toString(StatefulAllocTy StatefulAllocator) const;

private:
    template <class AllocTy>
    using StringTy = std::basic_string<Ty, traits_type, AllocTy>;

    template <class AllocTy>
    using StringAllocTy = typename std::allocator_traits<AllocTy>
                              ::template rebind_alloc<StringTy<AllocTy>>;

    template <class AllocTy>
    using StringVectorTy = std::vector<StringTy<AllocTy>, StringAllocTy<AllocTy>>;
public:
    template <class AllocTy = std::allocator<Ty>>
    [[nodiscard]] StringVectorTy<AllocTy> toStrings() const;

    // @param StatefulAllocator 有状态内存分配器
    // @note StatefulAllocator rebind 出来的分配器必须共享同一个 arena
    template <class StatefulAllocTy>
    [[nodiscard]] StringVectorTy<StatefulAllocTy> toStrings(StatefulAllocTy StatefulAllocator) const;
private:
    std::array<Ty, max_size> Data;
    std::array<size_t, max_size> CheckPoints;
};

class StringBuilder
{

};
