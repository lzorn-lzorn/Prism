#pragma once

#include <memory>
#include <stdexcept>
#include <RHI.hpp>
#include <vulkan/vulkan.hpp>
#include "VulkanContext.hpp"

namespace rhi
{

class VulkanDevice;

template <typename VkType, typename RHIType>
VkType toVk(RHIType Value);

inline auto toVk(EFormat Fomat) -> vk::Format
{
    switch (Fomat)
    {
    case EFormat::RGBA8_UNorm: return vk::Format::eR8G8B8A8Unorm;
    case EFormat::RGBA8_sRGB: return vk::Format::eR8G8B8A8Srgb;
    case EFormat::BGRA8_UNorm: return vk::Format::eB8G8R8A8Unorm;
    case EFormat::BGRA8_sRGB: return vk::Format::eB8G8R8A8Srgb;
    case EFormat::RGBA16_Float: return vk::Format::eR16G16B16A16Sfloat;
    case EFormat::RGBA32_Float: return vk::Format::eR32G32B32A32Sfloat;
    case EFormat::RGB10A2_UNorm: return vk::Format::eA2B10G10R10UnormPack32;
    case EFormat::D16_UNorm: return vk::Format::eD16Unorm;
    case EFormat::D24_UNorm_S8_UInt: return vk::Format::eD24UnormS8Uint;
    case EFormat::D32_Float: return vk::Format::eD32Sfloat;
    case EFormat::Undefined:
    default:
        return vk::Format::eUndefined;
    }
}


inline auto toVk(EIndexFormat Fomat) -> vk::IndexType
{
    switch (Fomat)
    {
    case EIndexFormat::UInt16: return vk::IndexType::eUint16;
    case EIndexFormat::UInt32: return vk::IndexType::eUint32;
    case EIndexFormat::None:
    default:
    #ifdef VK_INDEX_TYPE_NONE_KHR
        return vk::IndexType::eNoneKHR;
    #else
        return vk::IndexType::eUint32;
    #endif
    }
}

inline auto toVk(EShaderStage Stage) -> vk::ShaderStageFlags
{
{
    vk::ShaderStageFlags Result = vk::ShaderStageFlags();
    if (Stage.has(EShaderStage_t::Vertex)) Result |= vk::ShaderStageFlags::BitsType::eVertex;
    if (Stage.has(EShaderStage_t::Pixel)) Result |= vk::ShaderStageFlags::BitsType::eFragment;
    if (Stage.has(EShaderStage_t::Compute)) Result |= vk::ShaderStageFlags::BitsType::eCompute;
    if (Stage.has(EShaderStage_t::Geometry)) Result |= vk::ShaderStageFlags::BitsType::eGeometry;
    if (Stage.has(EShaderStage_t::Hull)) Result |= vk::ShaderStageFlags::BitsType::eTessellationControl;
    if (Stage.has(EShaderStage_t::Domain)) Result |= vk::ShaderStageFlags::BitsType::eTessellationEvaluation;
    if (Stage.has(EShaderStage_t::RayGeneration)) Result |= vk::ShaderStageFlags::BitsType::eRaygenKHR;
    if (Stage.has(EShaderStage_t::AnyHit)) Result |= vk::ShaderStageFlags::BitsType::eAnyHitKHR;
    if (Stage.has(EShaderStage_t::ClosestHit)) Result |= vk::ShaderStageFlags::BitsType::eClosestHitKHR;
    if (Stage.has(EShaderStage_t::Miss)) Result |= vk::ShaderStageFlags::BitsType::eMissKHR;
    if (Stage.has(EShaderStage_t::Intersection)) Result |= vk::ShaderStageFlags::BitsType::eIntersectionKHR;
    if (Stage.has(EShaderStage_t::Callable)) Result |= vk::ShaderStageFlags::BitsType::eCallableKHR;
#ifdef VK_SHADER_STAGE_MESH_BIT_EXT
    if (Stage.has(EShaderStage_t::Mesh)) result |= vk::ShaderStageFlags::BitsType::eMesh;
#endif
#ifdef VK_SHADER_STAGE_TASK_BIT_EXT
    if (Stage.has(EShaderStage_t::Amplification) || Stage.has(EShaderStage_t::Task))
        result |= vk::ShaderStageFlags::BitsType::eTask;
#endif
    return Result;
}
}
inline auto toVk(EBufferUsageFlags Usage) -> vk::BufferUsageFlags
{
    vk::BufferUsageFlags Flags = vk::BufferUsageFlags();
    if (Usage.has(EBufferUsage::Vertex)) Flags |= vk::BufferUsageFlags::BitsType::eVertexBuffer;
    if (Usage.has(EBufferUsage::Index)) Flags |= vk::BufferUsageFlags::BitsType::eIndexBuffer;
    if (Usage.has(EBufferUsage::Uniform)) Flags |= vk::BufferUsageFlags::BitsType::eUniformBuffer;
    if (Usage.has(EBufferUsage::Storage)) Flags |= vk::BufferUsageFlags::BitsType::eStorageBuffer;
    if (Usage.has(EBufferUsage::Indirect)) Flags |= vk::BufferUsageFlags::BitsType::eIndirectBuffer;
    if (Usage.has(EBufferUsage::TransferSrc)) Flags |= vk::BufferUsageFlags::BitsType::eTransferSrc;
    if (Usage.has(EBufferUsage::TransferDst)) Flags |= vk::BufferUsageFlags::BitsType::eTransferDst;
    if (Usage.has(EBufferUsage::UniformTexel)) Flags |= vk::BufferUsageFlags::BitsType::eUniformTexelBuffer;
    if (Usage.has(EBufferUsage::StorageTexel)) Flags |= vk::BufferUsageFlags::BitsType::eStorageTexelBuffer;
    if (Usage.has(EBufferUsage::DeviceAddress)) Flags |= vk::BufferUsageFlags::BitsType::eShaderDeviceAddress;
    if (Usage.has(EBufferUsage::AccelerationStructureBuildInput)) Flags |= vk::BufferUsageFlags::BitsType::eAccelerationStructureBuildInputReadOnlyKHR;
    if (Usage.has(EBufferUsage::AccelerationStructureStorage)) Flags |= vk::BufferUsageFlags::BitsType::eAccelerationStructureStorageKHR;
    if (Usage.has(EBufferUsage::ShaderBindingTable)) Flags |= vk::BufferUsageFlags::BitsType::eShaderBindingTableKHR;
    return Flags;
}

inline auto toVk(EDescriptorType Type, EDescriptorBindingFlags Flags = {}) -> vk::DescriptorType
{
    switch (Type)
    {
    case EDescriptorType::UniformBuffer:
        return Flags.has(EDescriptorBindingFlag_t::DynamicOffset)
            ? vk::DescriptorType::eUniformBufferDynamic
            : vk::DescriptorType::eUniformBuffer;
    case EDescriptorType::ReadOnlyStorageBuffer:
    case EDescriptorType::ReadWriteStorageBuffer:
        return Flags.has(EDescriptorBindingFlag_t::DynamicOffset)
            ? vk::DescriptorType::eStorageBufferDynamic
            : vk::DescriptorType::eStorageBuffer;
    case EDescriptorType::Sampler:
    case EDescriptorType::ComparisonSampler: return vk::DescriptorType::eSampler;
    case EDescriptorType::SampledTexture: return vk::DescriptorType::eSampledImage;
    case EDescriptorType::StorageTexture: return vk::DescriptorType::eStorageImage;
    case EDescriptorType::UniformTexelBuffer: return vk::DescriptorType::eUniformTexelBuffer;
    case EDescriptorType::StorageTexelBuffer: return vk::DescriptorType::eStorageTexelBuffer;
    case EDescriptorType::CombinedImageSampler: return vk::DescriptorType::eCombinedImageSampler;
    case EDescriptorType::InputAttachment: return vk::DescriptorType::eInputAttachment;
#ifdef VK_KHR_acceleration_structure
    case EDescriptorType::AccelerationStructure: return vk::DescriptorType::eAccelerationStructureKHR;
#else
    case EDescriptorType::AccelerationStructure:
        throw std::invalid_argument("Acceleration structures are unavailable in this Vulkan build.");
#endif
    default:
        throw std::invalid_argument("Unsupported descriptor type.");
    }
}

inline auto toVk(EDescriptorImageLayout Layout) -> vk::ImageLayout
{
    switch (Layout)
    {
    case EDescriptorImageLayout::General: return vk::ImageLayout::eGeneral;
    case EDescriptorImageLayout::DepthStencilReadOnly:
        return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    case EDescriptorImageLayout::ShaderReadOnly:
    default:
        return vk::ImageLayout::eShaderReadOnlyOptimal;
    }
}

inline auto toVk(EFilterMode Mode) -> vk::Filter
{
    return Mode == EFilterMode::Nearest ? vk::Filter::eNearest : vk::Filter::eLinear;
}

inline auto toVkMipmapMode(EFilterMode Mode) -> vk::SamplerMipmapMode
{
    return Mode == EFilterMode::Nearest
        ? vk::SamplerMipmapMode::eNearest
        : vk::SamplerMipmapMode::eLinear;
}

inline auto toVk(ESamplerAddressMode Mode) -> vk::SamplerAddressMode
{
    switch (Mode)
    {
    case ESamplerAddressMode::MirroredRepeat: return vk::SamplerAddressMode::eMirroredRepeat;
    case ESamplerAddressMode::ClampToEdge: return vk::SamplerAddressMode::eClampToEdge;
    case ESamplerAddressMode::ClampToBorder: return vk::SamplerAddressMode::eClampToBorder;
    case ESamplerAddressMode::Repeat:
    default:
        return vk::SamplerAddressMode::eRepeat;
    }
}

inline auto toVk(EImageUsage Usage) -> vk::ImageUsageFlags
{
    vk::ImageUsageFlags Flags = vk::ImageUsageFlags();
    if (Usage.has(EImageUsage_t::Sampled)) Flags |= vk::ImageUsageFlags::BitsType::eSampled;
    if (Usage.has(EImageUsage_t::Storage)) Flags |= vk::ImageUsageFlags::BitsType::eStorage;
    if (Usage.has(EImageUsage_t::Target)) Flags |= vk::ImageUsageFlags::BitsType::eColorAttachment;
    if (Usage.has(EImageUsage_t::DepthStencil)) Flags |= vk::ImageUsageFlags::BitsType::eDepthStencilAttachment;
    if (Usage.has(EImageUsage_t::TransferSrc)) Flags |= vk::ImageUsageFlags::BitsType::eTransferSrc;
    if (Usage.has(EImageUsage_t::TransferDst)) Flags |= vk::ImageUsageFlags::BitsType::eTransferDst;
    return Flags;
}

inline vk::PrimitiveTopology toVk(EPrimitiveTopology Topology)
{
    switch (Topology)
    {
    case EPrimitiveTopology::PointList: return vk::PrimitiveTopology::ePointList;
    case EPrimitiveTopology::LineList: return vk::PrimitiveTopology::eLineList;
    case EPrimitiveTopology::LineStrip: return vk::PrimitiveTopology::eLineStrip;
    case EPrimitiveTopology::TriangleStrip: return vk::PrimitiveTopology::eTriangleStrip;
    case EPrimitiveTopology::PatchList: return vk::PrimitiveTopology::ePatchList;
    case EPrimitiveTopology::TriangleList:
    default:
        return vk::PrimitiveTopology::eTriangleList;
    }
}

inline auto toVk(EFillMode Mode) -> vk::PolygonMode
{
    return Mode == EFillMode::Wireframe ? vk::PolygonMode::eLine : vk::PolygonMode::eFill;
}

inline auto toVk(ECullMode Mode) -> vk::CullModeFlags
{
    switch (Mode)
    {
    case ECullMode::Front: return vk::CullModeFlagBits::eFront;
    case ECullMode::Back: return vk::CullModeFlagBits::eBack;
    case ECullMode::None:
    default:
        return vk::CullModeFlagBits::eNone;
    }
}

inline auto toVk(ECompareOp Op) -> vk::CompareOp
{
    {
        switch (Op)
        {
        case ECompareOp::Less: return vk::CompareOp::eLess;
        case ECompareOp::LessEqual: return vk::CompareOp::eLessOrEqual;
        case ECompareOp::Equal: return vk::CompareOp::eEqual;
        case ECompareOp::NotEqual: return vk::CompareOp::eNotEqual;
        case ECompareOp::GreaterEqual: return vk::CompareOp::eGreaterOrEqual;
        case ECompareOp::Greater: return vk::CompareOp::eGreater;
        case ECompareOp::Never: return vk::CompareOp::eNever;
        case ECompareOp::Always:
        default:
            return vk::CompareOp::eAlways;
        }
    }
}

inline auto toVk(EStencilOp Op) -> vk::StencilOp
{
    switch (Op)
    {
    case EStencilOp::Keep: return vk::StencilOp::eKeep;
    case EStencilOp::Zero: return vk::StencilOp::eZero;
    case EStencilOp::Replace: return vk::StencilOp::eReplace;
    case EStencilOp::IncrementAndClamp: return vk::StencilOp::eIncrementAndClamp;
    case EStencilOp::DecrementAndClamp: return vk::StencilOp::eDecrementAndClamp;
    case EStencilOp::Invert: return vk::StencilOp::eInvert;
    case EStencilOp::IncrementAndWrap: return vk::StencilOp::eIncrementAndWrap;
    case EStencilOp::DecrementAndWrap: return vk::StencilOp::eDecrementAndWrap;
    default:
        return vk::StencilOp::eKeep;
    }
}

inline auto toVk(ESampleCount Count) -> vk::SampleCountFlagBits
{
    switch (Count)
    {
    case ESampleCount::Count2: return vk::SampleCountFlagBits::e2;
    case ESampleCount::Count4: return vk::SampleCountFlagBits::e4;
    case ESampleCount::Count8: return vk::SampleCountFlagBits::e8;
    case ESampleCount::Count16: return vk::SampleCountFlagBits::e16;
    case ESampleCount::Count32: return vk::SampleCountFlagBits::e32;
    case ESampleCount::Count64: return vk::SampleCountFlagBits::e64;
    case ESampleCount::Count1:
    default:
        return vk::SampleCountFlagBits::e1;
    }
}

inline auto toVk(EBlendFactor Factor) -> vk::BlendFactor
{
    switch (Factor)
    {
    case EBlendFactor::Zero: return vk::BlendFactor::eZero;
    case EBlendFactor::SrcColor: return vk::BlendFactor::eSrcColor;
    case EBlendFactor::SrcAlpha: return vk::BlendFactor::eSrcAlpha;
    case EBlendFactor::OneMinusSrcAlpha: return vk::BlendFactor::eOneMinusSrcAlpha;
    case EBlendFactor::DstAlpha: return vk::BlendFactor::eDstAlpha;
    case EBlendFactor::OneMinusDstAlpha: return vk::BlendFactor::eOneMinusDstAlpha;
    case EBlendFactor::DstColor: return vk::BlendFactor::eDstColor;
    case EBlendFactor::OneMinusDstColor: return vk::BlendFactor::eOneMinusDstColor;
    case EBlendFactor::One:
    default:
        return vk::BlendFactor::eOne;
    }
}

inline auto toVk(EBlendOp Op) -> vk::BlendOp
{
    switch (Op)
    {
    case EBlendOp::Subtract: return vk::BlendOp::eSubtract;
    case EBlendOp::ReverseSubtract: return vk::BlendOp::eReverseSubtract;
    case EBlendOp::Min: return vk::BlendOp::eMin;
    case EBlendOp::Max: return vk::BlendOp::eMax;
    case EBlendOp::Add:
    default:
        return vk::BlendOp::eAdd;
    }
}


inline auto toVk(EResourceState State) -> vk::ImageLayout
{
    switch (State)
    {
    case EResourceState::Undefined: return vk::ImageLayout::eUndefined;
    case EResourceState::Common: return vk::ImageLayout::eGeneral;
    case EResourceState::VertexBuffer: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case EResourceState::IndexBuffer: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case EResourceState::UniformBuffer: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case EResourceState::ConstantBuffer: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case EResourceState::StorageBuffer: return vk::ImageLayout::eGeneral;
    case EResourceState::UnorderedAccess: return vk::ImageLayout::eGeneral;
    case EResourceState::DepthWrite: return vk::ImageLayout::eDepthStencilAttachmentOptimal;
    case EResourceState::DepthRead: return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
    case EResourceState::CopySrc: return vk::ImageLayout::eTransferSrcOptimal;
    case EResourceState::CopyDst: return vk::ImageLayout::eTransferDstOptimal;
    case EResourceState::Present: return vk::ImageLayout::ePresentSrcKHR;
    case EResourceState::RenderTarget: return vk::ImageLayout::eColorAttachmentOptimal;
    case EResourceState::PixelShaderResource: return vk::ImageLayout::eShaderReadOnlyOptimal;
    case EResourceState::NonPixelShaderResource: return vk::ImageLayout::eShaderReadOnlyOptimal;
    default:
        return vk::ImageLayout::eGeneral;
    }
}

template <>
inline auto toVk(EResourceState State) -> vk::PipelineStageFlags
{
    switch (State)
    {
    case EResourceState::VertexBuffer:
    case EResourceState::IndexBuffer:
        return vk::PipelineStageFlags::BitsType::eVertexInput;
    case EResourceState::ConstantBuffer:
    case EResourceState::UniformBuffer:
    case EResourceState::StorageBuffer:
        return vk::PipelineStageFlags::BitsType::eVertexShader | vk::PipelineStageFlags::BitsType::eFragmentShader | vk::PipelineStageFlags::BitsType::eComputeShader;
    case EResourceState::UnorderedAccess:
        return vk::PipelineStageFlags::BitsType::eComputeShader;
    case EResourceState::RenderTarget:
        return vk::PipelineStageFlags::BitsType::eColorAttachmentOutput;
    case EResourceState::DepthWrite:
    case EResourceState::DepthRead:
        return vk::PipelineStageFlags::BitsType::eEarlyFragmentTests | vk::PipelineStageFlags::BitsType::eLateFragmentTests;
    case EResourceState::CopySrc:
    case EResourceState::CopyDst:
        return vk::PipelineStageFlags::BitsType::eTransfer;
    case EResourceState::Present:
        return vk::PipelineStageFlags::BitsType::eBottomOfPipe;
    case EResourceState::Undefined:
    case EResourceState::Common:
    default:
        return vk::PipelineStageFlags::BitsType::eTopOfPipe;
    }
}


inline auto toVk(ESharingMode Mode) -> vk::SharingMode
{
    return Mode == ESharingMode::Concurrent
        ? vk::SharingMode::eConcurrent
        : vk::SharingMode::eExclusive;
}

inline auto toVk(EMemoryPropertyFlags Properties) -> vk::MemoryPropertyFlags
{
    vk::MemoryPropertyFlags Flags = {};

    if (Properties.has(EMemoryProperty::DeviceLocal))
        Flags |= vk::MemoryPropertyFlagBits::eDeviceLocal;
    
    if (Properties.has(EMemoryProperty::HostVisible))
        Flags |= vk::MemoryPropertyFlagBits::eHostVisible;
    
    if (Properties.has(EMemoryProperty::HostCoherent))
        Flags |= vk::MemoryPropertyFlagBits::eHostCoherent;
    
    if (Properties.has(EMemoryProperty::HostCached))
        Flags |= vk::MemoryPropertyFlagBits::eHostCached;
    
    if (Properties.has(EMemoryProperty::LazilyAllocated))
        Flags |= vk::MemoryPropertyFlagBits::eLazilyAllocated;

    return Flags;
}


inline auto toVk(EImageDimension Dimension) -> vk::ImageType
{
    switch (Dimension)
    {
    case EImageDimension::Texture1D:
    case EImageDimension::Texture1DArray:
        return vk::ImageType::e1D;
    case EImageDimension::Texture3D:
        return vk::ImageType::e3D;
    case EImageDimension::Texture2D:
    case EImageDimension::Texture2DArray:
    case EImageDimension::Cube:
    case EImageDimension::CubeArray:
    default:
        return vk::ImageType::e2D;
    }
}

inline auto toVk(EImageViewDimension Dimension) -> vk::ImageViewType
{
    switch (Dimension)
    {
    case EImageViewDimension::Texture1D: return vk::ImageViewType::e1D;
    case EImageViewDimension::Texture1DArray: return vk::ImageViewType::e1DArray;
    case EImageViewDimension::Texture2D: return vk::ImageViewType::e2D;
    case EImageViewDimension::Texture2DArray: return vk::ImageViewType::e2DArray;
    case EImageViewDimension::Texture3D: return vk::ImageViewType::e3D;
    case EImageViewDimension::Cube: return vk::ImageViewType::eCube;
    case EImageViewDimension::CubeArray: return vk::ImageViewType::eCubeArray;
    case EImageViewDimension::Auto:
    default:
        throw std::invalid_argument("Vulkan image view dimension must be resolved before conversion.");
    }
}

inline auto toVk(EImageAspect Aspect) -> vk::ImageAspectFlags
{
    switch (Aspect)
    {
    case EImageAspect::Color: return vk::ImageAspectFlagBits::eColor;
    case EImageAspect::Depth: return vk::ImageAspectFlagBits::eDepth;
    case EImageAspect::Stencil: return vk::ImageAspectFlagBits::eStencil;
    case EImageAspect::DepthStencil:
        return vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
    case EImageAspect::Auto:
    default:
        throw std::invalid_argument("Vulkan image aspect must be resolved before conversion.");
    }
}

inline auto toVk(EPresentMode PresentMode) -> vk::PresentModeKHR
{
    switch (PresentMode)
    {
    case EPresentMode::Immediate:
        return vk::PresentModeKHR::eImmediate;
        break;
    case EPresentMode::Fifo:
        return vk::PresentModeKHR::eFifo;
        break;
    case EPresentMode::FifoRelaxed:
        return vk::PresentModeKHR::eFifoRelaxed;
        break;
    case EPresentMode::Mailbox:
        return vk::PresentModeKHR::eMailbox;
        break;
    default:
        throw std::runtime_error("Unsupported present mode.");
    }
}

inline auto toVk(EColorSpace ColorSpace) -> vk::ColorSpaceKHR
{
    switch (ColorSpace)
    {
    case EColorSpace::SRGB_Nonlinear:
        return vk::ColorSpaceKHR::eSrgbNonlinear;
    case EColorSpace::AdobeRGB:
        return vk::ColorSpaceKHR::eAdobergbNonlinearEXT;
    case EColorSpace::DCIP3:
        return vk::ColorSpaceKHR::eDciP3NonlinearEXT;
    case EColorSpace::Rec2020:
        // Vulkan 没有"BT.2020 非线性(gamma)"的枚举，
        // 只有线性版本或 HDR 传输函数(ST2084/HLG)
        // 若确定是 SDR 且使用线性传输, 可用 eBt2020LinearEXT；
        // 若是 HDR(PQ 曲线)，可用 eHdr10St2084EXT；
        // 否则建议抛出异常, 因为没有标准对应项.
        return vk::ColorSpaceKHR::eBt2020LinearEXT;
    case EColorSpace::HDR10_ST2084:
        return vk::ColorSpaceKHR::eHdr10St2084EXT;
    case EColorSpace::ExtendedSRGBLinear:
        return vk::ColorSpaceKHR::eExtendedSrgbLinearEXT;
    default:
        throw std::runtime_error("Unsupported color space.");
    }
}

inline auto toVk(ECompositeAlpha CompositeAlpha) -> vk::CompositeAlphaFlagBitsKHR
{
    switch (CompositeAlpha)
    {
    case ECompositeAlpha::Opaque:
        return vk::CompositeAlphaFlagBitsKHR::eOpaque;
    case ECompositeAlpha::PreMultiplied:
        return vk::CompositeAlphaFlagBitsKHR::ePreMultiplied;
    case ECompositeAlpha::PostMultiplied:
        return vk::CompositeAlphaFlagBitsKHR::ePostMultiplied;
    case ECompositeAlpha::Inherit:
        return vk::CompositeAlphaFlagBitsKHR::eInherit;
    default:
        throw std::runtime_error("Unsupported composite alpha.");
    }
}

inline auto toVk(ESurfaceTransform Transform) -> vk::SurfaceTransformFlagBitsKHR
{
    switch (Transform)
    {
    case ESurfaceTransform::Identity:
        return vk::SurfaceTransformFlagBitsKHR::eIdentity;
    case ESurfaceTransform::Rotate90:
        return vk::SurfaceTransformFlagBitsKHR::eRotate90;
    case ESurfaceTransform::Rotate180:
        return vk::SurfaceTransformFlagBitsKHR::eRotate180;
    case ESurfaceTransform::Rotate270:
        return vk::SurfaceTransformFlagBitsKHR::eRotate270;
    case ESurfaceTransform::HorizontalMirror:
        return vk::SurfaceTransformFlagBitsKHR::eHorizontalMirror;
    case ESurfaceTransform::HorizontalMirrorRotate90:
        return vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate90;
    case ESurfaceTransform::HorizontalMirrorRotate180:
        return vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate180;
    case ESurfaceTransform::HorizontalMirrorRotate270:
        return vk::SurfaceTransformFlagBitsKHR::eHorizontalMirrorRotate270;
    default:
        throw std::runtime_error("Unsupported surface transform.");
    }
}


uint64_t clampCopySize(uint64_t Offset, uint64_t RequestedSize, uint64_t MaxSize);


class VulkanRHI final : public IRHI
{
public:

public:
    VulkanRHI() = default;
    ~VulkanRHI() override;

    void initialize(const ui::GenericWindowPointer& Window) override;
    bool isInitialized() const noexcept override { return IsInitialized; }
    ESupportedBackendAPI getBackendAPI() const override { return ESupportedBackendAPI::Vulkan; }
    std::shared_ptr<RDevice> createDevice() override;
    [[nodiscard]] bool recoverSurface(const ui::GenericWindowPointer& Window) override;

public:
    vk::Device& getVkDevice() { return Context->Device.get(); }
private:
    void createVkInstance();
    void createVkSurface(const ui::GenericWindowPointer& Window);
    void pickPhysicalDevice();
    void createLogicalDevice();

    VulkanContextPtr Context { std::make_shared<VulkanContext>() };
    bool IsInitialized { false };

};


} // namespace rhi