#pragma once

#include <core/common/Global.hpp>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
/**
 * ============================================================================
 *  core/Handle.hpp
 * ============================================================================
 *
 *  [OVERVIEW]
 *  这是一个「泛型句柄(Handle)系统 + 类型擦除池管理器」.
 *  它把「资源标识」与「资源本体」彻底解耦: 
 *      - 句柄本身只保存 {Id, Generation}, 是一个轻量的 POD；
 *      - 资源本体存放在 HandlePool<Tag, T> 的槽位中；
 *      - 所有 Pool 由 HandleManager 单例统一持有.
 *
 *  其目的时旨在解决:
 *      1. 资源指针的生命周期混乱(谁持有, 谁释放)；
 *      2. 资源被销毁后, 旧指针悬空(dangling pointer)；
 *      3. 不同系统之间用 int / void* 互相传资源时, 类型无法区分.
 *
 * ----------------------------------------------------------------------------
 *
 *  [Design]
 *
 *  1. TypedHandle<Tag>
 *     - 由 Id(槽位索引)和 Generation(世代号)组成.
 *     - Tag 是编译期空类型, 用来让不同系统的句柄成为不同类型.
 *       例如 mesh::MeshTag 与 texture::TextureTag 生成的句柄无法隐式互转, 
 *       从而在编译期阻止「把网格句柄传给纹理系统」这类错误.
 *     - isValid() 只检查 Id != 0, 不检查池中的 Generation；
 *       完整有效性必须通过 HandleManager::isValid() 或 HandlePool::isValid().
 *
 *  2. Generation(世代号)
 *     - 作用: 在 Id 被复用时让旧句柄自动失效, 解决 ABA 问题.
 *     - 语义: 
 *          h1 = {Id=1, Gen=1}
 *          destroy(h1);            // 槽位 1 标记为死亡, 但 Generation 仍为 1
 *          h2 = create();          // 复用 Id=1, Generation 变为 2
 *          // 此时 h1.Gen(1) != 当前槽位 Gen(2), isValid(h1) == false
 *     - 注意: destroy() 不会增加 Generation, 只有「复用槽位」时才 ++.
 *
 *  3. 为什么不用 RTTI?
 *     - std::type_index / typeid 依赖 RTTI, 在 -fno-rtti / /GR- 下不可用.
 *     - 这里用 TypeKey<Tag> 生成编译期唯一键: 
 *          template <typename Tag> struct TypeKey {
 *              inline static constexpr std::byte Anchor{};
 *              static constexpr const void* get() noexcept { return &Anchor; }
 *          };
 *     - C++17 起 inline 变量保证跨 TU 合并, 因此 &Anchor 在整个链接单元里
 *       对同一个 Tag 是唯一地址；不同 Tag 地址不同.完全 constexpr, 
 *       不引用 <typeindex>, 可在关闭 RTTI 的 Vulkan / 游戏引擎下使用.
 *
 *  2. 跨动态库问题与 ExplicitTypeId
 *     - inline 变量只在「同一个链接单元」内保证合并, 跨 .so / .dll 不保证.
 *     - 若 Tag 定义在 A.so, HandleManager 在 B.so, TypeKey<Tag>::get()
 *       可能返回不同地址, 导致两边的 Pool 不一致.
 *     - 解决方案: 为这些 Tag 特化 ExplicitTypeId, 给一个显式 uint32_t: 
 *          template <> struct core::ExplicitTypeId<mesh::MeshTag> {
 *              static constexpr bool HasValue = true;
 *              static constexpr std::uint32_t Value = 0x0001'0001;
 *          };
 *       TypeKey::get() 会优先返回该常量地址, 从而跨模块一致.
 *
 *  3. HandleManager 的并发模型
 *     - 用 std::shared_mutex: 
 *          - registerPool / getPool / clearAll: unique_lock(写)
 *          - tryGetPool / isValid / getPoolCount: shared_lock(读)
 *     - 每个 <Tag, T> 组合有一个独立的 atomic 缓存指针, 
 *       热路径上只做一次 load(acquire), 无锁, 无哈希, 无 find.
 *
 *  4. HandlePool 的线程模型
 *     - 池内部不加锁, 因为大多数场景一个池只被一个系统/线程访问.
 *     - 需要跨线程时用 ConcurrentHandlePool 包装, 它内部用 shared_mutex: 
 *          create / destroy → unique_lock
 *          get / isValid    → shared_lock
 *
 *  5. SharedHandle / UniqueHandle / NameHandle
 *     - 它们都内含 TypedHandle<Tag>, 所以都接入 Generation.
 *     - isValid() 是「句柄有效 && 指针非空」的本地检查；
 *       isValidByManager() 会走 HandleManager, 带上 Generation 校验.
 *     - release() 会同时 reset 句柄, 避免指针和句柄状态不一致.
 *
 * ----------------------------------------------------------------------------
 *
 *  [Usage]
 *      // MeshSystem.hpp
 *      #include <core/Handle.hpp>
 *
 *      namespace mesh
 *      {
 *          struct Mesh {  ...  };
 *
 *          CORE_DEFINE_HANDLE(MeshTag);
 *          // 展开为: 
 *          //   struct MeshTag;
 *          //   using MeshTagHandle = core::TypedHandle<MeshTag>;
 *
 *          using MeshSharedHandle = core::SharedHandle<MeshTag, Mesh>;
 *          using MeshUniqueHandle = core::UniqueHandle<MeshTag, Mesh>;
 *          using MeshNameHandle   = core::NameHandle<MeshTag, Mesh>;
 *      }
 *
 *  系统 .cpp 中显式注册 Pool ────────────────────────────────
 *
 *      // MeshSystem.cpp
 *      #include "MeshSystem.hpp"
 *
 *      CORE_REGISTER_HANDLE_POOL(mesh::MeshTag, mesh::Mesh);
 *
 *      // 该宏会在匿名命名空间里生成一个静态 bool, 静态初始化阶段
 *      // 调用 HandleManager::self().registerPool<Tag, T>(), 幂等.
 *
 *      auto& Pool = core::HandleManager::self()
 *                       .getPool<mesh::MeshTag, mesh::Mesh>();
 *
 *      auto H = Pool.create(构造 Mesh 的参数);
 *
 *      if (core::HandleManager::self().isValid(H))
 *      {
 *          mesh::Mesh* P = Pool.get(H);
 *      }
 *
 *      Pool.destroy(H);
 *      // H 仍然存在, 但 isValid(H) 会因为 Generation 不匹配而返回 false.
 *
 *  NOTE: 跨动态库时特化 ExplicitTypeId ────────────────────
 *
 *      namespace mesh { struct MeshTag; }
 *
 *      template <>
 *      struct core::ExplicitTypeId<mesh::MeshTag>
 *      {
 *          static constexpr bool          HasValue = true;
 *          static constexpr std::uint32_t Value    = 0x0001'0001;
 *      };
 *
 * ============================================================================
 */
