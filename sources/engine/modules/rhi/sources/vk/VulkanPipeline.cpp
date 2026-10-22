#include "VulkanPipeline.hpp"

#include "VulkanDevice.hpp"
#include "VulkanRHI.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace rhi
{
namespace
{
constexpr uint64_t HashOffset = 14695981039346656037ull;
constexpr uint64_t HashPrime = 1099511628211ull;

void hashBytes(uint64_t& InOutHash, const void* Data, size_t Size)
{
    const auto* Bytes = static_cast<const std::byte*>(Data);
    for (size_t Index = 0; Index < Size; ++Index)
    {
        InOutHash ^= static_cast<uint64_t>(std::to_integer<uint8_t>(Bytes[Index]));
        InOutHash *= HashPrime;
    }
}

template <typename ValueType>
void hashValue(uint64_t& InOutHash, const ValueType& Value)
{
    static_assert(std::is_trivially_copyable_v<ValueType>);
    hashBytes(InOutHash, &Value, sizeof(Value));
}

template <typename ValueType>
void appendKey(std::vector<std::byte>& InOutKey, const ValueType& Value)
{
    static_assert(std::is_trivially_copyable_v<ValueType>);
    const auto* Begin = reinterpret_cast<const std::byte*>(&Value);
    InOutKey.insert(InOutKey.end(), Begin, Begin + sizeof(Value));
}

uint64_t calculateShaderHash(const ShaderDescriptor& Desc)
{
    uint64_t Hash = HashOffset;
    hashValue(Hash, Desc.Stage);
    hashBytes(Hash, Desc.EntryPoint.data(), Desc.EntryPoint.size());
    if (!Desc.ByteCode.empty())
        hashBytes(Hash, Desc.ByteCode.data(), Desc.ByteCode.size());
    return Hash;
}

vk::ShaderStageFlagBits toVk(EShaderStage_t Stage)
{
    switch (Stage)
    {
    case EShaderStage_t::Vertex: return vk::ShaderStageFlagBits::eVertex;
    case EShaderStage_t::Pixel: return vk::ShaderStageFlagBits::eFragment;
    case EShaderStage_t::Compute: return vk::ShaderStageFlagBits::eCompute;
    case EShaderStage_t::Geometry: return vk::ShaderStageFlagBits::eGeometry;
    case EShaderStage_t::Hull: return vk::ShaderStageFlagBits::eTessellationControl;
    case EShaderStage_t::Domain: return vk::ShaderStageFlagBits::eTessellationEvaluation;
#ifdef VK_EXT_mesh_shader
    case EShaderStage_t::Mesh: return vk::ShaderStageFlagBits::eMeshEXT;
    case EShaderStage_t::Amplification:
    case EShaderStage_t::Task: return vk::ShaderStageFlagBits::eTaskEXT;
#else
    case EShaderStage_t::Mesh:
    case EShaderStage_t::Amplification:
    case EShaderStage_t::Task:
        throw std::invalid_argument("Mesh and task shaders are not available in this Vulkan build.");
#endif
    default:
        throw std::invalid_argument("Unsupported shader stage.");
    }
}

vk::Format toVk(EVertexFormat Format)
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
    default: throw std::invalid_argument("Unsupported vertex format.");
    }
}

vk::FrontFace toVk(EFrontFace Face)
{
    return Face == EFrontFace::Clockwise
        ? vk::FrontFace::eClockwise
        : vk::FrontFace::eCounterClockwise;
}

vk::VertexInputRate toVk(EVertexInputRate Rate)
{
    return Rate == EVertexInputRate::PerInstance
        ? vk::VertexInputRate::eInstance
        : vk::VertexInputRate::eVertex;
}

vk::ColorComponentFlags toVk(EColorWriteMask Mask)
{
    vk::ColorComponentFlags Result;
    if (Mask.has(EColorWriteMask_t::R)) Result |= vk::ColorComponentFlagBits::eR;
    if (Mask.has(EColorWriteMask_t::G)) Result |= vk::ColorComponentFlagBits::eG;
    if (Mask.has(EColorWriteMask_t::B)) Result |= vk::ColorComponentFlagBits::eB;
    if (Mask.has(EColorWriteMask_t::A)) Result |= vk::ColorComponentFlagBits::eA;
    return Result;
}

vk::StencilOpState toVk(const StencilFaceState& State)
{
    return vk::StencilOpState(
        toVk(State.FailOp),
        toVk(State.PassOp),
        toVk(State.DepthFailOp),
        toVk(State.CompareOp),
        State.CompareMask,
        State.WriteMask,
        State.Reference);
}

vk::DynamicState toVk(EDynamicState_t State)
{
    switch (State)
    {
    case EDynamicState_t::Viewport: return vk::DynamicState::eViewport;
    case EDynamicState_t::Scissor: return vk::DynamicState::eScissor;
    case EDynamicState_t::BlendConstants: return vk::DynamicState::eBlendConstants;
    case EDynamicState_t::StencilReference: return vk::DynamicState::eStencilReference;
    case EDynamicState_t::DepthBias: return vk::DynamicState::eDepthBias;
    case EDynamicState_t::LineWidth: return vk::DynamicState::eLineWidth;
    // VK_EXT_extended_dynamic_state / Vulkan 1.3 command-state mappings.
    case EDynamicState_t::CullMode: return vk::DynamicState::eCullMode;
    case EDynamicState_t::FrontFace: return vk::DynamicState::eFrontFace;
    case EDynamicState_t::PrimitiveTopology: return vk::DynamicState::ePrimitiveTopology;
    case EDynamicState_t::DepthTestEnable: return vk::DynamicState::eDepthTestEnable;
    case EDynamicState_t::DepthWriteEnable: return vk::DynamicState::eDepthWriteEnable;
    case EDynamicState_t::DepthCompareOp: return vk::DynamicState::eDepthCompareOp;
    case EDynamicState_t::StencilTestEnable: return vk::DynamicState::eStencilTestEnable;
    case EDynamicState_t::StencilOperations: return vk::DynamicState::eStencilOp;
    case EDynamicState_t::StencilCompareMask: return vk::DynamicState::eStencilCompareMask;
    case EDynamicState_t::StencilWriteMask: return vk::DynamicState::eStencilWriteMask;
    // VK_EXT_vertex_input_dynamic_state supplies vkCmdSetVertexInputEXT.
    case EDynamicState_t::VertexInput: return vk::DynamicState::eVertexInputEXT;
    default:
        throw std::invalid_argument(
            "The requested extended dynamic state is not enabled by this Vulkan backend.");
    }
}

