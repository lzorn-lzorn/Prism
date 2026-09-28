#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace core
{

inline constexpr std::size_t DefaultDelegateInlineSize = 32;

struct DelegateHandle
{
    std::uint64_t OwnerId = 0;
    std::uint32_t SlotIndex = 0;
    std::uint32_t Generation = 0;

    [[nodiscard]]
    constexpr bool isValid() const noexcept
    {
        return OwnerId != 0 && Generation != 0;
    }

    constexpr void reset() noexcept
    {
        OwnerId = 0;
        SlotIndex = 0;
        Generation = 0;
    }

    [[nodiscard]]
    constexpr bool operator==(const DelegateHandle&) const noexcept = default;
};

struct DefaultDelegateAllocatePolicy
{
    [[nodiscard]]
    static void* allocate(std::size_t Size, std::size_t Alignment)
    {
        return ::operator new(Size, std::align_val_t{Alignment});
    }

    static void deallocate(void* Memory, std::size_t Size, std::size_t Alignment) noexcept
    {
        ::operator delete(Memory, Size, std::align_val_t{Alignment});
    }
};

namespace details
{

[[nodiscard]]
inline std::uint64_t allocateDelegateOwnerId() noexcept
{
    // 该计数器是整个程序中所有 MulticastDelegate 共享的
    static std::atomic<std::uint64_t> next_owner_id{1};

    const std::uint64_t id =
        next_owner_id.fetch_add(1, std::memory_order_relaxed);

    if (id == 0) [[unlikely]]
    {
        std::terminate();
    }

    return id;
}

template <std::size_t InlineSize>
struct InlineStorage
{
    alignas(std::max_align_t)
    std::byte Data[InlineSize];
};

template <>
struct InlineStorage<0>
{
};

template <typename RetType, bool IsNoexcept, typename... Args>
using InvokerPointer = RetType(*)(void*, Args&&...) noexcept(IsNoexcept);

template <
	typename RetType, 
	bool IsNoexcept, 
	bool IsCopyable, 
	std::size_t InlineSize,
	typename AllocationPolicy,
	typename... Args
>
class DelegateStorage
{
private:
    using self_type = DelegateStorage<RetType, IsNoexcept, IsCopyable, InlineSize, AllocationPolicy, Args...>;
    using invoker_type = InvokerPointer<RetType, IsNoexcept, Args...>;
    using destroyer_type = void(*)(void*) noexcept;
    using mover_type = void*(*)(void*, void*) noexcept;
    using copier_type = void*(*)(void*, const void*);

    struct Operations
    {
        invoker_type Invoke = nullptr;
        destroyer_type Destroy = nullptr;
        mover_type Move = nullptr;
        copier_type Copy = nullptr;
		const void* TypeTag = nullptr;
    };

    template <typename Callable>
    static consteval bool callableCompatible()
    {
        using callable_type = std::decay_t<Callable>;

        if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<RetType, callable_type&, Args...>;
        }
        else
        {
            return std::is_invocable_r_v<RetType, callable_type&, Args...>;
        }
    }

    template <auto Callable>
    static consteval bool staticCallableCompatible()
    {
        if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<RetType, decltype(Callable), Args...>;
        }
        else
        {
            return std::is_invocable_r_v<RetType, decltype(Callable), Args...>;
        }
    }

    template <auto Method, typename ClassType>
    static consteval bool rawMethodCompatible()
    {
        if constexpr (!std::is_member_function_pointer_v<decltype(Method)>)
        {
            return false;
        }
        else if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<
                RetType,
                decltype(Method),
                ClassType*,
                Args...
            >;
        }
        else
        {
            return std::is_invocable_r_v<
                RetType,
                decltype(Method),
                ClassType*,
                Args...
            >;
        }
    }

    template <auto Method, typename ClassType>
    static consteval bool sharedMethodCompatible()
    {
        if constexpr (!std::is_member_function_pointer_v<decltype(Method)>)
        {
            return false;
        }
        else if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<
                RetType,
                decltype(Method),
                ClassType*,
                Args...
            >;
        }
        else
        {
            return std::is_invocable_r_v<
                RetType,
                decltype(Method),
                ClassType*,
                Args...
            >;
        }
    }

public:
    DelegateStorage() noexcept = default;
    DelegateStorage(std::nullptr_t) noexcept
    {
    }

    template <typename Callable>
        requires (
            !std::same_as<std::remove_cvref_t<Callable>, self_type>
            && self_type::callableCompatible<Callable>()
            && (!IsCopyable || std::copy_constructible<std::decay_t<Callable>>)
            && std::move_constructible<std::decay_t<Callable>>
        )
    explicit DelegateStorage(Callable&& InCallable)
    {
        bind(std::forward<Callable>(InCallable));
    }

    DelegateStorage(const DelegateStorage& Other)
        requires IsCopyable
    {
        copyFrom(Other);
    }

    DelegateStorage(const DelegateStorage&)
        requires (!IsCopyable)
        = delete;

    DelegateStorage(DelegateStorage&& Other) noexcept
    {
        moveFrom(std::move(Other));
    }

    ~DelegateStorage()
    {
        reset();
    }

    DelegateStorage& operator=(const DelegateStorage& Other)
        requires IsCopyable
    {
        if (this != std::addressof(Other))
        {
            // 先复制到临时对象，保证强异常安全. 
            DelegateStorage Replacement(Other);

            reset();
            moveFrom(std::move(Replacement));
        }

        return *this;
    }

    DelegateStorage& operator=(const DelegateStorage&)
        requires (!IsCopyable)
        = delete;

    DelegateStorage& operator=(DelegateStorage&& Other) noexcept
    {
        if (this != std::addressof(Other))
        {
            reset();
            moveFrom(std::move(Other));
        }

        return *this;
    }

    DelegateStorage& operator=(std::nullptr_t) noexcept
    {
        reset();
        return *this;
    }

    [[nodiscard]]
    bool bound() const noexcept
    {
        return Ops != nullptr;
    }

    [[nodiscard]]
    explicit operator bool() const noexcept
    {
        return bound();
    }

    void reset() noexcept
    {
        if (Ops != nullptr && Ops->Destroy != nullptr)
        {
            Ops->Destroy(Object);
        }

        clear();
    }

    RetType execute(Args... InArgs) const noexcept(IsNoexcept)
    {
        if (Ops == nullptr) [[unlikely]]
        {
            handleEmptyInvocation();
        }

        return Ops->Invoke(
            Object,
            std::forward<Args>(InArgs)...
        );
    }

    RetType operator()(Args... InArgs) const noexcept(IsNoexcept)
    {
        return execute(std::forward<Args>(InArgs)...);
    }

