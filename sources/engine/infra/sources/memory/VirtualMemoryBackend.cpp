//
// Created by Lzorn on 2026/9/22.
//

#include <infra/memory/VirtualMemoryBackend.hpp>

namespace infra::mem
{
    void VirtualRegion::reset() noexcept
    {
        Base  = nullptr;
        ReservedSize = 0;
        CommittedSize = 0;
        PageSize = 0;
        NumaNode = -1;
        HugePage = false;
    }


    size_t VirtualMemoryBackend::getSystemPageSize() noexcept
    {
#if defined(_WIN32)
        SYSTEM_INFO Info{};
        ::GetSystemInfo(&Info);
        return static_cast<std::size_t>(Info.dwPageSize);
#else
        const long Value = ::sysconf(_SC_PAGESIZE);
        return Value > 0 ? static_cast<std::size_t>(Value) : 4096u;
#endif
    }

    size_t VirtualMemoryBackend::getHugePageSize() noexcept
    {
#if defined(_WIN32)
        const SIZE_T Value = ::GetLargePageMinimum();
        return Value == 0 ? 0u : static_cast<std::size_t>(Value);
#else
        return 2u * 1024u * 1024u;
#endif
    }

    VirtualRegion VirtualMemoryBackend::reserve(size_t RequestedBytes, bool IsHugePage, int32_t NumaNode) noexcept
    {
        const std::size_t systemPageSize = getSystemPageSize();
        const std::size_t hugePageSize = getHugePageSize();
        const bool requestHugePage = IsHugePage && hugePageSize > 0;
        const std::size_t requestPageSize = requestHugePage ? hugePageSize : systemPageSize;
        const std::size_t AlignedBytes = alignUp(RequestedBytes, requestPageSize);
        if (AlignedBytes == 0)
        {
            return {};
        }

#if defined(_WIN32)
        std::byte* Base = nullptr;
        bool HugePageMapped = false;

        if (requestHugePage)
        {
            const DWORD hugeAllocType = MEM_RESERVE | MEM_LARGE_PAGES;
            Base = static_cast<std::byte*>(
                allocateWindows(nullptr, AlignedBytes, hugeAllocType, PAGE_READWRITE, NumaNode));
            HugePageMapped = Base != nullptr;
        }

        if (!Base)
        {
            Base = static_cast<std::byte*>(allocateWindows(nullptr, AlignedBytes, MEM_RESERVE, PAGE_READWRITE, NumaNode));
            HugePageMapped = false;
        }

        if (!Base)
        {
            return {};
        }

        VirtualRegion Region;
        Region.Base = Base;
        Region.ReservedSize = AlignedBytes;
        Region.PageSize = HugePageMapped ? hugePageSize : systemPageSize;
        Region.NumaNode = NumaNode;
        Region.HugePage = HugePageMapped;
        return Region;
#else
        int Flags = MAP_PRIVATE | MAP_ANONYMOUS;
        bool HugePageMapped = false;
#   if defined(MAP_HUGETLB)
        if (requestHugePage)
        {
            Flags |= MAP_HUGETLB;
            HugePageMapped = true;
        }
#   endif
        void* Base = ::mmap(nullptr, AlignedBytes, PROT_NONE, Flags, -1, 0);

#   if defined(MAP_HUGETLB)
        if (Base == MAP_FAILED && HugePageMapped)
        {
            Flags &= ~MAP_HUGETLB;
            HugePageMapped = false;
            Base = ::mmap(nullptr, AlignedBytes, PROT_NONE, Flags, -1, 0);
        }
#   endif

        if (Base == MAP_FAILED)
        {
            return {};
        }

        VirtualRegion Region;
        Region.Base = static_cast<std::byte*>(Base);
        Region.ReservedSize = AlignedBytes;
        Region.PageSize = HugePageMapped ? hugePageSize : systemPageSize;
        Region.NumaNode = NumaNode;
        Region.HugePage = HugePageMapped;
        return Region;
#endif
    }

    bool VirtualMemoryBackend::commit(VirtualRegion& Region, size_t Offset, size_t Bytes) noexcept
    {
        if (!Region.isValid())
        {
            return false;
        }

        const std::size_t AlignedOffset = alignUp(Offset, Region.PageSize);
        const std::size_t AlignedBytes = alignUp(Bytes, Region.PageSize);
        if (AlignedOffset + AlignedBytes > Region.ReservedSize)
        {
            return false;
        }

#if defined(_WIN32)
        // Windows 这里会直接分配物理页面
        void* Committed = ::VirtualAlloc(Region.Base + AlignedOffset, AlignedBytes, MEM_COMMIT, PAGE_READWRITE);
        if (!Committed)
        {
            return false;
        }
#else
        // 对于支持 POSIX 标准的 mprotect(PROT_READ|WRITE) 也是只改变权限位, 依旧是只有真正用到才会分配
        if (::mprotect(Region.Base + AlignedOffset, AlignedBytes, PROT_READ | PROT_WRITE) != 0)
        {
            return false;
        }
#endif

        Region.CommittedSize = std::max(Region.CommittedSize, AlignedOffset + AlignedBytes);
        return true;
    }

    bool VirtualMemoryBackend::decommit(VirtualRegion& Region, size_t Offset, size_t Bytes) noexcept
    {
        if (!Region.isValid())
        {
            return false;
        }

        const std::size_t AlignedOffset = alignUp(Offset, Region.PageSize);
        const std::size_t AlignedBytes = alignUp(Bytes, Region.PageSize);
        if (AlignedOffset + AlignedBytes > Region.ReservedSize)
        {
            return false;
        }

#if defined(_WIN32)
        if (::VirtualFree(Region.Base + AlignedOffset, AlignedBytes, MEM_DECOMMIT) == 0)
        {
            return false;
        }
#else
        if (::mprotect(Region.Base + AlignedOffset, AlignedBytes, PROT_NONE) != 0)
        {
            return false;
        }
#endif

        if (AlignedOffset + AlignedBytes >= Region.CommittedSize)
        {
            Region.CommittedSize = AlignedOffset;
        }

        return true;
    }

    void VirtualMemoryBackend::release(VirtualRegion& Region) noexcept
    {
        if (!Region.isValid())
        {
            return;
        }

#if defined(_WIN32)
        ::VirtualFree(Region.Base, 0, MEM_RELEASE);
#else
        ::munmap(Region.Base, Region.ReservedSize);
#endif

        Region = {};
    }

#if defined(_WIN32)
    [[nodiscard]] void* VirtualMemoryBackend::allocateWindows(
        void* Hint,
        const std::size_t Size,
        const DWORD AllocType,
        const DWORD Protect,
        const std::int32_t NumaNode) noexcept
    {
        if (NumaNode < 0)
        {
            return ::VirtualAlloc(Hint, Size, AllocType, Protect);
        }

        using VirtualAllocExNumaFn = PVOID(WINAPI*)(HANDLE, PVOID, SIZE_T, DWORD, DWORD, DWORD);
        static VirtualAllocExNumaFn VirtualAllocExNuma =
            reinterpret_cast<VirtualAllocExNumaFn>(::GetProcAddress(
                ::GetModuleHandleW(L"kernel32.dll"),
                "VirtualAllocExNuma"));

        if (!VirtualAllocExNuma)
        {
            return ::VirtualAlloc(Hint, Size, AllocType, Protect);
        }

        return VirtualAllocExNuma(
            ::GetCurrentProcess(),
            Hint,
            Size,
            AllocType,
            Protect,
            static_cast<DWORD>(NumaNode));
    }
#endif
}
