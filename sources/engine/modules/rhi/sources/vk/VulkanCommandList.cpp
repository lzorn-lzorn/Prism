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

#include <infra/hash/Hash.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <optional>
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

/**
 * @brief 1.0 屏障状态(回滚路径).
 *
 * synchronization2 不可用时, 屏障必须回退到 vkCmdPipelineBarrier 的 1.0 阶段/访问位.
 * 这里提供与 convertResourceState / convertMemoryState 平行的 1.0 映射.
 */
struct VulkanLegacyResourceState
{
    vk::PipelineStageFlags Stages;
    vk::AccessFlags Access;
    vk::ImageLayout Layout;
};

VulkanLegacyResourceState convertLegacyResourceState(EResourceState State)
{
    using Stage = vk::PipelineStageFlagBits;
    using Access = vk::AccessFlagBits;

    switch (State)
    {
    case EResourceState::Undefined:
        return { Stage::eTopOfPipe, vk::AccessFlags(), vk::ImageLayout::eUndefined };
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
        return { Stage::eTransfer, Access::eTransferRead, vk::ImageLayout::eTransferSrcOptimal };
    case EResourceState::CopyDst:
        return { Stage::eTransfer, Access::eTransferWrite, vk::ImageLayout::eTransferDstOptimal };
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

VulkanLegacyResourceState convertLegacyMemoryState(EResourceState State)
{
    if (State == EResourceState::VertexBuffer || State == EResourceState::IndexBuffer)
        return { vk::PipelineStageFlagBits::eVertexInput,
            State == EResourceState::VertexBuffer
                ? vk::AccessFlagBits::eVertexAttributeRead
                : vk::AccessFlagBits::eIndexRead,
            vk::ImageLayout::eGeneral };
    if (State == EResourceState::UniformBuffer || State == EResourceState::ConstantBuffer)
        return { vk::PipelineStageFlagBits::eAllGraphics | vk::PipelineStageFlagBits::eComputeShader,
            vk::AccessFlagBits::eUniformRead, vk::ImageLayout::eGeneral };
    return convertLegacyResourceState(State);
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
    const auto* Result = dynamic_cast<const VulkanImageView*>(View.get());
    if (!Result)
        throw std::invalid_argument("Rendering received an image view from another backend.");
    return *Result;
}

const VulkanImage& getVulkanImage(const std::shared_ptr<RImage>& Image)
{
    if (!Image || !Image->isValid())
        throw std::invalid_argument("Image barrier requires a valid image.");
    const auto* Result = dynamic_cast<const VulkanImage*>(Image.get());
    if (!Result)
        throw std::invalid_argument("Image barrier received an image from another backend.");
    return *Result;
}

const VulkanBuffer& getVulkanBuffer(const std::shared_ptr<RBuffer>& Buffer)
{
    if (!Buffer || !Buffer->isValid())
        throw std::invalid_argument("Command requires a valid buffer.");
    const auto* Result = dynamic_cast<const VulkanBuffer*>(Buffer.get());
    if (!Result)
        throw std::invalid_argument("Command received a buffer from another backend.");
    return *Result;
}

const VulkanQueryPool& getVulkanQueryPool(const std::shared_ptr<RQueryPool>& Pool)
{
    const auto* Result = dynamic_cast<const VulkanQueryPool*>(Pool.get());
    if (!Result)
        throw std::invalid_argument("Command received a query pool from another backend.");
    return *Result;
}

vk::ImageSubresourceLayers toLayers(const ImageSubresourceRange& Range, EFormat Format)
{
    if (Range.MipLevelCount != 1 || Range.ArrayLayerCount == 0)
        throw std::invalid_argument("Image copy regions require exactly one mip level and at least one layer.");
    const EImageAspect Aspect = Range.Aspect == EImageAspect::Auto
        ? inferAspect(Format)
        : Range.Aspect;
    return vk::ImageSubresourceLayers(
        toVk(Aspect), Range.BaseMipLevel, Range.BaseArrayLayer, Range.ArrayLayerCount);
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

    const auto& SourceImage = Source->getImage()->getDescriptor();
    const auto& ResolveImage = Resolve->getImage()->getDescriptor();
    if (SourceImage.SampleCount == ESampleCount::Count1)
        throw std::invalid_argument("A resolve source must be multisampled.");
    if (ResolveImage.SampleCount != ESampleCount::Count1)
        throw std::invalid_argument("A resolve destination must have one sample.");
    if (Source->getDescriptor().Format != Resolve->getDescriptor().Format)
        throw std::invalid_argument("Resolve source and destination formats must match.");
    if (SourceImage.Width != ResolveImage.Width || SourceImage.Height != ResolveImage.Height)
        throw std::invalid_argument("Resolve source and destination extents must match.");
}

RenderingSignature inheritanceSignature(const SecondaryCommandListInheritance& Inheritance)
{
    RenderingSignature Result {};
    Result.ColorFormats = Inheritance.ColorFormats;
    Result.ColorAttachmentCount = Inheritance.ColorAttachmentCount;
    Result.DepthFormat = Inheritance.DepthFormat;
    Result.StencilFormat = Inheritance.StencilFormat;
    Result.SampleCount = Inheritance.SampleCount;
    Result.ViewMask = Inheritance.ViewMask;
    return Result;
}

bool sameTopologyClass(EPrimitiveTopology Left, EPrimitiveTopology Right)
{
    auto TopologyClass = [](EPrimitiveTopology Topology)
    {
        switch (Topology)
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
    return TopologyClass(Left) == TopologyClass(Right);
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
        const auto& Inheritance = *Desc.RenderingInheritance;
        if (Inheritance.ColorAttachmentCount > MaxColorAttachments ||
            Inheritance.ColorAttachmentCount > Device->getLimits().MaxColorAttachments)
            throw std::invalid_argument("Secondary rendering inheritance has too many color attachments.");
        for (uint32_t Index = 0; Index < MaxColorAttachments; ++Index)
        {
            const bool Used = Index < Inheritance.ColorAttachmentCount;
            if (Used == (Inheritance.ColorFormats[Index] == EFormat::Undefined) ||
                (Used && hasDepthAspect(Inheritance.ColorFormats[Index])))
                throw std::invalid_argument("Secondary rendering inheritance has invalid color formats.");
        }
        if ((Inheritance.DepthFormat != EFormat::Undefined && !hasDepthAspect(Inheritance.DepthFormat)) ||
            (Inheritance.StencilFormat != EFormat::Undefined && !hasStencilAspect(Inheritance.StencilFormat)))
            throw std::invalid_argument("Secondary rendering inheritance has invalid depth/stencil formats.");
        if (Inheritance.ViewMask != 0 && !Device->getFeatures().Multiview)
            throw std::invalid_argument("Secondary multiview inheritance is unsupported.");
    }
    CommandPool = Device->getVkDevice().createCommandPoolUnique(
        vk::CommandPoolCreateInfo(
            vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            QueueFamilyIndex));

    vk::CommandBufferAllocateInfo AllocationInfo(
        CommandPool.get(),
        Desc.Level == ECommandListLevel::Primary
            ? vk::CommandBufferLevel::ePrimary
            : vk::CommandBufferLevel::eSecondary,
        1);
    auto CommandBuffers = Device->getVkDevice().allocateCommandBuffersUnique(AllocationInfo);
    CommandBuffer = std::move(CommandBuffers.front());
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
        vk::CommandBufferUsageFlags Usage;
        if (Descriptor.OneTimeSubmit)
            Usage |= vk::CommandBufferUsageFlagBits::eOneTimeSubmit;

        vk::CommandBufferInheritanceRenderingInfo RenderingInheritance {};
        vk::CommandBufferInheritanceInfo InheritanceInfo {};
        std::array<vk::Format, MaxColorAttachments> ColorFormats {};
        const vk::CommandBufferInheritanceInfo* InheritancePointer = nullptr;
        if (Descriptor.Level == ECommandListLevel::Secondary)
        {
            InheritancePointer = &InheritanceInfo;
            if (Descriptor.RenderingInheritance)
            {
                const auto& Source = *Descriptor.RenderingInheritance;
                ActiveRenderingSignature = inheritanceSignature(Source);
                if (Device->getFeatures().DynamicRendering)
                {
                    for (uint32_t Index = 0; Index < Source.ColorAttachmentCount; ++Index)
                        ColorFormats[Index] = toVk(Source.ColorFormats[Index]);
                    // VkCommandBufferInheritanceRenderingInfo is required by dynamic rendering secondaries.
                    RenderingInheritance
                        .setViewMask(Source.ViewMask)
                        .setColorAttachmentCount(Source.ColorAttachmentCount)
                        .setPColorAttachmentFormats(ColorFormats.data())
                        .setDepthAttachmentFormat(toVk(Source.DepthFormat))
                        .setStencilAttachmentFormat(toVk(Source.StencilFormat))
                        .setRasterizationSamples(toVk(Source.SampleCount));
                    InheritanceInfo.pNext = &RenderingInheritance;
                }
                else
                {
                    // 1.0 基线: 二级命令缓冲继承兼容 renderpass.
                    InheritanceInfo.renderPass =
                        Device->getOrCreateCompatibleRenderPass(ActiveRenderingSignature);
                    InheritanceInfo.subpass = 0;
                }
                Usage |= vk::CommandBufferUsageFlagBits::eRenderPassContinue;
                InheritedRendering = true;
                InsideRendering = true;
            }
        }
        CommandBuffer->begin(vk::CommandBufferBeginInfo(Usage, InheritancePointer));
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
    for (const auto& Secondary : ExecutedSecondaries)
        Secondary->markSubmitted();
}

void VulkanCommandList::markComplete()
{
    if (State == ECommandListState::Pending)
        State = Descriptor.OneTimeSubmit
            ? ECommandListState::Completed
            : ECommandListState::Executable;
    for (const auto& Secondary : ExecutedSecondaries)
        Secondary->markComplete();
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

    // synchronization2 不可用时回滚到 1.0 的 vkCmdPipelineBarrier.
    if (!Device->getFeatures().Synchronization2)
    {
        barriersLegacy(GlobalBarriers, BufferBarriers, ImageBarriers);
        return;
    }

    std::vector<vk::MemoryBarrier2> VkGlobalBarriers;
    VkGlobalBarriers.reserve(GlobalBarriers.size());
    for (const auto& Barrier : GlobalBarriers)
    {
        const auto Before = convertMemoryState(Barrier.Before);
        const auto After = convertMemoryState(Barrier.After);
        VkGlobalBarriers.emplace_back(Before.Stages, Before.Access, After.Stages, After.Access);
    }

    std::vector<vk::BufferMemoryBarrier2> VkBufferBarriers;
    VkBufferBarriers.reserve(BufferBarriers.size());
    for (const auto& Barrier : BufferBarriers)
    {
        const auto& Buffer = getVulkanBuffer(Barrier.Buffer);
        const auto& Desc = Barrier.Buffer->getDescriptor();
        const uint64_t Size = clampCopySize(Barrier.Offset, Barrier.Size, Desc.Size);
        const auto Before = convertMemoryState(Barrier.Before);
        const auto After = convertMemoryState(Barrier.After);
        if (Barrier.SourceQueue.has_value() != Barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t SourceFamily = Barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*Barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t DestinationFamily = Barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*Barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        VkBufferBarriers.emplace_back(
            Before.Stages, Before.Access, After.Stages, After.Access,
            SourceFamily, DestinationFamily, Buffer.getVkBuffer(), Barrier.Offset, Size);
        RetainedResources.emplace_back(Barrier.Buffer);
    }

    std::vector<vk::ImageMemoryBarrier2> VkBarriers;
    VkBarriers.reserve(ImageBarriers.size());
    for (const auto& Barrier : ImageBarriers)
    {
        const auto& Image = getVulkanImage(Barrier.Image);
        const auto& Desc = Barrier.Image->getDescriptor();
        if (Barrier.Range.MipLevelCount == 0 || Barrier.Range.ArrayLayerCount == 0 ||
            Barrier.Range.BaseMipLevel >= Desc.MipLevels ||
            Barrier.Range.MipLevelCount > Desc.MipLevels - Barrier.Range.BaseMipLevel ||
            Barrier.Range.BaseArrayLayer >= Desc.ArrayLayers ||
            Barrier.Range.ArrayLayerCount > Desc.ArrayLayers - Barrier.Range.BaseArrayLayer)
        {
            throw std::out_of_range("Image barrier subresource range exceeds the image.");
        }

        const EImageAspect Aspect = Barrier.Range.Aspect == EImageAspect::Auto
            ? inferAspect(Desc.Format)
            : Barrier.Range.Aspect;
        const auto Before = convertResourceState(Barrier.Before);
        const auto After = convertResourceState(Barrier.After);
        if (Barrier.SourceQueue.has_value() != Barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t SourceFamily = Barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*Barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t DestinationFamily = Barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*Barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        VkBarriers.emplace_back(
            Before.Stages,
            Before.Access,
            After.Stages,
            After.Access,
            Before.Layout,
            After.Layout,
            SourceFamily,
            DestinationFamily,
            Image.getVkImage(),
            vk::ImageSubresourceRange(
                toVk(Aspect),
                Barrier.Range.BaseMipLevel,
                Barrier.Range.MipLevelCount,
                Barrier.Range.BaseArrayLayer,
                Barrier.Range.ArrayLayerCount));
        RetainedResources.emplace_back(Barrier.Image);
    }

    vk::DependencyInfo Dependency;
    Dependency.setMemoryBarriers(VkGlobalBarriers)
        .setBufferMemoryBarriers(VkBufferBarriers)
        .setImageMemoryBarriers(VkBarriers);
    CommandBuffer->pipelineBarrier2(Dependency);
}

void VulkanCommandList::barriersLegacy(
    std::span<const GlobalBarrier> GlobalBarriers,
    std::span<const BufferBarrier> BufferBarriers,
    std::span<const ImageBarrier> ImageBarriers)
{
    std::vector<vk::MemoryBarrier> VkGlobalBarriers;
    std::vector<vk::BufferMemoryBarrier> VkBufferBarriers;
    std::vector<vk::ImageMemoryBarrier> VkImageBarriers;
    VkGlobalBarriers.reserve(GlobalBarriers.size());
    VkBufferBarriers.reserve(BufferBarriers.size());
    VkImageBarriers.reserve(ImageBarriers.size());

    // 1.0 的 vkCmdPipelineBarrier 只接受一对全局阶段掩码, 故取所有屏障阶段的并集.
    vk::PipelineStageFlags SourceStages;
    vk::PipelineStageFlags DestinationStages;
    const auto accumulateStages = [&](const VulkanLegacyResourceState& Before,
                                      const VulkanLegacyResourceState& After)
    {
        SourceStages |= Before.Stages;
        DestinationStages |= After.Stages;
    };

    for (const auto& Barrier : GlobalBarriers)
    {
        const auto Before = convertLegacyMemoryState(Barrier.Before);
        const auto After = convertLegacyMemoryState(Barrier.After);
        VkGlobalBarriers.emplace_back(Before.Access, After.Access);
        accumulateStages(Before, After);
    }

    for (const auto& Barrier : BufferBarriers)
    {
        const auto& Buffer = getVulkanBuffer(Barrier.Buffer);
        const auto& Desc = Barrier.Buffer->getDescriptor();
        const uint64_t Size = clampCopySize(Barrier.Offset, Barrier.Size, Desc.Size);
        const auto Before = convertLegacyMemoryState(Barrier.Before);
        const auto After = convertLegacyMemoryState(Barrier.After);
        if (Barrier.SourceQueue.has_value() != Barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t SourceFamily = Barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*Barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t DestinationFamily = Barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*Barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        VkBufferBarriers.emplace_back(
            Before.Access, After.Access,
            SourceFamily, DestinationFamily, Buffer.getVkBuffer(), Barrier.Offset, Size);
        RetainedResources.emplace_back(Barrier.Buffer);
        accumulateStages(Before, After);
    }

    for (const auto& Barrier : ImageBarriers)
    {
        const auto& Image = getVulkanImage(Barrier.Image);
        const auto& Desc = Barrier.Image->getDescriptor();
        if (Barrier.Range.MipLevelCount == 0 || Barrier.Range.ArrayLayerCount == 0 ||
            Barrier.Range.BaseMipLevel >= Desc.MipLevels ||
            Barrier.Range.MipLevelCount > Desc.MipLevels - Barrier.Range.BaseMipLevel ||
            Barrier.Range.BaseArrayLayer >= Desc.ArrayLayers ||
            Barrier.Range.ArrayLayerCount > Desc.ArrayLayers - Barrier.Range.BaseArrayLayer)
        {
            throw std::out_of_range("Image barrier subresource range exceeds the image.");
        }

        const EImageAspect Aspect = Barrier.Range.Aspect == EImageAspect::Auto
            ? inferAspect(Desc.Format)
            : Barrier.Range.Aspect;
        const auto Before = convertLegacyResourceState(Barrier.Before);
        const auto After = convertLegacyResourceState(Barrier.After);
        if (Barrier.SourceQueue.has_value() != Barrier.DestinationQueue.has_value())
            throw std::invalid_argument("Queue ownership transfer requires both source and destination queues.");
        const uint32_t SourceFamily = Barrier.SourceQueue
            ? Device->getQueueFamilyIndex(*Barrier.SourceQueue) : VK_QUEUE_FAMILY_IGNORED;
        const uint32_t DestinationFamily = Barrier.DestinationQueue
            ? Device->getQueueFamilyIndex(*Barrier.DestinationQueue) : VK_QUEUE_FAMILY_IGNORED;
        VkImageBarriers.emplace_back(
            Before.Access, After.Access,
            Before.Layout, After.Layout,
            SourceFamily, DestinationFamily,
            Image.getVkImage(),
            vk::ImageSubresourceRange(
                toVk(Aspect),
                Barrier.Range.BaseMipLevel,
                Barrier.Range.MipLevelCount,
                Barrier.Range.BaseArrayLayer,
                Barrier.Range.ArrayLayerCount));
        RetainedResources.emplace_back(Barrier.Image);
        accumulateStages(Before, After);
    }

    CommandBuffer->pipelineBarrier(
        SourceStages, DestinationStages, vk::DependencyFlags(),
        VkGlobalBarriers, VkBufferBarriers, VkImageBarriers);
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

    // 1.0 基线: 无动态渲染时回退到 VkRenderPass / VkFramebuffer.
    if (!Device->getFeatures().DynamicRendering)
    {
        beginRenderingLegacy(Info);
        return;
    }

    std::array<vk::RenderingAttachmentInfo, MaxColorAttachments> ColorAttachments;
    ActiveRenderingSignature = {};
    ActiveRenderingSignature.ColorAttachmentCount = static_cast<uint32_t>(Info.ColorAttachments.size());
    ActiveRenderingSignature.ViewMask = Info.ViewMask;
    bool SampleCountSet = false;

    for (size_t Index = 0; Index < Info.ColorAttachments.size(); ++Index)
    {
        const auto& Source = Info.ColorAttachments[Index];
        const auto& View = getVulkanView(Source.View);
        const auto& ViewDesc = Source.View->getDescriptor();
        const auto& ImageDesc = Source.View->getImage()->getDescriptor();
        ActiveRenderingSignature.ColorFormats[Index] = ViewDesc.Format;
        if (!SampleCountSet)
        {
            ActiveRenderingSignature.SampleCount = ImageDesc.SampleCount;
            SampleCountSet = true;
        }

        auto& Target = ColorAttachments[Index];
        Target
            .setImageView(View.getVkImageView())
            .setImageLayout(vk::ImageLayout::eColorAttachmentOptimal)
            .setLoadOp(convertLoadOp(Source.LoadOp))
            .setStoreOp(convertStoreOp(Source.StoreOp))
            .setClearValue(convertClearColor(Source.ClearValue));
        if (Source.ResolveView)
        {
            const auto& ResolveView = getVulkanView(Source.ResolveView);
            Target
                .setResolveMode(convertResolveMode(Source.ResolveMode))
                .setResolveImageView(ResolveView.getVkImageView())
                .setResolveImageLayout(vk::ImageLayout::eColorAttachmentOptimal);
        }
    }

    vk::RenderingAttachmentInfo DepthAttachment;
    vk::RenderingAttachmentInfo StencilAttachment;
    const vk::RenderingAttachmentInfo* DepthPointer = nullptr;
    const vk::RenderingAttachmentInfo* StencilPointer = nullptr;

    if (Info.DepthStencil)
    {
        const auto& Attachment = *Info.DepthStencil;
        const auto& View = getVulkanView(Attachment.View);
        const EFormat Format = Attachment.View->getDescriptor().Format;
        const auto SampleCount = Attachment.View->getImage()->getDescriptor().SampleCount;
        if (!SampleCountSet)
            ActiveRenderingSignature.SampleCount = SampleCount;
        if (hasDepthAspect(Format))
        {
            ActiveRenderingSignature.DepthFormat = Format;
            DepthAttachment
                .setImageView(View.getVkImageView())
                .setImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                .setLoadOp(convertLoadOp(Attachment.Depth.LoadOp))
                .setStoreOp(convertStoreOp(Attachment.Depth.StoreOp))
                .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue(
                    Attachment.ClearValue.Depth,
                    Attachment.ClearValue.Stencil)));
            DepthPointer = &DepthAttachment;
        }
        if (hasStencilAspect(Format))
        {
            ActiveRenderingSignature.StencilFormat = Format;
            StencilAttachment
                .setImageView(View.getVkImageView())
                .setImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal)
                .setLoadOp(convertLoadOp(Attachment.Stencil.LoadOp))
                .setStoreOp(convertStoreOp(Attachment.Stencil.StoreOp))
                .setClearValue(vk::ClearValue(vk::ClearDepthStencilValue(
                    Attachment.ClearValue.Depth,
                    Attachment.ClearValue.Stencil)));
            StencilPointer = &StencilAttachment;
        }
        if (Attachment.ResolveView)
        {
            const auto& ResolveView = getVulkanView(Attachment.ResolveView);
            if (DepthPointer && Attachment.Depth.ResolveMode != EResolveMode::None)
                DepthAttachment
                    .setResolveMode(convertResolveMode(Attachment.Depth.ResolveMode))
                    .setResolveImageView(ResolveView.getVkImageView())
                    .setResolveImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
            if (StencilPointer && Attachment.Stencil.ResolveMode != EResolveMode::None)
                StencilAttachment
                    .setResolveMode(convertResolveMode(Attachment.Stencil.ResolveMode))
                    .setResolveImageView(ResolveView.getVkImageView())
                    .setResolveImageLayout(vk::ImageLayout::eDepthStencilAttachmentOptimal);
        }
    }

    vk::RenderingInfo RenderingInfo;
    RenderingInfo
        .setRenderArea(vk::Rect2D(
            vk::Offset2D(Info.Area.X, Info.Area.Y),
            vk::Extent2D(Info.Area.Width, Info.Area.Height)))
        .setLayerCount(Info.LayerCount)
        .setViewMask(Info.ViewMask)
        .setColorAttachmentCount(static_cast<uint32_t>(Info.ColorAttachments.size()))
        .setPColorAttachments(ColorAttachments.data())
        .setPDepthAttachment(DepthPointer)
        .setPStencilAttachment(StencilPointer);
    if (Info.SecondaryCommandBuffers)
    {
        // VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT makes this scope secondary-only.
        RenderingInfo.flags |= vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;
    }

    if (BoundGraphicsPipeline)
        validateRenderingCompatibility(*BoundGraphicsPipeline);
    CommandBuffer->beginRendering(RenderingInfo);
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
    if (!Device->getFeatures().DynamicRendering)
    {
        endRenderingLegacy();
        return;
    }
    CommandBuffer->endRendering();
    InsideRendering = false;
    RenderingUsesSecondaryCommandBuffers = false;
    ActiveRenderingSignature = {};
    // 管线只在某个渲染作用域内与其附件兼容, 不能跨作用域复用.
    // 结束作用域时清空绑定, 使下一次 beginRendering 的"已绑管线兼容性"检查
    // 针对的是本作用域真正会绑定的管线, 而不是上一个 Pass 残留的那条.
    BoundGraphicsPipeline.reset();
    BoundComputePipeline.reset();
    BoundRayTracingPipeline.reset();
}

void VulkanCommandList::beginRenderingLegacy(const RenderingInfo& Info)
{
    if (Info.ViewMask != 0)
        throw std::logic_error("Multiview rendering requires dynamic rendering.");

    // 与动态渲染路径一致地填充活动渲染签名, 供管线兼容性校验使用.
    ActiveRenderingSignature = {};
    ActiveRenderingSignature.ColorAttachmentCount = static_cast<uint32_t>(Info.ColorAttachments.size());
    ActiveRenderingSignature.ViewMask = Info.ViewMask;
    bool SampleCountSet = false;
    for (size_t Index = 0; Index < Info.ColorAttachments.size(); ++Index)
    {
        ActiveRenderingSignature.ColorFormats[Index] =
            Info.ColorAttachments[Index].View->getDescriptor().Format;
        if (!SampleCountSet)
        {
            ActiveRenderingSignature.SampleCount =
                Info.ColorAttachments[Index].View->getImage()->getDescriptor().SampleCount;
            SampleCountSet = true;
        }
    }
    if (Info.DepthStencil)
    {
        const EFormat Format = Info.DepthStencil->View->getDescriptor().Format;
        if (!SampleCountSet)
            ActiveRenderingSignature.SampleCount =
                Info.DepthStencil->View->getImage()->getDescriptor().SampleCount;
        if (hasDepthAspect(Format))
            ActiveRenderingSignature.DepthFormat = Format;
        if (hasStencilAspect(Format))
            ActiveRenderingSignature.StencilFormat = Format;
    }

    std::vector<vk::AttachmentDescription> AttachmentDescriptions;
    std::vector<vk::AttachmentReference> ColorReferences;
    std::vector<vk::AttachmentReference> ResolveReferences;
    std::vector<vk::ClearValue> ClearValues;
    std::vector<vk::ImageView> FramebufferViews;

    // 颜色附件(含可选 resolve 附件).
    for (const auto& Source : Info.ColorAttachments)
    {
        const auto& View = getVulkanView(Source.View);
        const auto& ViewDesc = Source.View->getDescriptor();
        const auto SampleCount = Source.View->getImage()->getDescriptor().SampleCount;
        const vk::Format Format = toVk(ViewDesc.Format);

        const uint32_t ColorIndex = static_cast<uint32_t>(AttachmentDescriptions.size());
        AttachmentDescriptions.emplace_back(
            vk::AttachmentDescriptionFlags(),
            Format, toVk(SampleCount),
            convertLoadOp(Source.LoadOp), convertStoreOp(Source.StoreOp),
            vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
            vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eColorAttachmentOptimal);
        ColorReferences.emplace_back(ColorIndex, vk::ImageLayout::eColorAttachmentOptimal);
        ClearValues.emplace_back(convertClearColor(Source.ClearValue));
        FramebufferViews.emplace_back(View.getVkImageView());

        if (Source.ResolveView)
        {
            const auto& ResolveView = getVulkanView(Source.ResolveView);
            const uint32_t ResolveIndex = static_cast<uint32_t>(AttachmentDescriptions.size());
            AttachmentDescriptions.emplace_back(
                vk::AttachmentDescriptionFlags(),
                Format, vk::SampleCountFlagBits::e1,
                vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eStore,
                vk::AttachmentLoadOp::eDontCare, vk::AttachmentStoreOp::eDontCare,
                vk::ImageLayout::eColorAttachmentOptimal, vk::ImageLayout::eColorAttachmentOptimal);
            ResolveReferences.emplace_back(ResolveIndex, vk::ImageLayout::eColorAttachmentOptimal);
            ClearValues.emplace_back(vk::ClearColorValue());
            FramebufferViews.emplace_back(ResolveView.getVkImageView());
        }
        else
        {
            ResolveReferences.emplace_back(VK_ATTACHMENT_UNUSED, vk::ImageLayout::eUndefined);
        }
    }

    // 深度/模板附件.
    std::optional<vk::AttachmentReference> DepthReference;
    if (Info.DepthStencil)
    {
        const auto& Attachment = *Info.DepthStencil;
        const auto& View = getVulkanView(Attachment.View);
        const EFormat Format = Attachment.View->getDescriptor().Format;
        const auto SampleCount = Attachment.View->getImage()->getDescriptor().SampleCount;
        const uint32_t DepthIndex = static_cast<uint32_t>(AttachmentDescriptions.size());
        AttachmentDescriptions.emplace_back(
            vk::AttachmentDescriptionFlags(),
            toVk(Format), toVk(SampleCount),
            convertLoadOp(Attachment.Depth.LoadOp), convertStoreOp(Attachment.Depth.StoreOp),
            convertLoadOp(Attachment.Stencil.LoadOp), convertStoreOp(Attachment.Stencil.StoreOp),
            vk::ImageLayout::eDepthStencilAttachmentOptimal, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        DepthReference.emplace(DepthIndex, vk::ImageLayout::eDepthStencilAttachmentOptimal);
        ClearValues.emplace_back(vk::ClearValue(vk::ClearDepthStencilValue(
            Attachment.ClearValue.Depth, Attachment.ClearValue.Stencil)));
        FramebufferViews.emplace_back(View.getVkImageView());
    }

    vk::SubpassDescription Subpass;
    Subpass.pipelineBindPoint = vk::PipelineBindPoint::eGraphics;
    Subpass.colorAttachmentCount = static_cast<uint32_t>(ColorReferences.size());
    Subpass.pColorAttachments = ColorReferences.empty() ? nullptr : ColorReferences.data();
    Subpass.pResolveAttachments = ResolveReferences.empty() ? nullptr : ResolveReferences.data();
    Subpass.pDepthStencilAttachment = DepthReference ? &*DepthReference : nullptr;

    vk::RenderPassCreateInfo RenderPassInfo(
        vk::RenderPassCreateFlags(),
        static_cast<uint32_t>(AttachmentDescriptions.size()),
        AttachmentDescriptions.empty() ? nullptr : AttachmentDescriptions.data(),
        1, &Subpass);

    // 以附件描述逐字段混合, 得到稳定的 renderpass 缓存 key.
    uint64_t Key = infra::Fnv1aOffsetBasis64;
    for (const auto& Attachment : AttachmentDescriptions)
    {
        infra::hashCombine(Key, static_cast<uint64_t>(static_cast<uint32_t>(Attachment.flags)));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.format));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.samples));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.loadOp));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.storeOp));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.stencilLoadOp));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.stencilStoreOp));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.initialLayout));
        infra::hashCombine(Key, static_cast<uint64_t>(Attachment.finalLayout));
    }

    const vk::RenderPass RenderPass = Device->getOrCreateRenderPass(Key, RenderPassInfo);
    vk::FramebufferCreateInfo FramebufferInfo(
        vk::FramebufferCreateFlags(), RenderPass,
        static_cast<uint32_t>(FramebufferViews.size()),
        FramebufferViews.empty() ? nullptr : FramebufferViews.data(),
        Info.Area.Width, Info.Area.Height, Info.LayerCount);
    ActiveFramebuffer = Device->getVkDevice().createFramebufferUnique(FramebufferInfo);

    vk::RenderPassBeginInfo BeginInfo(
        RenderPass, ActiveFramebuffer.get(),
        vk::Rect2D(vk::Offset2D(Info.Area.X, Info.Area.Y),
                   vk::Extent2D(Info.Area.Width, Info.Area.Height)),
        static_cast<uint32_t>(ClearValues.size()),
        ClearValues.empty() ? nullptr : ClearValues.data());
    const vk::SubpassContents Contents = Info.SecondaryCommandBuffers
        ? vk::SubpassContents::eSecondaryCommandBuffers
        : vk::SubpassContents::eInline;
    CommandBuffer->beginRenderPass(BeginInfo, Contents);

    InsideRendering = true;
    RenderingUsesSecondaryCommandBuffers = Info.SecondaryCommandBuffers;
    retainRenderingResources(Info);
}

