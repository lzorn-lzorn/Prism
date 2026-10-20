#include "VulkanCommandList.hpp"

#include "VulkanBindGroup.hpp"
#include "VulkanBuffer.hpp"
#include "VulkanDevice.hpp"
#include "VulkanImage.hpp"
#include "VulkanImageView.hpp"
#include "VulkanPipeline.hpp"
#include "VulkanRayTracing.hpp"
#include "VulkanRHI.hpp"
#include "VulkanSync.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <unordered_set>
#include <stdexcept>
#include <string>

namespace rhi
{
namespace
{
struct VulkanResourceState
{
    vk::PipelineStageFlags2 Stages;
    vk::AccessFlags2 Access;
    vk::ImageLayout Layout;
};

VulkanResourceState convertResourceState(EResourceState State)
{
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;

    switch (State)
    {
    case EResourceState::Undefined:
        return { Stage::eNone, Access::eNone, vk::ImageLayout::eUndefined };
    case EResourceState::Common:
        return {
            Stage::eAllCommands,
            Access::eMemoryRead | Access::eMemoryWrite,
            vk::ImageLayout::eGeneral };
    case EResourceState::UnorderedAccess:
    case EResourceState::StorageBuffer:
        return {
            Stage::eAllCommands,
            Access::eShaderRead | Access::eShaderWrite,
            vk::ImageLayout::eGeneral };
    case EResourceState::DepthWrite:
        return {
            Stage::eEarlyFragmentTests | Stage::eLateFragmentTests,
            Access::eDepthStencilAttachmentRead | Access::eDepthStencilAttachmentWrite,
            vk::ImageLayout::eDepthStencilAttachmentOptimal };
    case EResourceState::DepthRead:
        return {
            Stage::eEarlyFragmentTests | Stage::eLateFragmentTests | Stage::eFragmentShader,
            Access::eDepthStencilAttachmentRead | Access::eShaderRead,
            vk::ImageLayout::eDepthStencilReadOnlyOptimal };
    case EResourceState::CopySrc:
        return { Stage::eCopy, Access::eTransferRead, vk::ImageLayout::eTransferSrcOptimal };
    case EResourceState::CopyDst:
        return { Stage::eCopy, Access::eTransferWrite, vk::ImageLayout::eTransferDstOptimal };
    case EResourceState::Present:
        return { Stage::eAllCommands, Access::eMemoryRead, vk::ImageLayout::ePresentSrcKHR };
    case EResourceState::RenderTarget:
        return {
            Stage::eColorAttachmentOutput,
            Access::eColorAttachmentRead | Access::eColorAttachmentWrite,
            vk::ImageLayout::eColorAttachmentOptimal };
    case EResourceState::PixelShaderResource:
        return { Stage::eFragmentShader, Access::eShaderRead, vk::ImageLayout::eShaderReadOnlyOptimal };
    case EResourceState::NonPixelShaderResource:
        return {
            Stage::eVertexShader | Stage::eComputeShader,
            Access::eShaderRead,
            vk::ImageLayout::eShaderReadOnlyOptimal };
    case EResourceState::VertexBuffer:
    case EResourceState::IndexBuffer:
    case EResourceState::UniformBuffer:
    case EResourceState::ConstantBuffer:
        throw std::invalid_argument("A buffer-only resource state cannot be used for an image barrier.");
    }
    throw std::invalid_argument("Unsupported resource state.");
}

VulkanResourceState convertMemoryState(EResourceState State)
{
    if (State == EResourceState::VertexBuffer || State == EResourceState::IndexBuffer)
        return { vk::PipelineStageFlagBits2::eVertexInput,
            State == EResourceState::VertexBuffer
                ? vk::AccessFlagBits2::eVertexAttributeRead
                : vk::AccessFlagBits2::eIndexRead,
            vk::ImageLayout::eGeneral };
    if (State == EResourceState::UniformBuffer || State == EResourceState::ConstantBuffer)
        return { vk::PipelineStageFlagBits2::eAllGraphics | vk::PipelineStageFlagBits2::eComputeShader,
            vk::AccessFlagBits2::eUniformRead, vk::ImageLayout::eGeneral };
    return convertResourceState(State);
}

vk::AttachmentLoadOp convertLoadOp(ELoadOp Op)
{
    switch (Op)
    {
    case ELoadOp::Load: return vk::AttachmentLoadOp::eLoad;
    case ELoadOp::Clear: return vk::AttachmentLoadOp::eClear;
    case ELoadOp::DontCare: return vk::AttachmentLoadOp::eDontCare;
    }
    throw std::invalid_argument("Unsupported attachment load operation.");
}

vk::AttachmentStoreOp convertStoreOp(EStoreOp Op)
{
    switch (Op)
    {
    case EStoreOp::Store: return vk::AttachmentStoreOp::eStore;
    case EStoreOp::DontCare: return vk::AttachmentStoreOp::eDontCare;
    }
    throw std::invalid_argument("Unsupported attachment store operation.");
}

vk::ResolveModeFlagBits convertResolveMode(EResolveMode Mode)
{
    switch (Mode)
    {
    case EResolveMode::None: return vk::ResolveModeFlagBits::eNone;
    case EResolveMode::SampleZero: return vk::ResolveModeFlagBits::eSampleZero;
    case EResolveMode::Average: return vk::ResolveModeFlagBits::eAverage;
    case EResolveMode::Min: return vk::ResolveModeFlagBits::eMin;
    case EResolveMode::Max: return vk::ResolveModeFlagBits::eMax;
    }
    throw std::invalid_argument("Unsupported resolve mode.");
}

vk::ClearValue convertClearColor(const ClearColorValue& Value)
{
    switch (Value.Type)
    {
    case EClearColorType::Float:
        return vk::ClearValue(vk::ClearColorValue(Value.Float32));
    case EClearColorType::UInt:
        return vk::ClearValue(vk::ClearColorValue(Value.UInt32));
    case EClearColorType::SInt:
        return vk::ClearValue(vk::ClearColorValue(Value.SInt32));
    }
    throw std::invalid_argument("Unsupported clear color type.");
}

EImageAspect inferAspect(EFormat Format)
{
    if (hasDepthAspect(Format) && hasStencilAspect(Format))
        return EImageAspect::DepthStencil;
    if (hasDepthAspect(Format))
        return EImageAspect::Depth;
    if (hasStencilAspect(Format))
        return EImageAspect::Stencil;
    return EImageAspect::Color;
}

uint32_t mipExtent(uint32_t Extent, uint32_t MipLevel)
{
    return std::max(1u, Extent >> MipLevel);
}

const VulkanImageView& getVulkanView(const std::shared_ptr<RImageView>& View)
{
    if (!View || !View->isValid())
        throw std::invalid_argument("Rendering requires a valid image view.");
    const auto* result = dynamic_cast<const VulkanImageView*>(View.get());
    if (!result)
        throw std::invalid_argument("Rendering received an image view from another backend.");
    return *result;
}

const VulkanImage& getVulkanImage(const std::shared_ptr<RImage>& Image)
{
    if (!Image || !Image->isValid())
        throw std::invalid_argument("Image barrier requires a valid image.");
    const auto* result = dynamic_cast<const VulkanImage*>(Image.get());
    if (!result)
        throw std::invalid_argument("Image barrier received an image from another backend.");
    return *result;
}

const VulkanBuffer& getVulkanBuffer(const std::shared_ptr<RBuffer>& Buffer)
{
    if (!Buffer || !Buffer->isValid())
        throw std::invalid_argument("Command requires a valid buffer.");
    const auto* result = dynamic_cast<const VulkanBuffer*>(Buffer.get());
    if (!result)
        throw std::invalid_argument("Command received a buffer from another backend.");
    return *result;
}

const VulkanQueryPool& getVulkanQueryPool(const std::shared_ptr<RQueryPool>& Pool)
{
    const auto* result = dynamic_cast<const VulkanQueryPool*>(Pool.get());
    if (!result)
        throw std::invalid_argument("Command received a query pool from another backend.");
    return *result;
}

vk::ImageSubresourceLayers toLayers(const ImageSubresourceRange& Range, EFormat Format)
{
    if (Range.MipLevelCount != 1 || Range.ArrayLayerCount == 0)
        throw std::invalid_argument("Image copy regions require exactly one mip level and at least one layer.");
    const EImageAspect aspect = Range.Aspect == EImageAspect::Auto
        ? inferAspect(Format)
        : Range.Aspect;
    return vk::ImageSubresourceLayers(
        toVk(aspect), Range.BaseMipLevel, Range.BaseArrayLayer, Range.ArrayLayerCount);
}

void validateResolve(
    const std::shared_ptr<RImageView>& Source,
    const std::shared_ptr<RImageView>& Resolve,
    EResolveMode Mode,
    bool IsColor)
{
    if (!Resolve)
    {
        if (Mode != EResolveMode::None)
            throw std::invalid_argument("A resolve mode requires a resolve image view.");
        return;
    }
    if (Mode == EResolveMode::None)
        throw std::invalid_argument("A resolve image view requires a resolve mode.");
    if (IsColor && Mode != EResolveMode::Average)
        throw std::invalid_argument("Color attachment resolve currently requires Average mode.");

    const auto& source_image = Source->getImage()->getDescriptor();
    const auto& resolve_image = Resolve->getImage()->getDescriptor();
    if (source_image.SampleCount == ESampleCount::Count1)
        throw std::invalid_argument("A resolve source must be multisampled.");
    if (resolve_image.SampleCount != ESampleCount::Count1)
        throw std::invalid_argument("A resolve destination must have one sample.");
    if (Source->getDescriptor().Format != Resolve->getDescriptor().Format)
        throw std::invalid_argument("Resolve source and destination formats must match.");
    if (source_image.Width != resolve_image.Width || source_image.Height != resolve_image.Height)
        throw std::invalid_argument("Resolve source and destination extents must match.");
}

RenderingSignature inheritanceSignature(const SecondaryCommandListInheritance& Inheritance)
{
    RenderingSignature result {};
    result.ColorFormats = Inheritance.ColorFormats;
    result.ColorAttachmentCount = Inheritance.ColorAttachmentCount;
    result.DepthFormat = Inheritance.DepthFormat;
    result.StencilFormat = Inheritance.StencilFormat;
    result.SampleCount = Inheritance.SampleCount;
    result.ViewMask = Inheritance.ViewMask;
    return result;
}

bool sameTopologyClass(EPrimitiveTopology Left, EPrimitiveTopology Right)
{
    auto topology_class = [](EPrimitiveTopology topology)
    {
        switch (topology)
        {
        case EPrimitiveTopology::PointList: return 0;
        case EPrimitiveTopology::LineList:
        case EPrimitiveTopology::LineStrip: return 1;
        case EPrimitiveTopology::TriangleList:
        case EPrimitiveTopology::TriangleStrip: return 2;
        case EPrimitiveTopology::PatchList: return 3;
        }
        return -1;
    };
    return topology_class(Left) == topology_class(Right);
}

vk::Format vertexFormat(EVertexFormat Format)
{
    switch (Format)
    {
    case EVertexFormat::Float1: return vk::Format::eR32Sfloat;
    case EVertexFormat::Float2: return vk::Format::eR32G32Sfloat;
    case EVertexFormat::Float3: return vk::Format::eR32G32B32Sfloat;
    case EVertexFormat::Float4: return vk::Format::eR32G32B32A32Sfloat;
    case EVertexFormat::UInt1: return vk::Format::eR32Uint;
    case EVertexFormat::UInt2: return vk::Format::eR32G32Uint;
    case EVertexFormat::UInt3: return vk::Format::eR32G32B32Uint;
    case EVertexFormat::UInt4: return vk::Format::eR32G32B32A32Uint;
    case EVertexFormat::Short2: return vk::Format::eR16G16Sint;
    case EVertexFormat::Short4: return vk::Format::eR16G16B16A16Sint;
    }
    return vk::Format::eUndefined;
}
} // namespace

VulkanCommandList::VulkanCommandList(
    VulkanDevice& InDevice,
    uint32_t QueueFamilyIndex,
    const CommandListDescriptor& Desc)
    : Device(&InDevice), Descriptor(Desc)
{
    if (Desc.RenderingInheritance &&
        (Desc.Level != ECommandListLevel::Secondary || Desc.QueueType != ECommandQueueType::Graphics))
        throw std::invalid_argument("Rendering inheritance requires a secondary graphics command list.");
    if (Desc.RenderingInheritance)
    {
        const auto& inheritance = *Desc.RenderingInheritance;
        if (inheritance.ColorAttachmentCount > MaxColorAttachments ||
            inheritance.ColorAttachmentCount > Device->getLimits().MaxColorAttachments)
            throw std::invalid_argument("Secondary rendering inheritance has too many color attachments.");
        for (uint32_t index = 0; index < MaxColorAttachments; ++index)
        {
            const bool used = index < inheritance.ColorAttachmentCount;
            if (used == (inheritance.ColorFormats[index] == EFormat::Undefined) ||
                (used && hasDepthAspect(inheritance.ColorFormats[index])))
                throw std::invalid_argument("Secondary rendering inheritance has invalid color formats.");
        }
        if ((inheritance.DepthFormat != EFormat::Undefined && !hasDepthAspect(inheritance.DepthFormat)) ||
            (inheritance.StencilFormat != EFormat::Undefined && !hasStencilAspect(inheritance.StencilFormat)))
            throw std::invalid_argument("Secondary rendering inheritance has invalid depth/stencil formats.");
        if (inheritance.ViewMask != 0 && !Device->getFeatures().Multiview)
            throw std::invalid_argument("Secondary multiview inheritance is unsupported.");
    }
    CommandPool = Device->getVkDevice().createCommandPoolUnique(
        vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            QueueFamilyIndex));

