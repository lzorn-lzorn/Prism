#include "VulkanSwapchain.hpp"

#include "VulkanDevice.hpp"
#include "VulkanImage.hpp"
#include "VulkanRHI.hpp"
#include "VulkanSync.hpp"
#include "VulkanWSI.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace rhi
{
namespace
{
EFormat fromVkFormat(vk::Format Format)
{
    switch (Format)
    {
    case vk::Format::eR8G8B8A8Unorm: return EFormat::RGBA8_UNorm;
    case vk::Format::eR8G8B8A8Srgb: return EFormat::RGBA8_sRGB;
    case vk::Format::eB8G8R8A8Unorm: return EFormat::BGRA8_UNorm;
    case vk::Format::eB8G8R8A8Srgb: return EFormat::BGRA8_sRGB;
    case vk::Format::eA2B10G10R10UnormPack32: return EFormat::RGB10A2_UNorm;
    case vk::Format::eR16G16B16A16Sfloat: return EFormat::RGBA16_Float;
    default: return EFormat::Undefined;
    }
}

std::optional<EColorSpace> fromVkColorSpace(vk::ColorSpaceKHR ColorSpace)
{
    switch (ColorSpace)
    {
    case vk::ColorSpaceKHR::eSrgbNonlinear: return EColorSpace::SRGB_Nonlinear;
    case vk::ColorSpaceKHR::eAdobergbNonlinearEXT: return EColorSpace::AdobeRGB;
    case vk::ColorSpaceKHR::eDciP3NonlinearEXT: return EColorSpace::DCIP3;
    case vk::ColorSpaceKHR::eBt2020LinearEXT: return EColorSpace::Rec2020;
    case vk::ColorSpaceKHR::eHdr10St2084EXT: return EColorSpace::HDR10_ST2084;
    case vk::ColorSpaceKHR::eExtendedSrgbLinearEXT: return EColorSpace::ExtendedSRGBLinear;
    default: return std::nullopt;
    }
}

vk::CompositeAlphaFlagBitsKHR chooseCompositeAlpha(vk::CompositeAlphaFlagsKHR Supported)
{
    constexpr std::array candidates {
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
        vk::CompositeAlphaFlagBitsKHR::eInherit };
    for (const auto candidate : candidates)
        if (Supported & candidate) return candidate;
    throw std::runtime_error("The Vulkan surface exposes no supported composite alpha mode.");
}

VulkanSemaphore* semaphoreOf(const std::shared_ptr<RSemaphore>& Semaphore, VulkanDevice& Device)
{
    if (!Semaphore) return nullptr;
    auto* result = dynamic_cast<VulkanSemaphore*>(Semaphore.get());
    if (!result || &result->getDevice() != &Device || result->isTimeline())
        throw std::invalid_argument("Swapchain acquisition requires a binary semaphore from the same device.");
    return result;
}

VulkanFence* fenceOf(const std::shared_ptr<RFence>& Fence, VulkanDevice& Device)
{
    if (!Fence) return nullptr;
    auto* result = dynamic_cast<VulkanFence*>(Fence.get());
    if (!result || &result->getDevice() != &Device)
        throw std::invalid_argument("Acquire fence belongs to another RHI device or backend.");
    return result;
}
}

VulkanSwapchain::VulkanSwapchain(
    VulkanDevice& InDevice,
    VulkanContextPtr InContext,
    SwapchainDescriptor Desc)
    : Device(&InDevice)
    , Context(std::move(InContext))
    , Descriptor(std::move(Desc))
    , Surface(Context->Surface)
{
    if (!Surface)
        throw std::logic_error("Vulkan swapchain creation requires an initialized presentation surface.");
    if (Descriptor.MinimumImageCount == 0)
        throw std::invalid_argument("Swapchain image count must be non-zero.");
    if (Descriptor.Width != 0 && Descriptor.Height != 0)
        create();
}

VulkanSwapchain::~VulkanSwapchain()
{
    // A graphics timeline cannot prove that the presentation engine consumed its binary
    // wait semaphore. Queue-idle is therefore the conservative destruction boundary.
    retirePresentGeneration();
    releaseImages();
    Swapchain.reset();
}

RDevice& VulkanSwapchain::getDevice() const noexcept { return *Device; }

const std::shared_ptr<RImage>& VulkanSwapchain::getImage(uint32_t Index) const
{
    if (Index >= Images.size()) throw std::out_of_range("Swapchain image index is out of range.");
    return Images[Index];
}

const std::shared_ptr<RImageView>& VulkanSwapchain::getImageView(uint32_t Index) const
{
    if (Index >= ImageViews.size()) throw std::out_of_range("Swapchain view index is out of range.");
    return ImageViews[Index];
}

AcquireResult VulkanSwapchain::acquireNextImage(
    const std::shared_ptr<RSemaphore>& SignalSemaphore,
    const std::shared_ptr<RFence>& SignalFence,
    uint64_t TimeoutNanoseconds)
{
    if (Context->isDeviceLost()) return { EAcquireStatus::DeviceLost, 0, 0 };
    if (Status.load(std::memory_order_relaxed) == ESwapchainStatus::SurfaceLost ||
        !Surface || Surface != Context->Surface)
        return { EAcquireStatus::SurfaceLost, 0, 0 };
    if (!Swapchain) return { EAcquireStatus::NotReady, 0, 0 };
    auto* semaphore = semaphoreOf(SignalSemaphore, *Device);
    auto* fence = fenceOf(SignalFence, *Device);
    if (!semaphore && !fence)
        throw std::invalid_argument("Swapchain acquisition requires a semaphore or fence signal target.");
    try
    {
        const auto result = Context->Device->acquireNextImageKHR(
            Swapchain.get(),
            TimeoutNanoseconds,
            semaphore ? semaphore->getVkSemaphore() : vk::Semaphore{},
            fence ? fence->getVkFence() : vk::Fence{});
        const auto status = vulkan_wsi::mapAcquire(result.result);
        if (!status) throw std::runtime_error("Unexpected Vulkan image-acquisition result.");
        recordStatus(vulkan_wsi::mapStatus(result.result).value_or(ESwapchainStatus::OutOfDate));
        return { *status, result.value,
            (*status == EAcquireStatus::Success || *status == EAcquireStatus::Suboptimal) ? Generation : 0 };
    }
    catch (const vk::SystemError& error)
    {
        const auto result = vulkan_wsi::resultFromSystemError(error);
        const auto status = vulkan_wsi::mapAcquire(result);
        if (status)
        {
            if (const auto health = vulkan_wsi::mapStatus(result)) recordStatus(*health);
            return { *status, 0, 0 };
        }
        throw;
    }
}

void VulkanSwapchain::recreate(uint32_t Width, uint32_t Height)
{
    Descriptor.Width = Width;
    Descriptor.Height = Height;
    if (Width == 0 || Height == 0)
    {
        if (!retirePresentGeneration()) return;
        releaseImages();
        Swapchain.reset();
        Status.store(ESwapchainStatus::OutOfDate, std::memory_order_relaxed);
        return;
    }
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    if (!Surface || Surface != Context->Surface)
        throw std::runtime_error("Vulkan surface generation changed; call recoverSurface() on the swapchain.");
    if (!retirePresentGeneration())
        throw std::runtime_error("Vulkan swapchain retirement failed because the device was lost.");
    vk::UniqueSwapchainKHR old = std::move(Swapchain);
    releaseImages();
    create(old.get());
}

bool VulkanSwapchain::recoverSurface(uint32_t Width, uint32_t Height)
{
    if (Context->isDeviceLost() || !Context->Surface) return false;
    if (!retirePresentGeneration()) return false;
    releaseImages();
    Swapchain.reset();
    Surface = Context->Surface;
    Descriptor.Width = Width;
    Descriptor.Height = Height;
    if (Width == 0 || Height == 0)
    {
        Status.store(ESwapchainStatus::OutOfDate, std::memory_order_relaxed);
        return true;
    }
    try { create(); return true; }
    catch (const vk::SystemError& error)
    {
        if (const auto mapped = vulkan_wsi::mapStatus(vulkan_wsi::resultFromSystemError(error)))
            recordStatus(*mapped);
        return false;
    }
    catch (...) { return false; }
}

ESwapchainStatus VulkanSwapchain::getStatus() const noexcept
{
    if (Context->isDeviceLost()) return ESwapchainStatus::DeviceLost;
    if (!Surface || Surface != Context->Surface) return ESwapchainStatus::SurfaceLost;
    const auto persistent = Status.load(std::memory_order_relaxed);
    if (persistent == ESwapchainStatus::SurfaceLost || persistent == ESwapchainStatus::DeviceLost)
        return persistent;
    VkSurfaceCapabilitiesKHR capabilities {};
    const VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        static_cast<VkPhysicalDevice>(Context->PhysicalDevice),
        static_cast<VkSurfaceKHR>(Surface->Handle), &capabilities);
    if (const auto mapped = vulkan_wsi::mapStatus(static_cast<vk::Result>(result)))
    {
        if (*mapped == ESwapchainStatus::DeviceLost) Context->markDeviceLost();
        if (*mapped == ESwapchainStatus::SurfaceLost || *mapped == ESwapchainStatus::DeviceLost)
            Status.store(*mapped, std::memory_order_relaxed);
        return result == VK_SUCCESS ? persistent : *mapped;
    }
    return persistent;
}