// StageStorage 是 Vulkan Pipeline 创建期间使用的临时所有权容器
// 如果只返回一个 vk::PipelineShaderStageCreateInfo, 那么特化常量对应的数组和字节数据可能已经在函数返回时被销毁, 使其中指针悬空
struct StageStorage
{
    /**
    * Shader 中: [[vk::constant_id(0)]] const uint SampleCount = 4;
    *            [[vk::constant_id(1)]] const float Exposure = 1.0;
    * vk::SpecializationMapEntry {
    *     constantID, -> 0 -> 1
    *     offset,     -> 0 -> 4
    *     size        -> 4 -> 4
    * }
    */
    std::vector<vk::SpecializationMapEntry> MapEntries;
    /**
    * 所有 Specialization Constant 原始值拼接后的连续字节数组
    * [SampleCount 的 4 字节][Exposure 的 4 字节]
    */
    std::vector<std::byte> Data;

    /**
     *	Specialization Constant 与 Push Constant 不同：
     *	- Specialization Constant 在 Pipeline 创建阶段确定，可能参与驱动优化；
     *	- Push Constant 在 命令记录阶段更新；
     *	- Specialization Constant 的变化通常会产生不同 Pipeline；
     *	- Push Constant 的值不产生新 Pipeline. 
     *
     *  vk::SpecializationInfo 本身是 Vulkan 对特化常量的整体描述
    */
    vk::SpecializationInfo SpecializationInfo;
    vk::PipelineShaderStageCreateInfo CreateInfo;
    std::shared_ptr<VulkanShader> Shader;
    EShaderStage_t Stage { EShaderStage_t::Vertex };
    bool Finalized { false };

    /** 第一阶段只收集并拥有数据，不创建任何指向成员的 Vulkan 指针视图.  */
    static StageStorage collect(
        const PipelineShaderStage& InStage,
        EShaderStage_t ExpectedStage,
        VulkanDevice& Device)
    {
        auto Shader = std::dynamic_pointer_cast<VulkanShader>(InStage.Shader);
        if (!Shader || &Shader->getDevice() != &Device)
            throw std::invalid_argument("Pipeline shader belongs to another backend or device.");
        const bool TaskAlias = ExpectedStage == EShaderStage_t::Task &&
            Shader->getStage() == EShaderStage_t::Amplification;
        if (Shader->getStage() != ExpectedStage && !TaskAlias)
            throw std::invalid_argument("Pipeline shader stage does not match its descriptor slot.");

        StageStorage Result;
        Result.Shader = std::move(Shader);
        Result.Stage = ExpectedStage;
        std::unordered_set<uint32_t> Ids;
        for (const auto& Constant : InStage.SpecializationConstants)
        {
            if (Constant.Data.empty() || !Ids.emplace(Constant.Id).second)
                throw std::invalid_argument(
                    "Specialization constants require unique ids and non-empty data.");
            const uint32_t Offset = static_cast<uint32_t>(Result.Data.size());
            Result.Data.insert(Result.Data.end(), Constant.Data.begin(), Constant.Data.end());
            Result.MapEntries.emplace_back(Constant.Id, Offset, Constant.Data.size());
        }
        return Result;
    }

    /**
     * 第二阶段只能在最终容器完成扩容后执行. 此时才绑定指向 owning 成员的
     * pMapEntries, pData, pName 和 pSpecializationInfo. 
     */
    void finalize()
    {
        if (!Shader)
            throw std::logic_error("Cannot finalize an empty shader stage.");
        SpecializationInfo = vk::SpecializationInfo(
            static_cast<uint32_t>(MapEntries.size()),
            MapEntries.data(),
            Data.size(),
            Data.data());
        CreateInfo = vk::PipelineShaderStageCreateInfo(
            {},
            toVk(Stage),
            Shader->getVkShaderModule(),
            Shader->getEntryPoint().c_str(),
            MapEntries.empty() ? nullptr : &SpecializationInfo);
        Finalized = true;
    }

    [[nodiscard]] const vk::PipelineShaderStageCreateInfo& getCreateInfo() const
    {
        if (!Finalized)
            throw std::logic_error("Shader stage storage must be finalized before use.");
        return CreateInfo;
    }
};

void validateRenderingSignature(const RenderingSignature& Signature, const DeviceLimits& Limits)
{
    if (Signature.ColorAttachmentCount > MaxColorAttachments ||
        Signature.ColorAttachmentCount > Limits.MaxColorAttachments)
    {
        throw std::invalid_argument("Pipeline rendering signature has too many color attachments.");
    }
    for (uint32_t Index = 0; Index < MaxColorAttachments; ++Index)
    {
        const bool Used = Index < Signature.ColorAttachmentCount;
        if (Used == (Signature.ColorFormats[Index] == EFormat::Undefined))
            throw std::invalid_argument("Pipeline color formats are not normalized.");
        if (Used && isDepthFormat(Signature.ColorFormats[Index]))
            throw std::invalid_argument("Pipeline color attachment uses a depth/stencil format.");
    }
    if (Signature.DepthFormat != EFormat::Undefined && !hasDepthAspect(Signature.DepthFormat))
        throw std::invalid_argument("Pipeline depth format has no depth aspect.");
    if (Signature.StencilFormat != EFormat::Undefined && !hasStencilAspect(Signature.StencilFormat))
        throw std::invalid_argument("Pipeline stencil format has no stencil aspect.");
}

VulkanPipelineLayout& requireLayout(
    const std::shared_ptr<RPipelineLayout>& Layout,
    VulkanDevice& Device)
{
    auto* Result = dynamic_cast<VulkanPipelineLayout*>(Layout.get());
    if (!Result || &Result->getDevice() != &Device || !Result->isValid())
        throw std::invalid_argument("Pipeline layout belongs to another backend or device.");
    return *Result;
}

VulkanPipelineCache* getCache(
    const std::shared_ptr<RPipelineCache>& Cache,
    VulkanDevice& Device)
{
    if (!Cache)
        return nullptr;
    auto* Result = dynamic_cast<VulkanPipelineCache*>(Cache.get());
    if (!Result || &Result->getDevice() != &Device || !Result->isValid())
        throw std::invalid_argument("Pipeline cache belongs to another backend or device.");
    return Result;
}

vk::PipelineCreateFlags getPipelineFlags(EPipelineCompileFlags Flags)
{
    vk::PipelineCreateFlags Result;
#ifdef VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
    if (Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded))
        result |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
#else
    if (Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded))
        throw std::invalid_argument("FailIfCompileNeeded is unavailable in this Vulkan build.");
#endif
    if (!Flags.has(EPipelineCompileFlag_t::Optimize))
        Result |= vk::PipelineCreateFlagBits::eDisableOptimization;
    return Result;
}