private:
	// 这个函数声明纯粹是为了兼容 clangd 静态分析器, 这里没有模板的前向申明, 当前版本的
	// clangd 无法分析出来 emplaceCallable 在之后被定义
    template <typename Callable>
    void emplaceCallable(Callable&& InCallable)
    {
        using callable_type = std::decay_t<Callable>;

		if constexpr (fitsInline<callable_type>())
		{
			void* destination = inlineData();

			std::construct_at(
				static_cast<callable_type*>(destination),
				std::forward<Callable>(InCallable)
			);

			Object = destination;
			Ops = inlineOperations<callable_type>();
		}
		else
		{
			void* memory = AllocationPolicy::allocate(
				sizeof(callable_type),
				alignof(callable_type)
			);

			auto* heap_object =
				static_cast<callable_type*>(memory);

			try
			{
				std::construct_at(
					heap_object,
					std::forward<Callable>(InCallable)
				);
			}
			catch (...)
			{
				AllocationPolicy::deallocate(
					memory,
					sizeof(callable_type),
					alignof(callable_type)
				);

				throw;
			}

			Object = heap_object;
			Ops = heapOperations<callable_type>();
		}
    }
public:
    template <typename Callable>
        requires (
            self_type::callableCompatible<Callable>()
            && (!IsCopyable|| std::copy_constructible<std::decay_t<Callable>>)
            && std::move_constructible<std::decay_t<Callable>>
        )
    void bind(Callable&& InCallable)
    {
	    using callable_type = std::decay_t<Callable>;

	    if constexpr (std::is_pointer_v<callable_type> || std::is_member_pointer_v<callable_type>)
	    {
	        if (InCallable == nullptr)
	        {
	            reset();
	            return;
	        }
	    }

        DelegateStorage Replacement;
        Replacement.emplaceCallable(std::forward<Callable>(InCallable));

        reset();
        moveFrom(std::move(Replacement));
    }

    template <auto Callable>
        requires (self_type::staticCallableCompatible<Callable>())
    void bindStatic() noexcept
    {
        reset();

        Object = nullptr;
        Ops = staticOperations<Callable>();
    }
private:
    template <typename ObjectPointer, typename MethodPointer>
    struct RuntimeMemberBinding
    {
        ObjectPointer Object = nullptr;
        MethodPointer Method = nullptr;

        RetType operator()(Args... InArgs) noexcept(IsNoexcept)
        {
            return std::invoke(Method, Object, std::forward<Args>(InArgs)...);
        }
    };

public:
	/*
	 * @example
	```cpp
		using method_type = void(Receiver::*)(int);
		method_type method = static_cast<method_type>(&Receiver::handle);
		core::Delegate<void(int)> callback;
		callback.bindRaw(&receiver, method);
	```
	 */
 	template <typename ObjectType, typename MethodType>
        requires (
            std::is_member_function_pointer_v<MethodType>
            && (IsNoexcept ? std::is_nothrow_invocable_r_v<RetType, MethodType, ObjectType*, Args...> 
				:std::is_invocable_r_v<RetType, MethodType, ObjectType*, Args...>
            )
        )
    void bindRaw(ObjectType* Instance, MethodType Method)
    {
        if (Instance == nullptr)
        {
            throw std::invalid_argument(
                "DelegateStorage::bindRaw requires a non-null instance"
			);
        }

        if (Method == nullptr)
        {
            reset();
            return;
        }

        bind(RuntimeMemberBinding<ObjectType*, MethodType>
			{
                .Object = Instance,
                .Method = Method
            }
        );
    }
    template <auto Method, typename ClassType>
        requires (self_type::rawMethodCompatible<Method, ClassType>())
    void bindRaw(ClassType* Instance)
    {
        if (Instance == nullptr)
        {
            throw std::invalid_argument(
                "DelegateStorage::bindRaw requires a non-null instance"
            );
        }

        reset();

        Object = const_cast<void*>(static_cast<const void*>(Instance));
        Ops = rawOperations<Method, ClassType>();
    }

    template <auto Method, typename ClassType>
        requires (self_type::rawMethodCompatible<Method, ClassType>())
    void bindRaw(ClassType& Instance) noexcept
    {
        reset();
        Object = const_cast<void*>(static_cast<const void*>(std::addressof(Instance)));
        Ops = rawOperations<Method, ClassType>();
    }

    template <auto Method, typename ClassType>
        requires (self_type::sharedMethodCompatible<Method, ClassType>())
    void bindShared(std::shared_ptr<ClassType> Instance)
    {
        if (!Instance)
        {
            throw std::invalid_argument(
                "DelegateStorage::bindShared requires a non-null instance"
            );
        }

        bind(
			[Instance = std::move(Instance)](Args... InArgs) noexcept(IsNoexcept) -> RetType
            {
                return std::invoke(Method, Instance.get(), std::forward<Args>(InArgs)...);
            }
        );
    }

    template <auto Method, typename ClassType>
        requires (
            std::is_void_v<RetType>
            && self_type::sharedMethodCompatible<Method, ClassType>()
        )
    void bindWeak(std::weak_ptr<ClassType> Instance)
    {
        bind(
            [Instance = std::move(Instance)]
            (Args... InArgs) noexcept(IsNoexcept)
            {
                if (auto Shared = Instance.lock())
                {
                    std::invoke(Method, Shared.get(), std::forward<Args>(InArgs)...);
                }
            }
        );
    }

    template <auto Method, typename ClassType, typename ExpiredCallable>
        requires (
            !std::is_void_v<RetType>
            && self_type::sharedMethodCompatible<Method, ClassType>()
        )
    void bindWeakOr(std::weak_ptr<ClassType> Instance, ExpiredCallable&& OnExpired)
    {
        using fallback_type = std::decay_t<ExpiredCallable>;

        bind([
                Instance = std::move(Instance),
                Fallback = fallback_type(std::forward<ExpiredCallable>(OnExpired))
            ] 
			(Args... InArgs) mutable noexcept(IsNoexcept) -> RetType
            {
                if (auto Shared = Instance.lock())
                {
                    return std::invoke(Method, Shared.get(), std::forward<Args>(InArgs)...);
                }

                return std::invoke(Fallback, std::forward<Args>(InArgs)...);
            }
        );
    }
