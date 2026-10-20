#pragma once

#include <RHI.hpp>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** @brief Immutable Vulkan sampler resource. */
class VulkanSampler final : public RSampler
{
public:
    /**
     * @brief Creates a Vulkan sampler from a backend-neutral descriptor.
     * @param Device Device that owns the sampler.
     * @param Desc Sampler filtering, addressing and comparison state.
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
