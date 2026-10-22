#pragma once

#include <RHI.hpp>
#include <vulkan/vulkan.hpp>

#include <cstdint>
#include <vector>

namespace rhi
{

/**
 * @brief Vulkan 后端"版本 + 扩展 + 特性"的集中协商器.
 *
 * 把原本散落在 VulkanRHI::pickPhysicalDevice / createLogicalDevice 里的特性探测
 * 与启用逻辑收拢到本类型, 使默认基线降到 1.0:
 *
 *   - 所有特性都以 1.0 为基线实现(渲染走 VkRenderPass, 同步走二元信号量 + fence);
 *   - 探测到更高核心版本或同名扩展后, 才启用对应的增强特性(动态渲染 /
 *     synchronization2 / 时间线信号量等);
 *   - 查不到增强特性时, 对应能力位保持 false, 上层继续走 1.0 基线路径 —— 不存在
 *     "必需特性缺失即失败"的分支;
 *   - 对外只报告"实际通过 vkCreateDevice 启用成功"的能力位, 而非硬件原始支持.
 *
 * 线程约定: 一个 VulkanFeatureSet 实例只在设备创建阶段单线程使用, 之后只读.
 */
class VulkanFeatureSet final
{
public:
    /**
     * @brief 探测物理设备支持的能力, 并据此构建启用链.
     * @param PhysicalDevice 候选物理设备, 必须非空.
     */
    void detect(vk::PhysicalDevice PhysicalDevice);

    /**
     * @brief 生成 vkCreateDevice 需要的启用扩展列表.
     * @return 扩展名列表; 交换链恒在首位.
     */
    [[nodiscard]] std::vector<const char*> buildEnabledExtensions() const;

    /**
     * @brief 返回 vkCreateDevice 的 pNext 链头.
     * @return 指向内部存储的特性链头; 未 detect() 时为 nullptr.
     */
    [[nodiscard]] const void* buildFeatureChainHead() const noexcept;

    /**
     * @brief 返回 vkCreateDevice 的 1.0 核心特性集.
     * @return 仅含真正启用位(不是硬件原始支持).
     */
    [[nodiscard]] vk::PhysicalDeviceFeatures buildCoreFeatures() const;

    /**
     * @brief 把"实际启用"的能力位回写到 RHI DeviceFeatures.
     * @param Features 目标能力集, 会被就地覆写.
     */
    void writeBackTo(DeviceFeatures& Features) const;

    // ---- 查询 ----
    /** @brief 设备报告的 apiVersion. */
    [[nodiscard]] uint32_t getDeviceApiVersion() const noexcept { return DeviceApiVersion; }
    /** @brief 动态渲染是否已启用(1.3 核心或 VK_KHR_dynamic_rendering). */
    [[nodiscard]] bool isDynamicRenderingEnabled() const noexcept { return EnableDynamicRendering; }
    /** @brief synchronization2 是否已启用(1.3 核心或 VK_KHR_synchronization2). */
    [[nodiscard]] bool isSynchronization2Enabled() const noexcept { return EnableSynchronization2; }
    /** @brief 时间线信号量是否已启用(1.2 核心或 VK_KHR_timeline_semaphore). */
    [[nodiscard]] bool isTimelineSemaphoreEnabled() const noexcept { return EnableTimelineSemaphore; }

private:
    [[nodiscard]] bool hasExtension(const char* Name) const noexcept;
    void buildFeatureChain();

    // ---- 探测输入 ----
    uint32_t DeviceApiVersion { VK_API_VERSION_1_0 };
    std::vector<vk::ExtensionProperties> AvailableExtensions;

    // ---- 扩展可用性 ----
    bool DynamicRenderingExtension { false };
    bool Synchronization2Extension { false };
    bool TimelineSemaphoreExtension { false };
    bool MultiviewExtension { false };
    bool MeshShaderExtension { false };
    bool ExtendedDynamicStateExtension { false };
    bool ExtendedDynamicState2Extension { false };
    bool ExtendedDynamicState3Extension { false };
    bool VertexInputDynamicStateExtension { false };
    bool DeferredHostOperationsExtension { false };
    bool AccelerationStructureExtension { false };
    bool RayTracingPipelineExtension { false };
    bool RayQueryExtension { false };
    bool HdrMetadataExtension { false };

    // ---- 启用状态 ----
    bool EnableDynamicRendering { false };
    bool EnableSynchronization2 { false };
    bool EnableTimelineSemaphore { false };
    bool EnableMultiview { false };
    bool EnableDynamicState3 { false };
    bool EnableAccelerationStructure { false };
    bool EnableRayTracingPipeline { false };
    bool EnableRayQuery { false };

    // ---- 启用特性结构(内部存储, 供 pNext 链引用) ----
    vk::PhysicalDeviceVulkan13Features EnabledVulkan13 {};
    vk::PhysicalDeviceVulkan12Features EnabledVulkan12 {};
    vk::PhysicalDeviceMultiviewFeatures EnabledMultiview {};
    vk::PhysicalDeviceDynamicRenderingFeatures EnabledDynamicRendering {};
    vk::PhysicalDeviceSynchronization2Features EnabledSynchronization2 {};
    vk::PhysicalDeviceTimelineSemaphoreFeatures EnabledTimelineSemaphore {};
    vk::PhysicalDeviceMeshShaderFeaturesEXT EnabledMeshShader {};
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT EnabledDynamicState {};
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT EnabledDynamicState2 {};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT EnabledDynamicState3 {};
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT EnabledVertexInput {};
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR EnabledAccelerationStructure {};
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR EnabledRayTracingPipeline {};
    vk::PhysicalDeviceRayQueryFeaturesKHR EnabledRayQuery {};

    // ---- 1.0 核心特性 ----
    vk::PhysicalDeviceFeatures CoreFeatures {};

    // ---- 启用链头 ----
    const void* FeatureChainHead { nullptr };
};

} // namespace rhi