void VulkanCommandList::endRenderingLegacy()
{
    CommandBuffer->endRenderPass();
    ActiveFramebuffer.reset();
    InsideRendering = false;
    RenderingUsesSecondaryCommandBuffers = false;
    ActiveRenderingSignature = {};
}

void VulkanCommandList::setViewports(std::span<const Viewport> Viewports)
{
    requireRecording("setViewports");
    if (Viewports.empty() || Viewports.size() > Device->getLimits().MaxViewports)
        throw std::invalid_argument("Viewport count is invalid for this device.");
    std::vector<vk::Viewport> VkViewports;
    VkViewports.reserve(Viewports.size());
    for (const auto& Viewport : Viewports)
    {
        if (Viewport.Width <= 0.0f || Viewport.Height <= 0.0f ||
            Viewport.MinDepth < 0.0f || Viewport.MaxDepth > 1.0f ||
            Viewport.MinDepth > Viewport.MaxDepth)
        {
            throw std::invalid_argument("Viewport dimensions or depth range are invalid.");
        }
        VkViewports.emplace_back(
            Viewport.X,
            Viewport.Y,
            Viewport.Width,
            Viewport.Height,
            Viewport.MinDepth,
            Viewport.MaxDepth);
    }
    CommandBuffer->setViewport(0, VkViewports);
    InitializedDynamicStates.set(EDynamicState_t::Viewport);
}

