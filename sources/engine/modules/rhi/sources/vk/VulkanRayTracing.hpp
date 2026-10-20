#pragma once

#include <RHI.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** Owns VkAccelerationStructureKHR while retaining its backing buffer range. */
class VulkanAccelerationStructure final : public RAccelerationStructure
{
public:
    VulkanAccelerationStructure(VulkanDevice& Device, const AccelerationStructureDescriptor& Desc);
    ~VulkanAccelerationStructure() override;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] bool isValid() const noexcept override { return Handle != VK_NULL_HANDLE; }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] DeviceAddress getDeviceAddress() const noexcept override;
    [[nodiscard]] vk::AccelerationStructureKHR getVkHandle() const noexcept { return Handle; }
    [[nodiscard]] EAccelerationStructureType getType() const noexcept { return Type; }
    [[nodiscard]] DeviceSizeType getSize() const noexcept { return Size; }

private:
    VulkanDevice* Device { nullptr };
    std::shared_ptr<RBuffer> Storage;
    EAccelerationStructureType Type { EAccelerationStructureType::BottomLevel };
    DeviceSizeType Size { 0 };
    vk::AccelerationStructureKHR Handle {};
};

[[nodiscard]] AccelerationStructureBuildSizes queryVulkanAccelerationStructureBuildSizes(
    const VulkanDevice& Device,
    EAccelerationStructureType Type,
    EAccelerationStructureBuildFlags Flags,
    std::span<const AccelerationStructureGeometry> Geometries);

[[nodiscard]] bool recordVulkanAccelerationStructureBuilds(
    VulkanDevice& Device,
    vk::CommandBuffer CommandBuffer,
    std::span<const AccelerationStructureBuildDescriptor> Builds,
    std::vector<std::shared_ptr<void>>& RetainedResources);

} // namespace rhi