public:
	template <typename CallableType>
	[[nodiscard]]
	bool holdsTarget() const noexcept
	{
		using target_type = std::remove_cv_t<CallableType>;
		return Ops != nullptr && Ops->TypeTag == typeTag<target_type>();
	}

	template <typename CallableType>
	[[nodiscard]]
	CallableType* getTarget() noexcept
	{
		using target_type = std::remove_cv_t<CallableType>;

		if (!holdsTarget<target_type>())
		{
			return nullptr;
		}
		return static_cast<target_type*>(Object);
	}

	template <typename CallableType>
	[[nodiscard]]
	const CallableType* getTarget() const noexcept
	{
		using target_type = std::remove_cv_t<CallableType>;

		if (!holdsTarget<target_type>())
		{
			return nullptr;
		}

		return static_cast<const target_type*>(Object);
	}
private:
    template <typename CallableType>
    static consteval bool fitsInline()
    {
        if constexpr (InlineSize == 0)
        {
            return false;
        }
        else
        {
            return sizeof(CallableType) <= InlineSize
                && alignof(CallableType)
                    <= alignof(std::max_align_t)
                && std::is_nothrow_move_constructible_v<
                    CallableType
                >;
        }
    }

	template <typename Type>
	[[nodiscard]]
	static const void* getTypeTag() noexcept
	{
		static constexpr unsigned char tag = 0;
		return std::addressof(tag);
	}

    template <typename CallableType>
    static RetType invokeObject(void* Object, Args&&... InArgs) noexcept(IsNoexcept)
    {
        return std::invoke(
            *static_cast<CallableType*>(Object),
            std::forward<Args>(InArgs)...
        );
    }

    template <typename CallableType>
    static const Operations* inlineOperations() noexcept
    {
        static const Operations table
		{
            .Invoke = &invokeObject<CallableType>,
            .Destroy = +[](void* Object) noexcept
                {
                    std::destroy_at(static_cast<CallableType*>(Object));
                },

            .Move = +[](void* Destination, void* Source) noexcept -> void*
                {
                    auto* SourceCallable = static_cast<CallableType*>(Source);
                    auto* DestinationCallable = static_cast<CallableType*>(Destination);

                    std::construct_at(DestinationCallable, std::move(*SourceCallable));
                    std::destroy_at(SourceCallable);

                    return DestinationCallable;
                },

            .Copy = copyInlineOperation<CallableType>(),
			.TypeTag = getTypeTag<CallableType>()
        };

        return std::addressof(table);
    }

    template <typename CallableType>
    static const Operations* heapOperations() noexcept
    {
        static const Operations table{
			.Invoke = &invokeObject<CallableType>,
			.Destroy = +[](void* Object) noexcept
				{
					auto* callable = static_cast<CallableType*>(Object);

					std::destroy_at(callable);
					AllocationPolicy::deallocate(
						callable,
						sizeof(CallableType),
						alignof(CallableType)
					);
				},
			.Move = +[](void*, void* Source) noexcept -> void*
				{
					return Source;
				},
			.Copy = copyHeapOperation<CallableType>(),
			.TypeTag = getTypeTag<CallableType>()
		};

		return std::addressof(table);
	}

    template <auto CallableType>
    static const Operations* staticOperations() noexcept
    {
        static const Operations table
		{
            .Invoke = +[](void*, Args&&... InArgs)
                    noexcept(IsNoexcept) -> RetType
                {
                    return std::invoke(
                        CallableType,
                        std::forward<Args>(InArgs)...
                    );
                },
            .Destroy = nullptr,
            .Move = +[](void*, void*) noexcept -> void*
                {
                    return nullptr;
                },
            .Copy = +[](void*, const void*) -> void*
                {
                    return nullptr;
                },
			.TypeTag = getTypeTag<decltype(CallableType)>()
        };

        return std::addressof(table);
    }

    template <auto Method, typename ClassType>
    static const Operations* rawOperations() noexcept
    {
        static const Operations table
		{
            .Invoke = +[](void* Object, Args&&... InArgs)
                    noexcept(IsNoexcept) -> RetType
                {
                    return std::invoke(
                        Method,
                        static_cast<ClassType*>(Object),
                        std::forward<Args>(InArgs)...
                    );
                },
            .Destroy = nullptr,
            .Move = +[](void*, void* Source) noexcept -> void*
                {
                    return Source;
                },
            .Copy = +[](void*, const void* Source) -> void*
                {
                    return const_cast<void*>(Source);
                },
			.TypeTag = getTypeTag<decltype(Method)>()
        };

        return std::addressof(table);
    }

    template <typename CallableType>
    static consteval copier_type copyInlineOperation()
    {
        if constexpr (IsCopyable)
        {
            return +[](void* Destination, const void* Source) -> void*
            {
                auto* DestinationCallable =
                    static_cast<CallableType*>(Destination);

                std::construct_at(
                    DestinationCallable,
                    *static_cast<const CallableType*>(Source)
                );

                return DestinationCallable;
            };
        }
        else
        {
            return nullptr;
        }
    }

    template <typename CallableType>
    static consteval copier_type copyHeapOperation()
    {
        if constexpr (IsCopyable)
        {
            return +[](void*, const void* Source) -> void*
            {
                std::allocator<CallableType> Allocator;
                CallableType* Clone = Allocator.allocate(1);

                try
                {
                    std::construct_at(
                        Clone,
                        *static_cast<const CallableType*>(Source)
                    );
                }
                catch (...)
                {
                    Allocator.deallocate(Clone, 1);
                    throw;
                }

                return Clone;
            };
        }
        else
        {
            return nullptr;
        }
    }

    void copyFrom(const DelegateStorage& Other)
        requires IsCopyable
    {
        if (!Other.bound())
        {
            return;
        }

        void* CopiedObject = Other.Ops->Copy(
            inlineData(),
            Other.Object
        );

        Object = CopiedObject;
        Ops = Other.Ops;
    }

    void moveFrom(DelegateStorage&& Other) noexcept
    {
        if (!Other.bound())
        {
            return;
        }

        void* MovedObject = Other.Ops->Move(
            inlineData(),
            Other.Object
        );

        Object = MovedObject;
        Ops = Other.Ops;

        Other.clear();
    }

    void clear() noexcept
    {
        Object = nullptr;
        Ops = nullptr;
    }

    [[noreturn]]
    static void handleEmptyInvocation() noexcept(IsNoexcept)
    {
        if constexpr (IsNoexcept)
        {
            std::terminate();
        }
        else
        {
            throw std::bad_function_call();
        }
    }

    void* inlineData() noexcept
    {
        if constexpr (InlineSize == 0)
        {
            return nullptr;
        }
        else
        {
            return static_cast<void*>(Inline.Data);
        }
    }

