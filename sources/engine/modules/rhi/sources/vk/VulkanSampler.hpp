#pragma once

#include <RHI.hpp>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** @brief 不可变的 Vulkan sampler 资源。 */
class VulkanSampler final : public RSampler
{
public:
    /**
     * @brief 根据与后端无关的描述符创建 Vulkan sampler。
     * @param Device 拥有该 sampler 的设备。
     * @param Desc sampler 的过滤、寻址与比较状态。
     */
    VulkanSampler(VulkanDevice& Device, const Descriptor_t& Desc);
    ~VulkanSampler() override = default;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] const Descriptor_t& getDescriptor() const noexcept override { return Descriptor; }
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(Sampler); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::Sampler getVkSampler() const noexcept { return Sampler.get(); }

private:
    VulkanDevice* Device { nullptr };
    Descriptor_t Descriptor;
    vk::UniqueSampler Sampler;
};

} // namespace rhi
