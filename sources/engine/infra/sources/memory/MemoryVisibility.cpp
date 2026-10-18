//
// Created by Lzorn on 2026/9/26.
//
#include <infra/memory/MemoryVisibility.hpp>

#if CORE_MEM_ENABLE_GLOBAL_HOOKS

#include <cstdlib>
#include <mutex>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#elif defined(__GLIBC__) || defined(__linux__)
extern "C" void *__libc_malloc(size_t);
extern "C" void __libc_free(void *);
extern "C" void *__libc_calloc(size_t, size_t);
extern "C" void *__libc_realloc(void *, size_t);
#else
#include <dlfcn.h>
#endif

namespace
{
thread_local int HookRecursionDepth = 0;

struct RecursionGuard
{
    RecursionGuard()
    {
        ++HookRecursionDepth;
    }

    ~RecursionGuard()
    {
        --HookRecursionDepth;
    }

    [[nodiscard]] bool isRecursive() const noexcept
    {
        return HookRecursionDepth > 1;
    }
};

#if defined(_WIN32)
using MallocFn  = void* (*)(size_t);
using FreeFn    = void (*)(void *);
using CallocFn  = void* (*)(size_t, size_t);
using ReallocFn = void* (*)(void *, size_t);

MallocFn RealMalloc   = nullptr;
FreeFn RealFree       = nullptr;
CallocFn RealCalloc   = nullptr;
ReallocFn RealRealloc = nullptr;

void initRealFunctions()
{
    static std::once_flag InitFlag;
    std::call_once(InitFlag, []()
    {
        RealMalloc = [](size_t Size) -> void *
        {
            return std::malloc(Size);
        };
        RealFree = [](void *Ptr)
        {
            std::free(Ptr);
        };
        RealCalloc = [](size_t Count, size_t Size) -> void *
        {
            return std::calloc(Count, Size);
        };
        RealRealloc = [](void *Ptr, size_t Size) -> void *
        {
            return std::realloc(Ptr, Size);
        };
    });
}
#elif defined(__GLIBC__) || defined(__linux__)
using MallocFn  = void* (*)(size_t);
using FreeFn    = void (*)(void *);
using CallocFn  = void* (*)(size_t, size_t);
using ReallocFn = void* (*)(void *, size_t);

MallocFn RealMalloc   = nullptr;
FreeFn RealFree       = nullptr;
CallocFn RealCalloc   = nullptr;
ReallocFn RealRealloc = nullptr;

void initRealFunctions()
{
    if (!RealMalloc)
    {
        RealMalloc  = __libc_malloc;
        RealFree    = __libc_free;
        RealCalloc  = __libc_calloc;
        RealRealloc = __libc_realloc;
    }
}
#else
using MallocFn  = void* (*)(size_t);
using FreeFn    = void (*)(void *);
using CallocFn  = void* (*)(size_t, size_t);
using ReallocFn = void* (*)(void *, size_t);

MallocFn RealMalloc   = nullptr;
FreeFn RealFree       = nullptr;
CallocFn RealCalloc   = nullptr;
ReallocFn RealRealloc = nullptr;

void initRealFunctions()
{
    static std::once_flag InitFlag;
    std::call_once(InitFlag, []()
    {
        RealMalloc  = reinterpret_cast<MallocFn>(::dlsym(RTLD_NEXT, "malloc"));
        RealFree    = reinterpret_cast<FreeFn>(::dlsym(RTLD_NEXT, "free"));
        RealCalloc  = reinterpret_cast<CallocFn>(::dlsym(RTLD_NEXT, "calloc"));
        RealRealloc = reinterpret_cast<ReallocFn>(::dlsym(RTLD_NEXT, "realloc"));
        if (!RealMalloc || !RealFree || !RealCalloc || !RealRealloc)
        {
            std::abort();
        }
    });
}
#endif

[[nodiscard]] bool shouldTrack(const RecursionGuard &Guard) noexcept
{
    if (Guard.isRecursive())
    {
        return false;
    }

    if (infra::MemoryTracker::isTrackingSuppressedForCurrentThread())
    {
        return false;
    }

    return infra::MemoryTracker::isHookEnabled();
}

[[nodiscard]] void *captureCaller() noexcept
{
    return CORE_MEM_HOOK_RETURN_ADDRESS;
}
} // namespace

// MSVC UCRT marks malloc/free as dllimport when using /MD, which cannot be redefined
// in a regular object file. Keep C allocation hooks for non-Windows toolchains.
#if !defined(_WIN32)
extern "C"
{
void *malloc(size_t Size)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = RealMalloc(Size);
    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::Malloc,
                Ptr,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::Malloc,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
    }

    return Ptr;
}