    vk::CommandBufferAllocateInfo allocation_info(
        CommandPool.get(),
        Desc.Level == ECommandListLevel::Primary
            ? vk::CommandBufferLevel::ePrimary
            : vk::CommandBufferLevel::eSecondary,
        1);
    auto command_buffers = Device->getVkDevice().allocateCommandBuffersUnique(allocation_info);
    CommandBuffer = std::move(command_buffers.front());
}

void* VulkanCommandList::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkCommandBuffer>(CommandBuffer.get()));
}

RDevice& VulkanCommandList::getDevice() const noexcept
{
    return *Device;
}

void VulkanCommandList::begin()
{
    if (State != ECommandListState::Initial)
        throw std::logic_error("Command list begin requires the Initial state.");

    try
    {
        vk::CommandBufferUsageFlags usage;
        if (Descriptor.OneTimeSubmit)
            usage |= vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

        vk::CommandBufferInheritanceRenderingInfo rendering_inheritance {};
        vk::CommandBufferInheritanceInfo inheritance_info {};
        std::array<vk::Format, MaxColorAttachments> color_formats {};
        const vk::CommandBufferInheritanceInfo* inheritance_pointer = nullptr;
        if (Descriptor.Level == ECommandListLevel::Secondary)
        {
            inheritance_pointer = &inheritance_info;
            if (Descriptor.RenderingInheritance)
            {
                const auto& source = *Descriptor.RenderingInheritance;
                for (uint32_t index = 0; index < source.ColorAttachmentCount; ++index)
                    color_formats[index] = toVk(source.ColorFormats[index]);
                // VkCommandBufferInheritanceRenderingInfo is required by dynamic rendering secondaries.
                rendering_inheritance
                    .setViewMask(source.ViewMask)
                    .setColorAttachmentCount(source.ColorAttachmentCount)
                    .setPColorAttachmentFormats(color_formats.data())
                    .setDepthAttachmentFormat(toVk(source.DepthFormat))
                    .setStencilAttachmentFormat(toVk(source.StencilFormat))
                    .setRasterizationSamples(toVk(source.SampleCount));
                inheritance_info.pNext = &rendering_inheritance;
                usage |= vk::CommandBufferUsageFlagBits::eRenderPassContinue;
                InheritedRendering = true;
                InsideRendering = true;
                ActiveRenderingSignature = inheritanceSignature(source);
            }
        }
        CommandBuffer->begin(vk::CommandBufferBeginInfo(usage, inheritance_pointer));
        State = ECommandListState::Recording;
    }
    catch (...)
    {
        State = ECommandListState::Invalid;
        throw;
    }
}

void VulkanCommandList::end()
{
    requireRecording("end");
    if (InsideRendering && !InheritedRendering)
        requireOutsideRendering("end");
    if (ActiveQueryPool)
        throw std::logic_error("A command list cannot end with an active query.");
    try
    {
        CommandBuffer->end();
        State = ECommandListState::Executable;
    }
    catch (...)
    {
        State = ECommandListState::Invalid;
        throw;
    }
}

void VulkanCommandList::reset()
{
    if (State == ECommandListState::Recording)
        throw std::logic_error("A recording command list cannot be reset.");
    if (State == ECommandListState::Pending)
        throw std::logic_error("A pending command list cannot be reset.");
    CommandBuffer->reset();
    State = ECommandListState::Initial;
    InsideRendering = false;
    InheritedRendering = false;
    RenderingUsesSecondaryCommandBuffers = false;
    BoundGraphicsPipeline.reset();
    BoundComputePipeline.reset();
    BoundRayTracingPipeline.reset();
    ActiveRenderingSignature = {};
    InitializedDynamicStates = {};
    DynamicPrimitiveTopology.reset();
    ActiveQueryPool.reset();
    RetainedResources.clear();
    ExecutedSecondaries.clear();
}

void VulkanCommandList::markSubmitted()
{
    if (State != ECommandListState::Executable)
        throw std::logic_error("Only executable command lists may become pending.");
    State = ECommandListState::Pending;
    for (const auto& secondary : ExecutedSecondaries)
        secondary->markSubmitted();
}

void VulkanCommandList::markComplete()
{
    if (State == ECommandListState::Pending)
        State = Descriptor.OneTimeSubmit
            ? ECommandListState::Completed
            : ECommandListState::Executable;
    for (const auto& secondary : ExecutedSecondaries)
        secondary->markComplete();
}

