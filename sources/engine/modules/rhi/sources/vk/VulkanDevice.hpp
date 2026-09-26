#pragma once

#include <memory>
#include <mutex>
#include <RHI.hpp>
#include <optional>
#include <vector>
#include <vulkan/vulkan.hpp>
#include "VulkanContext.hpp"

namespace rhi
{
class VulkanDescriptorAllocator;
class VulkanMemoryAllocator;
struct QueueFamilyIndices {
    std::optional<uint32_t> GraphicsFamily;
    std::optional<uint32_t> PresentFamily;

    bool isComplete() {
        return GraphicsFamily.has_value() && PresentFamily.has_value();
    }
	
    static QueueFamilyIndices findQueueFamilies(vk::PhysicalDevice InDevice, vk::SurfaceKHR Surface) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(InDevice, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(InDevice, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                indices.GraphicsFamily = i;
            }

            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(InDevice, i, Surface, &presentSupport);

            if (presentSupport) {
                indices.PresentFamily = i;
            }

            if (indices.isComplete()) {
                break;
            }

            i++;
        }

        return indices;
    }
};

class VulkanDevice final : public RDevice
{
public:
    explicit VulkanDevice(VulkanContextPtr Context);
	
    ~VulkanDevice() override;
	
    uint32_t findMemoryType(uint32_t TypeBits, vk::MemoryPropertyFlags Properties);
    vk::PhysicalDevice& getVkPhysicalDevice() { return Context->PhysicalDevice; }
    vk::Device& getVkDevice() { return Context->Device.get(); }
    [[nodiscard]] const vk::Device& getVkDevice() const { return Context->Device.get(); }
    [[nodiscard]] const VulkanContextPtr& getContext() const noexcept { return Context; }
    [[nodiscard]] uint32_t getQueueFamilyIndex(ECommandQueueType Type) const noexcept
    {
        return Context->familyIndex(Type);
    }
public:
    std::shared_ptr<RBuffer> createBuffer(const RBuffer::Descriptor_t& Desc) override;
    std::shared_ptr<RImage> createImage(const RImage::Descriptor_t& Desc) override;
    std::shared_ptr<RImageView> createImageView(const RImageView::Descriptor_t& Desc) override;
    std::shared_ptr<RSampler> createSampler(const RSampler::Descriptor_t& Desc = {}) override;
    std::shared_ptr<RShader> createShader(const ShaderDescriptor& Desc) override;
    std::shared_ptr<RBindGroupLayout> createBindGroupLayout(
        const BindGroupLayoutDescriptor& Desc) override;
    std::shared_ptr<RBindGroup> createBindGroup(
        const BindGroupDescriptor& Desc) override;
    std::shared_ptr<RPipelineLayout> createPipelineLayout(
        const PipelineLayoutDescriptor& Desc) override;
    std::shared_ptr<RPipelineCache> createPipelineCache(
        const PipelineCacheDescriptor& Desc = {}) override;
    std::shared_ptr<RPipeline> createGraphicsPipeline(
        const GraphicsPipelineDescriptor& Desc) override;
    std::shared_ptr<RPipeline> createComputePipeline(
        const ComputePipelineDescriptor& Desc) override;
    std::shared_ptr<RPipeline> createRayTracingPipeline(
        const RayTracingPipelineDescriptor& Desc) override;
    std::shared_ptr<RAccelerationStructure> createAccelerationStructure(
        const AccelerationStructureDescriptor& Desc) override;
    [[nodiscard]] AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(
        EAccelerationStructureType Type,
        EAccelerationStructureBuildFlags Flags,
        std::span<const AccelerationStructureGeometry> Geometries) const override;
    [[nodiscard]] std::vector<std::byte> getRayTracingShaderGroupHandles(
        const std::shared_ptr<RPipeline>& Pipeline,
        uint32_t FirstGroup,
        uint32_t GroupCount) const override;
    std::shared_ptr<RCommandList> createCommandList(
        const CommandListDescriptor& Desc = {}) override;
    std::shared_ptr<RSwapchain> createSwapchain(const SwapchainDescriptor& Desc) override;
    std::shared_ptr<RQueue> getQueue(ECommandQueueType Type) override;
    std::shared_ptr<RFence> createFence(bool Signaled = false) override;
    std::shared_ptr<RSemaphore> createSemaphore() override;
    std::shared_ptr<RSemaphore> createTimelineSemaphore(uint64_t InitialValue = 0) override;
    std::shared_ptr<RQueryPool> createQueryPool(const QueryPoolDescriptor& Desc) override;
    [[nodiscard]] FormatCapabilities getFormatCapabilities(EFormat Format) const override;
	[[nodiscard]] std::optional<SwapchainCapabilities> getSwapchainCapabilities() const override;
	std::shared_ptr<RTexture> createTexture(const RTexture::Descriptor_t& Desc) override;
    std::shared_ptr<DeviceMemory> allocateMemory(
        MemoryRequirements Requirements,
        EMemoryProperty Property) override;
    std::shared_ptr<DeviceMemory> allocateMemory(
        const MemoryAllocationDescriptor& Desc) override;
    void freeMemory(std::shared_ptr<DeviceMemory> Memory) override;
    [[nodiscard]] bool deferRelease(
        std::shared_ptr<void> Resource,
        const std::shared_ptr<RSemaphore>& CompletionSemaphore,
        uint64_t CompletionValue) override;
    void collectDeferredReleases() override;

	[[nodiscard]] std::shared_ptr<DeviceMemory> allocateBufferMemory(
		const MemoryAllocationDescriptor& Desc,
        bool RequireDeviceAddress,
        vk::Buffer Buffer);
    [[nodiscard]] std::shared_ptr<DeviceMemory> allocateImageMemory(
        const MemoryAllocationDescriptor& Desc,
        vk::Image Image);

	void waitIdle() override;
	void* getNativeHandle() const override;
    [[nodiscard]] const DeviceLimits& getLimits() const noexcept override { return Limits; }
    [[nodiscard]] const DeviceFeatures& getFeatures() const noexcept override { return Features; }
    [[nodiscard]] EDeviceStatus getStatus() const noexcept override
    {
        return Context->DeviceStatus.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint32_t getGraphicsQueueFamilyIndex() const noexcept
    {
        return Context->GraphicsQueueFamilyIndex;
    }
    [[nodiscard]] vk::Queue getGraphicsQueue() const noexcept { return GraphicsQueue; }
private:
	void requireReady() const;
    struct DeferredReleaseEntry
    {
        uint64_t Sequence { 0 };
        uint64_t CompletionValue { 0 };
        std::shared_ptr<RSemaphore> CompletionSemaphore;
        std::shared_ptr<void> Resource;
    };
    void drainDeferredReleases() noexcept;

	VulkanContextPtr Context;
    vk::Queue GraphicsQueue;
    DeviceLimits Limits;
    DeviceFeatures Features;
    std::shared_ptr<VulkanDescriptorAllocator> DescriptorAllocator;
	std::array<std::shared_ptr<RQueue>, 3> Queues;
    std::unique_ptr<VulkanMemoryAllocator> MemoryAllocator;
    std::mutex DeferredReleaseMutex;
    std::vector<DeferredReleaseEntry> DeferredReleases;
    uint64_t NextDeferredReleaseSequence { 0 };
};

} // namespace rhi