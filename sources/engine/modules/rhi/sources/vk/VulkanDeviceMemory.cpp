#include "VulkanDeviceMemory.hpp"
#include "VulkanDevice.hpp"
#include "VulkanMemoryHeap.hpp"
#include "VulkanRHI.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <stdexcept>
#include <utility>

namespace rhi
{

namespace
{
constexpr vk::DeviceSize DeviceLocalPageSize = 64ull * 1024ull * 1024ull;
constexpr vk::DeviceSize HostVisiblePageSize = 16ull * 1024ull * 1024ull;

vk::DeviceSize alignUp(vk::DeviceSize Value, vk::DeviceSize Alignment)
{
    if (Alignment <= 1)
        return Value;
    const auto Remainder = Value % Alignment;
    if (Remainder == 0)
        return Value;
    if (Value > std::numeric_limits<vk::DeviceSize>::max() - (Alignment - Remainder))
        throw std::overflow_error("Vulkan allocation alignment overflow.");
    return Value + Alignment - Remainder;
}

EMemoryPropertyFlags fromVk(vk::MemoryPropertyFlags Properties)
{
    EMemoryPropertyFlags Result;
    if (Properties & vk::MemoryPropertyFlagBits::eDeviceLocal) Result.set(EMemoryProperty::DeviceLocal);
    if (Properties & vk::MemoryPropertyFlagBits::eHostVisible) Result.set(EMemoryProperty::HostVisible);
    if (Properties & vk::MemoryPropertyFlagBits::eHostCoherent) Result.set(EMemoryProperty::HostCoherent);
    if (Properties & vk::MemoryPropertyFlagBits::eHostCached) Result.set(EMemoryProperty::HostCached);
    if (Properties & vk::MemoryPropertyFlagBits::eLazilyAllocated) Result.set(EMemoryProperty::LazilyAllocated);
    return Result;
}
} // namespace

class VulkanMemoryBlock final
{
public:
    struct Range
    {
        vk::DeviceSize Offset { 0 };
        vk::DeviceSize Size { 0 };
    };

    VulkanMemoryBlock(
        VulkanContextPtr InContext,
        uint32_t InMemoryTypeIndex,
        vk::MemoryPropertyFlags InProperties,
        vk::DeviceSize InSize,
        bool InDeviceAddress,
        bool InDedicated,
        vk::Buffer InDedicatedBuffer,
        vk::Image InDedicatedImage)
        : Context(std::move(InContext))
        , MemoryTypeIndex(InMemoryTypeIndex)
        , Properties(InProperties)
        , Size(InSize)
        , DeviceAddress(InDeviceAddress)
        , Dedicated(InDedicated)
    {
        vk::MemoryDedicatedAllocateInfo DedicatedInfo;
        DedicatedInfo.buffer = InDedicatedBuffer;
        DedicatedInfo.image = InDedicatedImage;
        vk::MemoryAllocateFlagsInfo FlagsInfo;
        if (DeviceAddress)
            FlagsInfo.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
        const bool HasDedicatedResource = static_cast<bool>(InDedicatedBuffer) || static_cast<bool>(InDedicatedImage);
        FlagsInfo.pNext = HasDedicatedResource ? &DedicatedInfo : nullptr;
        vk::MemoryAllocateInfo AllocationInfo(Size, MemoryTypeIndex);
        AllocationInfo.pNext = DeviceAddress
            ? static_cast<void*>(&FlagsInfo)
            : HasDedicatedResource ? static_cast<void*>(&DedicatedInfo) : nullptr;
        Memory = Context->Device->allocateMemory(AllocationInfo);
        FreeRanges.push_back({ 0, Size });

        if (Properties & vk::MemoryPropertyFlagBits::eHostVisible)
        {
            Mapped = Context->Device->mapMemory(Memory, 0, VK_WHOLE_SIZE);
        }
        AtomSize = std::max<vk::DeviceSize>(
            1, Context->PhysicalDevice.getProperties().limits.nonCoherentAtomSize);
    }

    ~VulkanMemoryBlock()
    {
        if (!Context || !Context->Device || !Memory)
            return;
        if (Mapped)
            Context->Device->unmapMemory(Memory);
        Context->Device->freeMemory(Memory);
    }

