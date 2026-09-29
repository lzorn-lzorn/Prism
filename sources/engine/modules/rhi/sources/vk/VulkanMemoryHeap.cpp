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
	const auto memory_properties = Device.getVkPhysicalDevice().getMemoryProperties();
	const auto required = toVk(Desc.RequiredProperties);
	uint32_t selected_type = UINT32_MAX;
	float selected_score = -1.0f;
	for (uint32_t Index = 0; Index < memory_properties.memoryTypeCount; ++Index)
	{
		if (!(Desc.MemoryTypeBits & (1u << Index)))
			continue;
		const auto properties = memory_properties.memoryTypes[Index].propertyFlags;
		if ((properties & required) != required)
			continue;
		const float score = static_cast<float>(
			std::popcount(static_cast<uint32_t>(properties & toVk(Desc.PreferredProperties))));
		if (score > selected_score)
		{
			selected_score = score;
			selected_type = Index;
		}
	}
	if (selected_type == UINT32_MAX)
		return {};

	const auto allocation_size = alignUp(Desc.Size, std::max<DeviceSizeType>(1, Desc.Alignment));
	vk::MemoryAllocateInfo allocation_info(allocation_size, selected_type);
	vk::DeviceMemory memory;
	try
	{
		memory = Device.getVkDevice().allocateMemory(allocation_info);
	}
	catch (const std::exception&)
	{
		// 显存不足/选择的内存类型不可用: 交给调用方回退到逐资源创建.
		return {};
	}
	if (!memory)
		return {};
	// @note 这里不给 VkDeviceMemory 起调试名: DebugName 需要
	// VK_EXT_debug_utils 的函数指针, 而该扩展未在本后端启用.

	auto heap = std::shared_ptr<VulkanTransientHeap>(
		new VulkanTransientHeap(Device, Desc, memory));
	heap->Descriptor.Size = allocation_size;
	heap->MemoryTypeIndex = selected_type;
	return heap;
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

	std::scoped_lock lock(Mutex);
	for (size_t Index = 0; Index < FreeRanges.size(); ++Index)
	{
		const DeviceSizeType aligned_begin = alignUp(FreeRanges[Index].Begin, Alignment);
		if (aligned_begin + Size > FreeRanges[Index].End)
			continue;

		const DeviceSizeType aligned_end = aligned_begin + Size;
		const Range original = FreeRanges[Index];
		FreeRanges.erase(FreeRanges.begin() + static_cast<ptrdiff_t>(Index));
		if (original.Begin < aligned_begin)
			FreeRanges.insert(
				FreeRanges.begin() + static_cast<ptrdiff_t>(Index), { original.Begin, aligned_begin });
		if (aligned_end < original.End)
			FreeRanges.insert(
				FreeRanges.begin() + static_cast<ptrdiff_t>(Index) +
					(original.Begin < aligned_begin ? 1 : 0),
				{ aligned_end, original.End });
		return aligned_begin;
	}
	return std::nullopt;
}

void VulkanTransientHeap::release(DeviceSizeType Offset, DeviceSizeType Size)
{
	if (Size == 0)
		return;
	std::scoped_lock lock(Mutex);
	FreeRanges.push_back({ Offset, Offset + Size });
	std::sort(FreeRanges.begin(), FreeRanges.end(),
		[](const Range& Lhs, const Range& Rhs) { return Lhs.Begin < Rhs.Begin; });

	std::vector<Range> merged;
	merged.reserve(FreeRanges.size());
	for (const Range& current : FreeRanges)
	{
		if (!merged.empty() && merged.back().End >= current.Begin)
			merged.back().End = std::max(merged.back().End, current.End);
		else
			merged.push_back(current);
	}
	FreeRanges = std::move(merged);
}

} // namespace rhi
