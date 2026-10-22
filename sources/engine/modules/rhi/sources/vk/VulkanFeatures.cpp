#include "VulkanFeatures.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace rhi
{

namespace
{

[[nodiscard]] bool containsExtension(
    const std::vector<vk::ExtensionProperties>& Available,
    const char* Name)
{
    return std::ranges::any_of(Available, [Name](const vk::ExtensionProperties& Extension)
    {
        return std::strcmp(Extension.extensionName.data(), Name) == 0;
    });
}

/**
 * @brief 链式连接一个特性结构的节点及其 pNext 槽位.
 *
 * 采用"尾在前"压栈: 最后压入的节点成为链头, 每个节点的 pNext 指向此前压入的节点.
 * 通过显式传入 pNext 槽位地址避免对 Vulkan 特性结构布局做假设.
 */
struct ChainEntry
{
    void* Node;
    void** PNextSlot;
};

void linkChain(std::initializer_list<ChainEntry> Entries, void*& OutHead) noexcept
{
    OutHead = nullptr;
    for (const ChainEntry& Entry : Entries)
    {
        if (!Entry.Node)
            continue;
        *Entry.PNextSlot = OutHead;
        OutHead = Entry.Node;
    }
}

} // namespace

bool VulkanFeatureSet::hasExtension(const char* Name) const noexcept
{
    return containsExtension(AvailableExtensions, Name);
}

void VulkanFeatureSet::detect(vk::PhysicalDevice PhysicalDevice)
{
    DeviceApiVersion = PhysicalDevice.getProperties().apiVersion;
    AvailableExtensions = PhysicalDevice.enumerateDeviceExtensionProperties();

    const bool HasVulkan13 = DeviceApiVersion >= VK_API_VERSION_1_3;
    const bool HasVulkan12 = DeviceApiVersion >= VK_API_VERSION_1_2;
    const bool HasVulkan11 = DeviceApiVersion >= VK_API_VERSION_1_1;

    DynamicRenderingExtension = hasExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    Synchronization2Extension = hasExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    TimelineSemaphoreExtension = hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    MultiviewExtension = hasExtension(VK_KHR_MULTIVIEW_EXTENSION_NAME);
    MeshShaderExtension = hasExtension(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    ExtendedDynamicStateExtension = hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
    ExtendedDynamicState2Extension = hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
    ExtendedDynamicState3Extension = hasExtension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    VertexInputDynamicStateExtension = hasExtension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    DeferredHostOperationsExtension = hasExtension(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    AccelerationStructureExtension = hasExtension(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    RayTracingPipelineExtension = hasExtension(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    RayQueryExtension = hasExtension(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    HdrMetadataExtension = hasExtension(VK_EXT_HDR_METADATA_EXTENSION_NAME);

    // ---- 查询支持位: 按"核心版本 或 扩展"决定是否把对应结构加入查询链 ----
    vk::PhysicalDeviceVulkan13Features SupportedVulkan13 {};
    vk::PhysicalDeviceVulkan12Features SupportedVulkan12 {};
    vk::PhysicalDeviceMultiviewFeatures SupportedMultiview {};
    vk::PhysicalDeviceDynamicRenderingFeatures SupportedDynamicRendering {};
    vk::PhysicalDeviceSynchronization2Features SupportedSynchronization2 {};
    vk::PhysicalDeviceTimelineSemaphoreFeatures SupportedTimelineSemaphore {};
    vk::PhysicalDeviceMeshShaderFeaturesEXT SupportedMeshShader {};
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT SupportedDynamicState {};
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT SupportedDynamicState2 {};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT SupportedDynamicState3 {};
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT SupportedVertexInput {};
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR SupportedAccelerationStructure {};
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR SupportedRayTracingPipeline {};
    vk::PhysicalDeviceRayQueryFeaturesKHR SupportedRayQuery {};

    void* QueryHead = nullptr;
    linkChain({
        { RayQueryExtension ? static_cast<void*>(&SupportedRayQuery) : nullptr,
          &SupportedRayQuery.pNext },
        { RayTracingPipelineExtension ? static_cast<void*>(&SupportedRayTracingPipeline) : nullptr,
          &SupportedRayTracingPipeline.pNext },
        { AccelerationStructureExtension ? static_cast<void*>(&SupportedAccelerationStructure) : nullptr,
          &SupportedAccelerationStructure.pNext },
        { VertexInputDynamicStateExtension ? static_cast<void*>(&SupportedVertexInput) : nullptr,
          &SupportedVertexInput.pNext },
        { ExtendedDynamicState3Extension ? static_cast<void*>(&SupportedDynamicState3) : nullptr,
          &SupportedDynamicState3.pNext },
        { (HasVulkan13 || ExtendedDynamicState2Extension) ? static_cast<void*>(&SupportedDynamicState2) : nullptr,
          &SupportedDynamicState2.pNext },
        { (HasVulkan13 || ExtendedDynamicStateExtension) ? static_cast<void*>(&SupportedDynamicState) : nullptr,
          &SupportedDynamicState.pNext },
        { MeshShaderExtension ? static_cast<void*>(&SupportedMeshShader) : nullptr,
          &SupportedMeshShader.pNext },
        { (HasVulkan11 || MultiviewExtension) ? static_cast<void*>(&SupportedMultiview) : nullptr,
          &SupportedMultiview.pNext },
        { (!HasVulkan12 && TimelineSemaphoreExtension) ? static_cast<void*>(&SupportedTimelineSemaphore) : nullptr,
          &SupportedTimelineSemaphore.pNext },
        { HasVulkan12 ? static_cast<void*>(&SupportedVulkan12) : nullptr,
          &SupportedVulkan12.pNext },
        { (!HasVulkan13 && Synchronization2Extension) ? static_cast<void*>(&SupportedSynchronization2) : nullptr,
          &SupportedSynchronization2.pNext },
        { (!HasVulkan13 && DynamicRenderingExtension) ? static_cast<void*>(&SupportedDynamicRendering) : nullptr,
          &SupportedDynamicRendering.pNext },
        { HasVulkan13 ? static_cast<void*>(&SupportedVulkan13) : nullptr,
          &SupportedVulkan13.pNext },
    }, QueryHead);

    vk::PhysicalDeviceFeatures2 SupportedFeatures {};
    SupportedFeatures.pNext = QueryHead;
    PhysicalDevice.getFeatures2(&SupportedFeatures);

    // ---- 计算启用位: 优先核心版本, 否则同名扩展 ----
    EnableDynamicRendering = HasVulkan13
        ? SupportedVulkan13.dynamicRendering == VK_TRUE
        : (DynamicRenderingExtension && SupportedDynamicRendering.dynamicRendering == VK_TRUE);
    EnableSynchronization2 = HasVulkan13
        ? SupportedVulkan13.synchronization2 == VK_TRUE
        : (Synchronization2Extension && SupportedSynchronization2.synchronization2 == VK_TRUE);
    EnableTimelineSemaphore = HasVulkan12
        ? SupportedVulkan12.timelineSemaphore == VK_TRUE
        : (TimelineSemaphoreExtension && SupportedTimelineSemaphore.timelineSemaphore == VK_TRUE);
    EnableMultiview = SupportedMultiview.multiview == VK_TRUE;

    // 光追: 需要 deferred host operations + 加速结构 + bufferDeviceAddress + 硬件支持.
    EnableAccelerationStructure = DeferredHostOperationsExtension &&
        AccelerationStructureExtension &&
        HasVulkan12 &&
        SupportedVulkan12.bufferDeviceAddress == VK_TRUE &&
        SupportedAccelerationStructure.accelerationStructure == VK_TRUE;
    EnableRayTracingPipeline = EnableAccelerationStructure && RayTracingPipelineExtension &&
        SupportedRayTracingPipeline.rayTracingPipeline == VK_TRUE;
    EnableRayQuery = EnableAccelerationStructure && RayQueryExtension &&
        SupportedRayQuery.rayQuery == VK_TRUE;

    // ---- 填充启用结构 ----
    EnabledVulkan13.dynamicRendering = (HasVulkan13 && EnableDynamicRendering) ? VK_TRUE : VK_FALSE;
    EnabledVulkan13.synchronization2 = (HasVulkan13 && EnableSynchronization2) ? VK_TRUE : VK_FALSE;
    EnabledDynamicRendering.dynamicRendering =
        (!HasVulkan13 && EnableDynamicRendering) ? VK_TRUE : VK_FALSE;
    EnabledSynchronization2.synchronization2 =
        (!HasVulkan13 && EnableSynchronization2) ? VK_TRUE : VK_FALSE;

    if (HasVulkan12)
    {
        EnabledVulkan12.timelineSemaphore = EnableTimelineSemaphore ? VK_TRUE : VK_FALSE;
        EnabledVulkan12.descriptorIndexing = SupportedVulkan12.descriptorIndexing;
        EnabledVulkan12.runtimeDescriptorArray = SupportedVulkan12.runtimeDescriptorArray;
        EnabledVulkan12.descriptorBindingPartiallyBound = SupportedVulkan12.descriptorBindingPartiallyBound;
        EnabledVulkan12.descriptorBindingVariableDescriptorCount = SupportedVulkan12.descriptorBindingVariableDescriptorCount;
        EnabledVulkan12.descriptorBindingUniformBufferUpdateAfterBind = SupportedVulkan12.descriptorBindingUniformBufferUpdateAfterBind;
        EnabledVulkan12.descriptorBindingSampledImageUpdateAfterBind = SupportedVulkan12.descriptorBindingSampledImageUpdateAfterBind;
        EnabledVulkan12.descriptorBindingStorageImageUpdateAfterBind = SupportedVulkan12.descriptorBindingStorageImageUpdateAfterBind;
        EnabledVulkan12.descriptorBindingStorageBufferUpdateAfterBind = SupportedVulkan12.descriptorBindingStorageBufferUpdateAfterBind;
        EnabledVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind = SupportedVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind;
        EnabledVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind = SupportedVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
        EnabledVulkan12.separateDepthStencilLayouts = SupportedVulkan12.separateDepthStencilLayouts;
        EnabledVulkan12.bufferDeviceAddress = SupportedVulkan12.bufferDeviceAddress;
    }
    EnabledTimelineSemaphore.timelineSemaphore =
        (!HasVulkan12 && EnableTimelineSemaphore) ? VK_TRUE : VK_FALSE;

    EnabledMultiview.multiview = EnableMultiview ? VK_TRUE : VK_FALSE;
    EnabledMeshShader.meshShader = MeshShaderExtension ? SupportedMeshShader.meshShader : VK_FALSE;
    EnabledMeshShader.taskShader = MeshShaderExtension ? SupportedMeshShader.taskShader : VK_FALSE;
    EnabledDynamicState.extendedDynamicState = SupportedDynamicState.extendedDynamicState;
    EnabledDynamicState2.extendedDynamicState2 = SupportedDynamicState2.extendedDynamicState2;

    if (ExtendedDynamicState3Extension)
    {
        // 仅启用后端的可移植固定功能子集; 厂商/伴生扩展状态保持关闭.
        EnabledDynamicState3.extendedDynamicState3TessellationDomainOrigin = SupportedDynamicState3.extendedDynamicState3TessellationDomainOrigin;
        EnabledDynamicState3.extendedDynamicState3DepthClampEnable = SupportedDynamicState3.extendedDynamicState3DepthClampEnable;
        EnabledDynamicState3.extendedDynamicState3PolygonMode = SupportedDynamicState3.extendedDynamicState3PolygonMode;
        EnabledDynamicState3.extendedDynamicState3RasterizationSamples = SupportedDynamicState3.extendedDynamicState3RasterizationSamples;
        EnabledDynamicState3.extendedDynamicState3SampleMask = SupportedDynamicState3.extendedDynamicState3SampleMask;
        EnabledDynamicState3.extendedDynamicState3AlphaToCoverageEnable = SupportedDynamicState3.extendedDynamicState3AlphaToCoverageEnable;
        EnabledDynamicState3.extendedDynamicState3AlphaToOneEnable = SupportedDynamicState3.extendedDynamicState3AlphaToOneEnable;
        EnabledDynamicState3.extendedDynamicState3LogicOpEnable = SupportedDynamicState3.extendedDynamicState3LogicOpEnable;
        EnabledDynamicState3.extendedDynamicState3ColorBlendEnable = SupportedDynamicState3.extendedDynamicState3ColorBlendEnable;
        EnabledDynamicState3.extendedDynamicState3ColorBlendEquation = SupportedDynamicState3.extendedDynamicState3ColorBlendEquation;
        EnabledDynamicState3.extendedDynamicState3ColorWriteMask = SupportedDynamicState3.extendedDynamicState3ColorWriteMask;
    }
    EnableDynamicState3 =
        EnabledDynamicState3.extendedDynamicState3TessellationDomainOrigin ||
        EnabledDynamicState3.extendedDynamicState3DepthClampEnable ||
        EnabledDynamicState3.extendedDynamicState3PolygonMode ||
        EnabledDynamicState3.extendedDynamicState3RasterizationSamples ||
        EnabledDynamicState3.extendedDynamicState3SampleMask ||
        EnabledDynamicState3.extendedDynamicState3AlphaToCoverageEnable ||
        EnabledDynamicState3.extendedDynamicState3AlphaToOneEnable ||
        EnabledDynamicState3.extendedDynamicState3LogicOpEnable ||
        EnabledDynamicState3.extendedDynamicState3ColorBlendEnable ||
        EnabledDynamicState3.extendedDynamicState3ColorBlendEquation ||
        EnabledDynamicState3.extendedDynamicState3ColorWriteMask;

    EnabledVertexInput.vertexInputDynamicState = VertexInputDynamicStateExtension
        ? SupportedVertexInput.vertexInputDynamicState : VK_FALSE;
    EnabledAccelerationStructure.accelerationStructure = EnableAccelerationStructure ? VK_TRUE : VK_FALSE;
    EnabledRayTracingPipeline.rayTracingPipeline = EnableRayTracingPipeline ? VK_TRUE : VK_FALSE;
    EnabledRayQuery.rayQuery = EnableRayQuery ? VK_TRUE : VK_FALSE;

    // ---- 1.0 核心特性 ----
    const auto& SupportedCore = SupportedFeatures.features;
    CoreFeatures.geometryShader = SupportedCore.geometryShader;
    CoreFeatures.tessellationShader = SupportedCore.tessellationShader;
    CoreFeatures.fillModeNonSolid = SupportedCore.fillModeNonSolid;
    CoreFeatures.wideLines = SupportedCore.wideLines;
    CoreFeatures.depthClamp = SupportedCore.depthClamp;
    CoreFeatures.depthBounds = SupportedCore.depthBounds;
    CoreFeatures.sampleRateShading = SupportedCore.sampleRateShading;
    CoreFeatures.samplerAnisotropy = SupportedCore.samplerAnisotropy;
    CoreFeatures.alphaToOne = SupportedCore.alphaToOne;
    CoreFeatures.independentBlend = SupportedCore.independentBlend;
    CoreFeatures.pipelineStatisticsQuery = SupportedCore.pipelineStatisticsQuery;

    buildFeatureChain();
}

std::vector<const char*> VulkanFeatureSet::buildEnabledExtensions() const
{
    std::vector<const char*> Extensions { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    if (HdrMetadataExtension)
        Extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (EnabledMeshShader.meshShader)
        Extensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (EnableDynamicState3)
        Extensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (EnabledVertexInput.vertexInputDynamicState)
        Extensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    // 提升特性仅在"通过扩展启用"时才把扩展名加入列表.
    if (EnableDynamicRendering && DeviceApiVersion < VK_API_VERSION_1_3)
        Extensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    if (EnableSynchronization2 && DeviceApiVersion < VK_API_VERSION_1_3)
        Extensions.push_back(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    if (EnableTimelineSemaphore && DeviceApiVersion < VK_API_VERSION_1_2)
        Extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    if (EnableMultiview && DeviceApiVersion < VK_API_VERSION_1_1)
        Extensions.push_back(VK_KHR_MULTIVIEW_EXTENSION_NAME);
    if (EnableAccelerationStructure)
    {
        Extensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        Extensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        if (EnableRayTracingPipeline)
            Extensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (EnableRayQuery)
            Extensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }
    return Extensions;
}

const void* VulkanFeatureSet::buildFeatureChainHead() const noexcept
{
    return FeatureChainHead;
}

vk::PhysicalDeviceFeatures VulkanFeatureSet::buildCoreFeatures() const
{
    return CoreFeatures;
}

void VulkanFeatureSet::writeBackTo(DeviceFeatures& Features) const
{
    Features.DynamicRendering = EnableDynamicRendering;
    Features.Synchronization2 = EnableSynchronization2;
    Features.TimelineSemaphore = EnableTimelineSemaphore;
    Features.Multiview = EnableMultiview;
    Features.SeparateDepthStencilLayouts = EnabledVulkan12.separateDepthStencilLayouts == VK_TRUE;
    Features.GeometryShader = CoreFeatures.geometryShader == VK_TRUE;
    Features.TessellationShader = CoreFeatures.tessellationShader == VK_TRUE;
    Features.FillModeNonSolid = CoreFeatures.fillModeNonSolid == VK_TRUE;
    Features.WideLines = CoreFeatures.wideLines == VK_TRUE;
    Features.DepthClamp = CoreFeatures.depthClamp == VK_TRUE;
    Features.DepthBounds = CoreFeatures.depthBounds == VK_TRUE;
    Features.SampleRateShading = CoreFeatures.sampleRateShading == VK_TRUE;
    Features.SamplerAnisotropy = CoreFeatures.samplerAnisotropy == VK_TRUE;
    Features.AlphaToOne = CoreFeatures.alphaToOne == VK_TRUE;
    Features.IndependentBlend = CoreFeatures.independentBlend == VK_TRUE;
    Features.ExtendedDynamicState = EnabledDynamicState.extendedDynamicState == VK_TRUE;
    Features.ExtendedDynamicState2 = EnabledDynamicState2.extendedDynamicState2 == VK_TRUE;
    Features.ExtendedDynamicState3 = EnableDynamicState3;
    Features.DynamicVertexInput = EnabledVertexInput.vertexInputDynamicState == VK_TRUE;
    Features.DescriptorIndexing = EnabledVulkan12.descriptorIndexing == VK_TRUE;
    Features.RuntimeDescriptorArray = EnabledVulkan12.runtimeDescriptorArray == VK_TRUE;
    Features.PartiallyBoundDescriptors = EnabledVulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    Features.VariableDescriptorCount = EnabledVulkan12.descriptorBindingVariableDescriptorCount == VK_TRUE;
    Features.UpdateAfterBind =
        EnabledVulkan12.descriptorBindingUniformBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingSampledImageUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageImageUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    Features.MeshShader = EnabledMeshShader.meshShader == VK_TRUE;
    Features.TaskShader = Features.MeshShader && EnabledMeshShader.taskShader == VK_TRUE;
    Features.OcclusionQueries = true;
    Features.PipelineStatisticsQueries = CoreFeatures.pipelineStatisticsQuery == VK_TRUE;
    Features.QueryResultCopy = true;
    Features.BufferDeviceAddress = EnabledVulkan12.bufferDeviceAddress == VK_TRUE;
    Features.AccelerationStructure = EnableAccelerationStructure;
    Features.RayTracingPipeline = EnableRayTracingPipeline;
    Features.RayQuery = EnableRayQuery;
    Features.SecondaryCommandLists = true;
    Features.SwapchainStatus = true;
    Features.HDRMetadata = HdrMetadataExtension;
}

void VulkanFeatureSet::buildFeatureChain()
{
    const bool HasVulkan13 = DeviceApiVersion >= VK_API_VERSION_1_3;
    const bool HasVulkan12 = DeviceApiVersion >= VK_API_VERSION_1_2;

    // 尾在前构建, 最后压栈的是链头; 顺序与查询链一致.
    void* Head = nullptr;
    linkChain({
        { EnableRayQuery ? static_cast<void*>(&EnabledRayQuery) : nullptr,
          &EnabledRayQuery.pNext },
        { EnableRayTracingPipeline ? static_cast<void*>(&EnabledRayTracingPipeline) : nullptr,
          &EnabledRayTracingPipeline.pNext },
        { EnableAccelerationStructure ? static_cast<void*>(&EnabledAccelerationStructure) : nullptr,
          &EnabledAccelerationStructure.pNext },
        { EnabledVertexInput.vertexInputDynamicState ? static_cast<void*>(&EnabledVertexInput) : nullptr,
          &EnabledVertexInput.pNext },
        { EnableDynamicState3 ? static_cast<void*>(&EnabledDynamicState3) : nullptr,
          &EnabledDynamicState3.pNext },
        { EnabledDynamicState2.extendedDynamicState2 ? static_cast<void*>(&EnabledDynamicState2) : nullptr,
          &EnabledDynamicState2.pNext },
        { EnabledDynamicState.extendedDynamicState ? static_cast<void*>(&EnabledDynamicState) : nullptr,
          &EnabledDynamicState.pNext },
        { EnabledMeshShader.meshShader ? static_cast<void*>(&EnabledMeshShader) : nullptr,
          &EnabledMeshShader.pNext },
        { EnableMultiview ? static_cast<void*>(&EnabledMultiview) : nullptr,
          &EnabledMultiview.pNext },
        { (!HasVulkan12 && EnableTimelineSemaphore) ? static_cast<void*>(&EnabledTimelineSemaphore) : nullptr,
          &EnabledTimelineSemaphore.pNext },
        { HasVulkan12 ? static_cast<void*>(&EnabledVulkan12) : nullptr,
          &EnabledVulkan12.pNext },
        { (!HasVulkan13 && EnableSynchronization2) ? static_cast<void*>(&EnabledSynchronization2) : nullptr,
          &EnabledSynchronization2.pNext },
        { (!HasVulkan13 && EnableDynamicRendering) ? static_cast<void*>(&EnabledDynamicRendering) : nullptr,
          &EnabledDynamicRendering.pNext },
        { HasVulkan13 ? static_cast<void*>(&EnabledVulkan13) : nullptr,
          &EnabledVulkan13.pNext },
    }, Head);
    FeatureChainHead = Head;
}

} // namespace rhi
