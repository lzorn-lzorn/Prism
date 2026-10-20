#include "RHI.hpp"
#include "VulkanRHI.hpp"
#include "VulkanBindGroup.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanCommandList.hpp"
#include "VulkanDevice.hpp"
#include "VulkanDeviceMemory.hpp"
#include "VulkanImageView.hpp"
#include "VulkanImage.hpp"
#include "VulkanMemoryHeap.hpp"
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
    const auto QueueFamilies = Context->PhysicalDevice.getQueueFamilyProperties();
    if (Context->GraphicsQueueFamilyIndex >= QueueFamilies.size())
        throw std::out_of_range("Vulkan queue family index is out of range.");

    const auto Properties = Context->PhysicalDevice.getProperties();
    Limits.MaxColorAttachments = Properties.limits.maxColorAttachments;
    Limits.MaxViewports = Properties.limits.maxViewports;
    Limits.MaxFramebufferWidth = Properties.limits.maxFramebufferWidth;
    Limits.MaxFramebufferHeight = Properties.limits.maxFramebufferHeight;
    Limits.MaxVertexInputBindings = Properties.limits.maxVertexInputBindings;
    Limits.MaxVertexInputAttributes = Properties.limits.maxVertexInputAttributes;
    Limits.MaxPushConstantSize = Properties.limits.maxPushConstantsSize;
    Limits.MaxBoundBindGroups = Properties.limits.maxBoundDescriptorSets;
    Limits.MinUniformBufferOffsetAlignment = Properties.limits.minUniformBufferOffsetAlignment;
    Limits.MinStorageBufferOffsetAlignment = Properties.limits.minStorageBufferOffsetAlignment;
    Limits.MinTexelBufferOffsetAlignment = Properties.limits.minTexelBufferOffsetAlignment;
    Limits.TimestampValidBits = QueueFamilies[Context->GraphicsQueueFamilyIndex].timestampValidBits;
    // VkPhysicalDeviceLimits::timestampPeriod is nanoseconds per timestamp tick.
    Limits.TimestampPeriodNanoseconds = Properties.limits.timestampPeriod;

    Features = Context->EnabledFeatures;
    if (Features.AccelerationStructure || Features.RayTracingPipeline)
    {
        vk::PhysicalDeviceAccelerationStructurePropertiesKHR AccelerationProperties;
        vk::PhysicalDeviceRayTracingPipelinePropertiesKHR PipelineProperties;
        AccelerationProperties.pNext = Features.RayTracingPipeline ? &PipelineProperties : nullptr;
        vk::PhysicalDeviceProperties2 Properties2;
        Properties2.pNext = &AccelerationProperties;
        Context->PhysicalDevice.getProperties2(&Properties2);
        Limits.MinAccelerationStructureScratchOffsetAlignment =
            AccelerationProperties.minAccelerationStructureScratchOffsetAlignment;
        if (Features.RayTracingPipeline)
        {
            Limits.ShaderBindingTableAlignment = PipelineProperties.shaderGroupBaseAlignment;
            Limits.ShaderGroupHandleAlignment = PipelineProperties.shaderGroupHandleAlignment;
            Limits.ShaderGroupHandleSize = PipelineProperties.shaderGroupHandleSize;
            Limits.MaxShaderGroupStride = PipelineProperties.maxShaderGroupStride;
            Limits.MaxRayRecursionDepth = PipelineProperties.maxRayRecursionDepth;
            Limits.MaxRayDispatchInvocationCount = PipelineProperties.maxRayDispatchInvocationCount;
        }
    }
    Features.DeferredRelease = Features.TimelineSemaphore;
    Features.TimestampQueries = QueueFamilies[Context->GraphicsQueueFamilyIndex].timestampValidBits != 0;
    Features.DebugLabels = getVkDevice().getProcAddr("vkCmdBeginDebugUtilsLabelEXT") != nullptr;
    if (const auto Capabilities = getSwapchainCapabilities())
        Features.HDRSwapchain = std::ranges::any_of(Capabilities->Formats, [](const SurfaceFormat& Format)
        {
            return (Format.Format == EFormat::RGB10A2_UNorm || Format.Format == EFormat::RGBA16_Float) &&
                (Format.ColorSpace == EColorSpace::HDR10_ST2084 ||
                 Format.ColorSpace == EColorSpace::ExtendedSRGBLinear);
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

std::shared_ptr<RTransientHeap> VulkanDevice::createTransientHeap(
    const MemoryHeapDescriptor& Desc)
{
    requireReady();
    return VulkanTransientHeap::create(*this, Desc);
}

std::optional<MemoryRequirements> VulkanDevice::getBufferMemoryRequirements(
    const BufferRequirementsRequest& Desc)
{
    requireReady();
    RBuffer::Descriptor_t BufferDesc;
    BufferDesc.Size = Desc.Size;
    BufferDesc.Usage = Desc.Usage;
    BufferDesc.MemoryUsage = Desc.MemoryUsage;
    BufferDesc.MemoryProperty = Desc.MemoryProperty;
    BufferDesc.PreferredMemoryProperty = Desc.PreferredMemoryProperty;
    BufferDesc.DedicatedAllocation = Desc.DedicatedAllocation;

    auto Buffer = VulkanBuffer::createUnbound(*this, BufferDesc);
    if (!Buffer)
        return std::nullopt;
    return Buffer->getMemoryRequirements();
}

std::optional<MemoryRequirements> VulkanDevice::getImageMemoryRequirements(
    const ImageRequirementsRequest& Desc)
{
    requireReady();
    RImage::Descriptor_t ImageDesc;
    ImageDesc.Format = Desc.Format;
    ImageDesc.Dimension = Desc.Dimension;
    ImageDesc.Width = Desc.Width;
    ImageDesc.Height = Desc.Height;
    ImageDesc.Depth = Desc.Depth;
    ImageDesc.MipLevels = Desc.MipLevels;
    ImageDesc.ArrayLayers = Desc.ArrayLayers;
    ImageDesc.SharingMode = Desc.SharingMode;
    ImageDesc.Usage = Desc.Usage;
    ImageDesc.SampleCount = Desc.SampleCount;
    ImageDesc.MemoryProperty = Desc.MemoryProperty;

    // 用带别名标志的路径查询: 别名会改变驱动给出的显存需求估算.
    auto Image = VulkanImage::createUnboundAliasing(*this, ImageDesc);
    if (!Image)
        return std::nullopt;
    return Image->getMemoryRequirements();
}

std::shared_ptr<RBuffer> VulkanDevice::createPlacedBuffer(
    const RBuffer::Descriptor_t& Desc,
    const std::shared_ptr<RTransientHeap>& Heap,
    DeviceSizeType Offset)
{
    requireReady();
    auto VkHeap = std::dynamic_pointer_cast<VulkanTransientHeap>(Heap);
    if (!VkHeap || !VkHeap->isValid())
        return {};

    const auto Buffer = VulkanBuffer::createUnbound(*this, Desc);
    if (!Buffer)
        return {};

    const auto Requirements = Buffer->getMemoryRequirements();
    if (Offset % std::max<DeviceSizeType>(1, Requirements.Alignment) != 0 ||
        Offset + Requirements.Size > VkHeap->getSize() ||
        (Requirements.MemoryTypeBits & (1u << VkHeap->getMemoryTypeIndex())) == 0)
        return {};

    getVkDevice().bindBufferMemory(Buffer->getVkBuffer(), VkHeap->getVkDeviceMemory(), Offset);
    Buffer->bindPlacedMemory(
        std::make_shared<VulkanHeapSubAllocation>(VkHeap, Offset, Requirements.Size));
    return Buffer;
}

std::shared_ptr<RImage> VulkanDevice::createPlacedImage(
    const RImage::Descriptor_t& Desc,
    const std::shared_ptr<RTransientHeap>& Heap,
    DeviceSizeType Offset)
{
    requireReady();
    auto VkHeap = std::dynamic_pointer_cast<VulkanTransientHeap>(Heap);
    if (!VkHeap || !VkHeap->isValid())
        return {};

    // 复用同一段显存的所有 Image 都必须带 VK_IMAGE_CREATE_ALIAS_BIT.
    const auto Image = VulkanImage::createUnboundAliasing(*this, Desc);
    if (!Image)
        return {};

    const auto Requirements = Image->getMemoryRequirements();
    if (Offset % std::max<DeviceSizeType>(1, Requirements.Alignment) != 0 ||
        Offset + Requirements.Size > VkHeap->getSize() ||
        (Requirements.MemoryTypeBits & (1u << VkHeap->getMemoryTypeIndex())) == 0)
        return {};

    getVkDevice().bindImageMemory(Image->getVkImage(), VkHeap->getVkDeviceMemory(), Offset);
    Image->bindPlacedMemory(
        std::make_shared<VulkanHeapSubAllocation>(VkHeap, Offset, Requirements.Size));
    return Image;
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
    auto RayTracingPipeline = std::dynamic_pointer_cast<VulkanPipeline>(Pipeline);
    if (!RayTracingPipeline || &RayTracingPipeline->getDevice() != this ||
        RayTracingPipeline->getType() != EPipelineType::RayTracing || !RayTracingPipeline->isValid() ||
        FirstGroup > RayTracingPipeline->getRayTracingGroupCount() ||
        GroupCount > RayTracingPipeline->getRayTracingGroupCount() - FirstGroup)
        return {};
    std::vector<std::byte> Result(static_cast<size_t>(GroupCount) * Limits.ShaderGroupHandleSize);
    auto Function = reinterpret_cast<PFN_vkGetRayTracingShaderGroupHandlesKHR>(
        getVkDevice().getProcAddr("vkGetRayTracingShaderGroupHandlesKHR"));
    if (!Function || Function(static_cast<VkDevice>(getVkDevice()),
        static_cast<VkPipeline>(RayTracingPipeline->getVkPipeline()), FirstGroup, GroupCount,
        Result.size(), Result.data()) != VK_SUCCESS)
        return {};
    return Result;
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
    constexpr uint32_t StatisticMask = (1u << 11) - 1;
    if (Desc.Count == 0 || (Desc.PipelineStatistics.Value & ~StatisticMask) != 0 ||
        (Desc.Type == EQueryType::PipelineStatistics) != static_cast<bool>(Desc.PipelineStatistics))
        throw std::invalid_argument("Query pool count or pipeline-statistics mask is invalid.");
    return std::make_shared<VulkanQueryPool>(*this, Context, Desc);
}

FormatCapabilities VulkanDevice::getFormatCapabilities(EFormat Format) const
{
    requireReady();
    const auto Properties = Context->PhysicalDevice.getFormatProperties(toVk(Format));
    auto Convert = [](vk::FormatFeatureFlags Flags)
    {
        EFormatFeatures Result;
        if (Flags & vk::FormatFeatureFlagBits::eSampledImage) Result.set(EFormatFeature_t::Sampled);
        if (Flags & vk::FormatFeatureFlagBits::eStorageImage) Result.set(EFormatFeature_t::Storage);
        if (Flags & vk::FormatFeatureFlagBits::eColorAttachment) Result.set(EFormatFeature_t::ColorAttachment);
        if (Flags & vk::FormatFeatureFlagBits::eDepthStencilAttachment) Result.set(EFormatFeature_t::DepthStencilAttachment);
        if (Flags & vk::FormatFeatureFlagBits::eSampledImageFilterLinear) Result.set(EFormatFeature_t::LinearFiltering);
        if (Flags & vk::FormatFeatureFlagBits::eBlitSrc) Result.set(EFormatFeature_t::BlitSource);
        if (Flags & vk::FormatFeatureFlagBits::eBlitDst) Result.set(EFormatFeature_t::BlitDestination);
        if (Flags & vk::FormatFeatureFlagBits::eVertexBuffer) Result.set(EFormatFeature_t::VertexBuffer);
        return Result;
    };
    return { Convert(Properties.optimalTilingFeatures),
        Convert(Properties.linearTilingFeatures), Convert(Properties.bufferFeatures) };
}

std::optional<SwapchainCapabilities> VulkanDevice::getSwapchainCapabilities() const
{
    if (Context->isDeviceLost() || !Context->Surface) return std::nullopt;
    try
    {
        const auto Capabilities = Context->PhysicalDevice.getSurfaceCapabilitiesKHR(Context->Surface->Handle);
        const auto Formats = Context->PhysicalDevice.getSurfaceFormatsKHR(Context->Surface->Handle);
        const auto Modes = Context->PhysicalDevice.getSurfacePresentModesKHR(Context->Surface->Handle);
        SwapchainCapabilities Result;
        Result.MinimumImageCount = Capabilities.minImageCount;
        Result.MaximumImageCount = Capabilities.maxImageCount;
        Result.MinimumWidth = Capabilities.minImageExtent.width;
        Result.MinimumHeight = Capabilities.minImageExtent.height;
        Result.MaximumWidth = Capabilities.maxImageExtent.width;
        Result.MaximumHeight = Capabilities.maxImageExtent.height;
        for (const auto& Format : Formats)
        {
            EFormat MappedFormat = EFormat::Undefined;
            switch (Format.format)
            {
            case vk::Format::eR8G8B8A8Unorm: MappedFormat = EFormat::RGBA8_UNorm; break;
            case vk::Format::eR8G8B8A8Srgb: MappedFormat = EFormat::RGBA8_sRGB; break;
            case vk::Format::eB8G8R8A8Unorm: MappedFormat = EFormat::BGRA8_UNorm; break;
            case vk::Format::eB8G8R8A8Srgb: MappedFormat = EFormat::BGRA8_sRGB; break;
            case vk::Format::eA2B10G10R10UnormPack32: MappedFormat = EFormat::RGB10A2_UNorm; break;
            case vk::Format::eR16G16B16A16Sfloat: MappedFormat = EFormat::RGBA16_Float; break;
            default: break;
            }
            std::optional<EColorSpace> MappedSpace;
            switch (Format.colorSpace)
            {
            case vk::ColorSpaceKHR::eSrgbNonlinear: MappedSpace = EColorSpace::SRGB_Nonlinear; break;
            case vk::ColorSpaceKHR::eAdobergbNonlinearEXT: MappedSpace = EColorSpace::AdobeRGB; break;
            case vk::ColorSpaceKHR::eDciP3NonlinearEXT: MappedSpace = EColorSpace::DCIP3; break;
            case vk::ColorSpaceKHR::eBt2020LinearEXT: MappedSpace = EColorSpace::Rec2020; break;
            case vk::ColorSpaceKHR::eHdr10St2084EXT: MappedSpace = EColorSpace::HDR10_ST2084; break;
            case vk::ColorSpaceKHR::eExtendedSrgbLinearEXT: MappedSpace = EColorSpace::ExtendedSRGBLinear; break;
            default: break;
            }
            if (MappedFormat != EFormat::Undefined && MappedSpace)
                Result.Formats.push_back({ MappedFormat, *MappedSpace });
        }
        for (const auto Mode : Modes)
        {
            switch (Mode)
            {
            case vk::PresentModeKHR::eImmediate: Result.PresentModes.push_back(EPresentMode::Immediate); break;
            case vk::PresentModeKHR::eFifo: Result.PresentModes.push_back(EPresentMode::Fifo); break;
            case vk::PresentModeKHR::eFifoRelaxed: Result.PresentModes.push_back(EPresentMode::FifoRelaxed); break;
            case vk::PresentModeKHR::eMailbox: Result.PresentModes.push_back(EPresentMode::Mailbox); break;
            default: break;
            }
        }
        const bool HdrFormat = std::ranges::any_of(Result.Formats, [](const SurfaceFormat& Format)
        {
            return Format.ColorSpace == EColorSpace::HDR10_ST2084 ||
                Format.ColorSpace == EColorSpace::ExtendedSRGBLinear;
        });
        Result.SupportsHDRMetadata = Context->EnabledFeatures.HDRMetadata && HdrFormat;
        return Result;
    }
    catch (const vk::SystemError& Error)
    {
        const auto Status = vulkan_wsi::mapStatus(vulkan_wsi::resultFromSystemError(Error));
        if (Status && *Status == ESwapchainStatus::DeviceLost) Context->markDeviceLost();
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
    EMemoryPropertyFlags Property)
{
    requireReady();
    if (Requirements.Size == 0 || Requirements.MemoryTypeBits == 0)
    {
        throw std::invalid_argument("Vulkan memory requirements are invalid.");
    }

    const vk::MemoryAllocateInfo AllocationInfo(
        Requirements.Size,
        findMemoryType(Requirements.MemoryTypeBits, toVk(Property)));
    vk::DeviceMemory VkDeviceMemory = Context->Device->allocateMemory(AllocationInfo);

    auto& Memory = VulkanDeviceMemoryPool::self().allocateMemory();
    Memory.OwnerDevice = Context->Device.get();
    Memory.VkDeviceMemory = VkDeviceMemory;
    Memory.Requirements = Requirements;
    Memory.Property = Property;
    Memory.OwnershipState = DeviceMemory::EState::OwnsMemory;

    return std::shared_ptr<DeviceMemory>(
        &Memory,
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

    std::scoped_lock Lock(DeferredReleaseMutex);
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
    std::vector<DeferredReleaseEntry> Ready;
    {
        std::scoped_lock Lock(DeferredReleaseMutex);
        for (auto Entry = DeferredReleases.begin(); Entry != DeferredReleases.end();)
        {
            if (Entry->CompletionSemaphore->getCompletedValue() >= Entry->CompletionValue)
            {
                Ready.push_back(std::move(*Entry));
                Entry = DeferredReleases.erase(Entry);
            }
            else
            {
                ++Entry;
            }
        }
    }
    std::ranges::sort(Ready, {}, &DeferredReleaseEntry::Sequence);
    // Reset explicitly outside the mutex: destructors may call arbitrary engine code or enqueue again.
    for (auto& Entry : Ready)
    {
        Entry.Resource.reset();
        Entry.CompletionSemaphore.reset();
    }
}

void VulkanDevice::drainDeferredReleases() noexcept
{
    std::vector<DeferredReleaseEntry> Releases;
    {
        std::scoped_lock Lock(DeferredReleaseMutex);
        Releases.swap(DeferredReleases);
    }
    std::ranges::sort(Releases, {}, &DeferredReleaseEntry::Sequence);
    for (auto& Entry : Releases)
    {
        Entry.Resource.reset();
        Entry.CompletionSemaphore.reset();
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
    vk::PhysicalDeviceMemoryProperties MemoryProperties = Context->PhysicalDevice.getMemoryProperties();

    for (uint32_t I = 0; I < MemoryProperties.memoryTypeCount; I++)
    {
        if ((TypeBits & (1u << I)) &&
            (MemoryProperties.memoryTypes[I].propertyFlags & Properties) == Properties)
        {
            return I;
        }
    }
    throw std::runtime_error("Failed to find a suitable Vulkan memory type.");
}

}


 