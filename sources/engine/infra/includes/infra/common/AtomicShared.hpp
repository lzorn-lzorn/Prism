#pragma once 

#include <atomic>
#include <memory>
namespace infra
{

// apple clang 22.1.8 不支持 __cpp_lib_atomic_shared_ptr 所以简单封装一个,
// TODO: 参考 folly::AtomicSharedPtr 进行优化, 目前的实现每次 load/store 都会
// TODO: 进行一次 atomic_load/atomic_store, 可能会有性能损耗
#ifdef __cpp_lib_atomic_shared_ptr
template <typename Ty>
using AtomicShared = std::atomic<std::shared_ptr<Ty>>;

#else
template <typename Ty>
class AtomicShared
{
public:
    AtomicShared() noexcept = default;
    AtomicShared(std::shared_ptr<Ty> InPtr) noexcept
        : Ptr(std::move(InPtr))
    {
    }

    AtomicShared(const AtomicShared&) = delete;
    AtomicShared& operator=(const AtomicShared&) = delete;
    AtomicShared(AtomicShared&&) = delete;
    AtomicShared& operator=(AtomicShared&&) = delete;

    [[nodiscard]] std::shared_ptr<Ty> load(std::memory_order Order = std::memory_order_seq_cst) const noexcept
    {
        return std::atomic_load_explicit(&Ptr, Order);
    }

    void store(std::shared_ptr<Ty> InPtr, std::memory_order Order = std::memory_order_seq_cst) noexcept
    {
        std::atomic_store_explicit(&Ptr, std::move(InPtr), Order);
    }

    std::shared_ptr<Ty> exchange(std::shared_ptr<Ty> InPtr, std::memory_order Order = std::memory_order_seq_cst) noexcept
    {
        return std::atomic_exchange_explicit(&Ptr, std::move(InPtr), Order);
    }

    [[nodiscard]] bool compare_exchange_strong(
        std::shared_ptr<Ty>& Expected,
        std::shared_ptr<Ty> Desired,
        std::memory_order SuccessOrder = std::memory_order_seq_cst,
        std::memory_order FailureOrder = std::memory_order_seq_cst) noexcept
    {
        return std::atomic_compare_exchange_strong_explicit(
            &Ptr,
            &Expected,
            std::move(Desired),
            SuccessOrder,
            FailureOrder);
    }

    operator std::shared_ptr<Ty>() const noexcept
    {
        return load();
    }

    AtomicShared& operator=(std::shared_ptr<Ty> InNewPtr) noexcept
    {
        store(std::move(InNewPtr));
        return *this;
    }
private:
    std::shared_ptr<Ty> Ptr;
};
#endif

} // namespace infra