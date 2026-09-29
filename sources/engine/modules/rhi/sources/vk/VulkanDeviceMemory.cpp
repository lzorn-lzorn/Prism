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
    const auto remainder = Value % Alignment;
    if (remainder == 0)
        return Value;
    if (Value > std::numeric_limits<vk::DeviceSize>::max() - (Alignment - remainder))
        throw std::overflow_error("Vulkan allocation alignment overflow.");
    return Value + Alignment - remainder;
}

EMemoryProperty fromVk(vk::MemoryPropertyFlags Properties)
{
    EMemoryProperty result;
    if (Properties & vk::MemoryPropertyFlagBits::eDeviceLocal) result.set(EMemoryProperty_t::DeviceLocal);
    if (Properties & vk::MemoryPropertyFlagBits::eHostVisible) result.set(EMemoryProperty_t::HostVisible);
    if (Properties & vk::MemoryPropertyFlagBits::eHostCoherent) result.set(EMemoryProperty_t::HostCoherent);
    if (Properties & vk::MemoryPropertyFlagBits::eHostCached) result.set(EMemoryProperty_t::HostCached);
    if (Properties & vk::MemoryPropertyFlagBits::eLazilyAllocated) result.set(EMemoryProperty_t::LazilyAllocated);
    return result;
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
        vk::MemoryDedicatedAllocateInfo dedicated_info;
        dedicated_info.buffer = InDedicatedBuffer;
		dedicated_info.image = InDedicatedImage;
        vk::MemoryAllocateFlagsInfo flags_info;
        if (DeviceAddress)
            flags_info.flags = vk::MemoryAllocateFlagBits::eDeviceAddress;
        const bool has_dedicated_resource = static_cast<bool>(InDedicatedBuffer) || static_cast<bool>(InDedicatedImage);
        flags_info.pNext = has_dedicated_resource ? &dedicated_info : nullptr;
        vk::MemoryAllocateInfo allocation_info(Size, MemoryTypeIndex);
        allocation_info.pNext = DeviceAddress
            ? static_cast<void*>(&flags_info)
            : has_dedicated_resource ? static_cast<void*>(&dedicated_info) : nullptr;
        Memory = Context->Device->allocateMemory(allocation_info);
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
        std::scoped_lock lock(Mutex);
        size_t best_index = FreeRanges.size();
        vk::DeviceSize best_offset = 0;
        vk::DeviceSize best_waste = std::numeric_limits<vk::DeviceSize>::max();
        for (size_t index = 0; index < FreeRanges.size(); ++index)
        {
            const auto& range = FreeRanges[index];
            const auto offset = alignUp(range.Offset, Alignment);
            if (offset < range.Offset || offset - range.Offset > range.Size ||
                AllocationSize > range.Size - (offset - range.Offset))
                continue;
            const auto waste = range.Size - (offset - range.Offset) - AllocationSize;
            if (waste < best_waste)
            {
                best_index = index;
                best_offset = offset;
                best_waste = waste;
            }
        }
        if (best_index == FreeRanges.size())
            return std::nullopt;

        const Range original = FreeRanges[best_index];
        FreeRanges.erase(FreeRanges.begin() + static_cast<std::ptrdiff_t>(best_index));
        if (best_offset > original.Offset)
            FreeRanges.push_back({ original.Offset, best_offset - original.Offset });
        const auto end = best_offset + AllocationSize;
        const auto original_end = original.Offset + original.Size;
        if (end < original_end)
            FreeRanges.push_back({ end, original_end - end });
        std::ranges::sort(FreeRanges, {}, &Range::Offset);
        return best_offset;
    }

    void free(vk::DeviceSize Offset, vk::DeviceSize AllocationSize)
    {
        std::scoped_lock lock(Mutex);
        FreeRanges.push_back({ Offset, AllocationSize });
        std::ranges::sort(FreeRanges, {}, &Range::Offset);
        std::vector<Range> coalesced;
        coalesced.reserve(FreeRanges.size());
        for (const auto& range : FreeRanges)
        {
            if (!coalesced.empty() &&
                coalesced.back().Offset + coalesced.back().Size == range.Offset)
                coalesced.back().Size += range.Size;
            else
                coalesced.push_back(range);
        }
        FreeRanges = std::move(coalesced);
    }

    [[nodiscard]] bool isEmpty() const
    {
        std::scoped_lock lock(Mutex);
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
        const auto begin = Offset - Offset % AtomSize;
        const auto end = std::min(Size, alignUp(Offset + RangeSize, AtomSize));
        Context->Device->flushMappedMemoryRanges(vk::MappedMemoryRange(Memory, begin, end - begin));
    }

    void invalidate(vk::DeviceSize Offset, vk::DeviceSize RangeSize)
    {
        if (Properties & vk::MemoryPropertyFlagBits::eHostCoherent)
            return;
        const auto begin = Offset - Offset % AtomSize;
        const auto end = std::min(Size, alignUp(Offset + RangeSize, AtomSize));
        Context->Device->invalidateMappedMemoryRanges(vk::MappedMemoryRange(Memory, begin, end - begin));
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
    std::scoped_lock lock(MapMutex);
    if (!Block || !Info.Properties.has(EMemoryProperty_t::HostVisible))
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
    std::scoped_lock lock(MapMutex);
    if (!IsMapped)
        throw std::logic_error("Vulkan allocation is not mapped.");
    // Host-visible suballocation pages stay mapped for their lifetime; this ends client access only.
    IsMapped = false;
}