namespace core
{

template <typename Tag>
struct TypedHandle
{
    /**
     * @NOTE: Generation 的作用是: 在 Id 被复用时, 让旧句柄自动失效
     * @example:
     * ```cpp
        h1 = {Id=1, Gen=1};
        destroy(h1);
        h2 = {Id=1, Gen=2};
     * ```
     * 此时复用时 Generation++ , 旧的 h1 就会失效, 因为 h1.Gen != h2.Gen
     */
    HandleIdType         Id         = InvalidHandleId;
    HandleGenerationType Generation = InvalidHandleGeneration;

    constexpr TypedHandle() noexcept = default;

    constexpr TypedHandle(HandleIdType InId, HandleGenerationType InGen = 1) noexcept
        : Id(InId), Generation(InGen) {}

    [[nodiscard]] constexpr bool isValid() const noexcept
    {
        return Id != InvalidHandleId;
    }

    [[nodiscard]] constexpr explicit operator bool() const noexcept { return isValid(); }

    constexpr bool operator==(const TypedHandle&) const noexcept = default;
    constexpr auto operator<=>(const TypedHandle&) const noexcept = default;

    constexpr void reset() noexcept
    {
        Id         = InvalidHandleId;
        Generation = InvalidHandleGeneration;
    }
};


template <typename Tag>
struct ExplicitTypeId
{
    static constexpr bool          HasValue = false;
    static constexpr std::uint32_t Value    = 0;
};

template <typename Tag>
struct TypeKey
{
    inline static constexpr std::byte Anchor{};

