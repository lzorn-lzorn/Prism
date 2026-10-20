#include "VulkanImageView.hpp"

#include "VulkanDevice.hpp"
#include "VulkanImage.hpp"
#include "VulkanRHI.hpp"

#include <stdexcept>
#include <utility>

namespace rhi
{
namespace
{
EImageAspect inferAspect(EFormat Format)
{
    if (hasDepthAspect(Format) && hasStencilAspect(Format))
    {
        return EImageAspect::DepthStencil;
    }
    if (hasDepthAspect(Format))
    {
        return EImageAspect::Depth;
    }
    if (hasStencilAspect(Format))
    {
        return EImageAspect::Stencil;
    }
    return EImageAspect::Color;
}

void validateAspect(EImageAspect Aspect, EFormat Format)
{
    const bool has_depth = hasDepthAspect(Format);
    const bool has_stencil = hasStencilAspect(Format);

    switch (Aspect)
    {
    case EImageAspect::Color:
        if (has_depth || has_stencil)
            throw std::invalid_argument("A color image view requires a color format.");
        break;
    case EImageAspect::Depth:
        if (!has_depth)
            throw std::invalid_argument("A depth image view requires a depth format.");
        break;
    case EImageAspect::Stencil:
        if (!has_stencil)
            throw std::invalid_argument("A stencil image view requires a stencil format.");
        break;
    case EImageAspect::DepthStencil:
        if (!has_depth || !has_stencil)
            throw std::invalid_argument("A depth-stencil image view requires both format aspects.");
        break;
    case EImageAspect::Auto:
        throw std::logic_error("Image view aspect was not resolved.");
    }
}

EImageViewDimension inferDimension(
    EImageDimension ImageDimension,
    uint32_t BaseArrayLayer,
    uint32_t ArrayLayerCount)
{
    switch (ImageDimension)
    {
    case EImageDimension::Texture1D:
        return EImageViewDimension::Texture1D;
    case EImageDimension::Texture1DArray:
        return ArrayLayerCount == 1
            ? EImageViewDimension::Texture1D
            : EImageViewDimension::Texture1DArray;
    case EImageDimension::Texture2D:
        return EImageViewDimension::Texture2D;
    case EImageDimension::Texture2DArray:
        return ArrayLayerCount == 1
            ? EImageViewDimension::Texture2D
            : EImageViewDimension::Texture2DArray;
    case EImageDimension::Texture3D:
        return EImageViewDimension::Texture3D;
    case EImageDimension::Cube:
    case EImageDimension::CubeArray:
        if (ArrayLayerCount == 1)
            return EImageViewDimension::Texture2D;
        if (BaseArrayLayer % 6 == 0 && ArrayLayerCount == 6)
            return EImageViewDimension::Cube;
        if (BaseArrayLayer % 6 == 0 && ArrayLayerCount % 6 == 0)
            return EImageViewDimension::CubeArray;
        return EImageViewDimension::Texture2DArray;
    }

    throw std::invalid_argument("Unsupported image dimension.");
}

bool isDimensionCompatible(EImageDimension ImageDimension, EImageViewDimension ViewDimension)
{
    switch (ImageDimension)
    {
    case EImageDimension::Texture1D:
        return ViewDimension == EImageViewDimension::Texture1D;
    case EImageDimension::Texture1DArray:
        return ViewDimension == EImageViewDimension::Texture1D ||
            ViewDimension == EImageViewDimension::Texture1DArray;
    case EImageDimension::Texture2D:
        return ViewDimension == EImageViewDimension::Texture2D;
    case EImageDimension::Texture2DArray:
        return ViewDimension == EImageViewDimension::Texture2D ||
            ViewDimension == EImageViewDimension::Texture2DArray;
    case EImageDimension::Texture3D:
        return ViewDimension == EImageViewDimension::Texture3D;
    case EImageDimension::Cube:
    case EImageDimension::CubeArray:
        return ViewDimension == EImageViewDimension::Texture2D ||
            ViewDimension == EImageViewDimension::Texture2DArray ||
            ViewDimension == EImageViewDimension::Cube ||
            ViewDimension == EImageViewDimension::CubeArray;
    }

    return false;
}

void validateDimension(const RImageView::Descriptor_t& Desc, EImageDimension ImageDimension)
{
    if (!isDimensionCompatible(ImageDimension, Desc.Dimension))
    {
        throw std::invalid_argument("Image view dimension is incompatible with the image dimension.");
    }

    switch (Desc.Dimension)
    {
    case EImageViewDimension::Texture1D:
    case EImageViewDimension::Texture2D:
    case EImageViewDimension::Texture3D:
        if (Desc.ArrayLayerCount != 1)
            throw std::invalid_argument("A non-array image view requires exactly one array layer.");
        break;
    case EImageViewDimension::Cube:
        if (Desc.BaseArrayLayer % 6 != 0 || Desc.ArrayLayerCount != 6)
            throw std::invalid_argument("A cube image view requires six aligned array layers.");
        break;
    case EImageViewDimension::CubeArray:
        if (Desc.BaseArrayLayer % 6 != 0 ||
            Desc.ArrayLayerCount < 6 || Desc.ArrayLayerCount % 6 != 0)
        {
            throw std::invalid_argument(
                "A cube-array image view requires aligned layers in groups of six.");
        }
        break;
    case EImageViewDimension::Texture1DArray:
    case EImageViewDimension::Texture2DArray:
        break;
    case EImageViewDimension::Auto:
        throw std::logic_error("Image view dimension was not resolved.");
    }
}
} // namespace

VulkanImageView::VulkanImageView(Descriptor_t Desc, vk::UniqueImageView InImageView)
    : RImageView(std::move(Desc)), ImageView(std::move(InImageView))
{
}

std::shared_ptr<VulkanImageView> VulkanImageView::create(
    VulkanDevice& Device,
    const Descriptor_t& Desc)
{
    auto normalized_desc = normalizeDescriptor(Device, Desc);
    auto* image = static_cast<VulkanImage*>(normalized_desc.Image.get());
    auto image_view = Device.getVkDevice().createImageViewUnique(
        makeCreateInfo(normalized_desc, image->getVkImage()));

    return std::shared_ptr<VulkanImageView>(
        new VulkanImageView(std::move(normalized_desc), std::move(image_view)));
}

void* VulkanImageView::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkImageView>(ImageView.get()));
}