void VulkanCommandList::barriers(
    std::span<const GlobalBarrier> GlobalBarriers,
    std::span<const BufferBarrier> BufferBarriers,
    std::span<const ImageBarrier> ImageBarriers)
{
    requireRecording("barriers");
    requireOutsideRendering("barriers");
    if (GlobalBarriers.empty() && BufferBarriers.empty() && ImageBarriers.empty())
        return;

    std::vector<vk::MemoryBarrier2> global_barriers;
    global_barriers.reserve(GlobalBarriers.size());
    for (const auto& barrier : GlobalBarriers)
    {
        const auto before = convertMemoryState(barrier.Before);
        const auto after = convertMemoryState(barrier.After);
        global_barriers.emplace_back(before.Stages, before.Access, after.Stages, after.Access);
    }

    std::vector<vk::BufferMemoryBarrier2> buffer_barriers;
    buffer_barriers.reserve(BufferBarriers.size());
    for (const auto& barrier : BufferBarriers)
    {
        const auto& buffer = getVulkanBuffer(barrier.Buffer);
        const auto& desc = barrier.Buffer->getDescriptor();
        const uint64_t size = clampCopySize(barrier.Offset, barrier.Size, desc.Size);
        const auto before = convertMemoryState(barrier.Before);
        const auto after = convertMemoryState(barrier.After);
        if (barrier.SourceQueue.has_value() != barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t source_family = barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t destination_family = barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        buffer_barriers.emplace_back(
            before.Stages, before.Access, after.Stages, after.Access,
            source_family, destination_family, buffer.getVkBuffer(), barrier.Offset, size);
        RetainedResources.emplace_back(barrier.Buffer);
    }

    std::vector<vk::ImageMemoryBarrier2> vk_barriers;
    vk_barriers.reserve(ImageBarriers.size());
    for (const auto& barrier : ImageBarriers)
    {
        const auto& image = getVulkanImage(barrier.Image);
        const auto& desc = barrier.Image->getDescriptor();
        if (barrier.Range.MipLevelCount == 0 || barrier.Range.ArrayLayerCount == 0 ||
            barrier.Range.BaseMipLevel >= desc.MipLevels ||
            barrier.Range.MipLevelCount > desc.MipLevels - barrier.Range.BaseMipLevel ||
            barrier.Range.BaseArrayLayer >= desc.ArrayLayers ||
            barrier.Range.ArrayLayerCount > desc.ArrayLayers - barrier.Range.BaseArrayLayer)
        {
            throw std::out_of_range("Image barrier subresource range exceeds the image.");
        }

        const EImageAspect aspect = barrier.Range.Aspect == EImageAspect::Auto
            ? inferAspect(desc.Format)
            : barrier.Range.Aspect;
        const auto before = convertResourceState(barrier.Before);
        const auto after = convertResourceState(barrier.After);
        if (barrier.SourceQueue.has_value() != barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t source_family = barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t destination_family = barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        vk_barriers.emplace_back(
            before.Stages,
            before.Access,
            after.Stages,
            after.Access,
            before.Layout,
            after.Layout,
            source_family,
            destination_family,
            image.getVkImage(),
            vk::ImageSubresourceRange(
                toVk(aspect),
                barrier.Range.BaseMipLevel,
                barrier.Range.MipLevelCount,
                barrier.Range.BaseArrayLayer,
                barrier.Range.ArrayLayerCount));
        RetainedResources.emplace_back(barrier.Image);
    }

    vk::DependencyInfo dependency;
    dependency.setMemoryBarriers(global_barriers)
        .setBufferMemoryBarriers(buffer_barriers)
        .setImageMemoryBarriers(vk_barriers);
    CommandBuffer->pipelineBarrier2(dependency);
}

void VulkanCommandList::beginRendering(const RenderingInfo& Info)
{
    requireRecording("beginRendering");
    requireOutsideRendering("beginRendering");
    if (Descriptor.QueueType != ECommandQueueType::Graphics)
        throw std::logic_error("Rendering requires a graphics command list.");
    if (Descriptor.Level != ECommandListLevel::Primary)
        throw std::logic_error("A secondary command list cannot begin a rendering scope.");
    validateRenderingInfo(Info);

    std::array<vk::RenderingAttachmentInfo, MaxColorAttachments> color_attachments;
    ActiveRenderingSignature = {};
    ActiveRenderingSignature.ColorAttachmentCount = static_cast<uint32_t>(Info.ColorAttachments.size());
    ActiveRenderingSignature.ViewMask = Info.ViewMask;
    bool sample_count_set = false;

    for (size_t index = 0; index < Info.ColorAttachments.size(); ++index)
    {
        const auto& source = Info.ColorAttachments[index];
        const auto& view = getVulkanView(source.View);
        const auto& view_desc = source.View->getDescriptor();
        const auto& image_desc = source.View->getImage()->getDescriptor();
        ActiveRenderingSignature.ColorFormats[index] = view_desc.Format;
        if (!sample_count_set)
        {
            ActiveRenderingSignature.SampleCount = image_desc.SampleCount;
            sample_count_set = true;
        }

        auto& target = color_attachments[index];
        target
            .setImageView(view.getVkImageView())
            .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setLoadOp(convertLoadOp(source.LoadOp))
            .setStoreOp(convertStoreOp(source.StoreOp))
            .setClearValue(convertClearColor(source.ClearValue));
        if (source.ResolveView)
        {
            const auto& resolve_view = getVulkanView(source.ResolveView);
            target
                .setResolveMode(convertResolveMode(source.ResolveMode))
                .setResolveImageView(resolve_view.getVkImageView())
                .setResolveImageLayout(vk::ImageLayout::eColorAttachmentOptimal);
        }
    }

    vk::RenderingAttachmentInfo depth_attachment;
    vk::RenderingAttachmentInfo stencil_attachment;
    const vk::RenderingAttachmentInfo* depth_pointer = nullptr;
    const vk::RenderingAttachmentInfo* stencil_pointer = nullptr;

    if (Info.DepthStencil)
    {
        const auto& attachment = *Info.DepthStencil;
        const auto& view = getVulkanView(attachment.View);
        const EFormat format = attachment.View->getDescriptor().Format;
        const auto sample_count = attachment.View->getImage()->getDescriptor().SampleCount;
        if (!sample_count_set)
            ActiveRenderingSignature.SampleCount = sample_count;
        if (hasDepthAspect(format))
        {
            ActiveRenderingSignature.DepthFormat = format;
            depth_attachment
                .setImageView(view.getVkImageView())
                .setImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                .setLoadOp(convertLoadOp(attachment.Depth.LoadOp))
                .setStoreOp(convertStoreOp(attachment.Depth.StoreOp))
                .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue(
                    attachment.ClearValue.Depth,
                    attachment.ClearValue.Stencil)));
            depth_pointer = &depth_attachment;
        }
        if (hasStencilAspect(format))
        {
            ActiveRenderingSignature.StencilFormat = format;
            stencil_attachment
                .setImageView(view.getVkImageView())
                .setImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                .setLoadOp(convertLoadOp(attachment.Stencil.LoadOp))
                .setStoreOp(convertStoreOp(attachment.Stencil.StoreOp))
                .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue(
                    attachment.ClearValue.Depth,
                    attachment.ClearValue.Stencil)));
            stencil_pointer = &stencil_attachment;
        }
        if (attachment.ResolveView)
        {
            const auto& resolve_view = getVulkanView(attachment.ResolveView);
            if (depth_pointer && attachment.Depth.ResolveMode != EResolveMode::None)
                depth_attachment
                    .setResolveMode(convertResolveMode(attachment.Depth.ResolveMode))
                    .setResolveImageView(resolve_view.getVkImageView())
                    .setResolveImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if (stencil_pointer && attachment.Stencil.ResolveMode != EResolveMode::None)
                stencil_attachment
                    .setResolveMode(convertResolveMode(attachment.Stencil.ResolveMode))
                    .setResolveImageView(resolve_view.getVkImageView())
                    .setResolveImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        }
    }

    vk::RenderingInfo rendering_info;
    rendering_info
        .setRenderArea(vk::Rect2D(
            vk::Offset2D(Info.Area.X, Info.Area.Y),
            vk::Extent2D(Info.Area.Width, Info.Area.Height)))
        .setLayerCount(Info.LayerCount)
        .setViewMask(Info.ViewMask)
        .setColorAttachmentCount(static_cast<uint32_t>(Info.ColorAttachments.size()))
        .setPColorAttachments(color_attachments.data())
        .setPDepthAttachment(depth_pointer)
        .setPStencilAttachment(stencil_pointer);
    if (Info.SecondaryCommandBuffers)
    {
        // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT makes this scope secondary-only.
        rendering_info.flags |= vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;
    }

    if (BoundGraphicsPipeline)
        validateRenderingCompatibility(*BoundGraphicsPipeline);
    CommandBuffer->beginRendering(rendering_info);
    InsideRendering = true;
    RenderingUsesSecondaryCommandBuffers = Info.SecondaryCommandBuffers;
    retainRenderingResources(Info);
}

void VulkanCommandList::endRendering()
{
    requireRecording("endRendering");
    requireInsideRendering("endRendering");
    if (Descriptor.Level != ECommandListLevel::Primary || InheritedRendering)
        throw std::logic_error("Only a primary command list can end a rendering scope.");
    CommandBuffer->endRendering();
    InsideRendering = false;
    RenderingUsesSecondaryCommandBuffers = false;
    ActiveRenderingSignature = {};
}

void VulkanCommandList::setViewports(std::span<const Viewport> Viewports)
{
    requireRecording("setViewports");
    if (Viewports.empty() || Viewports.size() > Device->getLimits().MaxViewports)
        throw std::invalid_argument("Viewport count is invalid for this device.");
    std::vector<vk::Viewport> vk_viewports;
    vk_viewports.reserve(Viewports.size());
    for (const auto& viewport : Viewports)
    {
        if (viewport.Width <= 0.0f || viewport.Height <= 0.0f ||
            viewport.MinDepth < 0.0f || viewport.MaxDepth > 1.0f ||
            viewport.MinDepth > viewport.MaxDepth)
        {
            throw std::invalid_argument("Viewport dimensions or depth range are invalid.");
        }
        vk_viewports.emplace_back(
            viewport.X,
            viewport.Y,
            viewport.Width,
            viewport.Height,
            viewport.MinDepth,
            viewport.MaxDepth);
    }
    CommandBuffer->setViewport(0, vk_viewports);
    InitializedDynamicStates.set(EDynamicState_t::Viewport);
}

