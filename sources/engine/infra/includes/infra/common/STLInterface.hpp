
#pragma once

/**
 * @note: 对于 C++23 的 std::container_traits 需要手动特化:
```cpp
namespace std
{
    template<>
    struct container_traits<MyContainer<T>>
    {
        // 是否连续存储（vector=true, list=false）
        static constexpr bool is_contiguous = false;
        // 是否有序关联容器 set/map
        static constexpr bool is_ordered = false;
        // 是否无序关联容器 unordered_set
        static constexpr bool is_unordered = false;
        // 是否支持随机访问迭代器
        static constexpr bool supports_random_access = false;
        // 是否支持 O(1) 尾部插入 emplace_back/push_back
        static constexpr bool supports_back_inserter = true;
        // 是否支持头部 push_front
        static constexpr bool supports_front_inserter = false;
    };
}
```
 * @note 对于形如 `std::copy(src.begin(), src.end(), std::back_inserter(cont));`
 *       类型的支持, 需要提供以下成员:
 * 		- `back_inserter` -> `push_back(value_type)`
 * 		- `front_inserter` -> `push_front(value_type)`
 * 		- `inserter(it, val)` -> `insert(iterator pos, value_type)`
 */

#define CompatibilityLayer_STLContainer(STL_Container_Type, STL_Container_Value)\
	using value_type = typename STL_Container_Type::value_type; \
	using size_type = typename STL_Container_Type::size_type; \
	using difference_type = typename STL_Container_Type::difference_type; \
	using reference = typename STL_Container_Type::reference; \
	using const_reference = typename STL_Container_Type::const_reference; \
	using pointer = typename STL_Container_Type::pointer; \
	using const_pointer = typename STL_Container_Type::const_pointer; \
	using iterator =  STL_Container_Type::iterator;        \
	using const_iterator = typename STL_Container_Type::const_iterator; \
	using reverse_iterator = typename STL_Container_Type::reverse_iterator; \
	using const_reverse_iterator = typename STL_Container_Type::const_reverse_iterator; \
	\
	iterator begin() noexcept { return STL_Container_Value.begin(); } \
	iterator end() noexcept { return STL_Container_Value.end(); } \
	const_iterator begin() const noexcept { return STL_Container_Value.begin(); } \
	const_iterator end() const noexcept { return STL_Container_Value.end(); } \
	const_iterator cbegin() const noexcept { return STL_Container_Value.cbegin(); } \
	const_iterator cend() const noexcept { return STL_Container_Value.cend(); } \
	reverse_iterator rbegin() noexcept { return STL_Container_Value.rbegin(); } \
	reverse_iterator rend() noexcept { return STL_Container_Value.rend(); } \
	const_reverse_iterator rbegin() const noexcept { return STL_Container_Value.rbegin(); } \
	const_reverse_iterator rend() const noexcept { return STL_Container_Value.rend(); } \
	const_reverse_iterator crbegin() const noexcept { return STL_Container_Value.crbegin(); } \
	const_reverse_iterator crend() const noexcept { return STL_Container_Value.crend(); } \
