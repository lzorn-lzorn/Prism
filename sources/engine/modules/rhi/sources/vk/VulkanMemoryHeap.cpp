#include "VulkanMemoryHeap.hpp"

#include "VulkanDevice.hpp"
#include "VulkanRHI.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace rhi
{

namespace
{
[[nodiscard]] DeviceSizeType alignUp(DeviceSizeType Value, DeviceSizeType Alignment) noexcept
{
    if (Alignment <= 1)
        return Value;
    return (Value + Alignment - 1) / Alignment * Alignment;
}
} // namespace

VulkanTransientHeap::VulkanTransientHeap(
    VulkanDevice& InDevice,
    MemoryHeapDescriptor InDescriptor,
    vk::DeviceMemory InMemory)
    : Device(&InDevice)
    , Descriptor(std::move(InDescriptor))
    , Memory(InMemory)
{
    FreeRanges.push_back({ 0, Descriptor.Size });
}

VulkanTransientHeap::~VulkanTransientHeap()
{
    if (Device && Device->getVkDevice() && Memory)
    {
        Device->getVkDevice().freeMemory(Memory);
        Memory = VK_NULL_HANDLE;
    }
}

std::shared_ptr<VulkanTransientHeap> VulkanTransientHeap::create(
    VulkanDevice& Device,
    const MemoryHeapDescriptor& Desc)
{
    if (Desc.Size == 0 || Desc.MemoryTypeBits == 0)
        return {};

    // 堆允许使用的内存类型必须同时满足调用方要求.
    const auto MemoryProperties = Device.getVkPhysicalDevice().getMemoryProperties();
    const auto Required = toVk(Desc.RequiredProperties);
    uint32_t SelectedType = UINT32_MAX;
    float SelectedScore = -1.0f;
    for (uint32_t Index = 0; Index < MemoryProperties.memoryTypeCount; ++Index)
    {
        if (!(Desc.MemoryTypeBits & (1u << Index)))
            continue;
        const auto Properties = MemoryProperties.memoryTypes[Index].propertyFlags;
        if ((Properties & Required) != Required)
            continue;
        const float Score = static_cast<float>(
            std::popcount(static_cast<uint32_t>(Properties & toVk(Desc.PreferredProperties))));
        if (Score > SelectedScore)
        {
            SelectedScore = Score;
            SelectedType = Index;
        }
    }
    if (SelectedType == UINT32_MAX)
        return {};

    const auto AllocationSize = alignUp(Desc.Size, std::max<DeviceSizeType>(1, Desc.Alignment));
    vk::MemoryAllocateInfo AllocationInfo(AllocationSize, SelectedType);
    vk::DeviceMemory Memory;
    try
    {
        Memory = Device.getVkDevice().allocateMemory(AllocationInfo);
    }
    catch (const std::exception&)
    {
        // 显存不足/选择的内存类型不可用: 交给调用方回退到逐资源创建.
        return {};
    }
    if (!Memory)
        return {};
    // @note 这里不给 VkDeviceMemory 起调试名: DebugName 需要
    // VK_EXT_debug_utils 的函数指针, 而该扩展未在本后端启用.

    auto Heap = std::shared_ptr<VulkanTransientHeap>(
        new VulkanTransientHeap(Device, Desc, Memory));
    Heap->Descriptor.Size = AllocationSize;
    Heap->MemoryTypeIndex = SelectedType;
    return Heap;
}

RDevice& VulkanTransientHeap::getDevice() const noexcept
{
    return *Device;
}

void* VulkanTransientHeap::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkDeviceMemory>(Memory));
}

std::optional<DeviceSizeType> VulkanTransientHeap::suballocate(
    DeviceSizeType Size,
    DeviceSizeType Alignment,
    uint32_t TypeBits)
{
    if (Size == 0)
        return DeviceSizeType { 0 };
    // 子分配必须与堆的内存类型一致, 否则绑定会失败.
    if (TypeBits != 0 && (TypeBits & (1u << MemoryTypeIndex)) == 0)
        return std::nullopt;

    std::scoped_lock Lock(Mutex);
    for (size_t Index = 0; Index < FreeRanges.size(); ++Index)
    {
        const DeviceSizeType AlignedBegin = alignUp(FreeRanges[Index].Begin, Alignment);
        if (AlignedBegin + Size > FreeRanges[Index].End)
            continue;

        const DeviceSizeType AlignedEnd = AlignedBegin + Size;
        const Range Original = FreeRanges[Index];
        FreeRanges.erase(FreeRanges.begin() + static_cast<ptrdiff_t>(Index));
        if (Original.Begin < AlignedBegin)
            FreeRanges.insert(
                FreeRanges.begin() + static_cast<ptrdiff_t>(Index), { Original.Begin, AlignedBegin });
        if (AlignedEnd < Original.End)
            FreeRanges.insert(
                FreeRanges.begin() + static_cast<ptrdiff_t>(Index) +
                    (Original.Begin < AlignedBegin ? 1 : 0),
                { AlignedEnd, Original.End });
        return AlignedBegin;
    }
    return std::nullopt;
}

void VulkanTransientHeap::release(DeviceSizeType Offset, DeviceSizeType Size)
{
    if (Size == 0)
        return;
    std::scoped_lock Lock(Mutex);
    FreeRanges.push_back({ Offset, Offset + Size });
    std::sort(FreeRanges.begin(), FreeRanges.end(),
        [](const Range& Lhs, const Range& Rhs) { return Lhs.Begin < Rhs.Begin; });

    std::vector<Range> Merged;
    Merged.reserve(FreeRanges.size());
    for (const Range& Current : FreeRanges)
    {
        if (!Merged.empty() && Merged.back().End >= Current.Begin)
            Merged.back().End = std::max(Merged.back().End, Current.End);
        else
            Merged.push_back(Current);
    }
    FreeRanges = std::move(Merged);
}

} // namespace rhi