uint64_t calculateGraphicsPipelineHash(const GraphicsPipelineDescriptor& Desc)
{
    uint64_t Hash = HashOffset;
    const uint64_t LayoutHash = Desc.Layout->getCompatibilityHash();
    hashValue(Hash, LayoutHash);
    for (const auto& [stage, expected_stage] : std::array {
        std::pair { &Desc.Vertex, EShaderStage_t::Vertex },
        std::pair { &Desc.Pixel, EShaderStage_t::Pixel },
        std::pair { &Desc.Geometry, EShaderStage_t::Geometry },
        std::pair { &Desc.Hull, EShaderStage_t::Hull },
        std::pair { &Desc.Domain, EShaderStage_t::Domain },
        std::pair { &Desc.Task, EShaderStage_t::Task },
        std::pair { &Desc.Mesh, EShaderStage_t::Mesh } })
    {
        const uint64_t ShaderHash = stage->Shader ? stage->Shader->getContentHash() : 0;
        hashValue(Hash, ShaderHash);
        hashValue(Hash, expected_stage);
        if (stage->Shader)
            hashBytes(Hash, stage->Shader->getEntryPoint().data(), stage->Shader->getEntryPoint().size());
        auto Constants = stage->SpecializationConstants;
        std::ranges::sort(Constants, {}, &SpecializationConstant::Id);
        for (const auto& Constant : Constants)
        {
            hashValue(Hash, Constant.Id);
            hashBytes(Hash, Constant.Data.data(), Constant.Data.size());
        }
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::VertexInput))
    {
        auto Buffers = Desc.VertexInput.Buffers;
        auto Attributes = Desc.VertexInput.Attributes;
        std::ranges::sort(Buffers, {}, &VertexBufferLayout::Binding);
        std::ranges::sort(Attributes, {}, &VertexAttribute::Location);
        for (const auto& Buffer : Buffers)
        {
            hashValue(Hash, Buffer.Binding);
            hashValue(Hash, Buffer.Stride);
            hashValue(Hash, Buffer.InputRate);
        }
        for (const auto& Attribute : Attributes)
        {
            hashValue(Hash, Attribute.Location);
            hashValue(Hash, Attribute.Binding);
            hashValue(Hash, Attribute.Format);
            hashValue(Hash, Attribute.Offset);
        }
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::PrimitiveTopology))
        hashValue(Hash, Desc.InputAssembly.Topology);
    hashValue(Hash, Desc.InputAssembly.PrimitiveRestartEnable);
    hashValue(Hash, Desc.InputAssembly.PatchControlPoints);
    hashValue(Hash, Desc.Rasterizer.FillMode);
    if (!Desc.DynamicStates.has(EDynamicState_t::CullMode))
        hashValue(Hash, Desc.Rasterizer.CullMode);
    if (!Desc.DynamicStates.has(EDynamicState_t::FrontFace))
        hashValue(Hash, Desc.Rasterizer.FrontFace);
    hashValue(Hash, Desc.Rasterizer.DepthClampEnable);
    hashValue(Hash, Desc.Rasterizer.RasterizerDiscardEnable);
    hashValue(Hash, Desc.Rasterizer.DepthBiasEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthBias))
    {
        hashValue(Hash, Desc.Rasterizer.DepthBiasConstantFactor);
        hashValue(Hash, Desc.Rasterizer.DepthBiasClamp);
        hashValue(Hash, Desc.Rasterizer.DepthBiasSlopeFactor);
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::LineWidth))
        hashValue(Hash, Desc.Rasterizer.LineWidth);
    hashValue(Hash, Desc.Multisample.SampleShadingEnable);
    hashValue(Hash, Desc.Multisample.MinSampleShading);
    hashValue(Hash, Desc.Multisample.SampleMask);
    hashValue(Hash, Desc.Multisample.AlphaToCoverageEnable);
    hashValue(Hash, Desc.Multisample.AlphaToOneEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthTestEnable))
        hashValue(Hash, Desc.DepthStencil.DepthTestEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthWriteEnable))
        hashValue(Hash, Desc.DepthStencil.DepthWriteEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthCompareOp))
        hashValue(Hash, Desc.DepthStencil.DepthCompareOp);
    hashValue(Hash, Desc.DepthStencil.DepthBoundsTestEnable);
    hashValue(Hash, Desc.DepthStencil.MinDepthBounds);
    hashValue(Hash, Desc.DepthStencil.MaxDepthBounds);
    if (!Desc.DynamicStates.has(EDynamicState_t::StencilTestEnable))
        hashValue(Hash, Desc.DepthStencil.StencilTestEnable);
    for (const auto* Stencil : { &Desc.DepthStencil.Front, &Desc.DepthStencil.Back })
    {
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilOperations))
        {
            hashValue(Hash, Stencil->FailOp);
            hashValue(Hash, Stencil->PassOp);
            hashValue(Hash, Stencil->DepthFailOp);
            hashValue(Hash, Stencil->CompareOp);
        }
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilCompareMask))
            hashValue(Hash, Stencil->CompareMask);
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilWriteMask))
            hashValue(Hash, Stencil->WriteMask);
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilReference))
            hashValue(Hash, Stencil->Reference);
    }
    for (uint32_t Index = 0; Index < Desc.Rendering.ColorAttachmentCount; ++Index)
    {
        const auto& Attachment = Desc.Blend.Attachments[Index];
        hashValue(Hash, Attachment.BlendEnable);
        hashValue(Hash, Attachment.Color.SrcFactor);
        hashValue(Hash, Attachment.Color.DstFactor);
        hashValue(Hash, Attachment.Color.Operation);
        hashValue(Hash, Attachment.Alpha.SrcFactor);
        hashValue(Hash, Attachment.Alpha.DstFactor);
        hashValue(Hash, Attachment.Alpha.Operation);
        hashValue(Hash, Attachment.WriteMask.Value);
        hashValue(Hash, Desc.Rendering.ColorFormats[Index]);
    }
    hashValue(Hash, Desc.Rendering.ColorAttachmentCount);
    hashValue(Hash, Desc.Rendering.DepthFormat);
    hashValue(Hash, Desc.Rendering.StencilFormat);
    hashValue(Hash, Desc.Rendering.SampleCount);
    hashValue(Hash, Desc.Rendering.ViewMask);
    hashValue(Hash, Desc.DynamicStates.Value);
    return Hash;
}

uint64_t calculateComputePipelineHash(const ComputePipelineDescriptor& Desc)
{
    uint64_t Hash = HashOffset;
    const uint64_t LayoutHash = Desc.Layout->getCompatibilityHash();
    const uint64_t ShaderHash = Desc.Compute.Shader->getContentHash();
    hashValue(Hash, LayoutHash);
    hashValue(Hash, ShaderHash);
    hashValue(Hash, EShaderStage_t::Compute);
    hashBytes(
        Hash,
        Desc.Compute.Shader->getEntryPoint().data(),
        Desc.Compute.Shader->getEntryPoint().size());
    auto Constants = Desc.Compute.SpecializationConstants;
    std::ranges::sort(Constants, {}, &SpecializationConstant::Id);
    for (const auto& Constant : Constants)
    {
        hashValue(Hash, Constant.Id);
        hashBytes(Hash, Constant.Data.data(), Constant.Data.size());
    }
    return Hash;
}