void VulkanCommandList::setScissors(std::span<const RenderArea> Scissors)
{
    requireRecording("setScissors");
    if (Scissors.empty() || Scissors.size() > Device->getLimits().MaxViewports)
        throw std::invalid_argument("Scissor count is invalid for this device.");
    std::vector<vk::Rect2D> VkScissors;
    VkScissors.reserve(Scissors.size());
    for (const auto& Scissor : Scissors)
    {
        if (Scissor.X < 0 || Scissor.Y < 0 || Scissor.Width == 0 || Scissor.Height == 0)
            throw std::invalid_argument("Scissor rectangle is invalid.");
        VkScissors.emplace_back(
            vk::Offset2D(Scissor.X, Scissor.Y),
            vk::Extent2D(Scissor.Width, Scissor.Height));
    }
    CommandBuffer->setScissor(0, VkScissors);
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
    const vk::FrontFace FrontFace = Face == EFrontFace::Clockwise
        ? vk::FrontFace::eClockwise : vk::FrontFace::eCounterClockwise;
    try { CommandBuffer->setFrontFace(FrontFace); }
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

    const auto Properties = Device->getVkPhysicalDevice().getProperties();
    std::unordered_set<uint32_t> Bindings;
    std::vector<VkVertexInputBindingDescription2EXT> VkBindings;
    VkBindings.reserve(Input.Buffers.size());
    for (const auto& Binding : Input.Buffers)
    {
        if (Binding.Binding >= Device->getLimits().MaxVertexInputBindings || Binding.Stride == 0 ||
            Binding.Stride > Properties.limits.maxVertexInputBindingStride ||
            !Bindings.emplace(Binding.Binding).second)
            return false;
        VkBindings.push_back({ VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, nullptr,
            Binding.Binding, Binding.Stride,
            Binding.InputRate == EVertexInputRate::PerInstance
                ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1 });
    }

    std::unordered_set<uint32_t> Locations;
    std::vector<VkVertexInputAttributeDescription2EXT> VkAttributes;
    VkAttributes.reserve(Input.Attributes.size());
    for (const auto& Attribute : Input.Attributes)
    {
        const vk::Format Format = vertexFormat(Attribute.Format);
        if (!Bindings.contains(Attribute.Binding) ||
            Attribute.Location >= Device->getLimits().MaxVertexInputAttributes ||
            Attribute.Offset > Properties.limits.maxVertexInputAttributeOffset ||
            Format == vk::Format::eUndefined || !Locations.emplace(Attribute.Location).second)
            return false;
        VkAttributes.push_back({ VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, nullptr,
            Attribute.Location, Attribute.Binding, static_cast<VkFormat>(Format), Attribute.Offset });
    }

    const auto Function = reinterpret_cast<PFN_vkCmdSetVertexInputEXT>(
        Device->getVkDevice().getProcAddr("vkCmdSetVertexInputEXT"));
    if (!Function)
        return false;
    Function(static_cast<VkCommandBuffer>(CommandBuffer.get()),
        static_cast<uint32_t>(VkBindings.size()), VkBindings.data(),
        static_cast<uint32_t>(VkAttributes.size()), VkAttributes.data());
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
    auto VkLayout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Layout);
    if (!VkLayout || &VkLayout->getDevice() != Device || !VkLayout->isValid())
        throw std::invalid_argument("Push constant layout belongs to another backend or device.");
    if (!Stages || Data.empty() || Offset % 4 != 0 || Data.size() % 4 != 0 ||
        Data.size() > Device->getLimits().MaxPushConstantSize ||
        Offset > Device->getLimits().MaxPushConstantSize - Data.size())
    {
        throw std::invalid_argument("Push constant update is empty, unaligned or exceeds device limits.");
    }
    if (!VkLayout->supportsPushConstants(Stages, Offset, static_cast<uint32_t>(Data.size())))
        throw std::invalid_argument("Push constant update is not covered by the pipeline layout.");
    CommandBuffer->pushConstants(
        VkLayout->getVkPipelineLayout(),
        toVk(Stages),
        Offset,
        static_cast<uint32_t>(Data.size()),
        Data.data());
    RetainedResources.emplace_back(Layout);
}