    /**
     * @NOTE: inline 变量在同一个链接单元(inline 变量在同一个链接单元)内保证合并, 但是跨 .so 不保证.
     * @NOTE: 如果跨 .so 要保证唯一性(Tag 定义在 A.so 但是 HandleManager 在 B.so), TypeKey<Tag>::get()
     * @NOTE: 可能返回不同地址. 
     * @NOTE: 解决方案: 为这些 Tag 特化 ExplicitTypeId
     * ```cpp
        namespace mesh {
            struct MeshTag;
        }

        template <>
        struct core::ExplicitTypeId<mesh::MeshTag>
        {
            static constexpr bool          HasValue = true;
            static constexpr std::uint32_t Value    = 0x0001'0001;
        };
     * ```
     * @return 指向类型唯一标识的指针
     */
    [[nodiscard]] static constexpr const void* get() noexcept
    {
        if constexpr (ExplicitTypeId<Tag>::HasValue)
        {
           return &ExplicitTypeId<Tag>::Value;
        }
        return &Anchor;
    }
};


struct IHandlePoolBase
{
    virtual ~IHandlePoolBase() = default;

    [[nodiscard]] virtual size_t size() const noexcept = 0;
    virtual void clear() noexcept = 0;
    [[nodiscard]] virtual bool isValidById(HandleIdType Id,
                                           HandleGenerationType Gen) const noexcept = 0;
    [[nodiscard]] virtual const void* getTypeKey() const noexcept = 0;
};

template <typename Tag, typename T>
class HandlePool final : public IHandlePoolBase
{
public:
    using HandleType = TypedHandle<Tag>;

    HandlePool() = default;

    template <typename... Args>
    HandleType create(Args&&... InArgs)
    {
        HandleIdType Id;
        if (!FreeList.empty())
        {
            Id = FreeList.back();
            FreeList.pop_back();
            auto& Slot = Slots[Id - 1];
            Slot.Value = T(std::forward<Args>(InArgs)...);
            Slot.Generation++;
            Slot.IsAlive = true;
        }
        else
        {
            Id = static_cast<HandleIdType>(Slots.size() + 1);
            Slots.emplace_back();
            auto& Slot = Slots.back();
            Slot.Value = T(std::forward<Args>(InArgs)...);
            Slot.Generation = 1;
            Slot.IsAlive    = true;
        }
        return HandleType{ Id, Slots[Id - 1].Generation };
    }

    void destroy(HandleType H)
    {
        if (!isValid(H))
        {
            return ;
        }
        auto& Slot   = Slots[H.Id - 1];
        Slot.IsAlive = false;
        Slot.Value   = T{};
        FreeList.push_back(H.Id);
    }

    [[nodiscard]] T* get(HandleType H) noexcept
    {
        if (!isValid(H))
        {
            return nullptr;
        }
        return &Slots[H.Id - 1].Value;
    }
    [[nodiscard]] const T* get(HandleType H) const noexcept
    {
        if (!isValid(H))
        {
            return nullptr;
        }
        return &Slots[H.Id - 1].Value;
    }

    [[nodiscard]] bool isValid(HandleType H) const noexcept
    {
        if (H.Id == InvalidHandleId || H.Id > Slots.size()) 
        {
            return false;
        }
        const auto& Slot = Slots[H.Id - 1];
        return Slot.IsAlive && Slot.Generation == H.Generation;
    }

