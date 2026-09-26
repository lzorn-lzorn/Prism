#include "RHI.hpp"
#include "VulkanRHI.hpp"
#include "VulkanBindGroup.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanCommandList.hpp"
#include "VulkanDevice.hpp"
#include "VulkanDeviceMemory.hpp"
#include "VulkanImageView.hpp"
#include "VulkanImage.hpp"
#include "VulkanPipeline.hpp"
#include "VulkanRayTracing.hpp"
#include "VulkanSampler.hpp"
#include "VulkanSwapchain.hpp"
#include "VulkanSync.hpp"
#include "VulkanTexture.hpp"
#include "VulkanWSI.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rhi
{

void VulkanDevice::requireReady() const
{
	if (Context->isDeviceLost())
		throw std::runtime_error(
			"Vulkan device is lost; rebuild the RHI/device and all dependent resources.");
}

VulkanDevice::VulkanDevice(VulkanContextPtr InContext)
	: Context(std::move(InContext))
	, GraphicsQueue(Context->Device->getQueue(Context->GraphicsQueueFamilyIndex, 0))
	, DescriptorAllocator(std::make_shared<VulkanDescriptorAllocator>(*this))
{
	const auto queue_families = Context->PhysicalDevice.getQueueFamilyProperties();
	if (Context->GraphicsQueueFamilyIndex >= queue_families.size())
		throw std::out_of_range("Vulkan queue family index is out of range.");

	const auto properties = Context->PhysicalDevice.getProperties();
	Limits.MaxColorAttachments = properties.limits.maxColorAttachments;
	Limits.MaxViewports = properties.limits.maxViewports;
	Limits.MaxFramebufferWidth = properties.limits.maxFramebufferWidth;
	Limits.MaxFramebufferHeight = properties.limits.maxFramebufferHeight;
	Limits.MaxVertexInputBindings = properties.limits.maxVertexInputBindings;
	Limits.MaxVertexInputAttributes = properties.limits.maxVertexInputAttributes;
	Limits.MaxPushConstantSize = properties.limits.maxPushConstantsSize;
	Limits.MaxBoundBindGroups = properties.limits.maxBoundDescriptorSets;
	Limits.MinUniformBufferOffsetAlignment = properties.limits.minUniformBufferOffsetAlignment;
	Limits.MinStorageBufferOffsetAlignment = properties.limits.minStorageBufferOffsetAlignment;
	Limits.MinTexelBufferOffsetAlignment = properties.limits.minTexelBufferOffsetAlignment;
	Limits.TimestampValidBits = queue_families[Context->GraphicsQueueFamilyIndex].timestampValidBits;
	// VkPhysicalDeviceLimits::timestampPeriod is nanoseconds per timestamp tick.
	Limits.TimestampPeriodNanoseconds = properties.limits.timestampPeriod;

	Features = Context->EnabledFeatures;
	if (Features.AccelerationStructure || Features.RayTracingPipeline)
	{
		vk::PhysicalDeviceAccelerationStructurePropertiesKHR acceleration_properties;
		vk::PhysicalDeviceRayTracingPipelinePropertiesKHR pipeline_properties;
		acceleration_properties.pNext = Features.RayTracingPipeline ? &pipeline_properties : nullptr;
		vk::PhysicalDeviceProperties2 properties2;
		properties2.pNext = &acceleration_properties;
		Context->PhysicalDevice.getProperties2(&properties2);
		Limits.MinAccelerationStructureScratchOffsetAlignment =
			acceleration_properties.minAccelerationStructureScratchOffsetAlignment;
		if (Features.RayTracingPipeline)
		{
			Limits.ShaderBindingTableAlignment = pipeline_properties.shaderGroupBaseAlignment;
			Limits.ShaderGroupHandleAlignment = pipeline_properties.shaderGroupHandleAlignment;
			Limits.ShaderGroupHandleSize = pipeline_properties.shaderGroupHandleSize;
			Limits.MaxShaderGroupStride = pipeline_properties.maxShaderGroupStride;
			Limits.MaxRayRecursionDepth = pipeline_properties.maxRayRecursionDepth;
			Limits.MaxRayDispatchInvocationCount = pipeline_properties.maxRayDispatchInvocationCount;
		}
	}
	Features.DeferredRelease = Features.TimelineSemaphore;
	Features.TimestampQueries = queue_families[Context->GraphicsQueueFamilyIndex].timestampValidBits != 0;
	Features.DebugLabels = getVkDevice().getProcAddr("vkCmdBeginDebugUtilsLabelEXT") != nullptr;
	if (const auto capabilities = getSwapchainCapabilities())
		Features.HDRSwapchain = std::ranges::any_of(capabilities->Formats, [](const SurfaceFormat& format)
		{
			return (format.Format == EFormat::RGB10A2_UNorm || format.Format == EFormat::RGBA16_Float) &&
				(format.ColorSpace == EColorSpace::HDR10_ST2084 ||
				 format.ColorSpace == EColorSpace::ExtendedSRGBLinear);
		});

	Queues[0] = std::make_shared<VulkanQueue>(*this, Context, ECommandQueueType::Graphics);
	Queues[1] = std::make_shared<VulkanQueue>(*this, Context, ECommandQueueType::Compute);
	Queues[2] = std::make_shared<VulkanQueue>(*this, Context, ECommandQueueType::Copy);
	MemoryAllocator = std::make_unique<VulkanMemoryAllocator>(*this);
}

VulkanDevice::~VulkanDevice()
{
	try
	{
		waitIdle();
	}
	catch (...)
	{
		drainDeferredReleases();
	}
}

std::shared_ptr<RBuffer> VulkanDevice::createBuffer(const RBuffer::Descriptor_t& Desc)
{
	requireReady();
	return VulkanBuffer::create(*this, Desc);
}

std::shared_ptr<RImage> VulkanDevice::createImage(const RImage::Descriptor_t& Desc)
{
	requireReady();
	return VulkanImage::create(*this, Desc);
}

std::shared_ptr<RImageView> VulkanDevice::createImageView(
	const RImageView::Descriptor_t& Desc)
{
	requireReady();
	return VulkanImageView::create(*this, Desc);
}

std::shared_ptr<RSampler> VulkanDevice::createSampler(const RSampler::Descriptor_t& Desc)
{
	requireReady();
	return std::make_shared<VulkanSampler>(*this, Desc);
}

std::shared_ptr<RShader> VulkanDevice::createShader(const ShaderDescriptor& Desc)
{
	requireReady();
	return std::make_shared<VulkanShader>(*this, Desc);
}

std::shared_ptr<RBindGroupLayout> VulkanDevice::createBindGroupLayout(
	const BindGroupLayoutDescriptor& Desc)
{
	requireReady();
	return std::make_shared<VulkanBindGroupLayout>(*this, Desc);
}

std::shared_ptr<RBindGroup> VulkanDevice::createBindGroup(
	const BindGroupDescriptor& Desc)
{
	requireReady();
	return std::make_shared<VulkanBindGroup>(*this, DescriptorAllocator, Desc);
}

std::shared_ptr<RPipelineLayout> VulkanDevice::createPipelineLayout(
	const PipelineLayoutDescriptor& Desc)
{
	requireReady();
	return std::make_shared<VulkanPipelineLayout>(*this, Desc);
}

std::shared_ptr<RPipelineCache> VulkanDevice::createPipelineCache(
	const PipelineCacheDescriptor& Desc)
{
	requireReady();
	return std::make_shared<VulkanPipelineCache>(*this, Desc);
}

std::shared_ptr<RPipeline> VulkanDevice::createGraphicsPipeline(
	const GraphicsPipelineDescriptor& Desc)
{
	requireReady();
	return createVulkanGraphicsPipeline(*this, Desc);
}

std::shared_ptr<RPipeline> VulkanDevice::createComputePipeline(
	const ComputePipelineDescriptor& Desc)
{
	requireReady();
	return createVulkanComputePipeline(*this, Desc);
}

std::shared_ptr<RPipeline> VulkanDevice::createRayTracingPipeline(
	const RayTracingPipelineDescriptor& Desc)
{
	requireReady();
	return createVulkanRayTracingPipeline(*this, Desc);
}

std::shared_ptr<RAccelerationStructure> VulkanDevice::createAccelerationStructure(
	const AccelerationStructureDescriptor& Desc)
{
	requireReady();
	if (!Features.AccelerationStructure) return {};
	return std::make_shared<VulkanAccelerationStructure>(*this, Desc);
}

AccelerationStructureBuildSizes VulkanDevice::getAccelerationStructureBuildSizes(
	EAccelerationStructureType Type, EAccelerationStructureBuildFlags Flags,
	std::span<const AccelerationStructureGeometry> Geometries) const
{
	requireReady();
	return queryVulkanAccelerationStructureBuildSizes(*this, Type, Flags, Geometries);
}

std::vector<std::byte> VulkanDevice::getRayTracingShaderGroupHandles(
	const std::shared_ptr<RPipeline>& Pipeline, uint32_t FirstGroup, uint32_t GroupCount) const
{
	requireReady();
	if (!Features.RayTracingPipeline || !Pipeline || GroupCount == 0 ||
		Limits.ShaderGroupHandleSize == 0 ||
		GroupCount > std::numeric_limits<size_t>::max() / Limits.ShaderGroupHandleSize)
		return {};
	auto vk_pipeline = std::dynamic_pointer_cast<VulkanPipeline>(Pipeline);
	if (!vk_pipeline || &vk_pipeline->getDevice() != this ||
		vk_pipeline->getType() != EPipelineType::RayTracing || !vk_pipeline->isValid() ||
		FirstGroup > vk_pipeline->getRayTracingGroupCount() ||
		GroupCount > vk_pipeline->getRayTracingGroupCount() - FirstGroup)
		return {};
	std::vector<std::byte> result(static_cast<size_t>(GroupCount) * Limits.ShaderGroupHandleSize);
	auto function = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
		getVkDevice().getProcAddr("vkGetRayTracingShaderGroupHandlesKHR"));
	if (!function || function(static_cast<VkDevice>(getVkDevice()),
		static_cast<VkPipeline>(vk_pipeline->getVkPipeline()), FirstGroup, GroupCount,
		result.size(), result.data()) != VK_SUCCESS)
		return {};
	return result;
}