void VulkanCommandList::bindPipeline(const std::shared_ptr<RPipeline>& Pipeline)
{
    requireRecording("bindPipeline");
    auto VkPipeline = std::dynamic_pointer_cast<VulkanPipeline>(Pipeline);
    if (!VkPipeline || &VkPipeline->getDevice() != Device || !VkPipeline->isValid())
        throw std::invalid_argument("Cannot bind an invalid pipeline.");
    const vk::Pipeline NativePipeline = VkPipeline->getVkPipeline();
    switch (Pipeline->getType())
    {
    case EPipelineType::Graphics:
        if (InsideRendering)
            validateRenderingCompatibility(*Pipeline);
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eGraphics, NativePipeline);
        BoundGraphicsPipeline = Pipeline;
        break;
    case EPipelineType::Compute:
        requireOutsideRendering("bind compute pipeline");
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eCompute, NativePipeline);
        BoundComputePipeline = Pipeline;
        break;
    case EPipelineType::RayTracing:
        requireOutsideRendering("bind ray-tracing pipeline");
        if (!Device->getFeatures().RayTracingPipeline)
            throw std::invalid_argument("Ray-tracing pipelines are unsupported by this device.");
        CommandBuffer->bindPipeline(vk::PipelineBindPoint::eRayTracingKHR, NativePipeline);
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
    auto VkLayout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Layout);
    if (!VkLayout || &VkLayout->getDevice() != Device || !VkLayout->isValid())
        throw std::invalid_argument("BindGroup pipeline layout belongs to another backend or device.");
    if (PipelineType != EPipelineType::Graphics && PipelineType != EPipelineType::Compute &&
        PipelineType != EPipelineType::RayTracing)
        throw std::invalid_argument("BindGroups require a graphics, compute, or ray-tracing bind point.");
    if (PipelineType != EPipelineType::Graphics)
        requireOutsideRendering("bind non-graphics BindGroups");
    if (FirstGroup > VkLayout->getBindGroupLayoutCount() ||
        Groups.size() > VkLayout->getBindGroupLayoutCount() - FirstGroup)
        throw std::out_of_range("BindGroup range exceeds the pipeline layout.");

    std::vector<vk::DescriptorSet> DescriptorSets;
    DescriptorSets.reserve(Groups.size());
    size_t DynamicOffsetIndex = 0;
    for (size_t GroupIndex = 0; GroupIndex < Groups.size(); ++GroupIndex)
    {
        auto Group = std::dynamic_pointer_cast<VulkanBindGroup>(Groups[GroupIndex]);
        if (!Group || &Group->getDevice() != Device || !Group->isValid())
            throw std::invalid_argument("Cannot bind an invalid or foreign BindGroup.");
        const auto& ExpectedLayout = VkLayout->getBindGroupLayout(
            FirstGroup + static_cast<uint32_t>(GroupIndex));
        const auto ExpectedKey = ExpectedLayout->getCompatibilityKey();
        const auto ActualKey = Group->getLayout()->getCompatibilityKey();
        if (!std::ranges::equal(ExpectedKey, ActualKey))
            throw std::invalid_argument("BindGroup layout is incompatible with the pipeline layout slot.");

        for (const auto& LayoutEntry : Group->getLayout()->getEntries())
        {
            if (!LayoutEntry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset))
                continue;
            for (uint32_t Element = 0; Element < Group->getDescriptorCount(LayoutEntry); ++Element)
            {
                if (DynamicOffsetIndex >= DynamicOffsets.size())
                    throw std::invalid_argument("Too few dynamic offsets for the bound BindGroups.");
                const uint32_t DynamicOffset = DynamicOffsets[DynamicOffsetIndex++];
                const uint64_t Alignment = LayoutEntry.Type == EDescriptorType::UniformBuffer
                    ? Device->getLimits().MinUniformBufferOffsetAlignment
                    : Device->getLimits().MinStorageBufferOffsetAlignment;
                if (DynamicOffset % Alignment != 0)
                    throw std::invalid_argument("A dynamic buffer offset violates device alignment.");

                const auto Resource = std::ranges::find_if(Group->getEntries(), [&](const BindGroupEntry& Entry)
                {
                    return Entry.Binding == LayoutEntry.Binding && Entry.ArrayElement == Element;
                });
                if (Resource != Group->getEntries().end())
                {
                    const auto* Binding = std::get_if<BufferBinding>(&Resource->Resource);
                    if (!Binding || !Binding->Buffer)
                        throw std::logic_error("Dynamic descriptor does not reference a buffer.");
                    const auto& Desc = Binding->Buffer->getDescriptor();
                    const uint64_t Range = Binding->Size == 0
                        ? Desc.Size - Binding->Offset
                        : Binding->Size;
                    if (DynamicOffset > Desc.Size - Binding->Offset ||
                        Range > Desc.Size - Binding->Offset - DynamicOffset)
                        throw std::out_of_range("Dynamic buffer offset moves the descriptor range out of bounds.");
                }
            }
        }
        DescriptorSets.emplace_back(Group->getVkDescriptorSet());
        RetainedResources.emplace_back(std::move(Group));
    }
    if (DynamicOffsetIndex != DynamicOffsets.size())
        throw std::invalid_argument("Too many dynamic offsets for the bound BindGroups.");
    if (DescriptorSets.empty())
        return;

    vk::PipelineBindPoint BindPoint = vk::PipelineBindPoint::eGraphics;
    if (PipelineType == EPipelineType::Compute) BindPoint = vk::PipelineBindPoint::eCompute;
    if (PipelineType == EPipelineType::RayTracing) BindPoint = vk::PipelineBindPoint::eRayTracingKHR;
    CommandBuffer->bindDescriptorSets(
        BindPoint,
        VkLayout->getVkPipelineLayout(),
        FirstGroup,
        DescriptorSets,
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
    std::vector<vk::Buffer> Buffers;
    std::vector<vk::DeviceSize> Offsets;
    Buffers.reserve(Bindings.size());
    Offsets.reserve(Bindings.size());
    for (const auto& Binding : Bindings)
    {
        const auto& Buffer = getVulkanBuffer(Binding.Buffer);
        const auto& Desc = Binding.Buffer->getDescriptor();
        if (!Desc.Usage.has(EBufferUsage::Vertex) || Binding.Offset >= Desc.Size)
            throw std::invalid_argument("Vertex binding requires Vertex usage and a valid offset.");
        Buffers.emplace_back(Buffer.getVkBuffer());
        Offsets.emplace_back(Binding.Offset);
        RetainedResources.emplace_back(Binding.Buffer);
    }
    CommandBuffer->bindVertexBuffers(FirstBinding, Buffers, Offsets);
}