    // IHandlePoolBase
    [[nodiscard]] size_t size() const noexcept override
    {
        return Slots.size() - FreeList.size();
    }
    void clear() noexcept override
    {
        Slots.clear();
        FreeList.clear();
    }
    [[nodiscard]] bool isValidById(HandleIdType Id,
                                   HandleGenerationType Gen) const noexcept override
    {
        return isValid(HandleType{ Id, Gen });
    }

    [[nodiscard]] const void* getTypeKey() const noexcept override
    {
        return TypeKey<Tag>::get();
    }
private:
    struct Slot
    {
        T                    Value{};
        HandleGenerationType Generation{ 0 };
        bool                 IsAlive{ false };
    };

    std::vector<Slot>         Slots;
    std::vector<HandleIdType> FreeList;
};

/**
 * @NOTE: 池内部不加锁, 因为大多数场景一个池只被一个系统/一个线程访问; 如果存在跨线程访问的情况, 
 * @NOTE: 使用 ConcurrentHandlePool 来包装 HandlePool, 其内部会加锁.
 */
template <typename Tag, typename T>
class ConcurrentHandlePool
{
public:
    using HandleType = TypedHandle<Tag>;

    template <typename... Args>
    HandleType create(Args&&... InArgs)
    {
        std::unique_lock Lock(Mutex);
        return Pool.create(std::forward<Args>(InArgs)...);
    }

    void destroy(HandleType H)
    {
        std::unique_lock Lock(Mutex);
        Pool.destroy(H);
    }

    [[nodiscard]] T* get(HandleType H) noexcept
    {
        std::shared_lock Lock(Mutex);
        return Pool.get(H);
    }

    [[nodiscard]] bool isValid(HandleType H) const noexcept
    {
        std::shared_lock Lock(Mutex);
        return Pool.isValid(H);
    }

private:
    mutable std::shared_mutex Mutex;
    HandlePool<Tag, T>        Pool;
};


class HandleManager
{
public:
    static HandleManager& self() noexcept
    {
        static HandleManager Instance;
        return Instance;
    }

    HandleManager(const HandleManager&)            = delete;
    HandleManager& operator=(const HandleManager&) = delete;

    // ---------- 显式注册(幂等) ----------
    template <typename Tag, typename T>
    void registerPool()
    {
        const void* Key = TypeKey<Tag>::get();

        {
            std::shared_lock Lock(Mutex);
            if (Pools.find(Key) != Pools.end()) 
            {
                return;
            }
        }

        std::unique_lock Lock(Mutex);
        if (Pools.find(Key) != Pools.end())
        {
            return;
        }

        Pools.emplace(Key, std::make_unique<HandlePool<Tag, T>>());
    }

    // ---------- 获取 Pool(惰性注册 + 热路径缓存) ----------
    template <typename Tag, typename T>
    [[nodiscard]] HandlePool<Tag, T>& getPool()
    {
        // 每个 Tag+T 组合有独立的原子缓存
        static std::atomic<HandlePool<Tag, T>*> Cached{ nullptr };

        if (auto* P = Cached.load(std::memory_order_acquire))
        {
            return *P;
        }

        const void* Key = TypeKey<Tag>::get();

        std::unique_lock Lock(Mutex);
        auto It = Pools.find(Key);

        HandlePool<Tag, T>* Raw = nullptr;
        if (It == Pools.end())
        {
            auto Owned = std::make_unique<HandlePool<Tag, T>>();
            Raw = Owned.get();
            Pools.emplace(Key, std::move(Owned));
        }
        else
        {
            Raw = static_cast<HandlePool<Tag, T>*>(It->second.get());
        }

        Cached.store(Raw, std::memory_order_release);
        return *Raw;
    }

    // ---------- 只读查询 ----------
    template <typename Tag, typename T>
    [[nodiscard]] HandlePool<Tag, T>* tryGetPool() const noexcept
    {
        std::shared_lock Lock(Mutex);
        auto It = Pools.find(TypeKey<Tag>::get());
        if (It == Pools.end()) 
        {
            return nullptr;
        }
        return static_cast<HandlePool<Tag, T>*>(It->second.get());
    }