private:
    [[no_unique_address]]
    InlineStorage<InlineSize> Inline{};

    void* Object = nullptr;
    const Operations* Ops = nullptr;
};


template <typename Signature, bool IsCopyable, std::size_t InlineSize, typename AllocationPolicy>
struct DelegateSelector;

template <typename RetType, typename... Args, bool IsCopyable, std::size_t InlineSize, typename AllocationPolicy>
struct DelegateSelector<RetType(Args...), IsCopyable, InlineSize, AllocationPolicy>
{
    using storage_type 
		= DelegateStorage<RetType, false, IsCopyable, InlineSize, AllocationPolicy, Args...>;
};

template <typename RetType, typename... Args, bool IsCopyable, std::size_t InlineSize, typename AllocationPolicy>
struct DelegateSelector<RetType(Args...) noexcept, IsCopyable, InlineSize, AllocationPolicy>
{
    using storage_type 
		= DelegateStorage<RetType, true, IsCopyable, InlineSize, AllocationPolicy, Args...>;
};

template <typename RetType, bool IsNoexcept, typename... Args>
class DelegateRefStorage
{
private:
    using self_type = DelegateRefStorage<RetType, IsNoexcept, Args...>;
    using invoker_type = InvokerPointer<RetType, IsNoexcept, Args...>;

    template <typename Callable>
    static consteval bool callableCompatible()
    {
        using CallableType =
            std::remove_reference_t<Callable>;

        if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<RetType, CallableType&, Args...>;
        }
        else
        {
            return std::is_invocable_r_v< RetType, CallableType&, Args...>;
        }
    }

    template <auto Callable>
    static consteval bool staticCallableCompatible()
    {
        if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<RetType, decltype(Callable), Args...>;
        }
        else
        {
            return std::is_invocable_r_v<RetType, decltype(Callable), Args...>;
        }
    }

    template <auto Method, typename ClassType>
    static consteval bool rawMethodCompatible()
    {
        if constexpr (!std::is_member_function_pointer_v<decltype(Method)>)
        {
            return false;
        }
        else if constexpr (IsNoexcept)
        {
            return std::is_nothrow_invocable_r_v<RetType, decltype(Method), ClassType*, Args...>;
        }
        else
        {
            return std::is_invocable_r_v<RetType, decltype(Method), ClassType*, Args...>;
        }
    }

public:
    constexpr DelegateRefStorage() noexcept = default;

    constexpr DelegateRefStorage(std::nullptr_t) noexcept
    {
    }

private:
	// 不提前申明 clangd 无法分析 Object 和 Invoker 的存在
    void* ObjectRawPtr = nullptr;
    invoker_type Invoker = nullptr;