void VulkanCommandList::setScissors(std::span<const RenderArea> Scissors)
{
    requireRecording("setScissors");
    if (Scissors.empty() || Scissors.size() > Device->getLimits().MaxViewports)
        throw std::invalid_argument("Scissor count is invalid for this device.");
    std::vector<vk::Rect2D> vk_scissors;
    vk_scissors.reserve(Scissors.size());
    for (const auto& scissor : Scissors)
    {
        if (scissor.X < 0 || scissor.Y < 0 || scissor.Width == 0 || scissor.Height == 0)
            throw std::invalid_argument("Scissor rectangle is invalid.");
        vk_scissors.emplace_back(
            vk::Offset2D(scissor.X, scissor.Y),
            vk::Extent2D(scissor.Width, scissor.Height));
    }
    CommandBuffer->setScissor(0, vk_scissors);
    InitializedDynamicStates.set(EDynamicState_t::Scissor);
}

void VulkanCommandList::setBlendConstants(const std::array<float, 4>& Constants)
{
    requireRecording("setBlendConstants");
    CommandBuffer->setBlendConstants(Constants.data());
    InitializedDynamicStates.set(EDynamicState_t::BlendConstants);
}

void VulkanCommandList::setStencilReference(
    uint32_t FrontReference,
    uint32_t BackReference)
{
    requireRecording("setStencilReference");
    CommandBuffer->setStencilReference(vk::StencilFaceFlagBits::eFront, FrontReference);
    CommandBuffer->setStencilReference(vk::StencilFaceFlagBits::eBack, BackReference);
    InitializedDynamicStates.set(EDynamicState_t::StencilReference);
}

void VulkanCommandList::setDepthBias(float ConstantFactor, float Clamp, float SlopeFactor)
{
    requireRecording("setDepthBias");
    CommandBuffer->setDepthBias(ConstantFactor, Clamp, SlopeFactor);
    InitializedDynamicStates.set(EDynamicState_t::DepthBias);
}

void VulkanCommandList::setLineWidth(float Width)
{
    requireRecording("setLineWidth");
    if (Width <= 0.0f)
        throw std::invalid_argument("Dynamic line width must be positive.");
    if (Width != 1.0f && !Device->getFeatures().WideLines)
        throw std::invalid_argument("Wide lines are unsupported by this device.");
    CommandBuffer->setLineWidth(Width);
    InitializedDynamicStates.set(EDynamicState_t::LineWidth);
}

bool VulkanCommandList::setCullMode(ECullMode Mode)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try { CommandBuffer->setCullMode(toVk(Mode)); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::CullMode);
    return true;
}

bool VulkanCommandList::setFrontFace(EFrontFace Face)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    const vk::FrontFace face = Face == EFrontFace::Clockwise
        ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise;
    try { CommandBuffer->setFrontFace(face); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::FrontFace);
    return true;
}

bool VulkanCommandList::setPrimitiveTopology(EPrimitiveTopology Topology)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState ||
        (BoundGraphicsPipeline && !sameTopologyClass(
            Topology, static_cast<const VulkanPipeline&>(*BoundGraphicsPipeline).getPrimitiveTopology())))
        return false;
    try { CommandBuffer->setPrimitiveTopology(toVk(Topology)); }
    catch (...) { return false; }
    DynamicPrimitiveTopology = Topology;
    InitializedDynamicStates.set(EDynamicState_t::PrimitiveTopology);
    return true;
}

bool VulkanCommandList::setDepthTestEnable(bool Enable)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try { CommandBuffer->setDepthTestEnable(Enable); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::DepthTestEnable);
    return true;
}

bool VulkanCommandList::setDepthWriteEnable(bool Enable)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try { CommandBuffer->setDepthWriteEnable(Enable); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::DepthWriteEnable);
    return true;
}

bool VulkanCommandList::setDepthCompareOp(ECompareOp Operation)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try { CommandBuffer->setDepthCompareOp(toVk(Operation)); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::DepthCompareOp);
    return true;
}

bool VulkanCommandList::setStencilTestEnable(bool Enable)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try { CommandBuffer->setStencilTestEnable(Enable); }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::StencilTestEnable);
    return true;
}

bool VulkanCommandList::setStencilOperations(
    const StencilFaceState& Front,
    const StencilFaceState& Back)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().ExtendedDynamicState)
        return false;
    try
    {
        // vkCmdSetStencilOp updates operations and compare op; masks/reference use their own commands.
        CommandBuffer->setStencilOp(vk::StencilFaceFlagBits::eFront,
            toVk(Front.FailOp), toVk(Front.PassOp), toVk(Front.DepthFailOp), toVk(Front.CompareOp));
        CommandBuffer->setStencilOp(vk::StencilFaceFlagBits::eBack,
            toVk(Back.FailOp), toVk(Back.PassOp), toVk(Back.DepthFailOp), toVk(Back.CompareOp));
    }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::StencilOperations);
    return true;
}

bool VulkanCommandList::setStencilCompareMask(uint32_t FrontMask, uint32_t BackMask)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics)
        return false;
    try
    {
        CommandBuffer->setStencilCompareMask(vk::StencilFaceFlagBits::eFront, FrontMask);
        CommandBuffer->setStencilCompareMask(vk::StencilFaceFlagBits::eBack, BackMask);
    }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::StencilCompareMask);
    return true;
}

bool VulkanCommandList::setStencilWriteMask(uint32_t FrontMask, uint32_t BackMask)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics)
        return false;
    try
    {
        CommandBuffer->setStencilWriteMask(vk::StencilFaceFlagBits::eFront, FrontMask);
        CommandBuffer->setStencilWriteMask(vk::StencilFaceFlagBits::eBack, BackMask);
    }
    catch (...) { return false; }
    InitializedDynamicStates.set(EDynamicState_t::StencilWriteMask);
    return true;
}

bool VulkanCommandList::setVertexInput(const VertexInputState& Input)
{
    if (State != ECommandListState::Recording || Descriptor.QueueType != ECommandQueueType::Graphics ||
        !Device->getFeatures().DynamicVertexInput ||
        Input.Buffers.size() > Device->getLimits().MaxVertexInputBindings ||
        Input.Attributes.size() > Device->getLimits().MaxVertexInputAttributes)
        return false;

    const auto properties = Device->getVkPhysicalDevice().getProperties();
    std::unordered_set<uint32_t> bindings;
    std::vector<VkVertexInputBindingDescription2EXT> vk_bindings;
    vk_bindings.reserve(Input.Buffers.size());
    for (const auto& binding : Input.Buffers)
    {
        if (binding.Binding >= Device->getLimits().MaxVertexInputBindings || binding.Stride == 0 ||
            binding.Stride > properties.limits.maxVertexInputBindingStride ||
            !bindings.emplace(binding.Binding).second)
            return false;
        vk_bindings.push_back({ VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr,
            binding.Binding, binding.Stride,
            binding.InputRate == EVertexInputRate::PerInstance
                ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1 });
    }

    std::unordered_set<uint32_t> locations;
    std::vector<VkVertexInputAttributeDescription2EXT> vk_attributes;
    vk_attributes.reserve(Input.Attributes.size());
    for (const auto& attribute : Input.Attributes)
    {
        const vk::Format format = vertexFormat(attribute.Format);
        if (!bindings.contains(attribute.Binding) ||
            attribute.Location >= Device->getLimits().MaxVertexInputAttributes ||
            attribute.Offset > properties.limits.maxVertexInputAttributeOffset ||
            format == vk::Format::eUndefined || !locations.emplace(attribute.Location).second)
            return false;
        vk_attributes.push_back({ VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr,
            attribute.Location, attribute.Binding, static_cast<VkFormat>(format), attribute.Offset });
    }

    const auto function = reinterpret_cast<PFN_vkCmdSetVertexInputEXT>(
        Device->getVkDevice().getProcAddr("vkCmdSetVertexInputEXT"));
    if (!function)
        return false;
    function(static_cast<VkCommandBuffer>(CommandBuffer.get()),
        static_cast<uint32_t>(vk_bindings.size()), vk_bindings.data(),
        static_cast<uint32_t>(vk_attributes.size()), vk_attributes.data());
    InitializedDynamicStates.set(EDynamicState_t::VertexInput);
    return true;
}

void VulkanCommandList::pushConstants(
    const std::shared_ptr<RPipelineLayout>& Layout,
    EShaderStage Stages,
    uint32_t Offset,
    std::span<const std::byte> Data)
{
    requireRecording("pushConstants");
    auto vk_layout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Layout);
    if (!vk_layout || &vk_layout->getDevice() != Device || !vk_layout->isValid())
        throw std::invalid_argument("Push constant layout belongs to another backend or device.");
    if (!Stages || Data.empty() || Offset % 4 != 0 || Data.size() % 4 != 0 ||
        Data.size() > Device->getLimits().MaxPushConstantSize ||
        Offset > Device->getLimits().MaxPushConstantSize - Data.size())
    {
        throw std::invalid_argument("Push constant update is empty, unaligned or exceeds device limits.");
    }
    if (!vk_layout->supportsPushConstants(Stages, Offset, static_cast<uint32_t>(Data.size())))
        throw std::invalid_argument("Push constant update is not covered by the pipeline layout.");
    CommandBuffer->pushConstants(
        vk_layout->getVkPipelineLayout(),
        toVk(Stages),
        Offset,
        static_cast<uint32_t>(Data.size()),
        Data.data());
    RetainedResources.emplace_back(Layout);
}