struct GraphicsPipelineBuildState
{
    std::vector<StageStorage> StageStorageList;
    std::vector<vk::PipelineShaderStageCreateInfo> ShaderStages;
    std::vector<vk::VertexInputBindingDescription> VertexBindings;
    std::vector<vk::VertexInputAttributeDescription> VertexAttributes;
    vk::PipelineVertexInputStateCreateInfo VertexInput;
    vk::PipelineInputAssemblyStateCreateInfo InputAssembly;
    vk::PipelineTessellationStateCreateInfo Tessellation;
    vk::PipelineViewportStateCreateInfo Viewport;
    vk::PipelineRasterizationStateCreateInfo Rasterizer;
    std::array<vk::SampleMask, 2> SampleMask {};
    vk::PipelineMultisampleStateCreateInfo Multisample;
    vk::PipelineDepthStencilStateCreateInfo DepthStencil;
    std::vector<vk::PipelineColorBlendAttachmentState> BlendAttachments;
    vk::PipelineColorBlendStateCreateInfo ColorBlend;
    std::vector<vk::DynamicState> DynamicStates;
    vk::PipelineDynamicStateCreateInfo DynamicState;
    std::array<vk::Format, MaxColorAttachments> ColorFormats {};
    vk::PipelineRenderingCreateInfo Rendering;
};

void validateGraphicsPipelineDescriptor(
    const GraphicsPipelineDescriptor& Desc,
    VulkanDevice& Device)
{
    validateRenderingSignature(Desc.Rendering, Device.getLimits());
    const bool MeshPath = static_cast<bool>(Desc.Mesh.Shader);
    const bool VertexPath = static_cast<bool>(Desc.Vertex.Shader);
    if (MeshPath == VertexPath)
        throw std::invalid_argument("Graphics pipeline requires exactly one vertex or mesh shader path.");
    if (Desc.Task.Shader && !MeshPath)
        throw std::invalid_argument("Task shader requires a mesh shader.");
    if (MeshPath && (Desc.Geometry.Shader || Desc.Hull.Shader || Desc.Domain.Shader))
        throw std::invalid_argument("Mesh shader path cannot use geometry or tessellation shaders.");
    if (MeshPath && (!Desc.VertexInput.Buffers.empty() || !Desc.VertexInput.Attributes.empty()))
        throw std::invalid_argument("Mesh shader path cannot use traditional vertex input.");
    if (MeshPath && Desc.DynamicStates.has(EDynamicState_t::VertexInput))
        throw std::invalid_argument("Mesh shader pipelines cannot use dynamic vertex input.");
    if (MeshPath && !Device.getFeatures().MeshShader)
        throw std::invalid_argument("Mesh shaders are unsupported by this device.");
    if (Desc.Task.Shader && !Device.getFeatures().TaskShader)
        throw std::invalid_argument("Task shaders are unsupported by this device.");
    if (!Desc.Pixel.Shader && Desc.Rendering.ColorAttachmentCount != 0)
        throw std::invalid_argument("Graphics pipeline with color attachments requires a pixel shader.");
    if ((Desc.Hull.Shader == nullptr) != (Desc.Domain.Shader == nullptr))
        throw std::invalid_argument("Hull and domain shaders must be provided together.");
    if (Desc.Geometry.Shader && !Device.getFeatures().GeometryShader)
        throw std::invalid_argument("Geometry shaders are unsupported by this device.");
    if (Desc.Hull.Shader && !Device.getFeatures().TessellationShader)
        throw std::invalid_argument("Tessellation shaders are unsupported by this device.");
    if (!MeshPath && Desc.InputAssembly.Topology == EPrimitiveTopology::PatchList && !Desc.Hull.Shader)
        throw std::invalid_argument("Patch topology requires hull and domain shaders.");
    if (!MeshPath && Desc.InputAssembly.Topology != EPrimitiveTopology::PatchList && Desc.Hull.Shader)
        throw std::invalid_argument("Tessellation shaders require patch topology.");
    if (Desc.Hull.Shader && Desc.InputAssembly.PatchControlPoints == 0)
        throw std::invalid_argument("Tessellation pipeline requires patch control points.");
    if (Desc.Rendering.ViewMask != 0 && !Device.getFeatures().Multiview)
        throw std::invalid_argument("Multiview graphics pipeline is unsupported by this device.");
    if (!Desc.DynamicStates.has(EDynamicState_t::Viewport) ||
        !Desc.DynamicStates.has(EDynamicState_t::Scissor))
    {
        throw std::invalid_argument(
            "Viewport and scissor must be dynamic because the descriptor contains no static values.");
    }
    constexpr uint64_t KnownDynamicStates = (1ull << 17) - 1;
    if ((Desc.DynamicStates.Value & ~KnownDynamicStates) != 0)
        throw std::invalid_argument("Pipeline requests an unknown dynamic state bit.");
    const auto& Features = Device.getFeatures();
    const bool NeedsExtendedDynamicState =
        Desc.DynamicStates.has(EDynamicState_t::CullMode) ||
        Desc.DynamicStates.has(EDynamicState_t::FrontFace) ||
        Desc.DynamicStates.has(EDynamicState_t::PrimitiveTopology) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthTestEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthWriteEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthCompareOp) ||
        Desc.DynamicStates.has(EDynamicState_t::StencilTestEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::StencilOperations);
    if (NeedsExtendedDynamicState && !Features.ExtendedDynamicState)
        throw std::invalid_argument("Pipeline requests VK_EXT_extended_dynamic_state without device support.");
    if (Desc.DynamicStates.has(EDynamicState_t::VertexInput) && !Features.DynamicVertexInput)
        throw std::invalid_argument("Pipeline requests VK_EXT_vertex_input_dynamic_state without device support.");
    if ((Desc.DepthStencil.DepthTestEnable || Desc.DepthStencil.DepthWriteEnable) &&
        Desc.Rendering.DepthFormat == EFormat::Undefined)
        throw std::invalid_argument("Depth testing or writing requires a depth attachment format.");
    if (Desc.Rasterizer.FillMode != EFillMode::Solid && !Device.getFeatures().FillModeNonSolid)
        throw std::invalid_argument("Non-solid rasterization is unsupported by this device.");
    if (Desc.Rasterizer.DepthClampEnable && !Device.getFeatures().DepthClamp)
        throw std::invalid_argument("Depth clamp is unsupported by this device.");
    if (Desc.DepthStencil.DepthBoundsTestEnable && !Device.getFeatures().DepthBounds)
        throw std::invalid_argument("Depth bounds testing is unsupported by this device.");
    if (Desc.Multisample.SampleShadingEnable && !Device.getFeatures().SampleRateShading)
        throw std::invalid_argument("Sample-rate shading is unsupported by this device.");
    if (Desc.Multisample.AlphaToOneEnable && !Device.getFeatures().AlphaToOne)
        throw std::invalid_argument("Alpha-to-one is unsupported by this device.");
    if (!Desc.DynamicStates.has(EDynamicState_t::LineWidth) &&
        Desc.Rasterizer.LineWidth != 1.0f && !Device.getFeatures().WideLines)
        throw std::invalid_argument("Wide lines are unsupported by this device.");
    if (!Device.getFeatures().IndependentBlend && Desc.Rendering.ColorAttachmentCount > 1)
    {
        const auto& First = Desc.Blend.Attachments[0];
        for (uint32_t Index = 1; Index < Desc.Rendering.ColorAttachmentCount; ++Index)
            if (First != Desc.Blend.Attachments[Index])
                throw std::invalid_argument("Independent attachment blending is unsupported by this device.");
    }