void free(void *Ptr)
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::Free,
            Ptr,
            infra::MemoryTrackerUnknownSize,
            infra::MemoryTrackerUnknownSize,
            captureCaller());
    }

    RealFree(Ptr);
}

void *calloc(size_t Count, size_t Size)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = RealCalloc(Count, Size);
    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::Calloc,
                Ptr,
                Count * Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::Calloc,
                Count * Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
    }

    return Ptr;
}

void *realloc(void *Ptr, size_t Size)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *NewPtr = RealRealloc(Ptr, Size);
    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookReallocate(Ptr, NewPtr, Size, captureCaller());
    }

    return NewPtr;
}
} // extern "C"
#endif

void *operator new(size_t Size)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = RealMalloc(Size);
    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::New,
                Ptr,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::New,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
    }

    if (!Ptr)
    {
        throw std::bad_alloc();
    }

    return Ptr;
}

void operator delete(void *Ptr) noexcept
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::Delete,
            Ptr,
            infra::MemoryTrackerUnknownSize,
            infra::MemoryTrackerUnknownSize,
            captureCaller());
    }

    RealFree(Ptr);
}

void *operator new[](size_t Size)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = RealMalloc(Size);
    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::NewArray,
                Ptr,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::NewArray,
                Size,
                infra::MemoryTrackerUnknownSize,
                captureCaller());
        }
    }

    if (!Ptr)
    {
        throw std::bad_alloc();
    }

    return Ptr;
}

void operator delete[](void *Ptr) noexcept
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::DeleteArray,
            Ptr,
            infra::MemoryTrackerUnknownSize,
            infra::MemoryTrackerUnknownSize,
            captureCaller());
    }

    RealFree(Ptr);
}

#if __cplusplus >= 201402L
void operator delete(void *Ptr, size_t Size) noexcept
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::Delete,
            Ptr,
            Size,
            infra::MemoryTrackerUnknownSize,
            captureCaller());
    }

    RealFree(Ptr);
}

void operator delete[](void *Ptr, size_t Size) noexcept
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::DeleteArray,
            Ptr,
            Size,
            infra::MemoryTrackerUnknownSize,
            captureCaller());
    }

    RealFree(Ptr);
}
#endif

#if (__cplusplus > 201402L) || defined(__cpp_aligned_new)
#if defined(_WIN32)
void *operator new(size_t Size, std::align_val_t Alignment)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = _aligned_malloc(Size, static_cast<size_t>(Alignment));
    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::AlignedNew,
                Ptr,
                Size,
                static_cast<size_t>(Alignment),
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::AlignedNew,
                Size,
                static_cast<size_t>(Alignment),
                captureCaller());
        }
    }

    if (!Ptr)
    {
        throw std::bad_alloc();
    }

    return Ptr;
}

void operator delete(void *Ptr, std::align_val_t Alignment) noexcept
{
    RecursionGuard Guard;
    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::AlignedDelete,
            Ptr,
            infra::MemoryTrackerUnknownSize,
            static_cast<size_t>(Alignment),
            captureCaller());
    }

    _aligned_free(Ptr);
}
#else
void *operator new(size_t Size, std::align_val_t Alignment)
{
    RecursionGuard Guard;
    initRealFunctions();

    void *Ptr = nullptr;
    if (::posix_memalign(&Ptr, static_cast<size_t>(Alignment), Size) != 0)
    {
        Ptr = nullptr;
    }

    if (shouldTrack(Guard))
    {
        if (Ptr)
        {
            infra::MemoryTracker::onHookAllocation(
                infra::HookAllocationOp::AlignedNew,
                Ptr,
                Size,
                static_cast<size_t>(Alignment),
                captureCaller());
        }
        else
        {
            infra::MemoryTracker::onHookAllocationFailure(
                infra::HookAllocationOp::AlignedNew,
                Size,
                static_cast<size_t>(Alignment),
                captureCaller());
        }
    }

    if (!Ptr)
    {
        throw std::bad_alloc();
    }

    return Ptr;
}

void operator delete(void *Ptr, std::align_val_t Alignment) noexcept
{
    RecursionGuard Guard;
    initRealFunctions();

    if (shouldTrack(Guard))
    {
        infra::MemoryTracker::onHookFree(
            infra::HookAllocationOp::AlignedDelete,
            Ptr,
            infra::MemoryTrackerUnknownSize,
            static_cast<size_t>(Alignment),
            captureCaller());
    }

    RealFree(Ptr);
}
#endif

void *operator new[](size_t Size, std::align_val_t Alignment)
{
    return operator new(Size, Alignment);
}

void operator delete[](void *Ptr, std::align_val_t Alignment) noexcept
{
    operator delete(Ptr, Alignment);
}
#endif

#endif
