#pragma once

#include <RHI.hpp>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** @brief Vulkan 后端的高层纹理, 组合 VulkanImage + VulkanImageView + VulkanSampler. */
class VulkanTexture final : public RTexture
{
public:
    ~VulkanTexture() override = default;
    VulkanTexture(const VulkanTexture&) = delete;
    VulkanTexture& operator=(const VulkanTexture&) = delete;
    VulkanTexture(VulkanTexture&&) = delete;
    VulkanTexture& operator=(VulkanTexture&&) = delete;

    /**
     * @brief 创建后备 image、默认全范围视图和默认 sampler。
     * @param Device 拥有该 texture 及其各组成部分的设备。
     * @param Desc 不可变的 texture 描述。
     * @return 一个完整构建的 Vulkan texture。
     */
    [[nodiscard]] static std::shared_ptr<VulkanTexture> create(
        VulkanDevice& Device,
        const Descriptor_t& Desc);

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] const Descriptor_t& getDescriptor() const noexcept override { return Descriptor; }
    [[nodiscard]] const std::shared_ptr<RImage>& getImage() const noexcept override { return Image; }
    [[nodiscard]] const std::shared_ptr<RImageView>& getImageView() const noexcept override { return ImageView; }
    [[nodiscard]] const std::shared_ptr<RSampler>& getSampler() const noexcept override { return Sampler; }
    [[nodiscard]] bool isValid() const noexcept override;

private:
    VulkanTexture(
        VulkanDevice& Device,
        Descriptor_t Desc,
        std::shared_ptr<RImage> Image,
        std::shared_ptr<RImageView> ImageView,
        std::shared_ptr<RSampler> Sampler);

    VulkanDevice* Device = nullptr;
    Descriptor_t Descriptor;
    std::shared_ptr<RImage> Image;
    std::shared_ptr<RImageView> ImageView;
    std::shared_ptr<RSampler> Sampler;
};

} // namespace rhi