public:
    /**
     * 绑定一个由调用方管理生命周期的 callable 左值. 
     *
     * 不接受临时对象，也不直接接受函数引用. 自由函数应使用
     * bindStatic<&Function>(). 
     */
    template <typename Callable>
        requires (
            !std::same_as<std::remove_cvref_t<Callable>, self_type>
            && std::is_lvalue_reference_v<Callable&&>
            && std::is_object_v<std::remove_reference_t<Callable>>
            && self_type::callableCompatible<Callable>()
        )
    constexpr DelegateRefStorage(Callable&& InCallable) noexcept
    {
        using callable_type = std::remove_reference_t<Callable>;

        // 函数指针, 成员指针可以为空. 
        if constexpr (std::is_pointer_v<callable_type> || std::is_member_pointer_v<callable_type>)
        {
            if (InCallable == nullptr)
            {
                return;
            }
        }

        ObjectRawPtr = const_cast<void*>(
            static_cast<const volatile void*>(
                std::addressof(InCallable)
            )
        );

        Invoker = +[](void* ErasedObject, Args&&... InArgs)
                noexcept(IsNoexcept) -> RetType
            {
                return std::invoke(
                    *static_cast<callable_type*>(ErasedObject),
                    std::forward<Args>(InArgs)...
                );
            };
    }

    template <auto Callable>
        requires (self_type::staticCallableCompatible<Callable>())
    [[nodiscard]]
    static consteval DelegateRefStorage bindStatic() noexcept
    {
        return DelegateRefStorage{
            nullptr,
            +[](void*, Args&&... InArgs)
                noexcept(IsNoexcept) -> RetType
            {
                return std::invoke(Callable, std::forward<Args>(InArgs)...);
            }
        };
    }

    template <auto Method, typename ClassType>
        requires (self_type::rawMethodCompatible<Method, ClassType>())
    [[nodiscard]]
    static constexpr DelegateRefStorage bindRaw(ClassType& Instance) noexcept
    {
        return DelegateRefStorage{
            const_cast<void*>(
                static_cast<const volatile void*>(
                    std::addressof(Instance)
                )
            ),
            +[](void* ErasedObject, Args&&... InArgs)
                noexcept(IsNoexcept) -> RetType
            {
                return std::invoke(
                    Method,
                    static_cast<ClassType*>(ErasedObject),
                    std::forward<Args>(InArgs)...
                );
            }
        };
    }

    [[nodiscard]]
    constexpr bool bound() const noexcept
    {
        return Invoker != nullptr;
    }

    [[nodiscard]]
    constexpr explicit operator bool() const noexcept
    {
        return bound();
    }

    constexpr void reset() noexcept
    {
        ObjectRawPtr = nullptr;
        Invoker = nullptr;
    }

    RetType execute(Args... InArgs) const noexcept(IsNoexcept)
    {
        if (Invoker == nullptr) [[unlikely]]
        {
            handleEmptyInvocation();
        }

        return Invoker(ObjectRawPtr, std::forward<Args>(InArgs)...);
    }

    RetType operator()(Args... InArgs) const noexcept(IsNoexcept)
    {
        return execute(std::forward<Args>(InArgs)...);
    }

	/**
     * 仅为 void Delegate 提供无异常空检查调用. 
     *
     * 返回 true 表示已经调用，false 表示当前为空. 
     */
    bool executeIfBound(Args... InArgs) const
        noexcept(IsNoexcept)
        requires std::is_void_v<RetType>
    {
        if (Invoker == nullptr)
        {
            return false;
        }

        Invoker(ObjectRawPtr, std::forward<Args>(InArgs)...);
        return true;
    }
private:
    constexpr DelegateRefStorage(void* InObject, invoker_type InInvoker) noexcept
        : ObjectRawPtr(InObject),
          Invoker(InInvoker)
    {
    }

    [[noreturn]]
    static void handleEmptyInvocation() noexcept(IsNoexcept)
    {
        if constexpr (IsNoexcept)
        {
            std::terminate();
        }
        else
        {
            throw std::bad_function_call();
        }
    }

};

template <typename Signature>
struct DelegateRefSelector;

template <typename RetType, typename... Args>
struct DelegateRefSelector<RetType(Args...)>
{
    using storage_type = DelegateRefStorage<RetType, false, Args...>;
};

template <typename RetType, typename... Args>
struct DelegateRefSelector<RetType(Args...) noexcept>
{
    using storage_type = DelegateRefStorage<RetType, true, Args...>;
};


template <typename RetType, 
	bool IsNoexcept, 
	std::size_t InlineSize, 
	typename AllocationPolicy, 
	typename... Args>
class MulticastDelegateStorage
{
private:
    using storage_type 
		= DelegateStorage<RetType, IsNoexcept, false, InlineSize, AllocationPolicy, Args...>;
    static constexpr bool supports_fanout_arguments_v =
        (
            (
                !std::is_rvalue_reference_v<Args>
                && (std::is_reference_v<Args> || std::copy_constructible<Args>)
            )
            && ...
        );
    static constexpr bool SupportsFanoutArguments = supports_fanout_arguments_v;
	static constexpr std::uint32_t InvalidSlot = std::numeric_limits<std::uint32_t>::max();

	/*
	 *	Free -> Active -> Retired -> Free
     *                 -> Exhausted
	*/
	enum class SlotState : std::uint8_t
	{
		Free,
		Active,
		Retired,
		Exhausted
	};
    struct Slot
    {
        storage_type Callback;
        std::uint32_t Generation = 1;
        std::uint32_t NextFree = InvalidSlot;
        SlotState State = SlotState::Free;
    };

    struct OrderedSlot
    {
        std::uint32_t Index = InvalidSlot;
        std::uint32_t Generation = 0;
    };

public:
	// Multicast 常见风险是对象析构时忘记解除订阅, Multicast 已经禁止移动, 所以内部地址稳定
	// ScopedConnection 的生命周期不能超过 Multicast 本身
    class ScopedConnection
    {
    public:
        ScopedConnection() noexcept = default;
        ScopedConnection(MulticastDelegateStorage& InOwner, DelegateHandle InHandle) noexcept
            : Owner(std::addressof(InOwner)),
              Handle(InHandle)
        {
        }
        ScopedConnection(const ScopedConnection&) = delete;
        ScopedConnection& operator=(const ScopedConnection&) = delete;
        ScopedConnection(ScopedConnection&& Other) noexcept
            : Owner(std::exchange(Other.Owner, nullptr))
			, Handle(std::exchange(Other.Handle, DelegateHandle{}))
        {
        }
        ScopedConnection& operator=(ScopedConnection&& Other) noexcept
        {
            if (this != std::addressof(Other))
            {
                disconnect();
                Owner = std::exchange(Other.Owner, nullptr);
                Handle = std::exchange(Other.Handle, DelegateHandle{});
            }

            return *this;
        }

        ~ScopedConnection()
        {
            disconnect();
        }

