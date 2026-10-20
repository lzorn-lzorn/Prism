
#pragma once

#include <functional>
#include <type_traits>
#include <utility>
#include <atomic>
#include <concepts>

#include "infra/common/Common.hpp"

namespace infra::details
{

// 值存储层：独立，仅负责持有 Ty
template <typename Ty>
class Value_Storage
{
public:
    // 默认构造：仅 Ty 可默认构造时可用
    constexpr Value_Storage()
        requires std::default_initializable<Ty>
        : Value()
    {}

    // 原位构造
    template <typename... Args>
        requires std::constructible_from<Ty, Args...>
    constexpr explicit Value_Storage(std::in_place_t, Args&&... InArgs)
        : Value(std::forward<Args>(InArgs)...)
    {}

    // 平凡特殊成员
    constexpr ~Value_Storage() = default;
    constexpr Value_Storage(const Value_Storage&) = default;
    constexpr Value_Storage(Value_Storage&&) = default;
    constexpr Value_Storage& operator=(const Value_Storage&) = default;
    constexpr Value_Storage& operator=(Value_Storage&&) = default;

    [[nodiscard]] constexpr Ty& getValue() noexcept { return Value; }
    [[nodiscard]] constexpr const Ty& getValue() const noexcept { return Value; }
    [[nodiscard]] constexpr bool hasValue() const noexcept { return true; }

protected:
    constexpr Ty& Assign(const Ty& InValue)
    {
        Value = InValue;
        return Value;
    }
    constexpr Ty& Assign(Ty&& InValue)
    {
        if constexpr (std::is_move_assignable_v<Ty>)
            Value = std::move(InValue);
        else
            Value = InValue;   // 回退到拷贝赋值
        return Value;
    }

private:
    Ty Value;
};

template <typename Fn>
class Observer_List
{
private:
    struct Node_t
    {
        template <typename Callable>
        explicit Node_t(Callable&& F) : func(std::forward<Callable>(F)) {}
        Node_t() = default;

        Fn func;
        std::atomic<Node_t*> next{nullptr};
    };

    static_assert(std::default_initializable<Fn>,
        "Observer_List: Fn must be default constructible");

public:
    Observer_List()
    {
        Node_t* dummy = new Node_t{};
        Head.store(dummy, std::memory_order_relaxed);
        Tail.store(dummy, std::memory_order_relaxed);
    }

    ~Observer_List()
    {
        Clear();
        delete Head.load(std::memory_order_relaxed);
    }

    Observer_List(const Observer_List& Other)
    {
        Node_t* dummy = new Node_t{};
        Head.store(dummy, std::memory_order_relaxed);
        Tail.store(dummy, std::memory_order_relaxed);

        Node_t* cur = Other.Head.load(std::memory_order_acquire)->next.load(std::memory_order_acquire);
        while (cur)
        {
            pushBack(cur->func);
            cur = cur->next.load(std::memory_order_acquire);
        }
    }

    Observer_List(Observer_List&& Other) noexcept
        : Head(Other.Head.load(std::memory_order_relaxed))
        , Tail(Other.Tail.load(std::memory_order_relaxed))
    {
        Node_t* dummy = new Node_t{};
        Other.Head.store(dummy, std::memory_order_relaxed);
        Other.Tail.store(dummy, std::memory_order_relaxed);
    }

    Observer_List& operator=(const Observer_List& Other)
    {
        if (this == &Other) return *this;
        Observer_List temp(Other);
        swap(temp);
        return *this;
    }

    Observer_List& operator=(Observer_List&& Other) noexcept
    {
        if (this == &Other) return *this;
        Clear();
        delete Head.load(std::memory_order_relaxed);

        Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Node_t* dummy = new Node_t{};
        Other.Head.store(dummy, std::memory_order_relaxed);
        Other.Tail.store(dummy, std::memory_order_relaxed);
        return *this;
    }

    void pushBack(const Fn& F)
    {
        appendNode(new Node_t(F));
    }
    void pushBack(Fn&& F)
    {
        appendNode(new Node_t(std::move(F)));
    }

    template <typename Callback>
    void forEach(Callback&& InCallBack) const
    {
        Node_t* cur = Head.load(std::memory_order_acquire)->next.load(std::memory_order_acquire);
        while (cur)
        {
            std::forward<Callback>(InCallBack)(cur->func);
            cur = cur->next.load(std::memory_order_acquire);
        }
    }

    void Clear() noexcept
    {
        Node_t* dummy = Head.load(std::memory_order_relaxed);
        Node_t* cur = dummy->next.load(std::memory_order_relaxed);
        while (cur)
        {
            Node_t* next = cur->next.load(std::memory_order_relaxed);
            delete cur;
            cur = next;
        }
        dummy->next.store(nullptr, std::memory_order_relaxed);
        Tail.store(dummy, std::memory_order_relaxed);
    }