RImageView::Descriptor_t VulkanImageView::normalizeDescriptor(
    VulkanDevice& Device,
    const Descriptor_t& Desc)
{
    if (!Desc.Image)
        throw std::invalid_argument("Vulkan image view requires an image.");
    if (!Desc.Image->isValid())
        throw std::invalid_argument("Vulkan image view requires a valid image.");

    auto* image = static_cast<VulkanImage*>(Desc.Image.get());
    if (&image->getDevice() != &Device)
        throw std::invalid_argument("Vulkan image and image view must belong to the same device.");

    const auto& image_desc = Desc.Image->getDescriptor();
    Descriptor_t result = Desc;
    if (result.Format == EFormat::Undefined)
    {
        result.Format = image_desc.Format;
    }
    else if (result.Format != image_desc.Format)
    {
        throw std::invalid_argument("Vulkan image view format reinterpretation is not supported.");
    }

    if (result.MipLevelCount == 0 || result.ArrayLayerCount == 0)
        throw std::invalid_argument("Image view mip and array layer counts must be non-zero.");
    if (result.BaseMipLevel >= image_desc.MipLevels ||
        result.MipLevelCount > image_desc.MipLevels - result.BaseMipLevel)
    {
        throw std::out_of_range("Image view mip range exceeds the image.");
    }
    if (result.BaseArrayLayer >= image_desc.ArrayLayers ||
        result.ArrayLayerCount > image_desc.ArrayLayers - result.BaseArrayLayer)
    {
        throw std::out_of_range("Image view array layer range exceeds the image.");
    }
    if (image_desc.Dimension == EImageDimension::Texture3D &&
        (result.BaseArrayLayer != 0 || result.ArrayLayerCount != 1))
    {
        throw std::invalid_argument("A 3D image view must use its only array layer.");
    }

    if (result.Aspect == EImageAspect::Auto)
        result.Aspect = inferAspect(result.Format);
    validateAspect(result.Aspect, result.Format);

    if (result.Dimension == EImageViewDimension::Auto)
    {
        result.Dimension = inferDimension(
            image_desc.Dimension,
            result.BaseArrayLayer,
            result.ArrayLayerCount);
    }
    validateDimension(result, image_desc.Dimension);
    return result;
}

vk::ImageViewCreateInfo VulkanImageView::makeCreateInfo(
    const Descriptor_t& Desc,
    vk::Image Image)
{
    return vk::ImageViewCreateInfo()
        .setImage(Image)
        .setViewType(toVk(Desc.Dimension))
        .setFormat(toVk(Desc.Format))
        .setComponents(vk::ComponentMapping(
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity,
            vk::ComponentSwizzle::eIdentity))
        .setSubresourceRange(vk::ImageSubresourceRange(
            toVk(Desc.Aspect),
            Desc.BaseMipLevel,
            Desc.MipLevelCount,
            Desc.BaseArrayLayer,
            Desc.ArrayLayerCount));
}

} // namespace rhi