        bool disconnect() noexcept
        {
            if (Owner == nullptr)
            {
                return false;
            }

            auto* owner = std::exchange(Owner, nullptr);
            const DelegateHandle handle = std::exchange(Handle, DelegateHandle{});

            return owner->remove(handle);
        }

        [[nodiscard]]
        bool connected() const noexcept
        {
            return Owner != nullptr && Owner->contains(Handle);
        }

        [[nodiscard]]
        DelegateHandle handle() const noexcept
        {
            return Handle;
        }

        DelegateHandle release() noexcept
        {
            Owner = nullptr;
            return std::exchange(Handle, DelegateHandle{});
        }

    private:
        MulticastDelegateStorage* Owner = nullptr;
        DelegateHandle Handle{};
    };
public:
    using callback_type = storage_type;

    MulticastDelegateStorage()
        : OwnerId(allocateDelegateOwnerId())
    {
    }

    MulticastDelegateStorage(const MulticastDelegateStorage&) = delete;
    MulticastDelegateStorage& operator=(const MulticastDelegateStorage&) = delete;
    MulticastDelegateStorage(MulticastDelegateStorage&&) = delete;
    MulticastDelegateStorage& operator=(MulticastDelegateStorage&&) = delete;
    ~MulticastDelegateStorage() = default;

	/*
	 * @example
	```cpp
		class Widget
		{
		public:
			Widget(core::MulticastDelegate<void()>& event)
				: Connection(event.connect([this]{ refresh(); }))
			{ }

		private:
			void refresh()
			{ }

			core::MulticastDelegate<void()>::ScopedConnection Connection;
		};
	```
	 */
	template <typename Callable>
		requires std::constructible_from<storage_type, Callable>
	[[nodiscard]]
	ScopedConnection connect(Callable&& InCallable)
	{
		return ScopedConnection{
			*this,
			addCallable(
				std::forward<Callable>(InCallable)
			)
		};
	}
    DelegateHandle add(storage_type InCallback)
    {
        if (!InCallback)
        {
            throw std::invalid_argument(
                "MulticastDelegate::add requires a bound callback"
            );
        }

        const std::uint32_t index = acquireSlot();
        Slot& entry = Slots[index];

        entry.Callback = std::move(InCallback);

        try
        {
            Order.push_back(
                OrderedSlot{
                    .Index = index,
                    .Generation = entry.Generation
                }
            );
        }
        catch (...)
        {
            entry.Callback.reset();
            releaseUnboundSlot(index);
            throw;
        }

        entry.State = SlotState::Active;
        ++ActiveCount;

        return makeHandle(index, entry.Generation);
    }

    template <typename Callable>
        requires std::constructible_from<storage_type, Callable>
    DelegateHandle addCallable(Callable&& InCallable)
    {
        return add(
            storage_type{
                std::forward<Callable>(InCallable)
            }
        );
    }

    template <auto Callable>
    DelegateHandle addStatic()
    {
        storage_type callback;
        callback.template bindStatic<Callable>();
        return add(std::move(callback));
    }

    template <auto Method, typename ClassType>
    DelegateHandle addRaw(ClassType* Instance)
    {
        storage_type callback;
        callback.template bindRaw<Method>(Instance);
        return add(std::move(callback));
    }

    template <auto Method, typename ClassType>
    DelegateHandle addRaw(ClassType& Instance)
    {
        storage_type callback;
        callback.template bindRaw<Method>(Instance);
        return add(std::move(callback));
    }

    template <auto Method, typename ClassType>
    DelegateHandle addShared(std::shared_ptr<ClassType> Instance)
    {
        storage_type callback;
        callback.template bindShared<Method>(std::move(Instance));
        return add(std::move(callback));
    }

    template <auto Method, typename ClassType>
        requires std::is_void_v<RetType>
    DelegateHandle addWeak(std::weak_ptr<ClassType> Instance)
    {
        storage_type callback;
        callback.template bindWeak<Method>(std::move(Instance));
        return add(std::move(callback));
    }

    template <auto Method, typename ClassType, typename ExpiredCallable>
        requires (!std::is_void_v<RetType>)
    DelegateHandle addWeakOr(std::weak_ptr<ClassType> Instance, ExpiredCallable&& OnExpired)
    {
        storage_type callback;

        callback.template bindWeakOr<Method>(
            std::move(Instance),
            std::forward<ExpiredCallable>(OnExpired)
        );

        return add(std::move(callback));
    }

    bool remove(DelegateHandle InHandle)
    {
        if (!contains(InHandle))
        {
            return false;
        }

        retireSlot(InHandle.SlotIndex);

        if (BroadcastDepth == 0)
        {
            finalizeRetiredSlots();
        }

        return true;
    }

    [[nodiscard]]
    bool contains(DelegateHandle InHandle) const noexcept
    {
        return InHandle.isValid()
            && InHandle.OwnerId == OwnerId
            && isActive(InHandle.SlotIndex, InHandle.Generation);
    }

    void clear() noexcept
    {
        for (const OrderedSlot Entry : Order)
        {
            if (isActive(Entry.Index, Entry.Generation))
            {
                retireSlot(Entry.Index);
            }
        }

        if (BroadcastDepth == 0)
        {
            finalizeRetiredSlots();
        }
    }

    [[nodiscard]]
    bool bound() const noexcept
    {
        return ActiveCount != 0;
    }

    [[nodiscard]]
    bool empty() const noexcept
    {
        return ActiveCount == 0;
    }

    [[nodiscard]]
    std::size_t size() const noexcept
    {
        return ActiveCount;
    }

    void reserveOrder(std::size_t Capacity)
    {
        Order.reserve(Capacity);
    }

    // ------------------------------------------------------------------------
    // Void broadcast
    //
    // 广播开始后添加的回调，在下一次广播中执行. 
    // 广播期间删除尚未调用的回调，该回调本轮不会执行. 
    // ------------------------------------------------------------------------