std::shared_ptr<RCommandList> VulkanDevice::createCommandList(
	const CommandListDescriptor& Desc)
{
	requireReady();
	if (!Features.DynamicRendering || !Features.Synchronization2)
	{
		throw std::logic_error(
			"Vulkan command lists require dynamic rendering and synchronization2.");
	}
	if (Desc.Level == ECommandListLevel::Primary && Desc.RenderingInheritance)
		throw std::invalid_argument("Primary command lists cannot declare rendering inheritance.");
	if (Desc.RenderingInheritance && Desc.QueueType != ECommandQueueType::Graphics)
		throw std::invalid_argument("Rendering inheritance requires a graphics secondary command list.");
	return std::make_shared<VulkanCommandList>(
		*this,
		Context->familyIndex(Desc.QueueType),
		Desc);
}

std::shared_ptr<RSwapchain> VulkanDevice::createSwapchain(const SwapchainDescriptor& Desc)
{
	if (Context->isDeviceLost())
		throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
	return std::make_shared<VulkanSwapchain>(*this, Context, Desc);
}

std::shared_ptr<RQueue> VulkanDevice::getQueue(ECommandQueueType Type)
{
	requireReady();
	return Queues[static_cast<size_t>(Type)];
}

std::shared_ptr<RFence> VulkanDevice::createFence(bool Signaled)
{
	requireReady();
	return std::make_shared<VulkanFence>(*this, Context, Signaled);
}

