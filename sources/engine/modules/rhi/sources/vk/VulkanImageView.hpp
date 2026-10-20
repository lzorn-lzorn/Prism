#pragma once

#include <RHI.hpp>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

class VulkanImageView final : public RImageView
{
public:
    ~VulkanImageView() override = default;
    [[nodiscard]] RDevice& getDevice() const noexcept override { return getImage()->getDevice(); }
    VulkanImageView(const VulkanImageView&) = delete;
    VulkanImageView& operator=(const VulkanImageView&) = delete;
    VulkanImageView(VulkanImageView&&) = delete;
    VulkanImageView& operator=(VulkanImageView&&) = delete;

    [[nodiscard]] static std::shared_ptr<VulkanImageView> create(
        VulkanDevice& Device,
        const Descriptor_t& Desc);

    [[nodiscard]] bool isValid() const noexcept override
    {
        return static_cast<bool>(ImageView);
    }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::ImageView getVkImageView() const noexcept { return ImageView.get(); }

private:
    VulkanImageView(Descriptor_t Desc, vk::UniqueImageView ImageView);

    [[nodiscard]] static Descriptor_t normalizeDescriptor(
        VulkanDevice& Device,
        const Descriptor_t& Desc);
    [[nodiscard]] static vk::ImageViewCreateInfo makeCreateInfo(
        const Descriptor_t& Desc,
        vk::Image Image);

    vk::UniqueImageView ImageView;
};

} // namespace rhi
