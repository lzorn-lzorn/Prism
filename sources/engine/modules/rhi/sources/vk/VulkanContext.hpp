#pragma once

#include <RHI.hpp>
#include "VulkanFeatures.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace rhi
{

struct VulkanSurfaceState final
{
    vk::Instance Instance;
    vk::SurfaceKHR Handle { VK_NULL_HANDLE };
    uint64_t Generation { 0 };

    ~VulkanSurfaceState()
    {
        if (Instance && Handle) Instance.destroySurfaceKHR(Handle);
    }
};

/**
 * @brief Vulkan 后端共享所有权根. 
 *
 * RHI, Device, Queue 与 Swapchain 共享该对象，保证所有子对象销毁前
 * VkDevice, Surface 与 Instance 始终有效. 
 */
struct VulkanContext final
{
    ~VulkanContext()
    {
        Device.reset();
        Surface.reset();
        Instance.reset();
    }

    [[nodiscard]] bool isDeviceLost() const noexcept
    {
        return DeviceStatus.load(std::memory_order_relaxed) != EDeviceStatus::Ready;
    }

    void markDeviceLost() noexcept
    {
        DeviceStatus.store(EDeviceStatus::Lost, std::memory_order_relaxed);
    }

    [[nodiscard]] uint32_t familyIndex(ECommandQueueType Type) const noexcept
    {
        switch (Type)
        {
        case ECommandQueueType::Compute: return ComputeQueueFamilyIndex;
        case ECommandQueueType::Copy: return CopyQueueFamilyIndex;
        case ECommandQueueType::Graphics:
        default: return GraphicsQueueFamilyIndex;
        }
    }

    vk::UniqueInstance Instance;
    vk::PhysicalDevice PhysicalDevice;
    vk::UniqueDevice Device;
    // The initialized backend currently exposes one SDL surface. Each swapchain retains
    // its generation, so replacing this pointer never invalidates native destruction order.
    std::shared_ptr<VulkanSurfaceState> Surface;
    std::weak_ptr<ui::IGenericWindow> Window;
    uint64_t NextSurfaceGeneration { 1 };
    std::atomic<EDeviceStatus> DeviceStatus { EDeviceStatus::Ready };
    // Populated from the feature bits actually passed to vkCreateDevice, never raw hardware support.
    DeviceFeatures EnabledFeatures {};
    // 版本 + 扩展 + 特性的集中协商结果, 生命周期与本上下文一致.
    VulkanFeatureSet FeatureSet {};
    uint32_t GraphicsQueueFamilyIndex { UINT32_MAX };
    uint32_t ComputeQueueFamilyIndex { UINT32_MAX };
    uint32_t CopyQueueFamilyIndex { UINT32_MAX };
    uint32_t PresentQueueFamilyIndex { UINT32_MAX };
};

using VulkanContextPtr = std::shared_ptr<VulkanContext>;

} // namespace rhi