std::shared_ptr<RSemaphore> VulkanDevice::createSemaphore()
{
	requireReady();
	return std::make_shared<VulkanSemaphore>(*this, Context, false, 0);
}

std::shared_ptr<RSemaphore> VulkanDevice::createTimelineSemaphore(uint64_t InitialValue)
{
	requireReady();
	if (!Features.TimelineSemaphore)
		throw std::logic_error("Timeline semaphores are not supported by this Vulkan device.");
	return std::make_shared<VulkanSemaphore>(*this, Context, true, InitialValue);
}

std::shared_ptr<RQueryPool> VulkanDevice::createQueryPool(const QueryPoolDescriptor& Desc)
{
	requireReady();
	if ((Desc.Type == EQueryType::Timestamp && !Features.TimestampQueries) ||
		(Desc.Type == EQueryType::Occlusion && !Features.OcclusionQueries) ||
		(Desc.Type == EQueryType::PipelineStatistics && !Features.PipelineStatisticsQueries))
		throw std::logic_error("Requested query type is not supported by this Vulkan device.");
	constexpr uint32_t statistic_mask = (1u << 11) - 1;
	if (Desc.Count == 0 || (Desc.PipelineStatistics.Value & ~statistic_mask) != 0 ||
		(Desc.Type == EQueryType::PipelineStatistics) != static_cast<bool>(Desc.PipelineStatistics))
		throw std::invalid_argument("Query pool count or pipeline-statistics mask is invalid.");
	return std::make_shared<VulkanQueryPool>(*this, Context, Desc);
}