#if !RHI_ENABLE_PIPELINE_STATISTICS
    if (Desc.Compile.Flags.has(EPipelineCompileFlag_t::CaptureStatistics))
        throw std::invalid_argument("Pipeline statistics were disabled at build time.");
#endif
}

void buildShaderStages(
    const GraphicsPipelineDescriptor& Desc,
    VulkanDevice& Device,
    GraphicsPipelineBuildState& OutState)
{
    const std::array Stages {
        std::pair { &Desc.Vertex, EShaderStage_t::Vertex },
        std::pair { &Desc.Pixel, EShaderStage_t::Pixel },
        std::pair { &Desc.Geometry, EShaderStage_t::Geometry },
        std::pair { &Desc.Hull, EShaderStage_t::Hull },
        std::pair { &Desc.Domain, EShaderStage_t::Domain },
        std::pair { &Desc.Task, EShaderStage_t::Task },
        std::pair { &Desc.Mesh, EShaderStage_t::Mesh }
    };
    const size_t StageCount = std::ranges::count_if(Stages, [](const auto& Stage)
    {
        return static_cast<bool>(Stage.first->Shader);
    });
    OutState.StageStorageList.reserve(StageCount);
    for (const auto& [stage, expected_stage] : Stages)
        if (stage->Shader)
            OutState.StageStorageList.emplace_back(
                StageStorage::collect(*stage, expected_stage, Device));

    // StageStorageList 的容量从此不再变化，finalize() 建立的所有内部指针保持稳定. 
    OutState.ShaderStages.reserve(StageCount);
    for (auto& Storage : OutState.StageStorageList)
    {
        Storage.finalize();
        OutState.ShaderStages.emplace_back(Storage.getCreateInfo());
    }
}

void buildVertexInputState(
    const GraphicsPipelineDescriptor& Desc,
    VulkanDevice& Device,
    GraphicsPipelineBuildState& OutState)
{
    if (Desc.Mesh.Shader)
        return;
    if (Desc.VertexInput.Buffers.size() > Device.getLimits().MaxVertexInputBindings ||
        Desc.VertexInput.Attributes.size() > Device.getLimits().MaxVertexInputAttributes)
        throw std::invalid_argument("Vertex input exceeds device limits.");
    std::unordered_set<uint32_t> Bindings;
    for (const auto& Binding : Desc.VertexInput.Buffers)
    {
        if (Binding.Stride == 0 || !Bindings.emplace(Binding.Binding).second)
            throw std::invalid_argument(
                "Vertex bindings require a non-zero stride and unique binding index.");
        OutState.VertexBindings.emplace_back(Binding.Binding, Binding.Stride, toVk(Binding.InputRate));
    }
    std::unordered_set<uint32_t> Locations;
    for (const auto& Attribute : Desc.VertexInput.Attributes)
    {
        if (!Bindings.contains(Attribute.Binding) || !Locations.emplace(Attribute.Location).second)
            throw std::invalid_argument(
                "Vertex attributes require an existing binding and unique location.");
        OutState.VertexAttributes.emplace_back(
            Attribute.Location, Attribute.Binding, toVk(Attribute.Format), Attribute.Offset);
    }
    OutState.VertexInput = vk::PipelineVertexInputStateCreateInfo(
        {}, OutState.VertexBindings, OutState.VertexAttributes);
    OutState.InputAssembly = vk::PipelineInputAssemblyStateCreateInfo(
        {}, toVk(Desc.InputAssembly.Topology), Desc.InputAssembly.PrimitiveRestartEnable);
    OutState.Tessellation = vk::PipelineTessellationStateCreateInfo(
        {}, Desc.InputAssembly.PatchControlPoints);
}

void buildFixedFunctionState(
    const GraphicsPipelineDescriptor& Desc,
    GraphicsPipelineBuildState& OutState)
{
    OutState.Viewport = vk::PipelineViewportStateCreateInfo({}, 1, nullptr, 1, nullptr);
    OutState.Rasterizer = vk::PipelineRasterizationStateCreateInfo(
        {}, Desc.Rasterizer.DepthClampEnable, Desc.Rasterizer.RasterizerDiscardEnable,
        toVk(Desc.Rasterizer.FillMode), toVk(Desc.Rasterizer.CullMode),
        toVk(Desc.Rasterizer.FrontFace), Desc.Rasterizer.DepthBiasEnable,
        Desc.Rasterizer.DepthBiasConstantFactor, Desc.Rasterizer.DepthBiasClamp,
        Desc.Rasterizer.DepthBiasSlopeFactor, Desc.Rasterizer.LineWidth);
    OutState.SampleMask = {
        static_cast<vk::SampleMask>(Desc.Multisample.SampleMask),
        static_cast<vk::SampleMask>(Desc.Multisample.SampleMask >> 32)
    };
    OutState.Multisample = vk::PipelineMultisampleStateCreateInfo(
        {}, toVk(Desc.Rendering.SampleCount), Desc.Multisample.SampleShadingEnable,
        Desc.Multisample.MinSampleShading, OutState.SampleMask.data(),
        Desc.Multisample.AlphaToCoverageEnable, Desc.Multisample.AlphaToOneEnable);
    OutState.DepthStencil = vk::PipelineDepthStencilStateCreateInfo(
        {}, Desc.DepthStencil.DepthTestEnable, Desc.DepthStencil.DepthWriteEnable,
        toVk(Desc.DepthStencil.DepthCompareOp), Desc.DepthStencil.DepthBoundsTestEnable,
        Desc.DepthStencil.StencilTestEnable, toVk(Desc.DepthStencil.Front),
        toVk(Desc.DepthStencil.Back), Desc.DepthStencil.MinDepthBounds,
        Desc.DepthStencil.MaxDepthBounds);
    OutState.BlendAttachments.reserve(Desc.Rendering.ColorAttachmentCount);
    for (uint32_t Index = 0; Index < Desc.Rendering.ColorAttachmentCount; ++Index)
    {
        const auto& State = Desc.Blend.Attachments[Index];
        OutState.BlendAttachments.emplace_back(
            State.BlendEnable, toVk(State.Color.SrcFactor), toVk(State.Color.DstFactor),
            toVk(State.Color.Operation), toVk(State.Alpha.SrcFactor),
            toVk(State.Alpha.DstFactor), toVk(State.Alpha.Operation), toVk(State.WriteMask));
    }
    OutState.ColorBlend = vk::PipelineColorBlendStateCreateInfo(
        {}, false, vk::LogicOp::eCopy, OutState.BlendAttachments);
}