    template <typename Tag>
    [[nodiscard]] bool isValid(TypedHandle<Tag> H) const noexcept
    {
        if (!H.isValid())
        {
            return false;
        }
        std::shared_lock Lock(Mutex);
        auto It = Pools.find(TypeKey<Tag>::get());
        if (It == Pools.end())
        {
            return false;
        }
        return It->second->isValidById(H.Id, H.Generation);
    }

    void clearAll() noexcept
    {
        std::unique_lock Lock(Mutex);
        for (auto& [Key, Pool] : Pools) 
        {
            Pool->clear();
        }
    }

    [[nodiscard]] std::size_t getPoolCount() const noexcept
    {
        std::shared_lock Lock(Mutex);
        return Pools.size();
    }

private:
    HandleManager()  = default;
    ~HandleManager() = default;

    mutable std::shared_mutex Mutex;
    std::unordered_map<const void*, std::unique_ptr<IHandlePoolBase>> Pools;
};


template <typename Tag, typename T>
struct SharedHandle
{
    using tag_type   = Tag;
    using value_type = T;
    using HandleType = TypedHandle<Tag>;

    HandleType         Handle{};
    std::shared_ptr<T> Ptr;

    SharedHandle() noexcept = default;

    SharedHandle(HandleType InHandle, std::shared_ptr<T> InPtr) noexcept
        : Handle(InHandle), Ptr(std::move(InPtr)) {}

    template <typename Deleter>
    SharedHandle(HandleType InHandle, T* RawPtr, Deleter D) noexcept
        : Handle(InHandle), Ptr(RawPtr, std::move(D)) {}

    [[nodiscard]] bool isValid() const noexcept
    {
        return Handle.isValid() && static_cast<bool>(Ptr);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return isValid(); }

    [[nodiscard]] T* get() const noexcept { return Ptr.get(); }
    [[nodiscard]] T& operator*() const noexcept { return *Ptr; }
    [[nodiscard]] T* operator->() const noexcept { return Ptr.get(); }

    [[nodiscard]] const std::shared_ptr<T>& shared() const noexcept { return Ptr; }
    [[nodiscard]] std::shared_ptr<T> release() noexcept
    {
        Handle.reset();
        return std::move(Ptr);
    }

    [[nodiscard]] HandleIdType         getId()         const noexcept { return Handle.Id; }
    [[nodiscard]] HandleGenerationType getGeneration() const noexcept { return Handle.Generation; }
    [[nodiscard]] long                 getUseCount()   const noexcept { return Ptr.use_count(); }

    // 通过 HandleManager 校验, 会带上 Generation 检查
    [[nodiscard]] bool isValidByManager() const noexcept
    {
        return HandleManager::self().isValid(Handle);
    }

    void reset() noexcept
    {
        Handle.reset();
        Ptr.reset();
    }

    void swap(SharedHandle& Rhs) noexcept
    {
        std::swap(Handle, Rhs.Handle);
        std::swap(Ptr, Rhs.Ptr);
    }

    bool operator==(const SharedHandle& Rhs) const noexcept
    {
        return Handle == Rhs.Handle && Ptr == Rhs.Ptr;
    }
};

template <typename Tag, typename T>
struct UniqueHandle
{
    using tag_type   = Tag;
    using value_type = T;
    using HandleType = TypedHandle<Tag>;

    HandleType         Handle{};
    std::unique_ptr<T> Ptr;

    UniqueHandle() noexcept = default;

    UniqueHandle(HandleType InHandle, std::unique_ptr<T> InPtr) noexcept
        : Handle(InHandle), Ptr(std::move(InPtr)) {}

    [[nodiscard]] bool isValid() const noexcept
    {
        return Handle.isValid() && static_cast<bool>(Ptr);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return isValid(); }

