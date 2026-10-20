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
    const auto* bytes = static_cast<const std::byte*>(Data);
    for (size_t index = 0; index < Size; ++index)
    {
        InOutHash ^= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[index]));
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
    const auto* begin = reinterpret_cast<const std::byte*>(&Value);
    InOutKey.insert(InOutKey.end(), begin, begin + sizeof(Value));
}

uint64_t calculateShaderHash(const ShaderDescriptor& Desc)
{
    uint64_t hash = HashOffset;
    hashValue(hash, Desc.Stage);
    hashBytes(hash, Desc.EntryPoint.data(), Desc.EntryPoint.size());
    if (!Desc.ByteCode.empty())
        hashBytes(hash, Desc.ByteCode.data(), Desc.ByteCode.size());
    return hash;
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
    vk::ColorComponentFlags result;
    if (Mask.has(EColorWriteMask_t::R)) result |= vk::ColorComponentFlagBits::eR;
    if (Mask.has(EColorWriteMask_t::G)) result |= vk::ColorComponentFlagBits::eG;
    if (Mask.has(EColorWriteMask_t::B)) result |= vk::ColorComponentFlagBits::eB;
    if (Mask.has(EColorWriteMask_t::A)) result |= vk::ColorComponentFlagBits::eA;
    return result;
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
        auto shader = std::dynamic_pointer_cast<VulkanShader>(InStage.Shader);
        if (!shader || &shader->getDevice() != &Device)
            throw std::invalid_argument("Pipeline shader belongs to another backend or device.");
        const bool task_alias = ExpectedStage == EShaderStage_t::Task &&
            shader->getStage() == EShaderStage_t::Amplification;
        if (shader->getStage() != ExpectedStage && !task_alias)
            throw std::invalid_argument("Pipeline shader stage does not match its descriptor slot.");

        StageStorage result;
        result.Shader = std::move(shader);
        result.Stage = ExpectedStage;
        std::unordered_set<uint32_t> ids;
        for (const auto& constant : InStage.SpecializationConstants)
        {
            if (constant.Data.empty() || !ids.emplace(constant.Id).second)
                throw std::invalid_argument(
                    "Specialization constants require unique ids and non-empty data.");
            const uint32_t offset = static_cast<uint32_t>(result.Data.size());
            result.Data.insert(result.Data.end(), constant.Data.begin(), constant.Data.end());
            result.MapEntries.emplace_back(constant.Id, offset, constant.Data.size());
        }
        return result;
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
    for (uint32_t index = 0; index < MaxColorAttachments; ++index)
    {
        const bool used = index < Signature.ColorAttachmentCount;
        if (used == (Signature.ColorFormats[index] == EFormat::Undefined))
            throw std::invalid_argument("Pipeline color formats are not normalized.");
        if (used && isDepthFormat(Signature.ColorFormats[index]))
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
    auto* result = dynamic_cast<VulkanPipelineLayout*>(Layout.get());
    if (!result || &result->getDevice() != &Device || !result->isValid())
        throw std::invalid_argument("Pipeline layout belongs to another backend or device.");
    return *result;
}

VulkanPipelineCache* getCache(
    const std::shared_ptr<RPipelineCache>& Cache,
    VulkanDevice& Device)
{
    if (!Cache)
        return nullptr;
    auto* result = dynamic_cast<VulkanPipelineCache*>(Cache.get());
    if (!result || &result->getDevice() != &Device || !result->isValid())
        throw std::invalid_argument("Pipeline cache belongs to another backend or device.");
    return result;
}

vk::PipelineCreateFlags getPipelineFlags(EPipelineCompileFlags Flags)
{
    vk::PipelineCreateFlags result;
#ifdef VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
    if (Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded))
        result |= vk::PipelineCreateFlagBits::eFailOnPipelineCompileRequired;
#else
    if (Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded))
        throw std::invalid_argument("FailIfCompileNeeded is unavailable in this Vulkan build.");
#endif
    if (!Flags.has(EPipelineCompileFlag_t::Optimize))
        result |= vk::PipelineCreateFlagBits::eDisableOptimization;
    return result;
}

