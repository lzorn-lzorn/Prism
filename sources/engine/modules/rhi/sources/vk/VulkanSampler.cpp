#include "VulkanSampler.hpp"

#include "VulkanDevice.hpp"
#include "VulkanRHI.hpp"

#include <stdexcept>

namespace rhi
{

VulkanSampler::VulkanSampler(VulkanDevice& InDevice, const RSampler::Descriptor_t& Desc)
    : Device(&InDevice), Descriptor(Desc)
{
    if (Desc.MinLod > Desc.MaxLod)
        throw std::invalid_argument("Sampler minimum LOD must not exceed maximum LOD.");
    if (Desc.MaxAnisotropy < 1.0f)
        throw std::invalid_argument("Sampler anisotropy must be at least one.");
    if (Desc.MaxAnisotropy > 1.0f && !Device->getFeatures().SamplerAnisotropy)
        throw std::invalid_argument("Sampler anisotropy is unsupported by this device.");

    vk::SamplerCreateInfo CreateInfo;
    CreateInfo
        .setMagFilter(toVk(Desc.MagFilter))
        .setMinFilter(toVk(Desc.MinFilter))
        .setMipmapMode(toVkMipmapMode(Desc.MipmapFilter))
        .setAddressModeU(toVk(Desc.AddressU))
        .setAddressModeV(toVk(Desc.AddressV))
        .setAddressModeW(toVk(Desc.AddressW))
        .setMipLodBias(Desc.MipLodBias)
        .setAnisotropyEnable(Desc.MaxAnisotropy > 1.0f)
        .setMaxAnisotropy(Desc.MaxAnisotropy)
        .setCompareEnable(Desc.CompareEnable)
        .setCompareOp(toVk(Desc.CompareOperation))
        .setMinLod(Desc.MinLod)
        .setMaxLod(Desc.MaxLod)
        .setBorderColor(vk::BorderColor::eFloatTransparentBlack)
        .setUnnormalizedCoordinates(false);
    Sampler = Device->getVkDevice().createSamplerUnique(CreateInfo);
}

RDevice& VulkanSampler::getDevice() const noexcept
{
    return *Device;
}

void* VulkanSampler::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkSampler>(Sampler.get()));
}

} // namespace rhi