void VulkanCommandList::bindPipeline(const std::shared_ptr<RPipeline>& Pipeline)
{
    requireRecording("bindPipeline");
    auto vk_pipeline = std::dynamic_pointer_cast<VulkanPipeline>(Pipeline);
    if (!vk_pipeline || &vk_pipeline->getDevice() != Device || !vk_pipeline->isValid())
        throw std::invalid_argument("Cannot bind an invalid pipeline.");
    const vk::Pipeline native_pipeline = vk_pipeline->getVkPipeline();
    switch (Pipeline->getType())
    {
    case EPipelineType::Graphics:
        if (InsideRendering)
            validateRenderingCompatibility(*Pipeline);
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eGraphics, native_pipeline);
        BoundGraphicsPipeline = Pipeline;
        break;
    case EPipelineType::Compute:
        requireOutsideRendering("bind compute pipeline");
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eCompute, native_pipeline);
        BoundComputePipeline = Pipeline;
        break;
    case EPipelineType::RayTracing:
        requireOutsideRendering("bind ray-tracing pipeline");
        if (!Device->getFeatures().RayTracingPipeline)
            throw std::invalid_argument("Ray-tracing pipelines are unsupported by this device.");
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, native_pipeline);
        BoundRayTracingPipeline = Pipeline;
        break;
    case EPipelineType::None:
    default:
        throw std::invalid_argument("This command list cannot bind the requested pipeline type.");
    }
    RetainedResources.emplace_back(Pipeline);
}

void VulkanCommandList::bindBindGroups(
    EPipelineType PipelineType,
    const std::shared_ptr<RPipelineLayout>& Layout,
    uint32_t FirstGroup,
    std::span<const std::shared_ptr<RBindGroup>> Groups,
    std::span<const uint32_t> DynamicOffsets)
{
    requireRecording("bindBindGroups");
    auto vk_layout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Layout);
    if (!vk_layout || &vk_layout->getDevice() != Device || !vk_layout->isValid())
        throw std::invalid_argument("BindGroup pipeline layout belongs to another backend or device.");
    if (PipelineType != EPipelineType::Graphics && PipelineType != EPipelineType::Compute &&
        PipelineType != EPipelineType::RayTracing)
        throw std::invalid_argument("BindGroups require a graphics, compute, or ray-tracing bind point.");
    if (PipelineType != EPipelineType::Graphics)
        requireOutsideRendering("bind non-graphics BindGroups");
    if (FirstGroup > vk_layout->getBindGroupLayoutCount() ||
        Groups.size() > vk_layout->getBindGroupLayoutCount() - FirstGroup)
        throw std::out_of_range("BindGroup range exceeds the pipeline layout.");

    std::vector<vk::DescriptorSet> descriptor_sets;
    descriptor_sets.reserve(Groups.size());
    size_t dynamic_offset_index = 0;
    for (size_t group_index = 0; group_index < Groups.size(); ++group_index)
    {
        auto group = std::dynamic_pointer_cast<VulkanBindGroup>(Groups[group_index]);
        if (!group || &group->getDevice() != Device || !group->isValid())
            throw std::invalid_argument("Cannot bind an invalid or foreign BindGroup.");
        const auto& expected_layout = vk_layout->getBindGroupLayout(
            FirstGroup + static_cast<uint32_t>(group_index));
        const auto expected_key = expected_layout->getCompatibilityKey();
        const auto actual_key = group->getLayout()->getCompatibilityKey();
        if (!std::ranges::equal(expected_key, actual_key))
            throw std::invalid_argument("BindGroup layout is incompatible with the pipeline layout slot.");

        for (const auto& layout_entry : group->getLayout()->getEntries())
        {
            if (!layout_entry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset))
                continue;
            for (uint32_t element = 0; element < group->getDescriptorCount(layout_entry); ++element)
            {
                if (dynamic_offset_index >= DynamicOffsets.size())
                    throw std::invalid_argument("Too few dynamic offsets for the bound BindGroups.");
                const uint32_t dynamic_offset = DynamicOffsets[dynamic_offset_index++];
                const uint64_t alignment = layout_entry.Type == EDescriptorType::UniformBuffer
                    ? Device->getLimits().MinUniformBufferOffsetAlignment
                    : Device->getLimits().MinStorageBufferOffsetAlignment;
                if (dynamic_offset % alignment != 0)
                    throw std::invalid_argument("A dynamic buffer offset violates device alignment.");

                const auto resource = std::ranges::find_if(group->getEntries(), [&](const BindGroupEntry& entry)
                {
                    return entry.Binding == layout_entry.Binding && entry.ArrayElement == element;
                });
                if (resource != group->getEntries().end())
                {
                    const auto* buffer_binding = std::get_if<BufferBinding>(&resource->Resource);
                    if (!buffer_binding || !buffer_binding->Buffer)
                        throw std::logic_error("Dynamic descriptor does not reference a buffer.");
                    const auto& desc = buffer_binding->Buffer->getDescriptor();
                    const uint64_t range = buffer_binding->Size == 0
                        ? desc.Size - buffer_binding->Offset
                        : buffer_binding->Size;
                    if (dynamic_offset > desc.Size - buffer_binding->Offset ||
                        range > desc.Size - buffer_binding->Offset - dynamic_offset)
                        throw std::out_of_range("Dynamic buffer offset moves the descriptor range out of bounds.");
                }
            }
        }
        descriptor_sets.emplace_back(group->getVkDescriptorSet());
        RetainedResources.emplace_back(std::move(group));
    }
    if (dynamic_offset_index != DynamicOffsets.size())
        throw std::invalid_argument("Too many dynamic offsets for the bound BindGroups.");
    if (descriptor_sets.empty())
        return;

    vk::PipelineBindPoint bind_point = vk::PipelineBindPoint::eGraphics;
    if (PipelineType == EPipelineType::Compute) bind_point = vk::PipelineBindPoint::eCompute;
    if (PipelineType == EPipelineType::RayTracing) bind_point = vk::PipelineBindPoint::eRayTracingKHR;
    CommandBuffer->bindDescriptorSets(
        bind_point,
        vk_layout->getVkPipelineLayout(),
        FirstGroup,
        descriptor_sets,
        DynamicOffsets);
    RetainedResources.emplace_back(Layout);
}

void VulkanCommandList::bindVertexBuffers(
    uint32_t FirstBinding,
    std::span<const VertexBufferBinding> Bindings)
{
    requireRecording("bindVertexBuffers");
    if (Bindings.empty()) return;
    if (FirstBinding > Device->getLimits().MaxVertexInputBindings ||
        Bindings.size() > Device->getLimits().MaxVertexInputBindings - FirstBinding)
        throw std::out_of_range("Vertex buffer bindings exceed the device limit.");
    std::vector<vk::Buffer> buffers;
    std::vector<vk::DeviceSize> offsets;
    buffers.reserve(Bindings.size());
    offsets.reserve(Bindings.size());
    for (const auto& binding : Bindings)
    {
        const auto& buffer = getVulkanBuffer(binding.Buffer);
        const auto& desc = binding.Buffer->getDescriptor();
        if (!desc.Usage.has(EBufferUsage::Vertex) || binding.Offset >= desc.Size)
            throw std::invalid_argument("Vertex binding requires Vertex usage and a valid offset.");
        buffers.emplace_back(buffer.getVkBuffer());
        offsets.emplace_back(binding.Offset);
        RetainedResources.emplace_back(binding.Buffer);
    }
    CommandBuffer->bindVertexBuffers(FirstBinding, buffers, offsets);
}