uint64_t calculateGraphicsPipelineHash(const GraphicsPipelineDescriptor& Desc)
{
    uint64_t hash = HashOffset;
    const uint64_t layout_hash = Desc.Layout->getCompatibilityHash();
    hashValue(hash, layout_hash);
    for (const auto& [stage, expected_stage] : std::array {
        std::pair { &Desc.Vertex, EShaderStage_t::Vertex },
        std::pair { &Desc.Pixel, EShaderStage_t::Pixel },
        std::pair { &Desc.Geometry, EShaderStage_t::Geometry },
        std::pair { &Desc.Hull, EShaderStage_t::Hull },
        std::pair { &Desc.Domain, EShaderStage_t::Domain },
        std::pair { &Desc.Task, EShaderStage_t::Task },
        std::pair { &Desc.Mesh, EShaderStage_t::Mesh } })
    {
        const uint64_t shader_hash = stage->Shader ? stage->Shader->getContentHash() : 0;
        hashValue(hash, shader_hash);
        hashValue(hash, expected_stage);
        if (stage->Shader)
            hashBytes(hash, stage->Shader->getEntryPoint().data(), stage->Shader->getEntryPoint().size());
        auto constants = stage->SpecializationConstants;
        std::ranges::sort(constants, {}, &SpecializationConstant::Id);
        for (const auto& constant : constants)
        {
            hashValue(hash, constant.Id);
            hashBytes(hash, constant.Data.data(), constant.Data.size());
        }
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::VertexInput))
    {
        auto buffers = Desc.VertexInput.Buffers;
        auto attributes = Desc.VertexInput.Attributes;
        std::ranges::sort(buffers, {}, &VertexBufferLayout::Binding);
        std::ranges::sort(attributes, {}, &VertexAttribute::Location);
        for (const auto& buffer : buffers)
        {
            hashValue(hash, buffer.Binding);
            hashValue(hash, buffer.Stride);
            hashValue(hash, buffer.InputRate);
        }
        for (const auto& attribute : attributes)
        {
            hashValue(hash, attribute.Location);
            hashValue(hash, attribute.Binding);
            hashValue(hash, attribute.Format);
            hashValue(hash, attribute.Offset);
        }
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::PrimitiveTopology))
        hashValue(hash, Desc.InputAssembly.Topology);
    hashValue(hash, Desc.InputAssembly.PrimitiveRestartEnable);
    hashValue(hash, Desc.InputAssembly.PatchControlPoints);
    hashValue(hash, Desc.Rasterizer.FillMode);
    if (!Desc.DynamicStates.has(EDynamicState_t::CullMode))
        hashValue(hash, Desc.Rasterizer.CullMode);
    if (!Desc.DynamicStates.has(EDynamicState_t::FrontFace))
        hashValue(hash, Desc.Rasterizer.FrontFace);
    hashValue(hash, Desc.Rasterizer.DepthClampEnable);
    hashValue(hash, Desc.Rasterizer.RasterizerDiscardEnable);
    hashValue(hash, Desc.Rasterizer.DepthBiasEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthBias))
    {
        hashValue(hash, Desc.Rasterizer.DepthBiasConstantFactor);
        hashValue(hash, Desc.Rasterizer.DepthBiasClamp);
        hashValue(hash, Desc.Rasterizer.DepthBiasSlopeFactor);
    }
    if (!Desc.DynamicStates.has(EDynamicState_t::LineWidth))
        hashValue(hash, Desc.Rasterizer.LineWidth);
    hashValue(hash, Desc.Multisample.SampleShadingEnable);
    hashValue(hash, Desc.Multisample.MinSampleShading);
    hashValue(hash, Desc.Multisample.SampleMask);
    hashValue(hash, Desc.Multisample.AlphaToCoverageEnable);
    hashValue(hash, Desc.Multisample.AlphaToOneEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthTestEnable))
        hashValue(hash, Desc.DepthStencil.DepthTestEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthWriteEnable))
        hashValue(hash, Desc.DepthStencil.DepthWriteEnable);
    if (!Desc.DynamicStates.has(EDynamicState_t::DepthCompareOp))
        hashValue(hash, Desc.DepthStencil.DepthCompareOp);
    hashValue(hash, Desc.DepthStencil.DepthBoundsTestEnable);
    hashValue(hash, Desc.DepthStencil.MinDepthBounds);
    hashValue(hash, Desc.DepthStencil.MaxDepthBounds);
    if (!Desc.DynamicStates.has(EDynamicState_t::StencilTestEnable))
        hashValue(hash, Desc.DepthStencil.StencilTestEnable);
    for (const auto* stencil : { &Desc.DepthStencil.Front, &Desc.DepthStencil.Back })
    {
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilOperations))
        {
            hashValue(hash, stencil->FailOp);
            hashValue(hash, stencil->PassOp);
            hashValue(hash, stencil->DepthFailOp);
            hashValue(hash, stencil->CompareOp);
        }
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilCompareMask))
            hashValue(hash, stencil->CompareMask);
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilWriteMask))
            hashValue(hash, stencil->WriteMask);
        if (!Desc.DynamicStates.has(EDynamicState_t::StencilReference))
            hashValue(hash, stencil->Reference);
    }
    for (uint32_t index = 0; index < Desc.Rendering.ColorAttachmentCount; ++index)
    {
        const auto& attachment = Desc.Blend.Attachments[index];
        hashValue(hash, attachment.BlendEnable);
        hashValue(hash, attachment.Color.SrcFactor);
        hashValue(hash, attachment.Color.DstFactor);
        hashValue(hash, attachment.Color.Operation);
        hashValue(hash, attachment.Alpha.SrcFactor);
        hashValue(hash, attachment.Alpha.DstFactor);
        hashValue(hash, attachment.Alpha.Operation);
        hashValue(hash, attachment.WriteMask.Value);
        hashValue(hash, Desc.Rendering.ColorFormats[index]);
    }
    hashValue(hash, Desc.Rendering.ColorAttachmentCount);
    hashValue(hash, Desc.Rendering.DepthFormat);
    hashValue(hash, Desc.Rendering.StencilFormat);
    hashValue(hash, Desc.Rendering.SampleCount);
    hashValue(hash, Desc.Rendering.ViewMask);
    hashValue(hash, Desc.DynamicStates.Value);
    return hash;
}