    [[nodiscard]] std::optional<vk::DeviceSize> allocate(
        vk::DeviceSize AllocationSize,
        vk::DeviceSize Alignment)
    {
        std::scoped_lock Lock(Mutex);
        size_t BestIndex = FreeRanges.size();
        vk::DeviceSize BestOffset = 0;
        vk::DeviceSize BestWaste = std::numeric_limits<vk::DeviceSize>::max();
        for (size_t Index = 0; Index < FreeRanges.size(); ++Index)
        {
            const auto& Range = FreeRanges[Index];
            const auto Offset = alignUp(Range.Offset, Alignment);
            if (Offset < Range.Offset || Offset - Range.Offset > Range.Size ||
                AllocationSize > Range.Size - (Offset - Range.Offset))
                continue;
            const auto Waste = Range.Size - (Offset - Range.Offset) - AllocationSize;
            if (Waste < BestWaste)
            {
                BestIndex = Index;
                BestOffset = Offset;
                BestWaste = Waste;
            }
        }
        if (BestIndex == FreeRanges.size())
            return std::nullopt;

        const Range Original = FreeRanges[BestIndex];
        FreeRanges.erase(FreeRanges.begin() + static_cast<std::ptrdiff_t>(BestIndex));
        if (BestOffset > Original.Offset)
            FreeRanges.push_back({ Original.Offset, BestOffset - Original.Offset });
        const auto End = BestOffset + AllocationSize;
        const auto OriginalEnd = Original.Offset + Original.Size;
        if (End < OriginalEnd)
            FreeRanges.push_back({ End, OriginalEnd - End });
        std::ranges::sort(FreeRanges, {}, &Range::Offset);
        return BestOffset;
    }

    void free(vk::DeviceSize Offset, vk::DeviceSize AllocationSize)
    {
        std::scoped_lock Lock(Mutex);
        FreeRanges.push_back({ Offset, AllocationSize });
        std::ranges::sort(FreeRanges, {}, &Range::Offset);
        std::vector<Range> Coalesced;
        Coalesced.reserve(FreeRanges.size());
        for (const auto& Range : FreeRanges)
        {
            if (!Coalesced.empty() &&
                Coalesced.back().Offset + Coalesced.back().Size == Range.Offset)
                Coalesced.back().Size += Range.Size;
            else
                Coalesced.push_back(Range);
        }
        FreeRanges = std::move(Coalesced);
    }

    [[nodiscard]] bool isEmpty() const
    {
        std::scoped_lock Lock(Mutex);
        return FreeRanges.size() == 1 && FreeRanges.front().Offset == 0 &&
            FreeRanges.front().Size == Size;
    }

    [[nodiscard]] bool isCompatible(uint32_t TypeIndex, bool RequiresAddress) const noexcept
    {
        return !Dedicated && MemoryTypeIndex == TypeIndex && DeviceAddress == RequiresAddress;
    }

    [[nodiscard]] void* mappedAddress(vk::DeviceSize Offset) const noexcept
    {
        return Mapped ? static_cast<std::byte*>(Mapped) + Offset : nullptr;
    }

    void flush(vk::DeviceSize Offset, vk::DeviceSize RangeSize)
    {
        if (Properties & vk::MemoryPropertyFlagBits::eHostCoherent)
            return;
        const auto Begin = Offset - Offset % AtomSize;
        const auto End = std::min(Size, alignUp(Offset + RangeSize, AtomSize));
        Context->Device->flushMappedMemoryRanges(vk::MappedMemoryRange(Memory, Begin, End - Begin));
    }

    void invalidate(vk::DeviceSize Offset, vk::DeviceSize RangeSize)
    {
        if (Properties & vk::MemoryPropertyFlagBits::eHostCoherent)
            return;
        const auto Begin = Offset - Offset % AtomSize;
        const auto End = std::min(Size, alignUp(Offset + RangeSize, AtomSize));
        Context->Device->invalidateMappedMemoryRanges(vk::MappedMemoryRange(Memory, Begin, End - Begin));
    }