FormatCapabilities VulkanDevice::getFormatCapabilities(EFormat Format) const
{
	requireReady();
	const auto properties = Context->PhysicalDevice.getFormatProperties(toVk(Format));
	auto convert = [](vk::FormatFeatureFlags flags)
	{
		EFormatFeatures result;
		if (flags & vk::FormatFeatureFlagBits::eSampledImage) result.set(EFormatFeature_t::Sampled);
		if (flags & vk::FormatFeatureFlagBits::eStorageImage) result.set(EFormatFeature_t::Storage);
		if (flags & vk::FormatFeatureFlagBits::eColorAttachment) result.set(EFormatFeature_t::ColorAttachment);
		if (flags & vk::FormatFeatureFlagBits::eDepthStencilAttachment) result.set(EFormatFeature_t::DepthStencilAttachment);
		if (flags & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) result.set(EFormatFeature_t::LinearFiltering);
		if (flags & vk::FormatFeatureFlagBits::eBlitSrc) result.set(EFormatFeature_t::BlitSource);
		if (flags & vk::FormatFeatureFlagBits::eBlitDst) result.set(EFormatFeature_t::BlitDestination);
		if (flags & vk::FormatFeatureFlagBits::eVertexBuffer) result.set(EFormatFeature_t::VertexBuffer);
		return result;
	};
	return { convert(properties.optimalTilingFeatures),
		convert(properties.linearTilingFeatures), convert(properties.bufferFeatures) };
}