uint64_t calculateComputePipelineHash(const ComputePipelineDescriptor& Desc)
{
    uint64_t hash = HashOffset;
    const uint64_t layout_hash = Desc.Layout->getCompatibilityHash();
    const uint64_t shader_hash = Desc.Compute.Shader->getContentHash();
    hashValue(hash, layout_hash);
    hashValue(hash, shader_hash);
    hashValue(hash, EShaderStage_t::Compute);
    hashBytes(
        hash,
        Desc.Compute.Shader->getEntryPoint().data(),
        Desc.Compute.Shader->getEntryPoint().size());
    auto constants = Desc.Compute.SpecializationConstants;
    std::ranges::sort(constants, {}, &SpecializationConstant::Id);
    for (const auto& constant : constants)
    {
        hashValue(hash, constant.Id);
        hashBytes(hash, constant.Data.data(), constant.Data.size());
    }
    return hash;
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
    const bool mesh_path = static_cast<bool>(Desc.Mesh.Shader);
    const bool vertex_path = static_cast<bool>(Desc.Vertex.Shader);
    if (mesh_path == vertex_path)
        throw std::invalid_argument("Graphics pipeline requires exactly one vertex or mesh shader path.");
    if (Desc.Task.Shader && !mesh_path)
        throw std::invalid_argument("Task shader requires a mesh shader.");
    if (mesh_path && (Desc.Geometry.Shader || Desc.Hull.Shader || Desc.Domain.Shader))
        throw std::invalid_argument("Mesh shader path cannot use geometry or tessellation shaders.");
    if (mesh_path && (!Desc.VertexInput.Buffers.empty() || !Desc.VertexInput.Attributes.empty()))
        throw std::invalid_argument("Mesh shader path cannot use traditional vertex input.");
    if (mesh_path && Desc.DynamicStates.has(EDynamicState_t::VertexInput))
        throw std::invalid_argument("Mesh shader pipelines cannot use dynamic vertex input.");
    if (mesh_path && !Device.getFeatures().MeshShader)
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
    if (!mesh_path && Desc.InputAssembly.Topology == EPrimitiveTopology::PatchList && !Desc.Hull.Shader)
        throw std::invalid_argument("Patch topology requires hull and domain shaders.");
    if (!mesh_path && Desc.InputAssembly.Topology != EPrimitiveTopology::PatchList && Desc.Hull.Shader)
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
    constexpr uint64_t known_dynamic_states = (1ull << 17) - 1;
    if ((Desc.DynamicStates.Value & ~known_dynamic_states) != 0)
        throw std::invalid_argument("Pipeline requests an unknown dynamic state bit.");
    const auto& features = Device.getFeatures();
    const bool needs_extended_dynamic_state =
        Desc.DynamicStates.has(EDynamicState_t::CullMode) ||
        Desc.DynamicStates.has(EDynamicState_t::FrontFace) ||
        Desc.DynamicStates.has(EDynamicState_t::PrimitiveTopology) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthTestEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthWriteEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::DepthCompareOp) ||
        Desc.DynamicStates.has(EDynamicState_t::StencilTestEnable) ||
        Desc.DynamicStates.has(EDynamicState_t::StencilOperations);
    if (needs_extended_dynamic_state && !features.ExtendedDynamicState)
        throw std::invalid_argument("Pipeline requests VK_EXT_extended_dynamic_state without device support.");
    if (Desc.DynamicStates.has(EDynamicState_t::VertexInput) && !features.DynamicVertexInput)
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
        const auto& first = Desc.Blend.Attachments[0];
        for (uint32_t index = 1; index < Desc.Rendering.ColorAttachmentCount; ++index)
            if (first != Desc.Blend.Attachments[index])
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
    const std::array stages {
        std::pair { &Desc.Vertex, EShaderStage_t::Vertex },
        std::pair { &Desc.Pixel, EShaderStage_t::Pixel },
        std::pair { &Desc.Geometry, EShaderStage_t::Geometry },
        std::pair { &Desc.Hull, EShaderStage_t::Hull },
        std::pair { &Desc.Domain, EShaderStage_t::Domain },
        std::pair { &Desc.Task, EShaderStage_t::Task },
        std::pair { &Desc.Mesh, EShaderStage_t::Mesh }
    };
    const size_t stage_count = std::ranges::count_if(stages, [](const auto& stage)
    {
        return static_cast<bool>(stage.first->Shader);
    });
    OutState.StageStorageList.reserve(stage_count);
    for (const auto& [stage, expected_stage] : stages)
        if (stage->Shader)
            OutState.StageStorageList.emplace_back(
                StageStorage::collect(*stage, expected_stage, Device));

    // StageStorageList 的容量从此不再变化，finalize() 建立的所有内部指针保持稳定. 
    OutState.ShaderStages.reserve(stage_count);
    for (auto& storage : OutState.StageStorageList)
    {
        storage.finalize();
        OutState.ShaderStages.emplace_back(storage.getCreateInfo());
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
    std::unordered_set<uint32_t> bindings;
    for (const auto& binding : Desc.VertexInput.Buffers)
    {
        if (binding.Stride == 0 || !bindings.emplace(binding.Binding).second)
            throw std::invalid_argument(
                "Vertex bindings require a non-zero stride and unique binding index.");
        OutState.VertexBindings.emplace_back(binding.Binding, binding.Stride, toVk(binding.InputRate));
    }
    std::unordered_set<uint32_t> locations;
    for (const auto& attribute : Desc.VertexInput.Attributes)
    {
        if (!bindings.contains(attribute.Binding) || !locations.emplace(attribute.Location).second)
            throw std::invalid_argument(
                "Vertex attributes require an existing binding and unique location.");
        OutState.VertexAttributes.emplace_back(
            attribute.Location, attribute.Binding, toVk(attribute.Format), attribute.Offset);
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
    for (uint32_t index = 0; index < Desc.Rendering.ColorAttachmentCount; ++index)
    {
        const auto& state = Desc.Blend.Attachments[index];
        OutState.BlendAttachments.emplace_back(
            state.BlendEnable, toVk(state.Color.SrcFactor), toVk(state.Color.DstFactor),
            toVk(state.Color.Operation), toVk(state.Alpha.SrcFactor),
            toVk(state.Alpha.DstFactor), toVk(state.Alpha.Operation), toVk(state.WriteMask));
    }
    OutState.ColorBlend = vk::PipelineColorBlendStateCreateInfo(
        {}, false, vk::LogicOp::eCopy, OutState.BlendAttachments);
}

void buildDynamicState(
    const GraphicsPipelineDescriptor& Desc,
    GraphicsPipelineBuildState& OutState)
{
    constexpr std::array candidates {
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
    for (const auto state : candidates)
        if (Desc.DynamicStates.has(state)) OutState.DynamicStates.emplace_back(toVk(state));
    OutState.DynamicState = vk::PipelineDynamicStateCreateInfo({}, OutState.DynamicStates);
}

void buildRenderingState(
    const GraphicsPipelineDescriptor& Desc,
    GraphicsPipelineBuildState& OutState)
{
    for (uint32_t index = 0; index < Desc.Rendering.ColorAttachmentCount; ++index)
        OutState.ColorFormats[index] = toVk(Desc.Rendering.ColorFormats[index]);
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

    std::vector<uint32_t> words(Desc.ByteCode.size() / sizeof(uint32_t));
    std::memcpy(words.data(), Desc.ByteCode.data(), Desc.ByteCode.size());
    if (words.front() != 0x07230203u)
        throw std::invalid_argument("Shader bytecode is not a SPIR-V module.");
    ShaderModule = Device->getVkDevice().createShaderModuleUnique(
        vk::ShaderModuleCreateInfo({}, Desc.ByteCode.size(), words.data()));
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
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    std::vector<vk::DescriptorBindingFlags> binding_flags;
    bindings.reserve(Entries.size());
    binding_flags.reserve(Entries.size());
    CompatibilityHash = HashOffset;
    uint32_t previous_binding = 0;
    bool has_previous_binding = false;
    bool uses_update_after_bind = false;
    for (size_t index = 0; index < Entries.size(); ++index)
    {
        const auto& entry = Entries[index];
        if (entry.ArrayCount == 0 || !entry.Visibility ||
            (has_previous_binding && entry.Binding == previous_binding))
            throw std::invalid_argument("Bind group layout entries require unique bindings, visibility and array count.");
        if (entry.Type == EDescriptorType::AccelerationStructure &&
            !Device->getFeatures().AccelerationStructure)
            throw std::invalid_argument("Acceleration-structure descriptors are unsupported by this device.");
        if (entry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset) &&
            entry.Type != EDescriptorType::UniformBuffer &&
            entry.Type != EDescriptorType::ReadOnlyStorageBuffer &&
            entry.Type != EDescriptorType::ReadWriteStorageBuffer)
        {
            throw std::invalid_argument("Dynamic offsets are only valid for buffer descriptors.");
        }
        if (entry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound) &&
            !Device->getFeatures().PartiallyBoundDescriptors)
            throw std::invalid_argument("Partially-bound descriptors are unsupported by this device.");
        if (entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount) &&
            (!Device->getFeatures().VariableDescriptorCount || index + 1 != Entries.size()))
        {
            throw std::invalid_argument(
                "A variable descriptor array must be supported and use the greatest binding number.");
        }
        if (entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount) &&
            entry.Flags.has(EDescriptorBindingFlag_t::DynamicOffset))
            throw std::invalid_argument("Variable descriptor arrays cannot use dynamic offsets.");
        if (entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind) &&
            !Device->getFeatures().UpdateAfterBind)
            throw std::invalid_argument("Update-after-bind descriptors are unsupported by this device.");
        if (entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind) &&
            (entry.Type == EDescriptorType::InputAttachment ||
             entry.Type == EDescriptorType::AccelerationStructure))
        {
            throw std::invalid_argument(
                "This backend does not support update-after-bind for this descriptor type.");
        }
        previous_binding = entry.Binding;
        has_previous_binding = true;
        bindings.emplace_back(
            entry.Binding,
            toVk(entry.Type, entry.Flags),
            entry.ArrayCount,
            toVk(entry.Visibility));
        vk::DescriptorBindingFlags vk_flags;
        if (entry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound))
            vk_flags |= vk::DescriptorBindingFlagBits::ePartiallyBound;
        if (entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind))
        {
            vk_flags |= vk::DescriptorBindingFlagBits::eUpdateAfterBind;
            uses_update_after_bind = true;
        }
        if (entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount))
            vk_flags |= vk::DescriptorBindingFlagBits::eVariableDescriptorCount;
        binding_flags.emplace_back(vk_flags);
        hashValue(CompatibilityHash, entry.Binding);
        hashValue(CompatibilityHash, entry.Type);
        hashValue(CompatibilityHash, entry.ArrayCount);
        hashValue(CompatibilityHash, entry.Visibility.Value);
        hashValue(CompatibilityHash, entry.Flags.Value);
        appendKey(CompatibilityKey, entry.Binding);
        appendKey(CompatibilityKey, entry.Type);
        appendKey(CompatibilityKey, entry.ArrayCount);
        appendKey(CompatibilityKey, entry.Visibility.Value);
        appendKey(CompatibilityKey, entry.Flags.Value);
    }
    vk::DescriptorSetLayoutBindingFlagsCreateInfo binding_flags_info;
    binding_flags_info.setBindingFlags(binding_flags);
    const auto layout_flags = uses_update_after_bind
        ? vk::DescriptorSetLayoutCreateFlags(vk::DescriptorSetLayoutCreateFlagBits::eUpdateAfterBindPool)
        : vk::DescriptorSetLayoutCreateFlags();
    vk::DescriptorSetLayoutCreateInfo create_info(layout_flags, bindings);
    if (!binding_flags.empty())
        create_info.pNext = &binding_flags_info;
    DescriptorSetLayout = Device->getVkDevice().createDescriptorSetLayoutUnique(
        create_info);
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

    std::vector<vk::DescriptorSetLayout> layouts;
    layouts.reserve(BindGroupLayouts.size());
    CompatibilityHash = HashOffset;
    const uint64_t layout_count = BindGroupLayouts.size();
    appendKey(CompatibilityKey, layout_count);
    for (const auto& layout : BindGroupLayouts)
    {
        auto* vk_layout = dynamic_cast<VulkanBindGroupLayout*>(layout.get());
        if (!vk_layout || &vk_layout->getDevice() != Device || !vk_layout->isValid())
            throw std::invalid_argument("Bind group layout belongs to another backend or device.");
        layouts.emplace_back(vk_layout->getVkDescriptorSetLayout());
        const uint64_t layout_hash = vk_layout->getCompatibilityHash();
        hashValue(CompatibilityHash, layout_hash);
        const auto layout_key = vk_layout->getCompatibilityKey();
        const uint64_t layout_key_size = layout_key.size();
        appendKey(CompatibilityKey, layout_key_size);
        CompatibilityKey.insert(CompatibilityKey.end(), layout_key.begin(), layout_key.end());
    }

    std::vector<PushConstantRange> ranges = Desc.PushConstantRanges;
    std::ranges::sort(ranges, {}, &PushConstantRange::Offset);
    const uint64_t range_count = ranges.size();
    appendKey(CompatibilityKey, range_count);
    std::vector<vk::PushConstantRange> vk_ranges;
    vk_ranges.reserve(ranges.size());
    uint32_t previous_end = 0;
    for (const auto& range : ranges)
    {
        if (!range.Stages || range.Size == 0 || range.Offset % 4 != 0 || range.Size % 4 != 0 ||
            range.Offset < previous_end || range.Size > Device->getLimits().MaxPushConstantSize ||
            range.Offset > Device->getLimits().MaxPushConstantSize - range.Size)
        {
            throw std::invalid_argument("Push constant ranges are invalid, overlapping or out of device limits.");
        }
        previous_end = range.Offset + range.Size;
        vk_ranges.emplace_back(toVk(range.Stages), range.Offset, range.Size);
        hashValue(CompatibilityHash, range.Stages.Value);
        hashValue(CompatibilityHash, range.Offset);
        hashValue(CompatibilityHash, range.Size);
        appendKey(CompatibilityKey, range.Stages.Value);
        appendKey(CompatibilityKey, range.Offset);
        appendKey(CompatibilityKey, range.Size);
    }
    PipelineLayout = Device->getVkDevice().createPipelineLayoutUnique(
        vk::PipelineLayoutCreateInfo({}, layouts, vk_ranges));
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
    const uint32_t end = Offset + Size;
    return std::ranges::any_of(PushConstantRanges, [&](const PushConstantRange& range)
    {
        return Offset >= range.Offset && end <= range.Offset + range.Size &&
            !(Stages & ~range.Stages);
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
    std::vector<vk::PipelineCache> caches;
    std::vector<std::mutex*> mutexes { &Mutex };
    caches.reserve(Sources.size());
    mutexes.reserve(Sources.size() + 1);
    for (const auto& source : Sources)
    {
        auto* cache = dynamic_cast<VulkanPipelineCache*>(source.get());
        if (!cache || cache == this || &cache->getDevice() != Device || !cache->isValid())
            throw std::invalid_argument("Pipeline cache merge source is invalid.");
        caches.emplace_back(cache->getVkPipelineCache());
        mutexes.emplace_back(&cache->getMutex());
    }
    std::ranges::sort(mutexes);
    mutexes.erase(std::ranges::unique(mutexes).begin(), mutexes.end());
    std::vector<std::unique_lock<std::mutex>> locks;
    locks.reserve(mutexes.size());
    for (auto* mutex : mutexes)
        locks.emplace_back(*mutex);
    Device->getVkDevice().mergePipelineCaches(PipelineCache.get(), caches);
}

RDevice& VulkanPipelineCache::getDevice() const noexcept { return *Device; }
std::vector<std::byte> VulkanPipelineCache::serialize() const
{
    std::scoped_lock lock(Mutex);
    const auto bytes = Device->getVkDevice().getPipelineCacheData(PipelineCache.get());
    std::vector<std::byte> result(bytes.size());
    std::memcpy(result.data(), bytes.data(), bytes.size());
    return result;
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
    auto& layout = requireLayout(Desc.Layout, Device);
    validateGraphicsPipelineDescriptor(Desc, Device);
    GraphicsPipelineBuildState state;
    buildShaderStages(Desc, Device, state);
    buildVertexInputState(Desc, Device, state);
    buildFixedFunctionState(Desc, state);
    buildDynamicState(Desc, state);
    buildRenderingState(Desc, state);

    vk::GraphicsPipelineCreateInfo create_info;
    create_info.setFlags(getPipelineFlags(Desc.Compile.Flags))
        .setStages(state.ShaderStages)
        .setPVertexInputState(Desc.Mesh.Shader ? nullptr : &state.VertexInput)
        .setPInputAssemblyState(Desc.Mesh.Shader ? nullptr : &state.InputAssembly)
        .setPTessellationState(Desc.Hull.Shader ? &state.Tessellation : nullptr)
        .setPViewportState(&state.Viewport)
        .setPRasterizationState(&state.Rasterizer)
        .setPMultisampleState(&state.Multisample)
        .setPDepthStencilState(&state.DepthStencil)
        .setPColorBlendState(&state.ColorBlend)
        .setPDynamicState(&state.DynamicState)
        .setLayout(layout.getVkPipelineLayout())
        .setPNext(&state.Rendering);

    auto* cache = getCache(Desc.Compile.Cache, Device);
    std::unique_lock<std::mutex> cache_lock;
    if (cache) cache_lock = std::unique_lock(cache->getMutex());
    auto pipeline_result = Device.getVkDevice().createGraphicsPipelineUnique(
        cache ? cache->getVkPipelineCache() : vk::PipelineCache(), create_info);
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
        std::move(pipeline_result.value));
}