void VulkanSwapchain::recordStatus(ESwapchainStatus NewStatus) noexcept
{
    Status.store(NewStatus, std::memory_order_relaxed);
    if (NewStatus == ESwapchainStatus::DeviceLost) Context->markDeviceLost();
}

bool VulkanSwapchain::retirePresentGeneration() noexcept
{
    if (!Swapchain || Context->isDeviceLost()) return !Context->isDeviceLost();
    try
    {
        // Present completion is outside the graphics timeline domain. Waiting only the
        // presentation queue is the narrow synchronization that proves WSI binary waits
        // were consumed and the old swapchain can be destroyed; no device-wide idle occurs.
        Context->Device->getQueue(Context->PresentQueueFamilyIndex, 0).waitIdle();
        return true;
    }
    catch (const vk::SystemError& error)
    {
        if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
            Context->markDeviceLost();
        return false;
    }
}

void* VulkanSwapchain::getNativeHandle() const noexcept
{
    return static_cast<VkSwapchainKHR>(Swapchain.get());
}

void VulkanSwapchain::create(vk::SwapchainKHR OldSwapchain)
{
    const auto capabilities = Context->PhysicalDevice.getSurfaceCapabilitiesKHR(Surface->Handle);
    const auto formats = Context->PhysicalDevice.getSurfaceFormatsKHR(Surface->Handle);
    const auto present_modes = Context->PhysicalDevice.getSurfacePresentModesKHR(Surface->Handle);
    if (formats.empty() || present_modes.empty())
        throw std::runtime_error("The Vulkan surface has no swapchain formats or present modes.");

    auto selected_format = std::ranges::find_if(formats, [&](const vk::SurfaceFormatKHR& candidate)
    {
        return candidate.format == toVk(Descriptor.PreferredFormat) &&
            candidate.colorSpace == toVk(Descriptor.ColorSpace);
    });
    if (selected_format == formats.end() &&
        (Descriptor.RequireExactFormatAndColorSpace || Descriptor.HDR))
        throw std::runtime_error("The explicitly required swapchain format/color space is unavailable.");
    if (selected_format == formats.end())
        selected_format = std::ranges::find_if(formats, [](const vk::SurfaceFormatKHR& candidate)
        {
            return fromVkFormat(candidate.format) != EFormat::Undefined;
        });
    if (selected_format == formats.end())
        throw std::runtime_error("No supported swapchain format maps to an RHI format.");
    Format = fromVkFormat(selected_format->format);
    ColorSpace = fromVkColorSpace(selected_format->colorSpace).value_or(EColorSpace::SRGB_Nonlinear);

    vk::PresentModeKHR selected_present_mode = vk::PresentModeKHR::eFifo;
    const vk::PresentModeKHR preferred = toVk(Descriptor.PreferredPresentMode);
    if (std::ranges::find(present_modes, preferred) != present_modes.end())
        selected_present_mode = preferred;

    vk::Extent2D extent;
    if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
        extent = capabilities.currentExtent;
    else
    {
        extent.width = std::clamp(Descriptor.Width,
            capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
        extent.height = std::clamp(Descriptor.Height,
            capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
    }
    Descriptor.Width = extent.width;
    Descriptor.Height = extent.height;

    uint32_t image_count = std::max(Descriptor.MinimumImageCount, capabilities.minImageCount);
    if (capabilities.maxImageCount != 0)
        image_count = std::min(image_count, capabilities.maxImageCount);
    const vk::ImageUsageFlags requested_usage = toVk(Descriptor.ImageUsage);
    if ((capabilities.supportedUsageFlags & requested_usage) != requested_usage)
        throw std::invalid_argument("Requested swapchain image usage is not supported by the surface.");

    const uint32_t families[] = {
        Context->GraphicsQueueFamilyIndex,
        Context->PresentQueueFamilyIndex };
    const bool concurrent = families[0] != families[1];
    vk::SwapchainCreateInfoKHR create_info;
    create_info.setSurface(Surface->Handle)
        .setMinImageCount(image_count)
        .setImageFormat(selected_format->format)
        .setImageColorSpace(selected_format->colorSpace)
        .setImageExtent(extent)
        .setImageArrayLayers(1)
        .setImageUsage(requested_usage)
        .setImageSharingMode(concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
        .setPreTransform(capabilities.currentTransform)
        .setCompositeAlpha(chooseCompositeAlpha(capabilities.supportedCompositeAlpha))
        .setPresentMode(selected_present_mode)
        .setClipped(Descriptor.Clipped)
        .setOldSwapchain(OldSwapchain);
    if (concurrent)
        create_info.setQueueFamilyIndices(families);
    Swapchain = Context->Device->createSwapchainKHRUnique(create_info);
    if (++Generation == 0) ++Generation;
    Status.store(ESwapchainStatus::Ready, std::memory_order_relaxed);

    const auto native_images = Context->Device->getSwapchainImagesKHR(Swapchain.get());
    Images.reserve(native_images.size());
    ImageViews.reserve(native_images.size());
    for (const auto image : native_images)
    {
        RImage::Descriptor_t image_desc;
        image_desc.Format = Format;
        image_desc.Dimension = EImageDimension::Texture2D;
        image_desc.Width = Descriptor.Width;
        image_desc.Height = Descriptor.Height;
        image_desc.Usage = Descriptor.ImageUsage;
        auto wrapped = VulkanImage::wrapExternal(*Device, image_desc, image);
        RImageView::Descriptor_t view_desc;
        view_desc.Image = wrapped;
        view_desc.Format = Format;
        view_desc.Dimension = EImageViewDimension::Texture2D;
        view_desc.Aspect = EImageAspect::Color;
        Images.emplace_back(wrapped);
        ImageViews.emplace_back(Device->createImageView(view_desc));
    }
    if (Descriptor.HDR && !setHDRMetadata(*Descriptor.HDR))
        throw std::runtime_error("HDR metadata was explicitly requested but is not supported.");
}

bool VulkanSwapchain::setHDRMetadata(const HDRMetadata& Metadata)
{
    if (!Swapchain || !Context->EnabledFeatures.HDRMetadata ||
        (ColorSpace != EColorSpace::HDR10_ST2084 && ColorSpace != EColorSpace::ExtendedSRGBLinear))
        return false;
    auto function = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        Context->Device->getProcAddr("vkSetHdrMetadataEXT"));
    if (!function) return false;
    VkHdrMetadataEXT native { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    auto chromaticity = [](const std::array<float, 2>& value)
    {
        return VkXYColorEXT { value[0], value[1] };
    };
    native.displayPrimaryRed = chromaticity(Metadata.DisplayPrimaryRed);
    native.displayPrimaryGreen = chromaticity(Metadata.DisplayPrimaryGreen);
    native.displayPrimaryBlue = chromaticity(Metadata.DisplayPrimaryBlue);
    native.whitePoint = chromaticity(Metadata.WhitePoint);
    native.maxLuminance = Metadata.MaxLuminanceNits;
    native.minLuminance = Metadata.MinLuminanceNits;
    native.maxContentLightLevel = Metadata.MaxContentLightLevelNits;
    native.maxFrameAverageLightLevel = Metadata.MaxFrameAverageLightLevelNits;
    const VkSwapchainKHR swapchain = static_cast<VkSwapchainKHR>(Swapchain.get());
    function(static_cast<VkDevice>(Context->Device.get()), 1, &swapchain, &native);
    return true;
}

void VulkanSwapchain::releaseImages()
{
    ImageViews.clear();
    Images.clear();
}

} // namespace rhi
