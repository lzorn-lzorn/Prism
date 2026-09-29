#include "VulkanImage.hpp"

#include "VulkanDevice.hpp"
#include "VulkanDeviceMemory.hpp"
#include "VulkanRHI.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace rhi
{
namespace
{
uint32_t calculateMaximumMipLevels(const RImage::Descriptor_t& Desc)
{
	return std::bit_width(std::max({ Desc.Width, Desc.Height, Desc.Depth }));
}

vk::ImageCreateFlags getImageCreateFlags(EImageDimension Dimension)
{
	if (Dimension == EImageDimension::Cube || Dimension == EImageDimension::CubeArray)
	{
		return vk::ImageCreateFlagBits::eCubeCompatible;
	}
	return {};
}
} // namespace

VulkanImage::VulkanImage(VulkanDevice& InDevice, const Descriptor_t& Desc, vk::UniqueImage Image)
	: RImage(Desc), Device(&InDevice), Image(std::move(Image))
{
}

VulkanImage::VulkanImage(VulkanDevice& InDevice, const Descriptor_t& Desc, vk::Image Image)
	: RImage(Desc), Device(&InDevice), Image(Image)
{
}

std::shared_ptr<VulkanImage> VulkanImage::create(
	VulkanDevice& Device,
	const Descriptor_t& Desc,
	DeviceMemoryAllocator* Allocator)
{
	auto image = createUnbound(Device, Desc);
	image->allocateAndBindMemory(Allocator);
	return image;
}

std::shared_ptr<VulkanImage> VulkanImage::createUnbound(VulkanDevice& Device, const Descriptor_t& Desc)
{
	validateDescriptor(Desc);
	auto image = Device.getVkDevice().createImageUnique(makeCreateInfo(Desc, false));
	return std::shared_ptr<VulkanImage>(new VulkanImage(Device, Desc, std::move(image)));
}

std::shared_ptr<VulkanImage> VulkanImage::createUnboundAliasing(
	VulkanDevice& Device,
	const Descriptor_t& Desc)
{
	validateDescriptor(Desc);
	// VK_IMAGE_CREATE_ALIAS_BIT: 允许该 Image 与其它 Image 复用同一段显存.
	auto image = Device.getVkDevice().createImageUnique(makeCreateInfo(Desc, true));
	return std::shared_ptr<VulkanImage>(new VulkanImage(Device, Desc, std::move(image)));
}

MemoryRequirements VulkanImage::getMemoryRequirements() const
{
	if (isExternal() || !std::get<vk::UniqueImage>(Image))
		throw std::logic_error("Vulkan image is invalid for memory requirements.");
	vk::MemoryDedicatedRequirements dedicated_requirements;
	vk::MemoryRequirements2 requirements2;
	requirements2.pNext = &dedicated_requirements;
	const vk::ImageMemoryRequirementsInfo2 requirements_info(std::get<vk::UniqueImage>(Image).get());
	Device->getVkDevice().getImageMemoryRequirements2(&requirements_info, &requirements2);
	const vk::MemoryRequirements& vk_requirements = requirements2.memoryRequirements;
	return MemoryRequirements {
		.Size = vk_requirements.size,
		.Alignment = vk_requirements.alignment,
		.MemoryTypeBits = vk_requirements.memoryTypeBits,
		.PrefersDedicatedAllocation = dedicated_requirements.prefersDedicatedAllocation == VK_TRUE,
		.RequiresDedicatedAllocation = dedicated_requirements.requiresDedicatedAllocation == VK_TRUE
	};
}

void VulkanImage::bindPlacedMemory(std::shared_ptr<DeviceMemory> InMemory)
{
	Memory = std::move(InMemory);
}

std::shared_ptr<VulkanImage> VulkanImage::wrapExternal(
	VulkanDevice& Device,
	const Descriptor_t& Desc,
	vk::Image Image)
{
	validateDescriptor(Desc);
	if (!Image)
	{
		throw std::invalid_argument("Cannot wrap a null Vulkan image.");
	}
	return std::shared_ptr<VulkanImage>(new VulkanImage(Device, Desc, Image));
}

void VulkanImage::allocateAndBindMemory(DeviceMemoryAllocator* Allocator)
{
	if (isExternal())
	{
		throw std::logic_error("External Vulkan images cannot allocate owned memory.");
	}
	auto& owned_image = std::get<vk::UniqueImage>(Image);
	if (!owned_image || Memory)
	{
		throw std::logic_error(Memory ? "Vulkan image memory is already bound." : "Vulkan image is invalid.");
	}

	vk::MemoryDedicatedRequirements dedicated_requirements;
	vk::MemoryRequirements2 requirements2;
	requirements2.pNext = &dedicated_requirements;
	const vk::ImageMemoryRequirementsInfo2 requirements_info(owned_image.get());
	Device->getVkDevice().getImageMemoryRequirements2(&requirements_info, &requirements2);
	const vk::MemoryRequirements& vk_requirements = requirements2.memoryRequirements;
	const MemoryRequirements requirements {
		.Size = vk_requirements.size,
		.Alignment = vk_requirements.alignment,
		.MemoryTypeBits = vk_requirements.memoryTypeBits,
		.PrefersDedicatedAllocation = dedicated_requirements.prefersDedicatedAllocation == VK_TRUE,
		.RequiresDedicatedAllocation = dedicated_requirements.requiresDedicatedAllocation == VK_TRUE
	};

	std::shared_ptr<DeviceMemory> memory;
	if (Allocator)
		memory = Allocator->allocateMemory(requirements, getDescriptor().MemoryProperty);
	else
		memory = Device->allocateImageMemory({
			.Requirements = requirements,
			.Usage = EMemoryUsage::GPUOnly,
			.RequiredProperties = getDescriptor().MemoryProperty,
			.DebugName = "ImageMemory"
		}, owned_image.get());

	vk::DeviceMemory native_memory;
	DeviceSizeType memory_offset = 0;
	if (auto suballocation = std::dynamic_pointer_cast<VulkanMemoryAllocation>(memory))
	{
		native_memory = suballocation->getVkDeviceMemory();
		memory_offset = suballocation->getAllocationInfo()->Offset;
	}
	else if (auto legacy = std::dynamic_pointer_cast<VulkanDeviceMemory>(memory))
	{
		native_memory = legacy->getVkDeviceMemory();
	}
	else
	{
		throw std::invalid_argument("Image allocator returned non-Vulkan device memory.");
	}

	Device->getVkDevice().bindImageMemory(owned_image.get(), native_memory, memory_offset);
	Memory = std::move(memory);
}

void* VulkanImage::getNativeHandle() const noexcept
{
	return reinterpret_cast<void*>(static_cast<VkImage>(getVkImage()));
}

RDevice& VulkanImage::getDevice() const noexcept
{
	return *Device;
}

vk::Image VulkanImage::getVkImage() const noexcept
{
	if (const auto* external_image = std::get_if<vk::Image>(&Image))
	{
		return *external_image;
	}
	return std::get<vk::UniqueImage>(Image).get();
}

void VulkanImage::validateDescriptor(const Descriptor_t& Desc)
{
	if (Desc.Format == EFormat::Undefined)
		throw std::invalid_argument("Vulkan image format must be defined.");
	if (Desc.Width == 0 || Desc.Height == 0 || Desc.Depth == 0 || Desc.MipLevels == 0 || Desc.ArrayLayers == 0)
		throw std::invalid_argument("Image dimensions, mip levels and array layers must be non-zero.");
	if (toVk(Desc.Usage) == vk::ImageUsageFlags{})
		throw std::invalid_argument("Vulkan image usage must not be empty.");
	if (Desc.MipLevels > calculateMaximumMipLevels(Desc))
		throw std::invalid_argument("Image mip level count exceeds its dimensions.");
	if (Desc.SampleCount != ESampleCount::Count1 && Desc.MipLevels != 1)
		throw std::invalid_argument("Multisampled images must have exactly one mip level.");

	switch (Desc.Dimension)
	{
	case EImageDimension::Texture1D:
		if (Desc.Height != 1 || Desc.Depth != 1 || Desc.ArrayLayers != 1)
			throw std::invalid_argument("A 1D image requires height, depth and array layers equal to one.");
		break;
	case EImageDimension::Texture1DArray:
		if (Desc.Height != 1 || Desc.Depth != 1)
			throw std::invalid_argument("A 1D array image requires height and depth equal to one.");
		break;
	case EImageDimension::Texture2D:
		if (Desc.Depth != 1 || Desc.ArrayLayers != 1)
			throw std::invalid_argument("A 2D image requires depth and array layers equal to one.");
		break;
	case EImageDimension::Texture2DArray:
		if (Desc.Depth != 1)
			throw std::invalid_argument("A 2D array image requires depth equal to one.");
		break;
	case EImageDimension::Texture3D:
		if (Desc.ArrayLayers != 1)
			throw std::invalid_argument("A 3D image requires exactly one array layer.");
		break;
	case EImageDimension::Cube:
		if (Desc.Width != Desc.Height || Desc.Depth != 1 || Desc.ArrayLayers != 6)
			throw std::invalid_argument("A cube image requires square faces, depth one and six layers.");
		break;
	case EImageDimension::CubeArray:
		if (Desc.Width != Desc.Height || Desc.Depth != 1 || Desc.ArrayLayers % 6 != 0)
			throw std::invalid_argument("A cube array requires square faces and a layer count divisible by six.");
		break;
	}
}

vk::ImageCreateInfo VulkanImage::makeCreateInfo(const Descriptor_t& Desc, bool EnableAliasing)
{
	auto flags = getImageCreateFlags(Desc.Dimension);
	if (EnableAliasing)
		flags |= vk::ImageCreateFlagBits::eAlias;
	return vk::ImageCreateInfo()
		.setFlags(flags)
		.setImageType(toVk(Desc.Dimension))
		.setFormat(toVk(Desc.Format))
		.setExtent(vk::Extent3D(Desc.Width, Desc.Height, Desc.Depth))
		.setMipLevels(Desc.MipLevels)
		.setArrayLayers(Desc.ArrayLayers)
		.setSamples(toVk(Desc.SampleCount))
		.setTiling(vk::ImageTiling::eOptimal)
		.setUsage(toVk(Desc.Usage))
		.setSharingMode(Desc.SharingMode == ESharingMode::Concurrent
			? vk::SharingMode::eConcurrent
			: vk::SharingMode::eExclusive)
		.setInitialLayout(vk::ImageLayout::eUndefined);
}

} // namespace rhi