void buildDynamicState(
    const GraphicsPipelineDescriptor& Desc,
    GraphicsPipelineBuildState& OutState)
{
    constexpr std::array Candidates {
        EDynamicState_t::Viewport, EDynamicState_t::Scissor,
        EDynamicState_t::BlendConstants, EDynamicState_t::StencilReference,
        EDynamicState_t::DepthBias, EDynamicState_t::LineWidth,
        EDynamicState_t::CullMode, EDynamicState_t::FrontFace,
        EDynamicState_t::PrimitiveTopology, EDynamicState_t::DepthTestEnable,
        EDynamicState_t::DepthWriteEnable, EDynamicState_t::DepthCompareOp,
        EDynamicState_t::StencilTestEnable, EDynamicState_t::StencilOperations,
        EDynamicState_t::StencilCompareMask, EDynamicState_t::StencilWriteMask,
        EDynamicState_t::VertexInput
    };
    for (const auto State : Candidates)
        if (Desc.DynamicStates.has(State)) OutState.DynamicStates.emplace_back(toVk(State));
    OutState.DynamicState = vk::PipelineDynamicStateCreateInfo({}, OutState.DynamicStates);
}

void buildRenderingState(
    const GraphicsPipelineDescriptor& Desc,
    GraphicsPipelineBuildState& OutState)
{
    for (uint32_t Index = 0; Index < Desc.Rendering.ColorAttachmentCount; ++Index)
        OutState.ColorFormats[Index] = toVk(Desc.Rendering.ColorFormats[Index]);
    OutState.Rendering = vk::PipelineRenderingCreateInfo(
        Desc.Rendering.ViewMask, Desc.Rendering.ColorAttachmentCount,
        OutState.ColorFormats.data(), toVk(Desc.Rendering.DepthFormat),
        toVk(Desc.Rendering.StencilFormat));
}

} // namespace

VulkanShader::VulkanShader(VulkanDevice& InDevice, const ShaderDescriptor& Desc)
    : Device(&InDevice)
    , Stage(Desc.Stage)
    , ContentHash(Desc.ContentHash != 0 ? Desc.ContentHash : calculateShaderHash(Desc))
    , EntryPoint(Desc.EntryPoint)
    , DebugName(Desc.DebugName)
{
    if (Desc.ByteCode.empty() || Desc.ByteCode.size() % sizeof(uint32_t) != 0)
        throw std::invalid_argument("SPIR-V bytecode must be non-empty and four-byte aligned in size.");
    if (EntryPoint.empty())
        throw std::invalid_argument("Shader entry point cannot be empty.");

    std::vector<uint32_t> Words(Desc.ByteCode.size() / sizeof(uint32_t));
    std::memcpy(Words.data(), Desc.ByteCode.data(), Desc.ByteCode.size());
    if (Words.front() != 0x07230203u)
        throw std::invalid_argument("Shader bytecode is not a SPIR-V module.");
    ShaderModule = Device->getVkDevice().createShaderModuleUnique(
        vk::ShaderModuleCreateInfo({}, Desc.ByteCode.size(), Words.data()));
}

RDevice& VulkanShader::getDevice() const noexcept { return *Device; }
void* VulkanShader::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkShaderModule>(ShaderModule.get()));
}

VulkanBindGroupLayout::VulkanBindGroupLayout(
    VulkanDevice& InDevice,
    const BindGroupLayoutDescriptor& Desc)
    : Device(&InDevice), Entries(Desc.Entries), DebugName(Desc.DebugName)
{
    std::ranges::sort(Entries, {}, &BindGroupLayoutEntry::Binding);
    std::vector<vk::DescriptorSetLayoutBinding> Bindings;
    std::vector<vk::DescriptorBindingFlags> BindingFlags;
    Bindings.reserve(Entries.size());
    BindingFlags.reserve(Entries.size());
    CompatibilityHash = HashOffset;
    uint32_t PreviousBinding = 0;
    bool HasPreviousBinding = false;
    bool UsesUpdateAfterBind = false;
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        const auto& Entry = Entries[Index];
        if (Entry.ArrayCount == 0 || !Entry.Visibility ||
            (HasPreviousBinding && Entry.Binding == PreviousBinding))
            throw std::invalid_argument("Bind group layout entries require unique bindings, visibility and array count.");
        if (Entry.Type == EDescriptorType::AccelerationStructure &&
            !Device->getFeatures().AccelerationStructure)
            throw std::invalid_argument("Acceleration-structure descriptors are unsupported by this device.");
        if (Entry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset) &&
            Entry.Type != EDescriptorType::UniformBuffer &&
            Entry.Type != EDescriptorType::ReadOnlyStorageBuffer &&
            Entry.Type != EDescriptorType::ReadWriteStorageBuffer)
        {
            throw std::invalid_argument("Dynamic offsets are only valid for buffer descriptors.");
        }
        if (Entry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound) &&
            !Device->getFeatures().PartiallyBoundDescriptors)
            throw std::invalid_argument("Partially-bound descriptors are unsupported by this device.");
        if (Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount) &&
            (!Device->getFeatures().VariableDescriptorCount || Index + 1 != Entries.size()))
        {
            throw std::invalid_argument(
                "A variable descriptor array must be supported and use the greatest binding number.");
        }
        if (Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount) &&
            Entry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset))
            throw std::invalid_argument("Variable descriptor arrays cannot use dynamic offsets.");
        if (Entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind) &&
            !Device->getFeatures().UpdateAfterBind)
            throw std::invalid_argument("Update-after-bind descriptors are unsupported by this device.");
        if (Entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind) &&
            (Entry.Type == EDescriptorType::InputAttachment ||
             Entry.Type == EDescriptorType::AccelerationStructure))
        {
            throw std::invalid_argument(
                "This backend does not support update-after-bind for this descriptor type.");
        }
        PreviousBinding = Entry.Binding;
        HasPreviousBinding = true;
        Bindings.emplace_back(
            Entry.Binding,
            toVk(Entry.Type, Entry.Flags),
            Entry.ArrayCount,
            toVk(Entry.Visibility));
        vk::DescriptorBindingFlags VkFlags;
        if (Entry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound))
            VkFlags |= vk::DescriptorBindingFlagBits::ePartiallyBound;
        if (Entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind))
        {
            VkFlags |= vk::DescriptorBindingFlagBits::eUpdateAfterBind;
            UsesUpdateAfterBind = true;
        }
        if (Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount))
            VkFlags |= vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
        BindingFlags.emplace_back(VkFlags);
        hashValue(CompatibilityHash, Entry.Binding);
        hashValue(CompatibilityHash, Entry.Type);
        hashValue(CompatibilityHash, Entry.ArrayCount);
        hashValue(CompatibilityHash, Entry.Visibility.Value);
        hashValue(CompatibilityHash, Entry.Flags.Value);
        appendKey(CompatibilityKey, Entry.Binding);
        appendKey(CompatibilityKey, Entry.Type);
        appendKey(CompatibilityKey, Entry.ArrayCount);
        appendKey(CompatibilityKey, Entry.Visibility.Value);
        appendKey(CompatibilityKey, Entry.Flags.Value);
    }
    vk::DescriptorSetLayoutBindingFlagsCreateInfo BindingFlagsInfo;
    BindingFlagsInfo.setBindingFlags(BindingFlags);
    const auto LayoutFlags = UsesUpdateAfterBind
        ? vk::DescriptorSetLayoutCreateFlags(vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool)
        : vk::DescriptorSetLayoutCreateFlags();
    vk::DescriptorSetLayoutCreateInfo CreateInfo(LayoutFlags, Bindings);
    if (!BindingFlags.empty())
        CreateInfo.pNext = &BindingFlagsInfo;
    DescriptorSetLayout = Device->getVkDevice().createDescriptorSetLayoutUnique(
        CreateInfo);
}