    void swap(Observer_List& Other) noexcept
    {
        // 不能使用 std::swap(Head, Other.Head) —— std::atomic 不可移动
        Node_t* my_head = Head.load(std::memory_order_relaxed);
        Node_t* my_tail = Tail.load(std::memory_order_relaxed);

        Head.store(Other.Head.load(std::memory_order_relaxed), std::memory_order_relaxed);
        Tail.store(Other.Tail.load(std::memory_order_relaxed), std::memory_order_relaxed);

        Other.Head.store(my_head, std::memory_order_relaxed);
        Other.Tail.store(my_tail, std::memory_order_relaxed);
    }

private:
    void appendNode(Node_t* Node) noexcept
    {
        Node->next = nullptr;
        while (true)
        {
            Node_t* tail = Tail.load(std::memory_order_acquire);
            Node_t* next = tail->next.load(std::memory_order_acquire);
            if (tail == Tail.load(std::memory_order_acquire))
            {
                if (!next)
                {
                    if (tail->next.compare_exchange_weak(next, Node, std::memory_order_release))
                    {
                        Tail.compare_exchange_strong(tail, Node, std::memory_order_release);
                        return;
                    }
                }
                else
                {
                    Tail.compare_exchange_weak(tail, next, std::memory_order_release);
                }
            }
        }
    }

    std::atomic<Node_t*> Head{nullptr};
    std::atomic<Node_t*> Tail{nullptr};
};;

} // namespace infra::details
namespace infra
{
template <typename Ty>
class Observer
    : private details::Value_Storage<Ty>
    , private SMF_AutoControl<Ty>
{
public:
    // 1. 类型别名（前置定义，解决依赖报错）
    using value_type        = std::remove_cv_t<Ty>;
    using smf_ctrl          = SMF_AutoControl<Ty>;
    using observer_type     = std::function<void(const value_type&)>;
    using observer_list     = details::Observer_List<observer_type>;
    using storage_type      = details::Value_Storage<Ty>;

    // ===================== 构造函数 =====================
    constexpr Observer() requires std::default_initializable<Ty> : storage_type() {}

    // 拷贝构造
    constexpr Observer(const Observer& Other)
        requires smf_ctrl::enable_copy_ctor
        : storage_type(Other), Observers() {}

    Observer(const Observer&) requires (!smf_ctrl::enable_copy_ctor) = delete;

    // 移动构造
    constexpr Observer(Observer&& Other) noexcept
        requires smf_ctrl::enable_move_ctor
        : storage_type(std::move(Other)), Observers() {}

    Observer(Observer&&) requires (!smf_ctrl::enable_move_ctor) = delete;

    // 拷贝赋值
    constexpr Observer& operator=(const Observer& Other)
        requires (smf_ctrl::enable_copy_ctor && smf_ctrl::enable_copy_assign)
    {
        if (this != &Other) {
            this->Assign(Other.Value());
            notify();
        }
        return *this;
    }

    Observer& operator=(const Observer&) 
        requires (!(smf_ctrl::enable_copy_ctor && smf_ctrl::enable_copy_assign)) = delete;

    // 移动赋值
    constexpr Observer& operator=(Observer&& Other) noexcept
        requires (smf_ctrl::enable_move_ctor && smf_ctrl::enable_move_assign)
    {
        if (this != &Other) {
            this->Assign(std::move(Other.Value()));
            notify();
        }
        return *this;
    }

    Observer& operator=(Observer&&) 
        requires (!(smf_ctrl::enable_move_ctor && smf_ctrl::enable_move_assign)) = delete;

    // 直接用 Ty 构造
    Observer(const Ty& InValue)
        : storage_type(std::in_place, InValue)
    {}
    Observer(Ty&& InValue)
        : storage_type(std::in_place, std::move(InValue))
    {}

    // 原位构造
    template <typename... Args>
        requires std::constructible_from<Ty, Args...>
    Observer(std::in_place_t, Args&&... InArgs)
        : storage_type(std::in_place, std::forward<Args>(InArgs)...)
    {}

    // ===================== 赋值重载 =====================
    Observer& operator=(const Ty& InValue)
    {
        set_value(InValue);
        return *this;
    }
    Observer& operator=(Ty&& InValue)
    {
        set_value(std::move(InValue));
        return *this;
    }

    // ===================== 订阅接口（修复签名） =====================
    Observer& subscribe(const observer_type& Obs)
    {
        Observers.pushBack(Obs);
        return *this;
    }
    Observer& subscribe(observer_type&& Obs)
    {
        Observers.pushBack(std::move(Obs));
        return *this;
    }

    // ===================== 值修改 & 通知 =====================
    Observer& set_value(const Ty& InValue)
    {
        this->Assign(InValue);
        notify();
        return *this;
    }
    Observer& set_value(Ty&& InValue)
    {
        this->Assign(std::move(InValue));
        notify();
        return *this;
    }

  	template <typename Fn, typename... Args>
        requires std::invocable<Fn, Ty&, Args...>
    Observer& modify(Fn&& F, Args&&... InArgs)
        noexcept(std::is_nothrow_invocable_v<Fn, Ty&, Args...>)
    {
        std::invoke(std::forward<Fn>(F), this->Value(), std::forward<Args>(InArgs)...);
        notify();
        return *this;
    }

    // ===================== 取值接口 =====================
    [[nodiscard]] const Ty& value() const noexcept
    {
        return this->Value();
    }
    [[nodiscard]] bool has_value() const noexcept
    {
        return storage_type::has_value();
    }

private:
    // 通知所有观察者
    void notify() const
    {
        Observers.forEach([this](const observer_type& Obs)
        {
            Obs(this->value());
        });
    }

    observer_list Observers;
};

} // namespace infra