    void broadcast(Args... InArgs) const noexcept(IsNoexcept)
        requires (std::is_void_v<RetType> && supports_fanout_arguments_v)
    {
        BroadcastScope scope(*this);

        const std::size_t initial_order_size = Order.size();
        for (std::size_t position = 0; position < initial_order_size; ++position)
        {
            // 必须复制，不能在调用回调时持有 vector 元素引用. 
            const OrderedSlot entry = Order[position];

            if (isActive(entry.Index, entry.Generation))
            {
               invokeFanoutCallback(Slots[entry.Index].Callback, InArgs...);
            }
        }
    }

    // 捕获每个回调的异常并继续广播. 
    //
    // ErrorHandler 签名：
    //     void(DelegateHandle, std::exception_ptr)
    //
    // 仅为可抛异常签名提供；noexcept 事件不需要该接口. 
    template <typename ErrorHandler>
        requires (std::is_void_v<RetType>
            && !IsNoexcept
            && supports_fanout_arguments_v
            && std::is_nothrow_invocable_v< ErrorHandler&, DelegateHandle, std::exception_ptr>)
    void broadcastGuarded(ErrorHandler&& OnError, Args... InArgs) const
    {
        BroadcastScope scope(*this);

        const std::size_t initial_order_size = Order.size();
        for (std::size_t position = 0; position < initial_order_size; ++position)
        {
            const OrderedSlot entry = Order[position];
            if (!isActive(entry.Index, entry.Generation))
            {
                continue;
            }

            try
            {
                Slots[entry.Index].Callback.execute(InArgs...);
            }
            catch (...)
            {
                std::invoke(
                    OnError,
                    makeHandle(entry.Index, entry.Generation),
                    std::current_exception()
                );
            }
        }
    }

    // ------------------------------------------------------------------------
    // Non-void result visitation
    //
    // Visitor 返回 void：访问所有结果. 
    // Visitor 返回 bool：返回 false 时停止遍历. 
    // ------------------------------------------------------------------------
    template <typename Visitor>
        requires (!std::is_void_v<RetType>
            && supports_fanout_arguments_v
            && std::invocable<Visitor&, RetType>)
    void visitResults(Visitor&& ResultVisitor, Args... InArgs) const
    {
        BroadcastScope scope(*this);

        const std::size_t initial_order_size = Order.size();

        for (std::size_t position = 0; position < initial_order_size; ++position)
        {
            const OrderedSlot entry = Order[position];
            if (!isActive(entry.Index, entry.Generation))
            {
                continue;
            }

            if constexpr (std::convertible_to<std::invoke_result_t<Visitor&, RetType>, bool>)
            {
                if (!static_cast<bool>(std::invoke(ResultVisitor,
						Slots[entry.Index].Callback.execute(InArgs...))))
                {
                    break;
                }
            }
            else
            {
                std::invoke(
                    ResultVisitor,
                    Slots[entry.Index].Callback.execute(InArgs...)
                );
            }
        }
    }

    // 性能敏感代码优先使用 visitResults()，以避免 vector 分配. 
    [[nodiscard]]
    std::vector<RetType> collect(Args... InArgs) const
        requires (!std::is_void_v<RetType>
            && !std::is_reference_v<RetType>
            && std::move_constructible<RetType>
            && supports_fanout_arguments_v)
    {
        std::vector<RetType> results;
        results.reserve(ActiveCount);

        visitResults([&results](RetType Result)
            {
                results.push_back(std::move(Result));
            },
            InArgs...
        );

        return results;
    }
private:
    static void invokeFanoutCallback(storage_type& Callback, Args&... InArgs) 
		noexcept(IsNoexcept)
        requires std::is_void_v<RetType>
    {
        // 对值类型 Args：
        //   execute() 的值参数会为每个监听器分别复制. 
        //
        // 对引用类型 Args：
        //   Args& 经过引用折叠后保持原签名引用语义. 
        Callback.execute(InArgs...);
    }
private:
    class BroadcastScope
    {
    public:
        explicit BroadcastScope(const MulticastDelegateStorage& Owner) noexcept
            : OwnerRef(Owner)
        {
            ++OwnerRef.BroadcastDepth;
        }

        BroadcastScope(const BroadcastScope&) = delete;
        BroadcastScope& operator=(const BroadcastScope&) = delete;

        ~BroadcastScope() noexcept
        {
            --OwnerRef.BroadcastDepth;

            if (OwnerRef.BroadcastDepth == 0)
            {
                OwnerRef.finalizeRetiredSlots();
            }
        }

    private:
        const MulticastDelegateStorage& OwnerRef;
    };

    [[nodiscard]]
    DelegateHandle makeHandle(std::uint32_t SlotIndex, std::uint32_t Generation) const noexcept
    {
        return DelegateHandle{
            .OwnerId = OwnerId,
            .SlotIndex = SlotIndex,
            .Generation = Generation
        };
    }

    [[nodiscard]]
    bool isActive(std::uint32_t SlotIndex, std::uint32_t Generation) const noexcept
    {
        return SlotIndex < Slots.size()
            && Slots[SlotIndex].State == SlotState::Active
            && Slots[SlotIndex].Generation == Generation;
    }

    [[nodiscard]]
    std::uint32_t acquireSlot()
    {
        if (FreeHead != InvalidSlot)
        {
            const std::uint32_t index = FreeHead;
            Slot& entry = Slots[index];

            FreeHead = entry.NextFree;
            entry.NextFree = InvalidSlot;

            return index;
        }

        if (Slots.size() >= static_cast<std::size_t>(InvalidSlot))
        {
            throw std::length_error(
                "MulticastDelegate slot capacity exhausted"
            );
        }

        Slots.emplace_back();

        return static_cast<std::uint32_t>(Slots.size() - 1);
    }