    VulkanContextPtr Context;
    vk::DeviceMemory Memory { VK_NULL_HANDLE };
    uint32_t MemoryTypeIndex { 0 };
    vk::MemoryPropertyFlags Properties {};
    vk::DeviceSize Size { 0 };
    vk::DeviceSize AtomSize { 1 };
    void* Mapped { nullptr };
    bool DeviceAddress { false };
    bool Dedicated { false };
    mutable std::mutex Mutex;
    std::vector<Range> FreeRanges;
};

VulkanMemoryAllocation::VulkanMemoryAllocation(
    std::shared_ptr<VulkanMemoryBlock> InBlock,
    MemoryRequirements InRequirements,
    MemoryAllocationInfo InInfo,
    DeviceSizeType InReservedSize)
    : Block(std::move(InBlock))
    , Requirements(InRequirements)
    , Info(InInfo)
    , ReservedSize(InReservedSize)
{
    OwnershipState = EState::Writeable;
}

VulkanMemoryAllocation::~VulkanMemoryAllocation()
{
    release();
}

void* VulkanMemoryAllocation::map(DeviceSizeType Offset, DeviceSizeType Size)
{
    std::scoped_lock Lock(MapMutex);
    if (!Block || !Info.Properties.has(EMemoryProperty::HostVisible))
        throw std::logic_error("Cannot map non-host-visible Vulkan memory.");
    if (IsMapped)
        throw std::logic_error("Vulkan allocation is already mapped.");
    if (Offset >= Info.Size || (Size != 0 && Size > Info.Size - Offset))
        throw std::out_of_range("Vulkan allocation map range is out of bounds.");
    IsMapped = true;
    return Block->mappedAddress(Info.Offset + Offset);
}

void VulkanMemoryAllocation::unmap()
{
    std::scoped_lock Lock(MapMutex);
    if (!IsMapped)
        throw std::logic_error("Vulkan allocation is not mapped.");
    // Host-visible suballocation pages stay mapped for their lifetime; this ends client access only.
    IsMapped = false;
}

void VulkanMemoryAllocation::flush(DeviceSizeType Offset, DeviceSizeType Size)
{
    std::scoped_lock Lock(MapMutex);
    if (!Block || !IsMapped || Offset >= Info.Size || (Size != 0 && Size > Info.Size - Offset))
        throw std::out_of_range("Vulkan allocation flush range is invalid.");
    Block->flush(Info.Offset + Offset, Size == 0 ? Info.Size - Offset : Size);
}

void VulkanMemoryAllocation::invalidate(DeviceSizeType Offset, DeviceSizeType Size)
{
    std::scoped_lock Lock(MapMutex);
    if (!Block || !IsMapped || Offset >= Info.Size || (Size != 0 && Size > Info.Size - Offset))
        throw std::out_of_range("Vulkan allocation invalidate range is invalid.");
    Block->invalidate(Info.Offset + Offset, Size == 0 ? Info.Size - Offset : Size);
}

void VulkanMemoryAllocation::release()
{
    std::shared_ptr<VulkanMemoryBlock> BlockCopy;
    {
        std::scoped_lock Lock(MapMutex);
        if (!Block)
            return;
        // 移出成员(成员置空), 本地副本在锁外仍保持显存块存活.
        BlockCopy = std::move(Block);
        IsMapped = false;
        OwnershipState = EState::InValid;
    }
    BlockCopy->free(Info.Offset, ReservedSize);
}

vk::DeviceMemory VulkanMemoryAllocation::getVkDeviceMemory() const noexcept
{
    return Block ? Block->Memory : vk::DeviceMemory {};
}

VulkanMemoryAllocator::VulkanMemoryAllocator(VulkanDevice& InDevice)
    : Device(&InDevice)
{
}

VulkanMemoryAllocator::~VulkanMemoryAllocator() = default;

std::shared_ptr<VulkanMemoryAllocation> VulkanMemoryAllocator::allocate(
    const MemoryAllocationDescriptor& Desc,
    bool RequireDeviceAddress,
    vk::Buffer DedicatedBuffer,
    vk::Image DedicatedImage)
{
    if (!Device || Desc.Requirements.Size == 0 || Desc.Requirements.MemoryTypeBits == 0)
        throw std::invalid_argument("Vulkan memory allocation requirements are invalid.");

    auto Required = toVk(Desc.RequiredProperties);
    auto Preferred = toVk(Desc.PreferredProperties);
    switch (Desc.Usage)
    {
    case EMemoryUsage::GPUOnly:
        Required |= vk::MemoryPropertyFlagBits::eDeviceLocal;
        break;
    case EMemoryUsage::CPUToGPU:
        Required |= vk::MemoryPropertyFlagBits::eHostVisible;
        Preferred |= vk::MemoryPropertyFlagBits::eHostCoherent;
        break;
    case EMemoryUsage::GPUToCPU:
        Required |= vk::MemoryPropertyFlagBits::eHostVisible;
        Preferred |= vk::MemoryPropertyFlagBits::eHostCached;
        break;
    case EMemoryUsage::CPUOnly:
        Required |= vk::MemoryPropertyFlagBits::eHostVisible;
        Preferred |= vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostCached;
        break;
    case EMemoryUsage::Auto:
    default:
        break;
    }
    if (Desc.PersistentlyMapped && !(Required & vk::MemoryPropertyFlagBits::eHostVisible))
        throw std::invalid_argument("Persistently mapped Vulkan memory must be host-visible.");

    const auto MemoryProperties = Device->getVkPhysicalDevice().getMemoryProperties();
    uint32_t SelectedType = UINT32_MAX;
    int SelectedScore = -1;
    for (uint32_t Index = 0; Index < MemoryProperties.memoryTypeCount; ++Index)
    {
        if (!(Desc.Requirements.MemoryTypeBits & (1u << Index)))
            continue;
        const auto Properties = MemoryProperties.memoryTypes[Index].propertyFlags;
        if ((Properties & Required) != Required)
            continue;
        const int Score = std::popcount(static_cast<uint32_t>(Properties & Preferred));
        if (Score > SelectedScore)
        {
            SelectedType = Index;
            SelectedScore = Score;
        }
    }
    if (SelectedType == UINT32_MAX)
        throw std::runtime_error("Failed to find a suitable Vulkan memory type.");

    const auto ActualProperties = MemoryProperties.memoryTypes[SelectedType].propertyFlags;
    const vk::DeviceSize PageSize = ActualProperties & vk::MemoryPropertyFlagBits::eHostVisible
        ? HostVisiblePageSize : DeviceLocalPageSize;
    const bool Dedicated = Desc.Requirements.RequiresDedicatedAllocation ||
        Desc.Requirements.PrefersDedicatedAllocation || Desc.Requirements.Size >= PageSize / 2;
    const auto AtomSize = std::max<vk::DeviceSize>(
        1, Device->getVkPhysicalDevice().getProperties().limits.nonCoherentAtomSize);
    const bool NonCoherentHost =
        (ActualProperties & vk::MemoryPropertyFlagBits::eHostVisible) &&
        !(ActualProperties & vk::MemoryPropertyFlagBits::eHostCoherent);
    const auto AllocationAlignment = std::max<vk::DeviceSize>(
        std::max<uint64_t>(1, Desc.Requirements.Alignment),
        NonCoherentHost && !Dedicated ? AtomSize : 1);
    const auto ReservedSize = NonCoherentHost && !Dedicated
        ? alignUp(Desc.Requirements.Size, AtomSize)
        : Desc.Requirements.Size;

    std::scoped_lock Lock(Mutex);
    std::shared_ptr<VulkanMemoryBlock> Block;
    std::optional<vk::DeviceSize> Offset;
    if (!Dedicated)
    {
        for (const auto& Page : Pages)
        {
            if (!Page->isCompatible(SelectedType, RequireDeviceAddress))
                continue;
            Offset = Page->allocate(ReservedSize, AllocationAlignment);
            if (Offset)
            {
                Block = Page;
                break;
            }
        }
    }
    if (!Block)
    {
        const auto BlockSize = Dedicated
            ? ReservedSize
            : std::max<vk::DeviceSize>(PageSize, alignUp(ReservedSize, AllocationAlignment));
        Block = std::make_shared<VulkanMemoryBlock>(
            Device->getContext(), SelectedType, ActualProperties, BlockSize,
            RequireDeviceAddress, Dedicated,
            Dedicated ? DedicatedBuffer : vk::Buffer {},
            Dedicated ? DedicatedImage : vk::Image {});
        Offset = Block->allocate(ReservedSize, AllocationAlignment);
        if (!Dedicated)
            Pages.push_back(Block);
    }
    if (!Offset)
        throw std::runtime_error("Vulkan memory page could not satisfy a validated allocation.");

    MemoryAllocationInfo Info {
        .Size = Desc.Requirements.Size,
        .Offset = *Offset,
        .MemoryTypeIndex = SelectedType,
        .Properties = fromVk(ActualProperties),
        .Dedicated = Dedicated,
        .PersistentlyMapped = static_cast<bool>(ActualProperties & vk::MemoryPropertyFlagBits::eHostVisible)
    };
    return std::shared_ptr<VulkanMemoryAllocation>(new VulkanMemoryAllocation(
        Block, Desc.Requirements, Info, ReservedSize));
}

void VulkanMemoryAllocator::releaseCachedPages()
{
    std::scoped_lock Lock(Mutex);
    std::erase_if(Pages, [](const auto& Page)
    {
        return Page.use_count() == 1 && Page->isEmpty();
    });
}

VulkanDeviceMemory::VulkanDeviceMemory(VulkanDeviceMemory&& Other)
    : DeviceMemory(std::move(Other))
    , VkDeviceMemory(std::move(Other.VkDeviceMemory))
    , Requirements(std::move(Other.Requirements))
    , Property(Other.Property)
    , OwnerDevice(Other.OwnerDevice)
{
    Other.reset();
}
VulkanDeviceMemory& VulkanDeviceMemory::operator=(VulkanDeviceMemory&& Other)
{
    if (this != &Other)
    {
        release();
        DeviceMemory::operator=(std::move(Other));
        VkDeviceMemory = std::exchange(Other.VkDeviceMemory, VK_NULL_HANDLE);
        Requirements = std::move(Other.Requirements);
        Property = Other.Property;
        OwnerDevice = std::exchange(Other.OwnerDevice, vk::Device{ VK_NULL_HANDLE });
        Other.reset();
    }
    return *this;
}

void VulkanDeviceMemory::reset()
{
    VkDeviceMemory = VK_NULL_HANDLE;
    OwnershipState = DeviceMemory::EState::None;
    Requirements = {};
    Property = EMemoryPropertyFlags::enum_type::None;
    OwnerDevice = vk::Device{ VK_NULL_HANDLE };
}

void VulkanDeviceMemory::release() {
    if (OwnershipState == DeviceMemory::EState::OwnsMemory && VkDeviceMemory != VK_NULL_HANDLE)
    {
        if (OwnerDevice)
        {
            OwnerDevice.freeMemory(VkDeviceMemory);
        }
    }
    reset();
}

void* VulkanDeviceMemory::map(DeviceSizeType Offset, DeviceSizeType Size)
{
    if (!OwnerDevice || VkDeviceMemory == VK_NULL_HANDLE)
    {
        throw std::logic_error("Cannot map invalid Vulkan device memory.");
    }
    if (Offset >= Requirements.Size || (Size != 0 && Size > Requirements.Size - Offset))
    {
        throw std::out_of_range("Vulkan device memory map range is out of bounds.");
    }
    void* Data = nullptr;
    vk::Result Result = OwnerDevice.mapMemory(
        VkDeviceMemory,
        Offset,
        Size == 0 ? VK_WHOLE_SIZE : Size,
        vk::MemoryMapFlags(),
        &Data);
    if (Result != vk::Result::eSuccess) {
        // TODO: 处理映射失败的情况
        throw std::runtime_error("Failed to map Vulkan device memory.");
    }
    return Data;
}
void VulkanDeviceMemory::unmap()
{
    assert(OwnerDevice);
    OwnerDevice.unmapMemory(VkDeviceMemory);
}
void VulkanDeviceMemory::flush(DeviceSizeType Offset, DeviceSizeType Size)
{
    vk::Device Device = OwnerDevice;

    vk::MappedMemoryRange Range;
    Range.setMemory(VkDeviceMemory);
    Range.setOffset(Offset);
    Range.setSize(Size == 0 ? VK_WHOLE_SIZE : Size);

    // 刷新 CPU 写入的数据, 使其对 GPU 可见
    Device.flushMappedMemoryRanges({ Range });
}
void VulkanDeviceMemory::invalidate(DeviceSizeType Offset, DeviceSizeType Size)
{
    vk::Device Device = OwnerDevice;

    vk::MappedMemoryRange Range;
    Range.setMemory(VkDeviceMemory);
    Range.setOffset(Offset);
    Range.setSize(Size == 0 ? VK_WHOLE_SIZE : Size);

    // 使 GPU 写入的数据对 CPU 可见
    Device.invalidateMappedMemoryRanges({ Range });
}
MemoryRequirements VulkanDeviceMemory::getMemoryRequirements() const
{
    return Requirements;
}
EMemoryPropertyFlags VulkanDeviceMemory::getMemoryProperty() const
{
    return Property;
}



VulkanDeviceMemoryPool::VulkanDeviceMemoryPool()
{
    pointer NewMemory = static_cast<pointer>(std::malloc(sizeof(VulkanDeviceMemory) * InitialSize));
    if (!NewMemory)
    {
        throw std::bad_alloc();
    }
    Blocks.emplace_back(NewMemory, InitialSize);

    // 对每个对象执行 placement new，并放入 FreeList
    for (int32_t I = 0; I < InitialSize; ++I) {
        pointer Obj = new (&NewMemory[I]) VulkanDeviceMemory(); // 调用默认构造函数
        FreeList.push_back(Obj);
    }
}

VulkanDeviceMemoryPool::~VulkanDeviceMemoryPool()
{
    assert(UsedList.empty() && "Vulkan device memory objects are still in use during pool destruction.");
    UsedList.clear();
    FreeList.clear();

    // 对每个内存块中的每个对象调用析构函数
    for (auto& Block : Blocks) 
    {
        pointer Raw = Block.MemoryBlock;
        size_t Count = Block.Count;
        for (size_t I = 0; I < Count; ++I) 
        {
            Raw[I].~VulkanDeviceMemory(); // 显式析构
        }
        std::free(Raw); // 释放原始内存
    }
}

void VulkanDeviceMemoryPool::reclaim(VulkanDeviceMemory* Memory) 
{
    if (!Memory)
    {
        return;
    }
    auto Used = std::find(UsedList.begin(), UsedList.end(), Memory);
    if (Used == UsedList.end())
    {
        return;
    }
    UsedList.erase(Used);
    FreeList.push_back(Memory);
}

void VulkanDeviceMemoryPool::expand()
{
    // 使用两倍扩容策略:
    // TODO: 使用自定义的内存分配器接口, 而不是直接使用 malloc
    const size_t NewSize = Capacity;

    pointer Raw = static_cast<pointer>(
        std::malloc(sizeof(VulkanDeviceMemory) * NewSize)
    );
    if (!Raw) {
        throw std::bad_alloc();
    }

    Blocks.emplace_back(Raw, NewSize);

    for (size_t I = 0; I < NewSize; ++I) {
        pointer Obj = new (&Raw[I]) VulkanDeviceMemory();
        FreeList.push_back(Obj);
    }

    Capacity += NewSize;
}
VulkanDeviceMemory& VulkanDeviceMemoryPool::allocateMemory()
{
    if (FreeList.empty()) {
        expand();
    }
    // 从 FreeList 取出一个指针
    pointer Mem = FreeList.front();
    FreeList.pop_front();

    UsedList.push_back(Mem);

    return *Mem;
}

void VulkanDeviceMemoryDeleter::operator()(VulkanDeviceMemory* Ptr) const
{
    if (!Ptr)
    {
        return;
    }
    Ptr->release();

    if (Pool)
    {
        Pool->reclaim(Ptr);
    }
}



std::shared_ptr<DeviceMemory> VulkanDeviceMemoryAllocator::allocateMemory(MemoryRequirements Requirements, EMemoryPropertyFlags Property)
{
    if (!Device)
    {
        throw std::invalid_argument("Vulkan memory allocation requires a valid device.");
    }
    return Device->allocateMemory(Requirements, Property);
}

void VulkanDeviceMemoryAllocator::freeMemory(std::shared_ptr<DeviceMemory> Memory)
{
    if (!Device)
    {
        throw std::logic_error("Vulkan memory allocator has no device.");
    }
    Device->freeMemory(std::move(Memory));
}

void VulkanHeapSubAllocation::release()
{
    // 直接使用成员 Heap; 之前的"局部变量遮蔽成员并自移动"是 UB(读取未初始化的
    // 局部 shared_ptr), 会破坏子分配归还逻辑.
    if (Heap && Size != 0)
        Heap->release(Offset, Size);
    Size = 0;
    Offset = 0;
    OwnershipState = DeviceMemory::EState::InValid;
}

} // namespace rhi