std::shared_ptr<RPipeline> createVulkanComputePipeline(
    VulkanDevice& Device,
    const ComputePipelineDescriptor& Desc)
{
    auto& layout = requireLayout(Desc.Layout, Device);
    if (!Desc.Compute.Shader)
        throw std::invalid_argument("Compute pipeline requires a compute shader.");
#if !RHI_ENABLE_PIPELINE_STATISTICS
    if (Desc.Compile.Flags.has(EPipelineCompileFlag_t::CaptureStatistics))
        throw std::invalid_argument("Pipeline statistics were disabled at build time.");
#endif
    StageStorage stage = StageStorage::collect(Desc.Compute, EShaderStage_t::Compute, Device);
    stage.finalize();
    vk::ComputePipelineCreateInfo create_info(
        getPipelineFlags(Desc.Compile.Flags), stage.getCreateInfo(), layout.getVkPipelineLayout());
    auto* cache = getCache(Desc.Compile.Cache, Device);
    std::unique_lock<std::mutex> cache_lock;
    if (cache) cache_lock = std::unique_lock(cache->getMutex());
    auto pipeline_result = Device.getVkDevice().createComputePipelineUnique(
        cache ? cache->getVkPipelineCache() : vk::PipelineCache(), create_info);
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
        std::move(pipeline_result.value));
}

} // namespace rhi