std::optional<SwapchainCapabilities> VulkanDevice::getSwapchainCapabilities() const
{
	if (Context->isDeviceLost() || !Context->Surface) return std::nullopt;
	try
	{
		const auto capabilities = Context->PhysicalDevice.getSurfaceCapabilitiesKHR(Context->Surface->Handle);
		const auto formats = Context->PhysicalDevice.getSurfaceFormatsKHR(Context->Surface->Handle);
		const auto modes = Context->PhysicalDevice.getSurfacePresentModesKHR(Context->Surface->Handle);
		SwapchainCapabilities result;
		result.MinimumImageCount = capabilities.minImageCount;
		result.MaximumImageCount = capabilities.maxImageCount;
		result.MinimumWidth = capabilities.minImageExtent.width;
		result.MinimumHeight = capabilities.minImageExtent.height;
		result.MaximumWidth = capabilities.maxImageExtent.width;
		result.MaximumHeight = capabilities.maxImageExtent.height;
		for (const auto& format : formats)
		{
			EFormat mapped_format = EFormat::Undefined;
			switch (format.format)
			{
			case vk::Format::eR8G8B8A8Unorm: mapped_format = EFormat::RGBA8_UNorm; break;
			case vk::Format::eR8G8B8A8Srgb: mapped_format = EFormat::RGBA8_sRGB; break;
			case vk::Format::eB8G8R8A8Unorm: mapped_format = EFormat::BGRA8_UNorm; break;
			case vk::Format::eB8G8R8A8Srgb: mapped_format = EFormat::BGRA8_sRGB; break;
			case vk::Format::eA2B10G10R10UnormPack32: mapped_format = EFormat::RGB10A2_UNorm; break;
			case vk::Format::eR16G16B16A16Sfloat: mapped_format = EFormat::RGBA16_Float; break;
			default: break;
			}
			std::optional<EColorSpace> mapped_space;
			switch (format.colorSpace)
			{
			case vk::ColorSpaceKHR::eSrgbNonlinear: mapped_space = EColorSpace::SRGB_Nonlinear; break;
			case vk::ColorSpaceKHR::eAdobergbNonlinearEXT: mapped_space = EColorSpace::AdobeRGB; break;
			case vk::ColorSpaceKHR::eDciP3NonlinearEXT: mapped_space = EColorSpace::DCIP3; break;
			case vk::ColorSpaceKHR::eBt2020LinearEXT: mapped_space = EColorSpace::Rec2020; break;
			case vk::ColorSpaceKHR::eHdr10St2084EXT: mapped_space = EColorSpace::HDR10_ST2084; break;
			case vk::ColorSpaceKHR::eExtendedSrgbLinearEXT: mapped_space = EColorSpace::ExtendedSRGBLinear; break;
			default: break;
			}
			if (mapped_format != EFormat::Undefined && mapped_space)
				result.Formats.push_back({ mapped_format, *mapped_space });
		}
		for (const auto mode : modes)
		{
			switch (mode)
			{
			case vk::PresentModeKHR::eImmediate: result.PresentModes.push_back(EPresentMode::Immediate); break;
			case vk::PresentModeKHR::eFifo: result.PresentModes.push_back(EPresentMode::Fifo); break;
			case vk::PresentModeKHR::eFifoRelaxed: result.PresentModes.push_back(EPresentMode::FifoRelaxed); break;
			case vk::PresentModeKHR::eMailbox: result.PresentModes.push_back(EPresentMode::Mailbox); break;
			default: break;
			}
		}
		const bool hdr_format = std::ranges::any_of(result.Formats, [](const SurfaceFormat& format)
		{
			return format.ColorSpace == EColorSpace::HDR10_ST2084 ||
				format.ColorSpace == EColorSpace::ExtendedSRGBLinear;
		});
		result.SupportsHDRMetadata = Context->EnabledFeatures.HDRMetadata && hdr_format;
		return result;
	}
	catch (const vk::SystemError& error)
	{
		const auto status = vulkan_wsi::mapStatus(vulkan_wsi::resultFromSystemError(error));
		if (status && *status == ESwapchainStatus::DeviceLost) Context->markDeviceLost();
		return std::nullopt;
	}
}

std::shared_ptr<RTexture> VulkanDevice::createTexture(const RTexture::Descriptor_t& Desc)
{
	requireReady();
	return VulkanTexture::create(*this, Desc);
}

std::shared_ptr<DeviceMemory> VulkanDevice::allocateMemory(
	MemoryRequirements Requirements,
	EMemoryProperty Property)
{
	requireReady();
	if (Requirements.Size == 0 || Requirements.MemoryTypeBits == 0)
	{
		throw std::invalid_argument("Vulkan memory requirements are invalid.");
	}

	const vk::MemoryAllocateInfo allocation_info(
		Requirements.Size,
		findMemoryType(Requirements.MemoryTypeBits, toVk(Property)));
	vk::DeviceMemory device_memory = Context->Device->allocateMemory(allocation_info);

	auto& memory = VulkanDeviceMemoryPool::self().allocateMemory();
	memory.OwnerDevice = Context->Device.get();
	memory.VkDeviceMemory = device_memory;
	memory.Requirements = Requirements;
	memory.Property = Property;
	memory.OwnershipState = DeviceMemory::EState::OwnsMemory;

	return std::shared_ptr<DeviceMemory>(
		&memory,
		VulkanDeviceMemoryDeleter(&VulkanDeviceMemoryPool::self()));
}

std::shared_ptr<DeviceMemory> VulkanDevice::allocateMemory(
	const MemoryAllocationDescriptor& Desc)
{
	requireReady();
	return MemoryAllocator->allocate(Desc, false);
}