void VulkanCommandList::bindIndexBuffer(
    const std::shared_ptr<RBuffer>& Buffer,
    DeviceSizeType Offset,
    EIndexFormat Format)
{
    requireRecording("bindIndexBuffer");
    const auto& VkBuffer = getVulkanBuffer(Buffer);
    const auto& Desc = VkBuffer.getDescriptor();
    if (!Desc.Usage.has(EBufferUsage::Index) || Format == EIndexFormat::None || Offset >= Desc.Size)
        throw std::invalid_argument("Index binding requires Index usage, a format, and a valid offset.");
    CommandBuffer->bindIndexBuffer(VkBuffer.getVkBuffer(), Offset, toVk(Format));
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::copyBuffer(
    const std::shared_ptr<RBuffer>& Source,
    const std::shared_ptr<RBuffer>& Destination,
    std::span<const BufferCopyRegion> Regions)
{
    requireRecording("copyBuffer");
    requireOutsideRendering("copyBuffer");
    const auto& SourceBuffer = getVulkanBuffer(Source);
    const auto& DestinationBuffer = getVulkanBuffer(Destination);
    if (!SourceBuffer.getDescriptor().Usage.has(EBufferUsage::TransferSrc) ||
        !DestinationBuffer.getDescriptor().Usage.has(EBufferUsage::TransferDst))
        throw std::invalid_argument("Buffer copy resources require TransferSrc/TransferDst usage.");
    std::vector<vk::BufferCopy> VkRegions;
    VkRegions.reserve(Regions.size());
    for (const auto& Region : Regions)
    {
        if (Region.Size == 0 || Region.Size > SourceBuffer.getDescriptor().Size ||
            Region.Size > DestinationBuffer.getDescriptor().Size ||
            Region.SourceOffset > SourceBuffer.getDescriptor().Size - Region.Size ||
            Region.DestinationOffset > DestinationBuffer.getDescriptor().Size - Region.Size)
            throw std::out_of_range("Buffer copy region exceeds a resource.");
        VkRegions.emplace_back(Region.SourceOffset, Region.DestinationOffset, Region.Size);
    }
    if (!VkRegions.empty()) CommandBuffer->copyBuffer(SourceBuffer.getVkBuffer(), DestinationBuffer.getVkBuffer(), VkRegions);
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
    const auto& SourceBuffer = getVulkanBuffer(Source);
    const auto& DestinationImage = getVulkanImage(Destination);
    if (!SourceBuffer.getDescriptor().Usage.has(EBufferUsage::TransferSrc) ||
        !DestinationImage.getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Buffer-to-image copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::BufferImageCopy> VkRegions;
    VkRegions.reserve(Regions.size());
    for (const auto& Region : Regions)
        VkRegions.emplace_back(
            Region.BufferOffset, Region.BufferRowLength, Region.BufferImageHeight,
            toLayers(Region.Image, DestinationImage.getDescriptor().Format),
            vk::Offset3D(Region.ImageOffset.X, Region.ImageOffset.Y, Region.ImageOffset.Z),
            vk::Extent3D(Region.ImageExtent.Width, Region.ImageExtent.Height, Region.ImageExtent.Depth));
    if (!VkRegions.empty()) CommandBuffer->copyBufferToImage(
        SourceBuffer.getVkBuffer(), DestinationImage.getVkImage(), vk::ImageLayout::eTransferDstOptimal, VkRegions);
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
    const auto& SourceImage = getVulkanImage(Source);
    const auto& DestinationBuffer = getVulkanBuffer(Destination);
    if (!SourceImage.getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !DestinationBuffer.getDescriptor().Usage.has(EBufferUsage::TransferDst))
        throw std::invalid_argument("Image-to-buffer copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::BufferImageCopy> VkRegions;
    VkRegions.reserve(Regions.size());
    for (const auto& Region : Regions)
        VkRegions.emplace_back(
            Region.BufferOffset, Region.BufferRowLength, Region.BufferImageHeight,
            toLayers(Region.Image, SourceImage.getDescriptor().Format),
            vk::Offset3D(Region.ImageOffset.X, Region.ImageOffset.Y, Region.ImageOffset.Z),
            vk::Extent3D(Region.ImageExtent.Width, Region.ImageExtent.Height, Region.ImageExtent.Depth));
    if (!VkRegions.empty()) CommandBuffer->copyImageToBuffer(
        SourceImage.getVkImage(), vk::ImageLayout::eTransferSrcOptimal, DestinationBuffer.getVkBuffer(), VkRegions);
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
    const auto& SourceImage = getVulkanImage(Source);
    const auto& DestinationImage = getVulkanImage(Destination);
    if (!SourceImage.getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !DestinationImage.getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Image copy requires TransferSrc/TransferDst usage.");
    std::vector<vk::ImageCopy> VkRegions;
    VkRegions.reserve(Regions.size());
    for (const auto& Region : Regions)
        VkRegions.emplace_back(
            toLayers(Region.Source, SourceImage.getDescriptor().Format),
            vk::Offset3D(Region.SourceOffset.X, Region.SourceOffset.Y, Region.SourceOffset.Z),
            toLayers(Region.Destination, DestinationImage.getDescriptor().Format),
            vk::Offset3D(Region.DestinationOffset.X, Region.DestinationOffset.Y, Region.DestinationOffset.Z),
            vk::Extent3D(Region.Extent.Width, Region.Extent.Height, Region.Extent.Depth));
    if (!VkRegions.empty()) CommandBuffer->copyImage(
        SourceImage.getVkImage(), vk::ImageLayout::eTransferSrcOptimal,
        DestinationImage.getVkImage(), vk::ImageLayout::eTransferDstOptimal, VkRegions);
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
    const auto& SourceImage = getVulkanImage(Source);
    const auto& DestinationImage = getVulkanImage(Destination);
    if (!SourceImage.getDescriptor().Usage.has(EImageUsage_t::TransferSrc) ||
        !DestinationImage.getDescriptor().Usage.has(EImageUsage_t::TransferDst))
        throw std::invalid_argument("Image blit requires TransferSrc/TransferDst usage.");
    std::vector<vk::ImageBlit> VkRegions;
    VkRegions.reserve(Regions.size());
    for (const auto& Region : Regions)
    {
        std::array<vk::Offset3D, 2> SourceOffsets;
        std::array<vk::Offset3D, 2> DestinationOffsets;
        for (size_t Index = 0; Index < 2; ++Index)
        {
            SourceOffsets[Index] = vk::Offset3D(
                Region.SourceOffsets[Index].X,
                Region.SourceOffsets[Index].Y,
                Region.SourceOffsets[Index].Z);
            DestinationOffsets[Index] = vk::Offset3D(
                Region.DestinationOffsets[Index].X,
                Region.DestinationOffsets[Index].Y,
                Region.DestinationOffsets[Index].Z);
        }
        VkRegions.emplace_back(
            toLayers(Region.Source, SourceImage.getDescriptor().Format), SourceOffsets,
            toLayers(Region.Destination, DestinationImage.getDescriptor().Format), DestinationOffsets);
    }
    if (!VkRegions.empty()) CommandBuffer->blitImage(
        SourceImage.getVkImage(), vk::ImageLayout::eTransferSrcOptimal,
        DestinationImage.getVkImage(), vk::ImageLayout::eTransferDstOptimal,
        VkRegions, toVk(Filter));
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
    const auto& VkBuffer = getVulkanBuffer(Buffer);
    if (!VkBuffer.getDescriptor().Usage.has(EBufferUsage::TransferDst) || (Offset & 3u) != 0)
        throw std::invalid_argument("Buffer fill requires TransferDst usage and a 4-byte aligned offset.");
    const uint64_t ResolvedSize = clampCopySize(Offset, Size, VkBuffer.getDescriptor().Size);
    if ((ResolvedSize & 3u) != 0)
        throw std::invalid_argument("Buffer fill size must be a multiple of four.");
    CommandBuffer->fillBuffer(VkBuffer.getVkBuffer(), Offset, ResolvedSize, Value);
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
    const auto DrawMeshTasks = reinterpret_cast<PFN_vkCmdDrawMeshTasksEXT>(
        Device->getVkDevice().getProcAddr("vkCmdDrawMeshTasksEXT"));
    if (!DrawMeshTasks)
        throw std::runtime_error("vkCmdDrawMeshTasksEXT is unavailable on the logical device.");
    DrawMeshTasks(
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
    const auto& VkBuffer = getVulkanBuffer(Buffer);
    if (!VkBuffer.getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indirect draw requires Indirect buffer usage.");
    CommandBuffer->drawIndirect(VkBuffer.getVkBuffer(), Offset, DrawCount, Stride);
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
    const auto& VkBuffer = getVulkanBuffer(Buffer);
    if (!VkBuffer.getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indexed indirect draw requires Indirect buffer usage.");
    CommandBuffer->drawIndexedIndirect(VkBuffer.getVkBuffer(), Offset, DrawCount, Stride);
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
    const auto& VkBuffer = getVulkanBuffer(Buffer);
    if (!VkBuffer.getDescriptor().Usage.has(EBufferUsage::Indirect))
        throw std::invalid_argument("Indirect dispatch requires Indirect buffer usage.");
    CommandBuffer->dispatchIndirect(VkBuffer.getVkBuffer(), Offset);
    RetainedResources.emplace_back(Buffer);
}

void VulkanCommandList::resetQueries(
    const std::shared_ptr<RQueryPool>& Pool,
    uint32_t First,
    uint32_t Count)
{
    requireRecording("resetQueries");
    requireOutsideRendering("resetQueries");
    const auto& VkPool = getVulkanQueryPool(Pool);
    if (&VkPool.getDevice() != Device || Count == 0 || Count > VkPool.getDescriptor().Count ||
        First > VkPool.getDescriptor().Count - Count)
        throw std::out_of_range("Query reset range exceeds the query pool.");
    CommandBuffer->resetQueryPool(VkPool.getVkQueryPool(), First, Count);
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::beginQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("beginQuery");
    const auto& VkPool = getVulkanQueryPool(Pool);
    if (&VkPool.getDevice() != Device || ActiveQueryPool ||
        VkPool.getDescriptor().Type == EQueryType::Timestamp || Query >= VkPool.getDescriptor().Count)
        throw std::invalid_argument("beginQuery requires a valid non-timestamp query.");
    CommandBuffer->beginQuery(VkPool.getVkQueryPool(), Query, {});
    ActiveQueryPool = Pool;
    ActiveQuery = Query;
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::endQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("endQuery");
    const auto& VkPool = getVulkanQueryPool(Pool);
    if (&VkPool.getDevice() != Device || VkPool.getDescriptor().Type == EQueryType::Timestamp ||
        Query >= VkPool.getDescriptor().Count || ActiveQueryPool.get() != Pool.get() || ActiveQuery != Query)
        throw std::invalid_argument("endQuery must match the active query pool and slot.");
    CommandBuffer->endQuery(VkPool.getVkQueryPool(), Query);
    ActiveQueryPool.reset();
    RetainedResources.emplace_back(Pool);
}

void VulkanCommandList::writeTimestamp(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query)
{
    requireRecording("writeTimestamp");
    const auto& VkPool = getVulkanQueryPool(Pool);
    const auto QueueFamilies = Device->getVkPhysicalDevice().getQueueFamilyProperties();
    if (&VkPool.getDevice() != Device || VkPool.getDescriptor().Type != EQueryType::Timestamp ||
        Query >= VkPool.getDescriptor().Count ||
        QueueFamilies[Device->getQueueFamilyIndex(Descriptor.QueueType)].timestampValidBits == 0)
        throw std::invalid_argument("writeTimestamp requires a valid timestamp query.");
    if (Device->getFeatures().Synchronization2)
        CommandBuffer->writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, VkPool.getVkQueryPool(), Query);
    else
        CommandBuffer->writeTimestamp(vk::PipelineStageFlagBits::eAllCommands, VkPool.getVkQueryPool(), Query);
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
        const auto& VkPool = getVulkanQueryPool(Pool);
        const auto& DestinationBuffer = getVulkanBuffer(Destination);
        if (&VkPool.getDevice() != Device || &DestinationBuffer.getDevice() != Device ||
            First >= VkPool.getDescriptor().Count || Count > VkPool.getDescriptor().Count - First ||
            !DestinationBuffer.getDescriptor().Usage.has(EBufferUsage::TransferDst) ||
            (VkPool.getDescriptor().Type == EQueryType::Timestamp &&
                Flags.has(EQueryResultFlag_t::Partial)) ||
            (Flags.Value & ~((1u << 4) - 1)) != 0)
            return false;
        const DeviceSizeType WordSize = Flags.has(EQueryResultFlag_t::Result64) ? 8u : 4u;
        const DeviceSizeType ValueCount = VkPool.getResultValuesPerQuery() +
            (Flags.has(EQueryResultFlag_t::WithAvailability) ? 1u : 0u);
        const DeviceSizeType RecordSize = WordSize * ValueCount;
        if (Stride < RecordSize || Stride % WordSize != 0 || Offset % WordSize != 0)
            return false;
        const DeviceSizeType RequiredSize = RecordSize + static_cast<DeviceSizeType>(Count - 1) * Stride;
        if (Offset > DestinationBuffer.getDescriptor().Size ||
            RequiredSize > DestinationBuffer.getDescriptor().Size - Offset)
            return false;

        vk::QueryResultFlags VkFlags;
        if (Flags.has(EQueryResultFlag_t::Result64)) VkFlags |= vk::QueryResultFlagBits::e64;
        if (Flags.has(EQueryResultFlag_t::Wait)) VkFlags |= vk::QueryResultFlagBits::eWait;
        if (Flags.has(EQueryResultFlag_t::WithAvailability))
            VkFlags |= vk::QueryResultFlagBits::eWithAvailability;
        if (Flags.has(EQueryResultFlag_t::Partial)) VkFlags |= vk::QueryResultFlagBits::ePartial;
        // vkCmdCopyQueryPoolResults writes one result record per query at the supplied stride.
        CommandBuffer->copyQueryPoolResults(
            VkPool.getVkQueryPool(), First, Count, DestinationBuffer.getVkBuffer(), Offset, Stride, VkFlags);
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
    const auto& Limits = Device->getLimits();
    if (Desc.Width > Limits.MaxRayDispatchInvocationCount ||
        Desc.Height > Limits.MaxRayDispatchInvocationCount / Desc.Width ||
        Desc.Depth > Limits.MaxRayDispatchInvocationCount /
            (static_cast<uint64_t>(Desc.Width) * Desc.Height))
        return false;
    const uint64_t Invocations = static_cast<uint64_t>(Desc.Width) * Desc.Height * Desc.Depth;
    if (Invocations > Limits.MaxRayDispatchInvocationCount) return false;
    const auto Properties = Device->getVkPhysicalDevice().getProperties();
    for (uint32_t Index = 0; Index < 3; ++Index)
    {
        const uint64_t DimensionLimit =
            static_cast<uint64_t>(Properties.limits.maxComputeWorkGroupCount[Index]) *
            Properties.limits.maxComputeWorkGroupSize[Index];
        const uint32_t Dimension = Index == 0 ? Desc.Width : Index == 1 ? Desc.Height : Desc.Depth;
        if (Dimension > DimensionLimit) return false;
    }

    auto ValidRegion = [&](const ShaderBindingTableRegion& Region, bool RayGeneration)
    {
        if (Region.Size == 0) return Region.Address == 0 && Region.Stride == 0 && !RayGeneration;
        if (Region.Address == 0 || Region.Address % Limits.ShaderBindingTableAlignment != 0 ||
            Region.Stride < Limits.ShaderGroupHandleSize ||
            Region.Stride % Limits.ShaderGroupHandleAlignment != 0 ||
            Region.Stride > Limits.MaxShaderGroupStride)
            return false;
        // VkStridedDeviceAddressRegionKHR requires one record for raygen and whole records elsewhere.
        return RayGeneration ? Region.Size == Region.Stride : Region.Size % Region.Stride == 0;
    };
    if (!ValidRegion(Desc.RayGeneration, true) || !ValidRegion(Desc.Miss, false) ||
        !ValidRegion(Desc.Hit, false) || !ValidRegion(Desc.Callable, false))
        return false;

    const VkStridedDeviceAddressRegionKHR RayGeneration {
        Desc.RayGeneration.Address, Desc.RayGeneration.Stride, Desc.RayGeneration.Size };
    const VkStridedDeviceAddressRegionKHR Miss { Desc.Miss.Address, Desc.Miss.Stride, Desc.Miss.Size };
    const VkStridedDeviceAddressRegionKHR Hit { Desc.Hit.Address, Desc.Hit.Stride, Desc.Hit.Size };
    const VkStridedDeviceAddressRegionKHR Callable { Desc.Callable.Address, Desc.Callable.Stride, Desc.Callable.Size };
    auto Function = reinterpret_cast<PFN_vkCmdTraceRaysKHR>(
        Device->getVkDevice().getProcAddr("vkCmdTraceRaysKHR"));
    if (!Function) return false;
    // vkCmdTraceRaysKHR consumes four VkStridedDeviceAddressRegionKHR SBT regions.
    Function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &RayGeneration, &Miss, &Hit,
        &Callable, Desc.Width, Desc.Height, Desc.Depth);
    return true;
}

void VulkanCommandList::beginDebugLabel(
    std::string_view Name,
    const std::array<float, 4>& Color)
{
    requireRecording("beginDebugLabel");
    if (Name.empty()) return;
    const auto Function = reinterpret_cast<PFN_vkCmdBeginDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdBeginDebugUtilsLabelEXT"));
    if (!Function) return;
    const std::string LabelName(Name);
    VkDebugUtilsLabelEXT Label { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    Label.pLabelName = LabelName.c_str();
    std::copy(Color.begin(), Color.end(), Label.color);
    Function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &Label);
}

void VulkanCommandList::endDebugLabel()
{
    requireRecording("endDebugLabel");
    const auto Function = reinterpret_cast<PFN_vkCmdEndDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdEndDebugUtilsLabelEXT"));
    if (Function) Function(static_cast<VkCommandBuffer>(CommandBuffer.get()));
}

void VulkanCommandList::insertDebugLabel(
    std::string_view Name,
    const std::array<float, 4>& Color)
{
    requireRecording("insertDebugLabel");
    if (Name.empty()) return;
    const auto Function = reinterpret_cast<PFN_vkCmdInsertDebugUtilsLabelEXT>(
        Device->getVkDevice().getProcAddr("vkCmdInsertDebugUtilsLabelEXT"));
    if (!Function) return;
    const std::string LabelName(Name);
    VkDebugUtilsLabelEXT Label { VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT };
    Label.pLabelName = LabelName.c_str();
    std::copy(Color.begin(), Color.end(), Label.color);
    Function(static_cast<VkCommandBuffer>(CommandBuffer.get()), &Label);
}

void VulkanCommandList::executeSecondary(
    std::span<const std::shared_ptr<RCommandList>> CommandLists)
{
    requireRecording("executeSecondary");
    if (Descriptor.Level != ECommandListLevel::Primary)
        throw std::logic_error("Only a primary command list can execute secondary command lists.");
    if (InsideRendering && !RenderingUsesSecondaryCommandBuffers)
        throw std::logic_error("The active rendering scope was not declared for secondary command buffers.");
    std::vector<vk::CommandBuffer> Native;
    Native.reserve(CommandLists.size());
    for (const auto& Command : CommandLists)
    {
        auto* Secondary = dynamic_cast<VulkanCommandList*>(Command.get());
        if (!Secondary || &Secondary->getDevice() != Device ||
            Secondary->getLevel() != ECommandListLevel::Secondary ||
            Secondary->getQueueType() != Descriptor.QueueType ||
            Secondary->getState() != ECommandListState::Executable)
            throw std::invalid_argument("executeSecondary received an incompatible command list.");
        if (InsideRendering)
        {
            if (!Secondary->Descriptor.RenderingInheritance ||
                !isRenderingCompatible(
                    inheritanceSignature(*Secondary->Descriptor.RenderingInheritance),
                    ActiveRenderingSignature))
                throw std::invalid_argument("Secondary rendering inheritance is incompatible with the active scope.");
        }
        else if (Secondary->Descriptor.RenderingInheritance)
        {
            throw std::invalid_argument("A rendering-inherited secondary cannot execute outside rendering.");
        }
        Native.emplace_back(Secondary->getVkCommandBuffer());
        if (std::ranges::find_if(ExecutedSecondaries, [Secondary](const auto& Item)
            { return Item.get() == Secondary; }) == ExecutedSecondaries.end())
            ExecutedSecondaries.emplace_back(Secondary);
        RetainedResources.emplace_back(Command);
    }
    if (!Native.empty()) CommandBuffer->executeCommands(Native);
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

    ESampleCount CommonSamples = ESampleCount::Count1;
    bool SamplesSet = false;
    auto ValidateView = [&](const std::shared_ptr<RImageView>& View, bool Color)
    {
        getVulkanView(View);
        const auto& ViewDesc = View->getDescriptor();
        const auto& ImageDesc = View->getImage()->getDescriptor();
        if (Color ? hasDepthAspect(ViewDesc.Format) : !hasDepthAspect(ViewDesc.Format))
            throw std::invalid_argument(Color
                ? "A color attachment cannot use a depth/stencil format."
                : "A depth attachment requires a depth/stencil format.");
        if (ViewDesc.MipLevelCount != 1)
            throw std::invalid_argument("A rendering attachment view must select exactly one mip level.");
        const uint32_t RequiredLayers = Info.ViewMask == 0
            ? Info.LayerCount
            : std::bit_width(Info.ViewMask);
        if (ViewDesc.ArrayLayerCount < RequiredLayers)
            throw std::invalid_argument("An attachment view does not contain enough array layers.");
        if (Color && !ImageDesc.Usage.has(EImageUsage_t::Target))
            throw std::invalid_argument("A color attachment image requires Target usage.");
        if (!Color && !ImageDesc.Usage.has(EImageUsage_t::DepthStencil))
            throw std::invalid_argument("A depth/stencil attachment image requires DepthStencil usage.");
        const uint32_t Width = mipExtent(ImageDesc.Width, ViewDesc.BaseMipLevel);
        const uint32_t Height = mipExtent(ImageDesc.Height, ViewDesc.BaseMipLevel);
        const uint64_t Right = static_cast<uint64_t>(Info.Area.X) + Info.Area.Width;
        const uint64_t Bottom = static_cast<uint64_t>(Info.Area.Y) + Info.Area.Height;
        if (Right > Width || Bottom > Height)
            throw std::out_of_range("Rendering area exceeds an attachment extent.");
        if (!SamplesSet)
        {
            CommonSamples = ImageDesc.SampleCount;
            SamplesSet = true;
        }
        else if (CommonSamples != ImageDesc.SampleCount)
        {
            throw std::invalid_argument("All rendering attachments must use the same sample count.");
        }
    };

    for (const auto& Attachment : Info.ColorAttachments)
    {
        ValidateView(Attachment.View, true);
        validateResolve(Attachment.View, Attachment.ResolveView, Attachment.ResolveMode, true);
        if (Attachment.ResolveView)
            getVulkanView(Attachment.ResolveView);
    }
    if (Info.DepthStencil)
    {
        ValidateView(Info.DepthStencil->View, false);
        const EFormat Format = Info.DepthStencil->View->getDescriptor().Format;
        if (!hasDepthAspect(Format) && Info.DepthStencil->Depth.ResolveMode != EResolveMode::None)
            throw std::invalid_argument("Depth resolve requires a depth aspect.");
        if (!hasStencilAspect(Format) && Info.DepthStencil->Stencil.ResolveMode != EResolveMode::None)
            throw std::invalid_argument("Stencil resolve requires a stencil aspect.");
        if (Info.DepthStencil->ResolveView)
        {
            getVulkanView(Info.DepthStencil->ResolveView);
            const EResolveMode Mode = Info.DepthStencil->Depth.ResolveMode != EResolveMode::None
                ? Info.DepthStencil->Depth.ResolveMode
                : Info.DepthStencil->Stencil.ResolveMode;
            validateResolve(
                Info.DepthStencil->View,
                Info.DepthStencil->ResolveView,
                Mode,
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
    const auto* Signature = Pipeline.getRenderingSignature();
    if (!Signature)
        throw std::logic_error("A graphics pipeline must expose a rendering signature.");
    if (!isRenderingCompatible(*Signature, ActiveRenderingSignature))
        throw std::logic_error("The graphics pipeline is incompatible with the active rendering attachments.");
}

void VulkanCommandList::validateDynamicStates(const RPipeline& Pipeline) const
{
    const EDynamicStates Missing = Pipeline.getDynamicStates() & ~InitializedDynamicStates;
    if (Missing)
        throw std::logic_error("Draw requires every dynamic pipeline state to be initialized.");
}

void VulkanCommandList::retainRenderingResources(const RenderingInfo& Info)
{
    for (const auto& Attachment : Info.ColorAttachments)
    {
        RetainedResources.emplace_back(Attachment.View);
        if (Attachment.ResolveView)
            RetainedResources.emplace_back(Attachment.ResolveView);
    }
    if (Info.DepthStencil)
    {
        RetainedResources.emplace_back(Info.DepthStencil->View);
        if (Info.DepthStencil->ResolveView)
            RetainedResources.emplace_back(Info.DepthStencil->ResolveView);
    }
}

} // namespace rhi