void VulkanMemoryAllocation::flush(DeviceSizeType Offset, DeviceSizeType Size)
{
    std::scoped_lock lock(MapMutex);
    if (!Block || !IsMapped || Offset >= Info.Size || (Size != 0 && Size > Info.Size - Offset))
        throw std::out_of_range("Vulkan allocation flush range is invalid.");
    Block->flush(Info.Offset + Offset, Size == 0 ? Info.Size - Offset : Size);
}

void VulkanMemoryAllocation::invalidate(DeviceSizeType Offset, DeviceSizeType Size)
{
    std::scoped_lock lock(MapMutex);
    if (!Block || !IsMapped || Offset >= Info.Size || (Size != 0 && Size > Info.Size - Offset))
        throw std::out_of_range("Vulkan allocation invalidate range is invalid.");
    Block->invalidate(Info.Offset + Offset, Size == 0 ? Info.Size - Offset : Size);
}

void VulkanMemoryAllocation::release()
{
    std::shared_ptr<VulkanMemoryBlock> block;
    {
        std::scoped_lock lock(MapMutex);
        if (!Block)
            return;
        IsMapped = false;
        block = std::move(Block);
        OwnershipState = EState::InValid;
    }
    block->free(Info.Offset, ReservedSize);
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

    auto required = toVk(Desc.RequiredProperties);
    auto preferred = toVk(Desc.PreferredProperties);
    switch (Desc.Usage)
    {
    case EMemoryUsage::GPUOnly:
        required |= vk::MemoryPropertyFlagBits::eDeviceLocal;
        break;
    case EMemoryUsage::CPUToGPU:
        required |= vk::MemoryPropertyFlagBits::eHostVisible;
        preferred |= vk::MemoryPropertyFlagBits::eHostCoherent;
        break;
    case EMemoryUsage::GPUToCPU:
        required |= vk::MemoryPropertyFlagBits::eHostVisible;
        preferred |= vk::MemoryPropertyFlagBits::eHostCached;
        break;
    case EMemoryUsage::CPUOnly:
        required |= vk::MemoryPropertyFlagBits::eHostVisible;
        preferred |= vk::MemoryPropertyFlagBits::eHostCoherent | vk::MemoryPropertyFlagBits::eHostCached;
        break;
    case EMemoryUsage::Auto:
    default:
        break;
    }
    if (Desc.PersistentlyMapped && !(required & vk::MemoryPropertyFlagBits::eHostVisible))
        throw std::invalid_argument("Persistently mapped Vulkan memory must be host-visible.");

    const auto memory_properties = Device->getVkPhysicalDevice().getMemoryProperties();
    uint32_t selected_type = UINT32_MAX;
    int selected_score = -1;
    for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index)
    {
        if (!(Desc.Requirements.MemoryTypeBits & (1u << index)))
            continue;
        const auto properties = memory_properties.memoryTypes[index].propertyFlags;
        if ((properties & required) != required)
            continue;
        const int score = std::popcount(static_cast<uint32_t>(properties & preferred));
        if (score > selected_score)
        {
            selected_type = index;
            selected_score = score;
        }
    }
    if (selected_type == UINT32_MAX)
        throw std::runtime_error("Failed to find a suitable Vulkan memory type.");

    const auto actual_properties = memory_properties.memoryTypes[selected_type].propertyFlags;
    const vk::DeviceSize page_size = actual_properties & vk::MemoryPropertyFlagBits::eHostVisible
        ? HostVisiblePageSize : DeviceLocalPageSize;
    const bool dedicated = Desc.Requirements.RequiresDedicatedAllocation ||
        Desc.Requirements.PrefersDedicatedAllocation || Desc.Requirements.Size >= page_size / 2;
    const auto atom_size = std::max<vk::DeviceSize>(
        1, Device->getVkPhysicalDevice().getProperties().limits.nonCoherentAtomSize);
    const bool non_coherent_host =
        (actual_properties & vk::MemoryPropertyFlagBits::eHostVisible) &&
        !(actual_properties & vk::MemoryPropertyFlagBits::eHostCoherent);
    const auto allocation_alignment = std::max<vk::DeviceSize>(
        std::max<uint64_t>(1, Desc.Requirements.Alignment),
        non_coherent_host && !dedicated ? atom_size : 1);
    const auto reserved_size = non_coherent_host && !dedicated
        ? alignUp(Desc.Requirements.Size, atom_size)
        : Desc.Requirements.Size;

    std::scoped_lock lock(Mutex);
    std::shared_ptr<VulkanMemoryBlock> block;
    std::optional<vk::DeviceSize> offset;
    if (!dedicated)
    {
        for (const auto& page : Pages)
        {
            if (!page->isCompatible(selected_type, RequireDeviceAddress))
                continue;
            offset = page->allocate(reserved_size, allocation_alignment);
            if (offset)
            {
                block = page;
                break;
            }
        }
    }
    if (!block)
    {
        const auto block_size = dedicated
            ? reserved_size
            : std::max<vk::DeviceSize>(page_size, alignUp(reserved_size, allocation_alignment));
        block = std::make_shared<VulkanMemoryBlock>(
            Device->getContext(), selected_type, actual_properties, block_size,
            RequireDeviceAddress, dedicated,
            dedicated ? DedicatedBuffer : vk::Buffer {},
            dedicated ? DedicatedImage : vk::Image {});
        offset = block->allocate(reserved_size, allocation_alignment);
        if (!dedicated)
            Pages.push_back(block);
    }
    if (!offset)
        throw std::runtime_error("Vulkan memory page could not satisfy a validated allocation.");

    MemoryAllocationInfo info {
        .Size = Desc.Requirements.Size,
        .Offset = *offset,
        .MemoryTypeIndex = selected_type,
        .Properties = fromVk(actual_properties),
        .Dedicated = dedicated,
        .PersistentlyMapped = static_cast<bool>(actual_properties & vk::MemoryPropertyFlagBits::eHostVisible)
    };
    return std::shared_ptr<VulkanMemoryAllocation>(new VulkanMemoryAllocation(
        block, Desc.Requirements, info, reserved_size));
}