std::shared_ptr<DeviceMemory> VulkanDevice::allocateBufferMemory(
	const MemoryAllocationDescriptor& Desc,
	bool RequireDeviceAddress,
	vk::Buffer Buffer)
{
	requireReady();
	return MemoryAllocator->allocate(Desc, RequireDeviceAddress, Buffer);
}

std::shared_ptr<DeviceMemory> VulkanDevice::allocateImageMemory(
	const MemoryAllocationDescriptor& Desc,
	vk::Image Image)
{
	requireReady();
	return MemoryAllocator->allocate(Desc, false, {}, Image);
}

void VulkanDevice::freeMemory(std::shared_ptr<DeviceMemory> Memory)
{
	if (Memory)
	{
		Memory->release();
	}
}

bool VulkanDevice::deferRelease(
	std::shared_ptr<void> Resource,
	const std::shared_ptr<RSemaphore>& CompletionSemaphore,
	uint64_t CompletionValue)
{
	if (Context->isDeviceLost()) return false;
	if (!Features.DeferredRelease || !Resource || !CompletionSemaphore ||
		!CompletionSemaphore->isTimeline() || &CompletionSemaphore->getDevice() != this)
		return false;

	std::scoped_lock lock(DeferredReleaseMutex);
	DeferredReleases.push_back({
		.Sequence = NextDeferredReleaseSequence++,
		.CompletionValue = CompletionValue,
		.CompletionSemaphore = CompletionSemaphore,
		.Resource = std::move(Resource)
	});
	return true;
}

void VulkanDevice::collectDeferredReleases()
{
	if (Context->isDeviceLost()) { drainDeferredReleases(); return; }
	std::vector<DeferredReleaseEntry> ready;
	{
		std::scoped_lock lock(DeferredReleaseMutex);
		for (auto entry = DeferredReleases.begin(); entry != DeferredReleases.end();)
		{
			if (entry->CompletionSemaphore->getCompletedValue() >= entry->CompletionValue)
			{
				ready.push_back(std::move(*entry));
				entry = DeferredReleases.erase(entry);
			}
			else
			{
				++entry;
			}
		}
	}
	std::ranges::sort(ready, {}, &DeferredReleaseEntry::Sequence);
	// Reset explicitly outside the mutex: destructors may call arbitrary engine code or enqueue again.
	for (auto& entry : ready)
	{
		entry.Resource.reset();
		entry.CompletionSemaphore.reset();
	}
}

void VulkanDevice::drainDeferredReleases() noexcept
{
	std::vector<DeferredReleaseEntry> releases;
	{
		std::scoped_lock lock(DeferredReleaseMutex);
		releases.swap(DeferredReleases);
	}
	std::ranges::sort(releases, {}, &DeferredReleaseEntry::Sequence);
	for (auto& entry : releases)
	{
		entry.Resource.reset();
		entry.CompletionSemaphore.reset();
	}
}

void VulkanDevice::waitIdle()
{
	requireReady();
	try { Context->Device->waitIdle(); }
	catch (const vk::DeviceLostError&) { Context->markDeviceLost(); throw; }
	// Retirement invariant: idle proves every queued completion point, so no semaphore wait is needed.
	drainDeferredReleases();
	if (MemoryAllocator)
		MemoryAllocator->releaseCachedPages();
}

void* VulkanDevice::getNativeHandle() const
{
	if (Context->isDeviceLost()) return nullptr;
	return static_cast<VkDevice>(Context->Device.get());
}

uint32_t VulkanDevice::findMemoryType(uint32_t TypeBits, vk::MemoryPropertyFlags Properties)
{
	vk::PhysicalDeviceMemoryProperties memory_properties = Context->PhysicalDevice.getMemoryProperties();

	for (uint32_t i = 0; i < memory_properties.memoryTypeCount; i++)
	{
		if ((TypeBits & (1u << i)) &&
			(memory_properties.memoryTypes[i].propertyFlags & Properties) == Properties)
		{
			return i;
		}
	}
	throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
}

}


 