#pragma once

#include "VulkanContext.hpp"
#include <RHI.hpp>
#include <atomic>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

class VulkanSwapchain final : public RSwapchain
{
public:
    VulkanSwapchain(VulkanDevice& Device, VulkanContextPtr Context, SwapchainDescriptor Desc);
    ~VulkanSwapchain() override;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] const SwapchainDescriptor& getDescriptor() const noexcept override { return Descriptor; }
    [[nodiscard]] EFormat getFormat() const noexcept override { return Format; }
    [[nodiscard]] EColorSpace getColorSpace() const noexcept override { return ColorSpace; }
    [[nodiscard]] ESwapchainStatus getStatus() const noexcept override;
    [[nodiscard]] uint64_t getGeneration() const noexcept override { return Generation; }
    [[nodiscard]] uint32_t getImageCount() const noexcept override { return static_cast<uint32_t>(Images.size()); }
    [[nodiscard]] const std::shared_ptr<RImage>& getImage(uint32_t Index) const override;
    [[nodiscard]] const std::shared_ptr<RImageView>& getImageView(uint32_t Index) const override;
    [[nodiscard]] AcquireResult acquireNextImage(
        const std::shared_ptr<RSemaphore>& SignalSemaphore,
        const std::shared_ptr<RFence>& SignalFence,
        uint64_t TimeoutNanoseconds) override;
    void recreate(uint32_t Width, uint32_t Height) override;
    [[nodiscard]] bool recoverSurface(uint32_t Width, uint32_t Height) override;
    [[nodiscard]] bool setHDRMetadata(const HDRMetadata& Metadata) override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::SwapchainKHR getVkSwapchain() const noexcept { return Swapchain.get(); }
    void recordStatus(ESwapchainStatus NewStatus) noexcept;

private:
    void create(vk::SwapchainKHR OldSwapchain = VK_NULL_HANDLE);
    void releaseImages();
    bool retirePresentGeneration() noexcept;

    VulkanDevice* Device;
    VulkanContextPtr Context;
    SwapchainDescriptor Descriptor;
    EFormat Format { EFormat::Undefined };
    EColorSpace ColorSpace { EColorSpace::SRGB_Nonlinear };
    std::shared_ptr<VulkanSurfaceState> Surface;
    mutable std::atomic<ESwapchainStatus> Status { ESwapchainStatus::OutOfDate };
    vk::UniqueSwapchainKHR Swapchain;
    std::vector<std::shared_ptr<RImage>> Images;
    std::vector<std::shared_ptr<RImageView>> ImageViews;
    uint64_t Generation { 0 };
};

} // namespace rhi