    void releaseUnboundSlot(std::uint32_t SlotIndex) noexcept
    {
        Slot& entry = Slots[SlotIndex];

        entry.Callback.reset();
        entry.State = SlotState::Free;
        entry.NextFree = FreeHead;

        FreeHead = SlotIndex;
    }

    void retireSlot(std::uint32_t SlotIndex) noexcept
    {
        Slot& entry = Slots[SlotIndex];

        entry.State = SlotState::Retired;

        // generation 回绕后不再重用该 Slot，彻底避免非常旧的
        if (entry.Generation == std::numeric_limits<std::uint32_t>::max())
        {
            entry.Generation = 0;
        }
        else
        {
            ++entry.Generation;
        }

        entry.NextFree = RetiredHead;
        RetiredHead = SlotIndex;

        --ActiveCount;
        ++TombstoneCount;
    }

    void releaseRetiredSlots() const noexcept
    {
        while (RetiredHead != InvalidSlot)
        {
            const std::uint32_t index = RetiredHead;
            Slot& entry = Slots[index];

            RetiredHead = entry.NextFree;

            // 到广播完全结束后再销毁回调，保证回调可以安全地
            // 在执行过程中移除自身. 
            entry.Callback.reset();
            entry.NextFree = InvalidSlot;

            if (entry.Generation != 0)
            {
                // 可复用的槽位回到空闲链表.
                entry.State = SlotState::Free;
                entry.NextFree = FreeHead;
                FreeHead = index;
            }
            else
            {
                // Generation 回绕归零的槽位永久耗尽, 不再复用.
                entry.State = SlotState::Exhausted;
            }
        }
    }

    [[nodiscard]]
    bool shouldCompactOrder() const noexcept
    {
        return ActiveCount == 0 || (TombstoneCount >= 16 && TombstoneCount * 2 >= Order.size());
    }

    void finalizeRetiredSlots() const noexcept
    {
        releaseRetiredSlots();
        if (shouldCompactOrder())
        {
            compactOrder();
        }
    }

    void compactOrder() const noexcept
    {
        Order.erase(
            std::remove_if(
                Order.begin(),
                Order.end(),
                [this](const OrderedSlot& Entry)
                {
                    return !isActive(Entry.Index, Entry.Generation);
                }
            ),
            Order.end()
        );

        TombstoneCount = 0;
    }

private:
    // deque 保证 add 导致扩容时，正在执行的 Callback 对象不会
    // 像 vector 元素那样被移动和销毁. 
    mutable std::deque<Slot> Slots;
    mutable std::vector<OrderedSlot> Order;

    const std::uint64_t OwnerId;

    mutable std::uint32_t FreeHead = InvalidSlot;
    mutable std::uint32_t RetiredHead = InvalidSlot;

    mutable std::size_t ActiveCount = 0;
    mutable std::size_t BroadcastDepth = 0;
    mutable std::size_t TombstoneCount = 0;
};

template <typename Signature, std::size_t InlineSize, typename AllocationPolicy>
struct MulticastDelegateSelector;

template <typename RetType, typename... Args, std::size_t InlineSize, typename AllocationPolicy>
struct MulticastDelegateSelector<RetType(Args...), InlineSize, AllocationPolicy>
{
    using storage_type = 
		MulticastDelegateStorage<RetType, false, InlineSize, AllocationPolicy, Args...>;
};

template <typename RetType, typename... Args, std::size_t InlineSize, typename AllocationPolicy>
struct MulticastDelegateSelector<RetType(Args...) noexcept, InlineSize, AllocationPolicy>
{
    using storage_type = 
		MulticastDelegateStorage< RetType, true, InlineSize, AllocationPolicy, Args...>;
};

} // namespace details

/**
 * 可复制, 拥有 callable 的委托. 
 *
 * - 小 callable 使用 SBO. 
 * - 大 callable 或过度对齐 callable 使用堆分配. 
 * - callable 必须可复制. 
 */
template <typename Signature, 
	std::size_t InlineSize = DefaultDelegateInlineSize, 
	typename AllocationPolicy = DefaultDelegateAllocatePolicy
>
using Delegate 
	= typename details::DelegateSelector<Signature, true, InlineSize, AllocationPolicy>::storage_type;

/**
 * 仅移动, 拥有 callable 的委托. 
 *
 * - 支持捕获 unique_ptr 等 move-only callable. 
 * - 推荐用于任务系统以及 multicast 内部存储. 
 */
template <typename Signature, 
	std::size_t InlineSize = DefaultDelegateInlineSize, 
	typename AllocationPolicy = DefaultDelegateAllocatePolicy>
using UniqueDelegate 
	= typename details::DelegateSelector<Signature, false, InlineSize, AllocationPolicy>::storage_type;

/**
 * 非拥有委托. 
 *
 * - 通常为两个指针大小. 
 * - 永不分配. 
 * - 从 callable 构造时只接受左值，避免直接绑定临时 lambda. 
 * - 调用者必须保证目标生命周期. 
 */
template <typename Signature>
using DelegateRef = typename details::DelegateRefSelector<Signature>::storage_type;

/**
 * 顺序, 多播委托. 
 *
 * - 内部 Callback 为 unique_delegate. 
 * - 支持 move-only lambda. 
 * - 支持广播期间添加, 移除和清空. 
 * - 非线程安全；订阅, 退订和广播应在同一所有者线程执行. 
 */
template <typename Signature, 
	std::size_t InlineSize = DefaultDelegateInlineSize, 
	typename AllocationPolicy = DefaultDelegateAllocatePolicy>
using MulticastDelegate =
    typename details::MulticastDelegateSelector<Signature, InlineSize, AllocationPolicy>::storage_type;

// 无 SBO 便利别名. 捕获 callable 总是进入堆分配. 
template <typename Signature>
using HeapDelegate = Delegate<Signature, 0>;

template <typename Signature>
using HeapUniqueDelegate = UniqueDelegate<Signature, 0>;

} // namespace core