void VulkanMemoryAllocator::releaseCachedPages()
{
    std::scoped_lock lock(Mutex);
    std::erase_if(Pages, [](const auto& page)
    {
        return page.use_count() == 1 && page->isEmpty();
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
	Property = EMemoryProperty::enum_type::None;
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
	void* data = nullptr;
    vk::Result result = OwnerDevice.mapMemory(
        VkDeviceMemory,
        Offset,
        Size == 0 ? VK_WHOLE_SIZE : Size,
        vk::MemoryMapFlags(),
        &data);
	if (result != vk::Result::eSuccess) {
		// TODO: 处理映射失败的情况
		throw std::runtime_error("Failed to map Vulkan device memory.");
	}
	return data;
}
void VulkanDeviceMemory::unmap()
{
	assert(OwnerDevice);
	OwnerDevice.unmapMemory(VkDeviceMemory);
}
void VulkanDeviceMemory::flush(DeviceSizeType Offset, DeviceSizeType Size)
{
	vk::Device device = OwnerDevice;

    vk::MappedMemoryRange range;
    range.setMemory(VkDeviceMemory);
    range.setOffset(Offset);
    range.setSize(Size == 0 ? VK_WHOLE_SIZE : Size);

    // 刷新 CPU 写入的数据, 使其对 GPU 可见
    device.flushMappedMemoryRanges({ range });
}
void VulkanDeviceMemory::invalidate(DeviceSizeType Offset, DeviceSizeType Size)
{
	vk::Device device = OwnerDevice;

    vk::MappedMemoryRange range;
    range.setMemory(VkDeviceMemory);
    range.setOffset(Offset);
    range.setSize(Size == 0 ? VK_WHOLE_SIZE : Size);

    // 使 GPU 写入的数据对 CPU 可见
    device.invalidateMappedMemoryRanges({ range });
}
MemoryRequirements VulkanDeviceMemory::getMemoryRequirements() const
{
	return Requirements;
}
EMemoryProperty VulkanDeviceMemory::getMemoryProperty() const
{
	return Property;
}



VulkanDeviceMemoryPool::VulkanDeviceMemoryPool()
{
    pointer new_memory = static_cast<pointer>(std::malloc(sizeof(VulkanDeviceMemory) * InitialSize));
	if (!new_memory)
	{
        throw std::bad_alloc();
	}
	Blocks.emplace_back(new_memory, InitialSize);

    // 对每个对象执行 placement new，并放入 FreeList
    for (int32_t i = 0; i < InitialSize; ++i) {
        pointer obj = new (&new_memory[i]) VulkanDeviceMemory(); // 调用默认构造函数
        FreeList.push_back(obj);
    }
}

VulkanDeviceMemoryPool::~VulkanDeviceMemoryPool()
{
    assert(UsedList.empty() && "Vulkan device memory objects are still in use during pool destruction.");
    UsedList.clear();
    FreeList.clear();

    // 对每个内存块中的每个对象调用析构函数
    for (auto& block : Blocks) 
	{
        pointer raw = block.MemoryBlock;
        size_t count = block.Count;
        for (size_t i = 0; i < count; ++i) 
		{
            raw[i].~VulkanDeviceMemory(); // 显式析构
        }
        std::free(raw); // 释放原始内存
    }
}

void VulkanDeviceMemoryPool::reclaim(VulkanDeviceMemory* Memory) 
{
    if (!Memory)
    {
        return;
    }
    auto used = std::find(UsedList.begin(), UsedList.end(), Memory);
    if (used == UsedList.end())
    {
        return;
    }
    UsedList.erase(used);
    FreeList.push_back(Memory);
}

void VulkanDeviceMemoryPool::expand()
{
	// 使用两倍扩容策略:
	// TODO: 使用自定义的内存分配器接口, 而不是直接使用 malloc
    const size_t new_size = Capacity;

    pointer raw = static_cast<pointer>(
        std::malloc(sizeof(VulkanDeviceMemory) * new_size)
    );
    if (!raw) {
        throw std::bad_alloc();
    }

    Blocks.emplace_back(raw, new_size);

    for (size_t i = 0; i < new_size; ++i) {
        pointer obj = new (&raw[i]) VulkanDeviceMemory();
        FreeList.push_back(obj);
    }

    Capacity += new_size;
}
VulkanDeviceMemory& VulkanDeviceMemoryPool::allocateMemory()
{
	if (FreeList.empty()) {
		expand();
	}
	// 从 FreeList 取出一个指针
    pointer mem = FreeList.front();
    FreeList.pop_front();

    UsedList.push_back(mem);

    return *mem;
}

void VulkanDeviceMemoryDeleter::operator()(VulkanDeviceMemory* ptr) const
{
	if (!ptr)
	{
		return;
	}
	ptr->release();

    if (Pool)
    {
        Pool->reclaim(ptr);
    }
}



std::shared_ptr<DeviceMemory> VulkanDeviceMemoryAllocator::allocateMemory(MemoryRequirements Requirements, EMemoryProperty Property)
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
	std::shared_ptr<VulkanTransientHeap> heap = std::move(Heap);
	if (heap && Size != 0)
		heap->release(Offset, Size);
	Size = 0;
	Offset = 0;
	OwnershipState = DeviceMemory::EState::InValid;
}

} // namespace rhi