RDevice& VulkanBindGroupLayout::getDevice() const noexcept { return *Device; }
void* VulkanBindGroupLayout::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkDescriptorSetLayout>(DescriptorSetLayout.get()));
}

VulkanPipelineLayout::VulkanPipelineLayout(
    VulkanDevice& InDevice,
    const PipelineLayoutDescriptor& Desc)
    : Device(&InDevice)
    , DebugName(Desc.DebugName)
    , BindGroupLayouts(Desc.BindGroupLayouts)
    , PushConstantRanges(Desc.PushConstantRanges)
{
    if (BindGroupLayouts.size() > Device->getLimits().MaxBoundBindGroups)
        throw std::invalid_argument("Pipeline layout uses too many bind groups.");

    std::vector<vk::DescriptorSetLayout> Layouts;
    Layouts.reserve(BindGroupLayouts.size());
    CompatibilityHash = HashOffset;
    const uint64_t LayoutCount = BindGroupLayouts.size();
    appendKey(CompatibilityKey, LayoutCount);
    for (const auto& Layout : BindGroupLayouts)
    {
        auto* VkLayout = dynamic_cast<VulkanBindGroupLayout*>(Layout.get());
        if (!VkLayout || &VkLayout->getDevice() != Device || !VkLayout->isValid())
            throw std::invalid_argument("Bind group layout belongs to another backend or device.");
        Layouts.emplace_back(VkLayout->getVkDescriptorSetLayout());
        const uint64_t LayoutHash = VkLayout->getCompatibilityHash();
        hashValue(CompatibilityHash, LayoutHash);
        const auto LayoutKey = VkLayout->getCompatibilityKey();
        const uint64_t LayoutKeySize = LayoutKey.size();
        appendKey(CompatibilityKey, LayoutKeySize);
        CompatibilityKey.insert(CompatibilityKey.end(), LayoutKey.begin(), LayoutKey.end());
    }

    std::vector<PushConstantRange> Ranges = Desc.PushConstantRanges;
    std::ranges::sort(Ranges, {}, &PushConstantRange::Offset);
    const uint64_t RangeCount = Ranges.size();
    appendKey(CompatibilityKey, RangeCount);
    std::vector<vk::PushConstantRange> VkRanges;
    VkRanges.reserve(Ranges.size());
    uint32_t PreviousEnd = 0;
    for (const auto& Range : Ranges)
    {
        if (!Range.Stages || Range.Size == 0 || Range.Offset % 4 != 0 || Range.Size % 4 != 0 ||
            Range.Offset < PreviousEnd || Range.Size > Device->getLimits().MaxPushConstantSize ||
            Range.Offset > Device->getLimits().MaxPushConstantSize - Range.Size)
        {
            throw std::invalid_argument("Push constant ranges are invalid, overlapping or out of device limits.");
        }
        PreviousEnd = Range.Offset + Range.Size;
        VkRanges.emplace_back(toVk(Range.Stages), Range.Offset, Range.Size);
        hashValue(CompatibilityHash, Range.Stages.Value);
        hashValue(CompatibilityHash, Range.Offset);
        hashValue(CompatibilityHash, Range.Size);
        appendKey(CompatibilityKey, Range.Stages.Value);
        appendKey(CompatibilityKey, Range.Offset);
        appendKey(CompatibilityKey, Range.Size);
    }
    PipelineLayout = Device->getVkDevice().createPipelineLayoutUnique(
        vk::PipelineLayoutCreateInfo({}, Layouts, VkRanges));
}

RDevice& VulkanPipelineLayout::getDevice() const noexcept { return *Device; }
const std::shared_ptr<RBindGroupLayout>& VulkanPipelineLayout::getBindGroupLayout(
    uint32_t GroupIndex) const
{
    if (GroupIndex >= BindGroupLayouts.size())
        throw std::out_of_range("Bind group layout index is out of range.");
    return BindGroupLayouts[GroupIndex];
}
bool VulkanPipelineLayout::supportsPushConstants(
    EShaderStage Stages,
    uint32_t Offset,
    uint32_t Size) const noexcept
{
    if (!Stages || Size == 0 || Offset > std::numeric_limits<uint32_t>::max() - Size)
        return false;
    const uint32_t End = Offset + Size;
    return std::ranges::any_of(PushConstantRanges, [&](const PushConstantRange& Range)
    {
        return Offset >= Range.Offset && End <= Range.Offset + Range.Size &&
            !(Stages & ~Range.Stages);
    });
}

void* VulkanPipelineLayout::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkPipelineLayout>(PipelineLayout.get()));
}

VulkanPipelineCache::VulkanPipelineCache(
    VulkanDevice& InDevice,
    const PipelineCacheDescriptor& Desc)
    : Device(&InDevice), DebugName(Desc.DebugName)
{
    PipelineCache = Device->getVkDevice().createPipelineCacheUnique(
        vk::PipelineCacheCreateInfo({}, Desc.InitialData.size(), Desc.InitialData.data()));
}