void VulkanCommandList::bindIndexBuffer(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset,
    EIndexFormat Format)
{
    requireRecording("bindIndexBuffer");
    const auto& buffer = getVulkanBuffer(Buffer);
    const auto& desc = Buffer->getDescriptor();
    if (!desc.Usage.has(EBufferUsage::Index) || Format == EIndexFormat::None || Offset >= desc.Size)
        throw std::invalid_argument("Index binding requires Index usage, a format, and a valid offset.");
    CommandBuffer->bindIndexBuffer(buffer.getVkBuffer(), Offset, toVk(Format));
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::copyBuffer(
    const std::shared_ptr<RBuffer>& Source,
    const std::shared_ptr<RBuffer>& Destination,
    std::span<const BufferCopyRegion> Regions)
{
    requireRecording("copyBuffer");
    requireOutsideRendering("copyBuffer");
    const auto& source = getVulkanBuffer(Source);
    const auto& destination = getVulkanBuffer(Destination);
    if (!Source->getDescriptor().Usage.has(EBufferUsage::TransferSrc) ||
        !Destination->getDescriptor().Usage.has(EBufferUsage::TransferDst))
        throw std::invalid_argument("Buffer copy resources require TransferSrc/TransferDst usage.");
    std::vector<vk::BufferCopy> regions;
    regions.reserve(Regions.size());
    for (const auto& region : Regions)
    {
        if (region.Size == 0 || region.Size > Source->getDescriptor().Size ||
            region.Size > Destination->getDescriptor().Size ||
            region.SourceOffset > Source->getDescriptor().Size - region.Size ||
            region.DestinationOffset > Destination->getDescriptor().Size - region.Size)
            throw std::out_of_range("Buffer copy region exceeds a resource.");
        regions.emplace_back(region.SourceOffset, region.DestinationOffset, region.Size);
    }
    if (!regions.empty()) CommandBuffer->copyBuffer(source.getVkBuffer(), destination.getVkBuffer(), regions);
    RetainedResources.emplace_back(Source);
    RetainedResources.emplace_back(Destination);
}

void VulkanCommandList::copyBufferToImage(
    const std::shared_ptr<RBuffer>& Source,
    const std::shared_ptr<RImage>& Destination,
    std::span<const BufferImageCopyRegion> Regions)
{
    requireRecording("copyBufferToImage");
    requireOutsideRendering("copyBufferToImage");
    const auto& source = getVulkanBuffer(Source);
    const auto& destination = getVulkanImage(Destination);
    if (!Source->getDescriptor().Usage.has(EBufferUsage::TransferSrc) ||
        !Destination->getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Buffer-to-image copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(Regions.size());
    for (const auto& region : Regions)
        regions.emplace_back(
            region.BufferOffset, region.BufferRowLength, region.BufferImageHeight,
            toLayers(region.Image, Destination->getDescriptor().Format),
            vk::Offset3D(region.ImageOffset.X, region.ImageOffset.Y, region.ImageOffset.Z),
            vk::Extent3D(region.ImageExtent.Width, region.ImageExtent.Height, region.ImageExtent.Depth));
    if (!regions.empty()) CommandBuffer->copyBufferToImage(
        source.getVkBuffer(), destination.getVkImage(), vk::ImageLayout::eTransferDstOptimal, regions);
    RetainedResources.emplace_back(Source);
    RetainedResources.emplace_back(Destination);
}

void VulkanCommandList::copyImageToBuffer(
    const std::shared_ptr<RImage>& Source,
    const std::shared_ptr<RBuffer>& Destination,
    std::span<const BufferImageCopyRegion> Regions)
{
    requireRecording("copyImageToBuffer");
    requireOutsideRendering("copyImageToBuffer");
    const auto& source = getVulkanImage(Source);
    const auto& destination = getVulkanBuffer(Destination);
    if (!Source->getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !Destination->getDescriptor().Usage.has(EBufferUsage::TransferDst))
        throw std::invalid_argument("Image-to-buffer copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::BufferImageCopy> regions;
    regions.reserve(Regions.size());
    for (const auto& region : Regions)
        regions.emplace_back(
            region.BufferOffset, region.BufferRowLength, region.BufferImageHeight,
            toLayers(region.Image, Source->getDescriptor().Format),
            vk::Offset3D(region.ImageOffset.X, region.ImageOffset.Y, region.ImageOffset.Z),
            vk::Extent3D(region.ImageExtent.Width, region.ImageExtent.Height, region.ImageExtent.Depth));
    if (!regions.empty()) CommandBuffer->copyImageToBuffer(
        source.getVkImage(), vk::ImageLayout::eTransferSrcOptimal, destination.getVkBuffer(), regions);
    RetainedResources.emplace_back(Source);
    RetainedResources.emplace_back(Destination);
}

void VulkanCommandList::copyImage(
    const std::shared_ptr<RImage>& Source,
    const std::shared_ptr<RImage>& Destination,
    std::span<const ImageCopyRegion> Regions)
{
    requireRecording("copyImage");
    requireOutsideRendering("copyImage");
    const auto& source = getVulkanImage(Source);
    const auto& destination = getVulkanImage(Destination);
    if (!Source->getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !Destination->getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Image copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::ImageCopy> regions;
    regions.reserve(Regions.size());
    for (const auto& region : Regions)
        regions.emplace_back(
            toLayers(region.Source, Source->getDescriptor().Format),
            vk::Offset3D(region.SourceOffset.X, region.SourceOffset.Y, region.SourceOffset.Z),
            toLayers(region.Destination, Destination->getDescriptor().Format),
            vk::Offset3D(region.DestinationOffset.X, region.DestinationOffset.Y, region.DestinationOffset.Z),
            vk::Extent3D(region.Extent.Width, region.Extent.Height, region.Extent.Depth));
    if (!regions.empty()) CommandBuffer->copyImage(
        source.getVkImage(), vk::ImageLayout::eTransferSrcOptimal,
        destination.getVkImage(), vk::ImageLayout::eTransferDstOptimal, regions);
    RetainedResources.emplace_back(Source);
    RetainedResources.emplace_back(Destination);
}

void VulkanCommandList::blitImage(
    const std::shared_ptr<RImage>& Source,
    const std::shared_ptr<RImage>& Destination,
    std::span<const ImageBlitRegion> Regions,
    EFilterMode Filter)
{
    requireRecording("blitImage");
    requireOutsideRendering("blitImage");
    const auto& source = getVulkanImage(Source);
    const auto& destination = getVulkanImage(Destination);
    if (!Source->getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !Destination->getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Image blit requires TransferSrc/TransferDst usage.");
    std::vector<vk::ImageBlit> regions;
    regions.reserve(Regions.size());
    for (const auto& region : Regions)
    {
        std::array<vk::Offset3D, 2> source_offsets;
        std::array<vk::Offset3D, 2> destination_offsets;
        for (size_t index = 0; index < 2; ++index)
        {
            source_offsets[index] = vk::Offset3D(
                region.SourceOffsets[index].X,
                region.SourceOffsets[index].Y,
                region.SourceOffsets[index].Z);
            destination_offsets[index] = vk::Offset3D(
                region.DestinationOffsets[index].X,
                region.DestinationOffsets[index].Y,
                region.DestinationOffsets[index].Z);
        }
        regions.emplace_back(
            toLayers(region.Source, Source->getDescriptor().Format), source_offsets,
            toLayers(region.Destination, Destination->getDescriptor().Format), destination_offsets);
    }
    if (!regions.empty()) CommandBuffer->blitImage(
        source.getVkImage(), vk::ImageLayout::eTransferSrcOptimal,
        destination.getVkImage(), vk::ImageLayout::eTransferDstOptimal,
        regions, toVk(Filter));
    RetainedResources.emplace_back(Source);
    RetainedResources.emplace_back(Destination);
}

void VulkanCommandList::fillBuffer(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset,
    DeviceSizeType Size,
    uint32_t Value)
{
    requireRecording("fillBuffer");
    requireOutsideRendering("fillBuffer");
    const auto& buffer = getVulkanBuffer(Buffer);
    if (!Buffer->getDescriptor().Usage.has(EBufferUsage::TransferDst) || (Offset & 3u) != 0)
        throw std::invalid_argument("Buffer fill requires TransferDst usage and a 4-byte aligned offset.");
    const uint64_t size = clampCopySize(Offset, Size, Buffer->getDescriptor().Size);
    if ((size & 3u) != 0)
        throw std::invalid_argument("Buffer fill size must be a multiple of four.");
    CommandBuffer->fillBuffer(buffer.getVkBuffer(), Offset, size, Value);
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::draw(
    uint32_t VertexCount,
    uint32_t InstanceCount,
    uint32_t FirstVertex,
    uint32_t FirstInstance)
{
    requireRecording("draw");
    requireInsideRendering("draw");
    validateGraphicsPipeline();
    if (BoundGraphicsPipeline->usesMeshShaders())
        throw std::logic_error("A mesh graphics pipeline requires drawMeshTasks.");
    if (VertexCount == 0 || InstanceCount == 0)
        return;
    CommandBuffer->draw(VertexCount, InstanceCount, FirstVertex, FirstInstance);
}

void VulkanCommandList::drawIndexed(
    uint32_t IndexCount,
    uint32_t InstanceCount,
    uint32_t FirstIndex,
    int32_t VertexOffset,
    uint32_t FirstInstance)
{
    requireRecording("drawIndexed");
    requireInsideRendering("drawIndexed");
    validateGraphicsPipeline();
    if (BoundGraphicsPipeline->usesMeshShaders())
        throw std::logic_error("A mesh graphics pipeline requires drawMeshTasks.");
    if (IndexCount == 0 || InstanceCount == 0)
        return;
    CommandBuffer->drawIndexed(IndexCount, InstanceCount, FirstIndex, VertexOffset, FirstInstance);
}

void VulkanCommandList::drawMeshTasks(
    uint32_t GroupCountX,
    uint32_t GroupCountY,
    uint32_t GroupCountZ)
{
    requireRecording("drawMeshTasks");
    requireInsideRendering("drawMeshTasks");
    validateGraphicsPipeline();
    if (!BoundGraphicsPipeline->usesMeshShaders())
        throw std::logic_error("drawMeshTasks requires a mesh graphics pipeline.");
    if (!Device->getFeatures().MeshShader)
        throw std::logic_error("Mesh shaders are not enabled on this device.");
    if (GroupCountX == 0 || GroupCountY == 0 || GroupCountZ == 0)
        return;
    const auto draw_mesh_tasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        Device->getVkDevice().getProcAddr("vkCmdDrawMeshTasksEXT"));
    if (!draw_mesh_tasks)
        throw std::runtime_error("vkCmdDrawMeshTasksEXT is unavailable on the logical device.");
    draw_mesh_tasks(
        static_cast<VkCommandBuffer>(CommandBuffer.get()),
        GroupCountX,
        GroupCountY,
        GroupCountZ);
}

void VulkanCommandList::dispatch(
    uint32_t GroupCountX,
    uint32_t GroupCountY,
    uint32_t GroupCountZ)
{
    requireRecording("dispatch");
    requireOutsideRendering("dispatch");
    if (!BoundComputePipeline)
        throw std::logic_error("Dispatch requires a bound compute pipeline.");
    if (GroupCountX == 0 || GroupCountY == 0 || GroupCountZ == 0)
        return;
    CommandBuffer->dispatch(GroupCountX, GroupCountY, GroupCountZ);
}

void VulkanCommandList::drawIndirect(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset,
    uint32_t DrawCount,
    uint32_t Stride)
{
    requireRecording("drawIndirect");
    requireInsideRendering("drawIndirect");
    validateGraphicsPipeline();
    const auto& buffer = getVulkanBuffer(Buffer);
    if (!Buffer->getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indirect draw requires Indirect buffer usage.");
    CommandBuffer->drawIndirect(buffer.getVkBuffer(), Offset, DrawCount, Stride);
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::drawIndexedIndirect(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset,
    uint32_t DrawCount,
    uint32_t Stride)
{
    requireRecording("drawIndexedIndirect");
    requireInsideRendering("drawIndexedIndirect");
    validateGraphicsPipeline();
    const auto& buffer = getVulkanBuffer(Buffer);
    if (!Buffer->getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indexed indirect draw requires Indirect buffer usage.");
    CommandBuffer->drawIndexedIndirect(buffer.getVkBuffer(), Offset, DrawCount, Stride);
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::dispatchIndirect(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset)
{
    requireRecording("dispatchIndirect");
    requireOutsideRendering("dispatchIndirect");
    if (!BoundComputePipeline)
        throw std::logic_error("Indirect dispatch requires a bound compute pipeline.");
    const auto& buffer = getVulkanBuffer(Buffer);
    if (!Buffer->getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indirect dispatch requires Indirect buffer usage.");
    CommandBuffer->dispatchIndirect(buffer.getVkBuffer(), Offset);
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::resetQueries(
    const std::shared_ptr<RQueryPool>& Pool,
    uint32_t First,
    uint32_t Count)
{
    requireRecording("resetQueries");
    requireOutsideRendering("resetQueries");
    const auto& pool = getVulkanQueryPool(Pool);
    if (&pool.getDevice() != Device || Count == 0 || Count > Pool->getDescriptor().Count ||
        First > Pool->getDescriptor().Count - Count)
        throw std::out_of_range("Query reset range exceeds the query pool.");
    CommandBuffer->resetQueryPool(pool.getVkQueryPool(), First, Count);
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::beginQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("beginQuery");
    const auto& pool = getVulkanQueryPool(Pool);
    if (&pool.getDevice() != Device || ActiveQueryPool ||
        Pool->getDescriptor().Type == EQueryType::Timestamp || Query >= Pool->getDescriptor().Count)
        throw std::invalid_argument("beginQuery requires a valid non-timestamp query.");
    CommandBuffer->beginQuery(pool.getVkQueryPool(), Query, {});
    ActiveQueryPool = Pool;
    ActiveQuery = Query;
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::endQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("endQuery");
    const auto& pool = getVulkanQueryPool(Pool);
    if (&pool.getDevice() != Device || Pool->getDescriptor().Type == EQueryType::Timestamp ||
        Query >= Pool->getDescriptor().Count || ActiveQueryPool.get() != Pool.get() || ActiveQuery != Query)
        throw std::invalid_argument("endQuery must match the active query pool and slot.");
    CommandBuffer->endQuery(pool.getVkQueryPool(), Query);
    ActiveQueryPool.reset();
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::writeTimestamp(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("writeTimestamp");
    const auto& pool = getVulkanQueryPool(Pool);
    const auto queue_families = Device->getVkPhysicalDevice().getQueueFamilyProperties();
    if (&pool.getDevice() != Device || Pool->getDescriptor().Type != EQueryType::Timestamp ||
        Query >= Pool->getDescriptor().Count ||
        queue_families[Device->getQueueFamilyIndex(Descriptor.QueueType)].timestampValidBits == 0)
        throw std::invalid_argument("writeTimestamp requires a valid timestamp query.");
    CommandBuffer->writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool.getVkQueryPool(), Query);
    RetainedResources.emplace_back(Pool);
}

bool VulkanCommandList::copyQueryResults(
    const std::shared_ptr<RQueryPool>& Pool,
    uint32_t First,
    uint32_t Count,
    const std::shared_ptr<RBuffer>& Destination,
    DeviceSizeType Offset,
    DeviceSizeType Stride,
    EQueryResultFlags Flags)
{
    if (State != ECommandListState::Recording || InsideRendering ||
        !Device->getFeatures().QueryResultCopy || Count == 0)
        return false;
    try
    {
        const auto& pool = getVulkanQueryPool(Pool);
        const auto& buffer = getVulkanBuffer(Destination);
        if (&pool.getDevice() != Device || &buffer.getDevice() != Device ||
            First >= Pool->getDescriptor().Count || Count > Pool->getDescriptor().Count - First ||
            !Destination->getDescriptor().Usage.has(EBufferUsage::TransferDst) ||
            (Pool->getDescriptor().Type == EQueryType::Timestamp &&
                Flags.has(EQueryResultFlag_t::Partial)) ||
            (Flags.Value & ~((1u << 4) - 1)) != 0)
            return false;
        const DeviceSizeType word_size = Flags.has(EQueryResultFlag_t::Result64) ? 8u : 4u;
        const DeviceSizeType value_count = pool.getResultValuesPerQuery() +
            (Flags.has(EQueryResultFlag_t::WithAvailability) ? 1u : 0u);
        const DeviceSizeType record_size = word_size * value_count;
        if (Stride < record_size || Stride % word_size != 0 || Offset % word_size != 0)
            return false;
        const DeviceSizeType required_size = record_size + static_cast<DeviceSizeType>(Count - 1) * Stride;
        if (Offset > Destination->getDescriptor().Size ||
            required_size > Destination->getDescriptor().Size - Offset)
            return false;

        vk::QueryResultFlags vk_flags;
        if (Flags.has(EQueryResultFlag_t::Result64)) vk_flags |= vk::QueryResultFlagBits::e64;
        if (Flags.has(EQueryResultFlag_t::Wait)) vk_flags |= vk::QueryResultFlagBits::eWait;
        if (Flags.has(EQueryResultFlag_t::WithAvailability))
            vk_flags |= vk::QueryResultFlagBits::eWithAvailability;
        if (Flags.has(EQueryResultFlag_t::Partial)) vk_flags |= vk::QueryResultFlagBits::ePartial;
        // vkCmdCopyQueryPoolResults writes one result record per query at the supplied stride.
        CommandBuffer->copyQueryPoolResults(
            pool.getVkQueryPool(), First, Count, buffer.getVkBuffer(), Offset, Stride, vk_flags);
        RetainedResources.emplace_back(Pool);
        RetainedResources.emplace_back(Destination);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool VulkanCommandList::buildAccelerationStructures(
    std::span<const AccelerationStructureBuildDescriptor> Builds)
{
    if (State != ECommandListState::Recording || InsideRendering ||
        Descriptor.QueueType == ECommandQueueType::Copy)
        return false;
    return recordVulkanAccelerationStructureBuilds(
        *Device, CommandBuffer.get(), Builds, RetainedResources);
}

bool VulkanCommandList::traceRays(const TraceRaysDescriptor& Desc)
{
    if (State != ECommandListState::Recording || InsideRendering ||
        Descriptor.QueueType == ECommandQueueType::Copy ||
        !Device->getFeatures().RayTracingPipeline || !BoundRayTracingPipeline ||
        Desc.Width == 0 || Desc.Height == 0 || Desc.Depth == 0)
        return false;
    const auto& limits = Device->getLimits();
    if (Desc.Width > limits.MaxRayDispatchInvocationCount ||
        Desc.Height > limits.MaxRayDispatchInvocationCount / Desc.Width ||
        Desc.Depth > limits.MaxRayDispatchInvocationCount /
            (static_cast<uint64_t>(Desc.Width) * Desc.Height))
        return false;
    const uint64_t invocations = static_cast<uint64_t>(Desc.Width) * Desc.Height * Desc.Depth;
    if (invocations > limits.MaxRayDispatchInvocationCount) return false;
    const auto properties = Device->getVkPhysicalDevice().getProperties();
    for (uint32_t index = 0; index < 3; ++index)
    {
        const uint64_t dimension_limit =
            static_cast<uint64_t>(properties.limits.maxComputeWorkGroupCount[index]) *
            properties.limits.maxComputeWorkGroupSize[index];
        const uint32_t dimension = index == 0 ? Desc.Width : index == 1 ? Desc.Height : Desc.Depth;
        if (dimension > dimension_limit) return false;
    }

    auto valid_region = [&](const ShaderBindingTableRegion& region, bool ray_generation)
    {
        if (region.Size == 0) return region.Address == 0 && region.Stride == 0 && !ray_generation;
        if (region.Address == 0 || region.Address % limits.ShaderBindingTableAlignment != 0 ||
            region.Stride < limits.ShaderGroupHandleSize ||
            region.Stride % limits.ShaderGroupHandleAlignment != 0 ||
            region.Stride > limits.MaxShaderGroupStride)
            return false;
        // VkStridedDeviceAddressRegionKHR requires one record for raygen and whole records elsewhere.
        return ray_generation ? region.Size == region.Stride : region.Size % region.Stride == 0;
    };
    if (!valid_region(Desc.RayGeneration, true) || !valid_region(Desc.Miss, false) ||
        !valid_region(Desc.Hit, false) || !valid_region(Desc.Callable, false))
        return false;

    const VkStridedDeviceAddressRegionKHR ray_generation {
        Desc.RayGeneration.Address, Desc.RayGeneration.Stride, Desc.RayGeneration.Size };
    const VkStridedDeviceAddressRegionKHR miss { Desc.Miss.Address, Desc.Miss.Stride, Desc.Miss.Size };
    const VkStridedDeviceAddressRegionKHR hit { Desc.Hit.Address, Desc.Hit.Stride, Desc.Hit.Size };
    const VkStridedDeviceAddressRegionKHR callable { Desc.Callable.Address, Desc.Callable.Stride, Desc.Callable.Size };
    auto function = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        Device->getVkDevice().getProcAddr("vkCmdTraceRaysKHR"));
    if (!function) return false;
    // vkCmdTraceRaysKHR consumes four VkStridedDeviceAddressRegionKHR SBT regions.
    function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &ray_generation, &miss, &hit,
        &callable, Desc.Width, Desc.Height, Desc.Depth);
    return true;
}

void VulkanCommandList::beginDebugLabel(
    std::string_view Name,
    const std::array<float, 4>& Color)
{
    requireRecording("beginDebugLabel");
    if (Name.empty()) return;
    const auto function = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdBeginDebugUtilsLabelEXT"));
    if (!function) return;
    const std::string name(Name);
    VkDebugUtilsLabelEXT label { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    label.pLabelName = name.c_str();
    std::copy(Color.begin(), Color.end(), label.color);
    function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &label);
}

void VulkanCommandList::endDebugLabel()
{
    requireRecording("endDebugLabel");
    const auto function = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdEndDebugUtilsLabelEXT"));
    if (function) function(static_cast<VkCommandBuffer>(CommandBuffer.get()));
}

void VulkanCommandList::insertDebugLabel(
    std::string_view Name,
    const std::array<float, 4>& Color)
{
    requireRecording("insertDebugLabel");
    if (Name.empty()) return;
    const auto function = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdInsertDebugUtilsLabelEXT"));
    if (!function) return;
    const std::string name(Name);
    VkDebugUtilsLabelEXT label { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    label.pLabelName = name.c_str();
    std::copy(Color.begin(), Color.end(), label.color);
    function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &label);
}

void VulkanCommandList::executeSecondary(
    std::span<const std::shared_ptr<RCommandList>> CommandLists)
{
    requireRecording("executeSecondary");
    if (Descriptor.Level != ECommandListLevel::Primary)
        throw std::logic_error("Only a primary command list can execute secondary command lists.");
    if (InsideRendering && !RenderingUsesSecondaryCommandBuffers)
        throw std::logic_error("The active rendering scope was not declared for secondary command buffers.");
    std::vector<vk::CommandBuffer> native;
    native.reserve(CommandLists.size());
    for (const auto& command : CommandLists)
    {
        auto* secondary = dynamic_cast<VulkanCommandList*>(command.get());
        if (!secondary || &secondary->getDevice() != Device ||
            secondary->getLevel() != ECommandListLevel::Secondary ||
            secondary->getQueueType() != Descriptor.QueueType ||
            secondary->getState() != ECommandListState::Executable)
            throw std::invalid_argument("executeSecondary received an incompatible command list.");
        if (InsideRendering)
        {
            if (!secondary->Descriptor.RenderingInheritance ||
                !isRenderingCompatible(
                    inheritanceSignature(*secondary->Descriptor.RenderingInheritance),
                    ActiveRenderingSignature))
                throw std::invalid_argument("Secondary rendering inheritance is incompatible with the active scope.");
        }
        else if (secondary->Descriptor.RenderingInheritance)
        {
            throw std::invalid_argument("A rendering-inherited secondary cannot execute outside rendering.");
        }
        native.emplace_back(secondary->getVkCommandBuffer());
        if (std::ranges::find_if(ExecutedSecondaries, [secondary](const auto& item)
            { return item.get() == secondary; }) == ExecutedSecondaries.end())
            ExecutedSecondaries.emplace_back(secondary);
        RetainedResources.emplace_back(command);
    }
    if (!native.empty()) CommandBuffer->executeCommands(native);
}

void VulkanCommandList::requireRecording(const char* Operation) const
{
    if (State != ECommandListState::Recording)
        throw std::logic_error(std::string(Operation) + " requires a recording command list.");
}

void VulkanCommandList::requireInsideRendering(const char* Operation) const
{
    if (!InsideRendering)
        throw std::logic_error(std::string(Operation) + " requires an active rendering scope.");
}

void VulkanCommandList::requireOutsideRendering(const char* Operation) const
{
    if (InsideRendering)
        throw std::logic_error(std::string(Operation) + " is not allowed inside a rendering scope.");
}

void VulkanCommandList::validateRenderingInfo(const RenderingInfo& Info) const
{
    if (Info.Area.X < 0 || Info.Area.Y < 0 || Info.Area.Width == 0 || Info.Area.Height == 0)
        throw std::invalid_argument("Rendering area is invalid.");
    if (Info.ColorAttachments.size() > MaxColorAttachments ||
        Info.ColorAttachments.size() > Device->getLimits().MaxColorAttachments)
    {
        throw std::invalid_argument("Rendering uses too many color attachments.");
    }
    if (Info.ColorAttachments.empty() && !Info.DepthStencil)
        throw std::invalid_argument("Rendering requires at least one attachment.");
    if (Info.LayerCount == 0 || (Info.ViewMask != 0 && Info.LayerCount != 1))
        throw std::invalid_argument("Rendering layer count is invalid.");
    if (Info.ViewMask != 0 && !Device->getFeatures().Multiview)
        throw std::invalid_argument("Multiview rendering is not supported by this device.");

    ESampleCount common_samples = ESampleCount::Count1;
    bool samples_set = false;
    auto validate_view = [&](const std::shared_ptr<RImageView>& view, bool color)
    {
        getVulkanView(view);
        const auto& view_desc = view->getDescriptor();
        const auto& image_desc = view->getImage()->getDescriptor();
        if (color ? hasDepthAspect(view_desc.Format) : !hasDepthAspect(view_desc.Format))
            throw std::invalid_argument(color
                ? "A color attachment cannot use a depth/stencil format."
                : "A depth attachment requires a depth/stencil format.");
        if (view_desc.MipLevelCount != 1)
            throw std::invalid_argument("A rendering attachment view must select exactly one mip level.");
        const uint32_t required_layers = Info.ViewMask == 0
            ? Info.LayerCount
            : std::bit_width(Info.ViewMask);
        if (view_desc.ArrayLayerCount < required_layers)
            throw std::invalid_argument("An attachment view does not contain enough array layers.");
        if (color && !image_desc.Usage.has(EImageUsage_t::Target))
            throw std::invalid_argument("A color attachment image requires Target usage.");
        if (!color && !image_desc.Usage.has(EImageUsage_t::DepthStencil))
            throw std::invalid_argument("A depth/stencil attachment image requires DepthStencil usage.");
        const uint32_t width = mipExtent(image_desc.Width, view_desc.BaseMipLevel);
        const uint32_t height = mipExtent(image_desc.Height, view_desc.BaseMipLevel);
        const uint64_t right = static_cast<uint64_t>(Info.Area.X) + Info.Area.Width;
        const uint64_t bottom = static_cast<uint64_t>(Info.Area.Y) + Info.Area.Height;
        if (right > width || bottom > height)
            throw std::out_of_range("Rendering area exceeds an attachment extent.");
        if (!samples_set)
        {
            common_samples = image_desc.SampleCount;
            samples_set = true;
        }
        else if (common_samples != image_desc.SampleCount)
        {
            throw std::invalid_argument("All rendering attachments must use the same sample count.");
        }
    };

    for (const auto& attachment : Info.ColorAttachments)
    {
        validate_view(attachment.View, true);
        validateResolve(attachment.View, attachment.ResolveView, attachment.ResolveMode, true);
        if (attachment.ResolveView)
            getVulkanView(attachment.ResolveView);
    }
    if (Info.DepthStencil)
    {
        validate_view(Info.DepthStencil->View, false);
        const EFormat format = Info.DepthStencil->View->getDescriptor().Format;
        if (!hasDepthAspect(format) && Info.DepthStencil->Depth.ResolveMode != EResolveMode::None)
            throw std::invalid_argument("Depth resolve requires a depth aspect.");
        if (!hasStencilAspect(format) && Info.DepthStencil->Stencil.ResolveMode != EResolveMode::None)
            throw std::invalid_argument("Stencil resolve requires a stencil aspect.");
        if (Info.DepthStencil->ResolveView)
        {
            getVulkanView(Info.DepthStencil->ResolveView);
            const EResolveMode mode = Info.DepthStencil->Depth.ResolveMode != EResolveMode::None
                ? Info.DepthStencil->Depth.ResolveMode
                : Info.DepthStencil->Stencil.ResolveMode;
            validateResolve(
                Info.DepthStencil->View,
                Info.DepthStencil->ResolveView,
                mode,
                false);
        }
        else if (Info.DepthStencil->Depth.ResolveMode != EResolveMode::None ||
            Info.DepthStencil->Stencil.ResolveMode != EResolveMode::None)
        {
            throw std::invalid_argument("Depth/stencil resolve modes require a resolve view.");
        }
    }
}

void VulkanCommandList::validateGraphicsPipeline() const
{
    if (!BoundGraphicsPipeline)
        throw std::logic_error("Draw requires a bound graphics pipeline.");
    if (Descriptor.Level == ECommandListLevel::Primary && RenderingUsesSecondaryCommandBuffers)
        throw std::logic_error("Inline draws are forbidden in a secondary-command-buffer rendering scope.");
    validateRenderingCompatibility(*BoundGraphicsPipeline);
    validateDynamicStates(*BoundGraphicsPipeline);
    if (DynamicPrimitiveTopology &&
        !sameTopologyClass(*DynamicPrimitiveTopology,
            static_cast<const VulkanPipeline&>(*BoundGraphicsPipeline).getPrimitiveTopology()))
        throw std::logic_error("Dynamic primitive topology changes the pipeline topology class.");
}

void VulkanCommandList::validateRenderingCompatibility(const RPipeline& Pipeline) const
{
    const auto* signature = Pipeline.getRenderingSignature();
    if (!signature)
        throw std::logic_error("A graphics pipeline must expose a rendering signature.");
    if (!isRenderingCompatible(*signature, ActiveRenderingSignature))
        throw std::logic_error("The graphics pipeline is incompatible with the active rendering attachments.");
}

void VulkanCommandList::validateDynamicStates(const RPipeline& Pipeline) const
{
    const EDynamicStates missing = Pipeline.getDynamicStates() & ~InitializedDynamicStates;
    if (missing)
        throw std::logic_error("Draw requires every dynamic pipeline state to be initialized.");
}

void VulkanCommandList::retainRenderingResources(const RenderingInfo& Info)
{
    for (const auto& attachment : Info.ColorAttachments)
    {
        RetainedResources.emplace_back(attachment.View);
        if (attachment.ResolveView)
            RetainedResources.emplace_back(attachment.ResolveView);
    }
    if (Info.DepthStencil)
    {
        RetainedResources.emplace_back(Info.DepthStencil->View);
        if (Info.DepthStencil->ResolveView)
            RetainedResources.emplace_back(Info.DepthStencil->ResolveView);
    }
}

} // namespace rhi