    [[nodiscard]] T* get() const noexcept { return Ptr.get(); }
    [[nodiscard]] T& operator*() const noexcept { return *Ptr; }
    [[nodiscard]] T* operator->() const noexcept { return Ptr.get(); }

    [[nodiscard]] std::unique_ptr<T> release() noexcept
    {
        Handle.reset();
        return std::move(Ptr);
    }

    [[nodiscard]] HandleIdType         getId()         const noexcept { return Handle.Id; }
    [[nodiscard]] HandleGenerationType getGeneration() const noexcept { return Handle.Generation; }

    [[nodiscard]] bool isValidByManager() const noexcept
    {
        return HandleManager::self().isValid(Handle);
    }

    void reset() noexcept
    {
        Handle.reset();
        Ptr.reset();
    }

    bool operator==(const UniqueHandle& Rhs) const noexcept
    {
        return Handle == Rhs.Handle;
    }
};

template <typename Tag, typename T = void>
struct NameHandle
{
    using tag_type   = Tag;
    using value_type = T;
    using HandleType = TypedHandle<Tag>;

    HandleType  Handle{};
    std::string Name;

    [[nodiscard]] bool isValid() const noexcept { return Handle.isValid(); }
    [[nodiscard]] explicit operator bool() const noexcept { return isValid(); }

    [[nodiscard]] HandleIdType         getId()         const noexcept { return Handle.Id; }
    [[nodiscard]] HandleGenerationType getGeneration() const noexcept { return Handle.Generation; }

    [[nodiscard]] bool isValidByManager() const noexcept
    {
        return HandleManager::self().isValid(Handle);
    }

    constexpr bool operator==(const NameHandle&) const noexcept = default;
    constexpr auto operator<=>(const NameHandle&) const noexcept = default;
};
template <typename Tag, typename T, typename... Args>
SharedHandle<Tag, T> makeSharedHandle(TypedHandle<Tag> H, Args&&... InArgs)
{
    return SharedHandle<Tag, T>{ H, std::make_shared<T>(std::forward<Args>(InArgs)...) };
}

template <typename Tag, typename T, typename Deleter>
SharedHandle<Tag, T> makeSharedHandle(TypedHandle<Tag> H, T* RawPtr, Deleter D)
{
    return SharedHandle<Tag, T>{ H, RawPtr, std::move(D) };
}

template <typename Tag, typename T>
UniqueHandle<Tag, T> makeUniqueHandle(TypedHandle<Tag> H, std::unique_ptr<T> Ptr)
{
    return UniqueHandle<Tag, T>{ H, std::move(Ptr) };
}

} // namespace core

/**
 * CORE_DEFINE_HANDLE_TAG(TagName)
 *   在系统头文件里声明 Tag 及 TypedHandle 别名.
 *
 *   namespace mesh { CORE_DEFINE_HANDLE_TAG(MeshTag); }
 */
#define CORE_DEFINE_HANDLE(TagName)                        \
    struct TagName;                                            \
    using TagName##Handle = ::core::TypedHandle<TagName>

/**
 * CORE_REGISTER_HANDLE_POOL(TagName, ValueType)
 *   在系统 .cpp 里显式注册.
 *
 *   CORE_REGISTER_HANDLE_POOL(mesh::MeshTag, mesh::Mesh);
 */
#define CORE_REGISTER_HANDLE_POOL(TagName, ValueType)                  \
    namespace {                                                        \
        [[maybe_unused]] const bool CORE_CONCAT(g_PoolReg_, __LINE__) = \
            (::core::HandleManager::self()                             \
                 .template registerPool<TagName, ValueType>(), true);  \
    }

namespace std
{
template <typename Tag>
struct hash<::core::TypedHandle<Tag>>
{
    std::size_t operator()(const ::core::TypedHandle<Tag>& H) const noexcept
    {
        return std::hash<std::uint64_t>{}(
            (static_cast<std::uint64_t>(H.Generation) << 32) |
             static_cast<std::uint64_t>(H.Id));
    }
};


}