void VulkanPipelineCache::merge(std::span<const std::shared_ptr<RPipelineCache>> Sources)
{
    std::vector<vk::PipelineCache> Caches;
    std::vector<std::mutex*> Mutexes { &Mutex };
    Caches.reserve(Sources.size());
    Mutexes.reserve(Sources.size() + 1);
    for (const auto& Source : Sources)
    {
        auto* Cache = dynamic_cast<VulkanPipelineCache*>(Source.get());
        if (!Cache || Cache == this || &Cache->getDevice() != Device || !Cache->isValid())
            throw std::invalid_argument("Pipeline cache merge source is invalid.");
        Caches.emplace_back(Cache->getVkPipelineCache());
        Mutexes.emplace_back(&Cache->getMutex());
    }
    std::ranges::sort(Mutexes);
    Mutexes.erase(std::ranges::unique(Mutexes).begin(), Mutexes.end());
    std::vector<std::unique_lock<std::mutex>> Locks;
    Locks.reserve(Mutexes.size());
    for (auto* Mutex : Mutexes)
        Locks.emplace_back(*Mutex);
    Device->getVkDevice().mergePipelineCaches(PipelineCache.get(), Caches);
}

RDevice& VulkanPipelineCache::getDevice() const noexcept { return *Device; }
std::vector<std::byte> VulkanPipelineCache::serialize() const
{
    std::scoped_lock Lock(Mutex);
    const auto Bytes = Device->getVkDevice().getPipelineCacheData(PipelineCache.get());
    std::vector<std::byte> Result(Bytes.size());
    std::memcpy(Result.data(), Bytes.data(), Bytes.size());
    return Result;
}

void* VulkanPipelineCache::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkPipelineCache>(PipelineCache.get()));
}

VulkanPipeline::VulkanPipeline(
    VulkanDevice& InDevice,
    EPipelineType InType,
    std::shared_ptr<RPipelineLayout> InLayout,
    RenderingSignature InRendering,
    EDynamicStates InDynamicStates,
    EPrimitiveTopology InPrimitiveTopology,
    bool InUsesMeshShaders,
    uint64_t InCacheKey,
    std::string InDebugName,
    vk::UniquePipeline InPipeline,
    uint32_t InRayTracingGroupCount)
    : Device(&InDevice)
    , Type(InType)
    , Layout(std::move(InLayout))
    , Rendering(InRendering)
    , DynamicStates(InDynamicStates)
    , PrimitiveTopology(InPrimitiveTopology)
    , UsesMeshShaders(InUsesMeshShaders)
    , CacheKey(InCacheKey)
    , DebugName(std::move(InDebugName))
    , Pipeline(std::move(InPipeline))
    , RayTracingGroupCount(InRayTracingGroupCount)
{
}

RDevice& VulkanPipeline::getDevice() const noexcept { return *Device; }
void* VulkanPipeline::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkPipeline>(Pipeline.get()));
}

std::shared_ptr<RPipeline> createVulkanGraphicsPipeline(
    VulkanDevice& Device,
    const GraphicsPipelineDescriptor& Desc)
{
    auto& Layout = requireLayout(Desc.Layout, Device);
    validateGraphicsPipelineDescriptor(Desc, Device);
    GraphicsPipelineBuildState State;
    buildShaderStages(Desc, Device, State);
    buildVertexInputState(Desc, Device, State);
    buildFixedFunctionState(Desc, State);
    buildDynamicState(Desc, State);

    vk::GraphicsPipelineCreateInfo CreateInfo;
    CreateInfo.setFlags(getPipelineFlags(Desc.Compile.Flags))
        .setStages(State.ShaderStages)
        .setPVertexInputState(Desc.Mesh.Shader ? nullptr : &State.VertexInput)
        .setPInputAssemblyState(Desc.Mesh.Shader ? nullptr : &State.InputAssembly)
        .setPTessellationState(Desc.Hull.Shader ? &State.Tessellation : nullptr)
        .setPViewportState(&State.Viewport)
        .setPRasterizationState(&State.Rasterizer)
        .setPMultisampleState(&State.Multisample)
        .setPDepthStencilState(&State.DepthStencil)
        .setPColorBlendState(&State.ColorBlend)
        .setPDynamicState(&State.DynamicState)
        .setLayout(Layout.getVkPipelineLayout());
    if (Device.getFeatures().DynamicRendering)
    {
        buildRenderingState(Desc, State);
        CreateInfo.setPNext(&State.Rendering);
    }
    else
    {
        // 1.0 基线: 管线绑定到与 RenderingSignature 兼容的 renderpass.
        CreateInfo.setRenderPass(Device.getOrCreateCompatibleRenderPass(Desc.Rendering));
    }

    auto* Cache = getCache(Desc.Compile.Cache, Device);
    std::unique_lock<std::mutex> CacheLock;
    if (Cache) CacheLock = std::unique_lock(Cache->getMutex());
    auto PipelineResult = Device.getVkDevice().createGraphicsPipelineUnique(
        Cache ? Cache->getVkPipelineCache() : vk::PipelineCache(), CreateInfo);
    return std::make_shared<VulkanPipeline>(
        Device,
        EPipelineType::Graphics,
        Desc.Layout,
        Desc.Rendering,
        Desc.DynamicStates,
        Desc.InputAssembly.Topology,
        static_cast<bool>(Desc.Mesh.Shader),
        calculateGraphicsPipelineHash(Desc),
        Desc.DebugName,
        std::move(PipelineResult.value));
}

std::shared_ptr<RPipeline> createVulkanComputePipeline(
    VulkanDevice& Device,
    const ComputePipelineDescriptor& Desc)
{
    auto& Layout = requireLayout(Desc.Layout, Device);
    if (!Desc.Compute.Shader)
        throw std::invalid_argument("Compute pipeline requires a compute shader.");
#if !RHI_ENABLE_PIPELINE_STATISTICS
    if (Desc.Compile.Flags.has(EPipelineCompileFlag_t::CaptureStatistics))
        throw std::invalid_argument("Pipeline statistics were disabled at build time.");
#endif
    StageStorage Stage = StageStorage::collect(Desc.Compute, EShaderStage_t::Compute, Device);
    Stage.finalize();
    vk::ComputePipelineCreateInfo CreateInfo(
        getPipelineFlags(Desc.Compile.Flags), Stage.getCreateInfo(), Layout.getVkPipelineLayout());
    auto* Cache = getCache(Desc.Compile.Cache, Device);
    std::unique_lock<std::mutex> CacheLock;
    if (Cache) CacheLock = std::unique_lock(Cache->getMutex());
    auto PipelineResult = Device.getVkDevice().createComputePipelineUnique(
        Cache ? Cache->getVkPipelineCache() : vk::PipelineCache(), CreateInfo);
    return std::make_shared<VulkanPipeline>(
        Device,
        EPipelineType::Compute,
        Desc.Layout,
        RenderingSignature {},
        EDynamicStates {},
        EPrimitiveTopology::TriangleList,
        false,
        calculateComputePipelineHash(Desc),
        Desc.DebugName,
        std::move(PipelineResult.value));
}

} // namespace rhi
