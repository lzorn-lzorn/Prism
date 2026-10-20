#include "VulkanTexture.hpp"

#include "VulkanDevice.hpp"
#include "VulkanRHI.hpp"

#include <stdexcept>
#include <utility>

namespace rhi
{

VulkanTexture::VulkanTexture(
    VulkanDevice& InDevice,
    Descriptor_t Desc,
    std::shared_ptr<RImage> InImage,
    std::shared_ptr<RImageView> InImageView,
    std::shared_ptr<RSampler> InSampler)
    : Device(&InDevice)
    , Descriptor(std::move(Desc))
    , Image(std::move(InImage))
    , ImageView(std::move(InImageView))
    , Sampler(std::move(InSampler))
{
}

std::shared_ptr<VulkanTexture> VulkanTexture::create(
    VulkanDevice& Device,
    const Descriptor_t& Desc)
{
    if (Desc.Image.Format == EFormat::Undefined)
        throw std::invalid_argument("Vulkan texture image format must be defined.");

    // 规范化为后端实际使用的描述, 使 getDescriptor() 能反映真实创建参数.
    Descriptor_t Normalized = Desc;
    if (!Normalized.Image.Usage)
    {
        // 纹理本质上是可采样的; 同时默认允许作为拷贝目标, 方便后续上传.
        Normalized.Image.Usage.set(EImageUsage_t::Sampled);
        Normalized.Image.Usage.set(EImageUsage_t::TransferDst);
    }
    if (!Normalized.Image.Usage.has(EImageUsage_t::Sampled))
        throw std::invalid_argument("A texture image must declare the Sampled usage.");

    auto Image = Device.createImage(Normalized.Image);

    // 默认视图覆盖完整 mip/layer 范围; 格式/维度/aspect 均由后端从图像推导.
    RImageView::Descriptor_t ViewDesc;
    ViewDesc.Image = Image;
    ViewDesc.Format = EFormat::Undefined;
    ViewDesc.Dimension = EImageViewDimension::Auto;
    ViewDesc.Aspect = EImageAspect::Auto;
    ViewDesc.BaseMipLevel = 0;
    ViewDesc.MipLevelCount = Normalized.Image.MipLevels;
    ViewDesc.BaseArrayLayer = 0;
    ViewDesc.ArrayLayerCount = Normalized.Image.ArrayLayers;
    auto ImageView = Device.createImageView(ViewDesc);

    auto Sampler = Device.createSampler(Normalized.Sampler);

    return std::shared_ptr<VulkanTexture>(new VulkanTexture(
        Device,
        std::move(Normalized),
        std::move(Image),
        std::move(ImageView),
        std::move(Sampler)));
}

RDevice& VulkanTexture::getDevice() const noexcept
{
    return *Device;
}

bool VulkanTexture::isValid() const noexcept
{
    return Image && Image->isValid() &&
        ImageView && ImageView->isValid() &&
        Sampler && Sampler->isValid();
}

} // namespace rhi
