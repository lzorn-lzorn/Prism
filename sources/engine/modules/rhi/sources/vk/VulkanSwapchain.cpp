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
    constexpr std::array Candidates {
        vk::CompositeAlphaFlagBitsKHR::eOpaque,
        vk::CompositeAlphaFlagBitsKHR::ePreMultiplied,
        vk::CompositeAlphaFlagBitsKHR::ePostMultiplied,
        vk::CompositeAlphaFlagBitsKHR::eInherit };
    for (const auto Candidate : Candidates)
        if (Supported & Candidate) return Candidate;
    throw std::runtime_error("The Vulkan surface exposes no supported composite alpha mode.");
}

VulkanSemaphore* semaphoreOf(const std::shared_ptr<RSemaphore>& Semaphore, VulkanDevice& Device)
{
    if (!Semaphore) return nullptr;
    auto* Result = dynamic_cast<VulkanSemaphore*>(Semaphore.get());
    if (!Result || &Result->getDevice() != &Device || Result->isTimeline())
        throw std::invalid_argument("Swapchain acquisition requires a binary semaphore from the same device.");
    return Result;
}

VulkanFence* fenceOf(const std::shared_ptr<RFence>& Fence, VulkanDevice& Device)
{
    if (!Fence) return nullptr;
    auto* Result = dynamic_cast<VulkanFence*>(Fence.get());
    if (!Result || &Result->getDevice() != &Device)
        throw std::invalid_argument("Acquire fence belongs to another RHI device or backend.");
    return Result;
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
    // 窗口缩放后, 部分平台(如 Windows)的 acquireNextImageKHR 仍会返回 Success,
    // 不会主动报告 OutOfDate; 这里在 acquire 前主动比对 surface 尺寸, 变化时直接
    // 返回 OutOfDate, 由上层 recreate() 用 surface 的 currentExtent 重建交换链.
    if (isSurfaceExtentChanged())
    {
        recordStatus(ESwapchainStatus::OutOfDate);
        return { EAcquireStatus::OutOfDate, 0, 0 };
    }
    auto* Semaphore = semaphoreOf(SignalSemaphore, *Device);
    auto* Fence = fenceOf(SignalFence, *Device);
    if (!Semaphore && !Fence)
        throw std::invalid_argument("Swapchain acquisition requires a semaphore or fence signal target.");
    try
    {
        const auto Result = Context->Device->acquireNextImageKHR(
            Swapchain.get(),
            TimeoutNanoseconds,
            Semaphore ? Semaphore->getVkSemaphore() : vk::Semaphore{},
            Fence ? Fence->getVkFence() : vk::Fence{});
        const auto Status = vulkan_wsi::mapAcquire(Result.result);
        if (!Status) throw std::runtime_error("Unexpected Vulkan image-acquisition result.");
        recordStatus(vulkan_wsi::mapStatus(Result.result).value_or(ESwapchainStatus::OutOfDate));
        return { *Status, Result.value,
            (*Status == EAcquireStatus::Success || *Status == EAcquireStatus::Suboptimal) ? Generation : 0 };
    }
    catch (const vk::SystemError& Error)
    {
        const auto Result = vulkan_wsi::resultFromSystemError(Error);
        const auto Status = vulkan_wsi::mapAcquire(Result);
        if (Status)
        {
            if (const auto Health = vulkan_wsi::mapStatus(Result)) recordStatus(*Health);
            return { *Status, 0, 0 };
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
    vk::UniqueSwapchainKHR Old = std::move(Swapchain);
    releaseImages();
    create(Old.get());
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
    catch (const vk::SystemError& Error)
    {
        if (const auto Mapped = vulkan_wsi::mapStatus(vulkan_wsi::resultFromSystemError(Error)))
            recordStatus(*Mapped);
        return false;
    }
    catch (...) { return false; }
}

ESwapchainStatus VulkanSwapchain::getStatus() const noexcept
{
    if (Context->isDeviceLost()) return ESwapchainStatus::DeviceLost;
    if (!Surface || Surface != Context->Surface) return ESwapchainStatus::SurfaceLost;
    const auto Persistent = Status.load(std::memory_order_relaxed);
    if (Persistent == ESwapchainStatus::SurfaceLost || Persistent == ESwapchainStatus::DeviceLost)
        return Persistent;
    VkSurfaceCapabilitiesKHR Capabilities {};
    const VkResult Result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        static_cast<VkPhysicalDevice>(Context->PhysicalDevice),
        static_cast<VkSurfaceKHR>(Surface->Handle), &Capabilities);
    if (const auto Mapped = vulkan_wsi::mapStatus(static_cast<vk::Result>(Result)))
    {
        if (*Mapped == ESwapchainStatus::DeviceLost) Context->markDeviceLost();
        if (*Mapped == ESwapchainStatus::SurfaceLost || *Mapped == ESwapchainStatus::DeviceLost)
            Status.store(*Mapped, std::memory_order_relaxed);
        return Result == VK_SUCCESS ? Persistent : *Mapped;
    }
    return Persistent;
}

bool VulkanSwapchain::isSurfaceExtentChanged() const noexcept
{
    if (!Surface || !Swapchain)
        return false;
    VkSurfaceCapabilitiesKHR Capabilities {};
    const VkResult Result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        static_cast<VkPhysicalDevice>(Context->PhysicalDevice),
        static_cast<VkSurfaceKHR>(Surface->Handle), &Capabilities);
    if (Result != VK_SUCCESS)
        return false;
    constexpr uint32_t UndefinedExtent = std::numeric_limits<uint32_t>::max();
    // currentExtent 不确定(部分平台)时无法据此判断, 交由 acquire 的失败/OutOfDate 信号.
    if (Capabilities.currentExtent.width == UndefinedExtent ||
        Capabilities.currentExtent.height == UndefinedExtent)
        return false;
    // 最小化或 0 尺寸: 保持现状, 上层会因 acquire 失败/空图跳过渲染.
    if (Capabilities.currentExtent.width == 0 || Capabilities.currentExtent.height == 0)
        return false;
    return Capabilities.currentExtent.width != Descriptor.Width ||
           Capabilities.currentExtent.height != Descriptor.Height;
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
    catch (const vk::SystemError& Error)
    {
        if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
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
    const auto Capabilities = Context->PhysicalDevice.getSurfaceCapabilitiesKHR(Surface->Handle);
    const auto Formats = Context->PhysicalDevice.getSurfaceFormatsKHR(Surface->Handle);
    const auto PresentModes = Context->PhysicalDevice.getSurfacePresentModesKHR(Surface->Handle);
    if (Formats.empty() || PresentModes.empty())
        throw std::runtime_error("The Vulkan surface has no swapchain formats or present modes.");

    auto SelectedFormat = std::ranges::find_if(Formats, [&](const vk::SurfaceFormatKHR& Candidate)
    {
        return Candidate.format == toVk(Descriptor.PreferredFormat) &&
            Candidate.colorSpace == toVk(Descriptor.ColorSpace);
    });
    if (SelectedFormat == Formats.end() &&
        (Descriptor.RequireExactFormatAndColorSpace || Descriptor.HDR))
        throw std::runtime_error("The explicitly required swapchain format/color space is unavailable.");
    if (SelectedFormat == Formats.end())
        SelectedFormat = std::ranges::find_if(Formats, [](const vk::SurfaceFormatKHR& Candidate)
        {
            return fromVkFormat(Candidate.format) != EFormat::Undefined;
        });
    if (SelectedFormat == Formats.end())
        throw std::runtime_error("No supported swapchain format maps to an RHI format.");
    Format = fromVkFormat(SelectedFormat->format);
    ColorSpace = fromVkColorSpace(SelectedFormat->colorSpace).value_or(EColorSpace::SRGB_Nonlinear);

    vk::PresentModeKHR SelectedPresentMode = vk::PresentModeKHR::eFifo;
    const vk::PresentModeKHR Preferred = toVk(Descriptor.PreferredPresentMode);
    if (std::ranges::find(PresentModes, Preferred) != PresentModes.end())
        SelectedPresentMode = Preferred;

    vk::Extent2D Extent;
    if (Capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max())
        Extent = Capabilities.currentExtent;
    else
    {
        Extent.width = std::clamp(Descriptor.Width,
            Capabilities.minImageExtent.width, Capabilities.maxImageExtent.width);
        Extent.height = std::clamp(Descriptor.Height,
            Capabilities.minImageExtent.height, Capabilities.maxImageExtent.height);
    }
    Descriptor.Width = Extent.width;
    Descriptor.Height = Extent.height;

    uint32_t ImageCount = std::max(Descriptor.MinimumImageCount, Capabilities.minImageCount);
    if (Capabilities.maxImageCount != 0)
        ImageCount = std::min(ImageCount, Capabilities.maxImageCount);
    const vk::ImageUsageFlags RequestedUsage = toVk(Descriptor.ImageUsage);
    if ((Capabilities.supportedUsageFlags & RequestedUsage) != RequestedUsage)
        throw std::invalid_argument("Requested swapchain image usage is not supported by the surface.");

    const uint32_t Families[] = {
        Context->GraphicsQueueFamilyIndex,
        Context->PresentQueueFamilyIndex };
    const bool Concurrent = Families[0] != Families[1];
    vk::SwapchainCreateInfoKHR CreateInfo;
    CreateInfo.setSurface(Surface->Handle)
        .setMinImageCount(ImageCount)
        .setImageFormat(SelectedFormat->format)
        .setImageColorSpace(SelectedFormat->colorSpace)
        .setImageExtent(Extent)
        .setImageArrayLayers(1)
        .setImageUsage(RequestedUsage)
        .setImageSharingMode(Concurrent ? vk::SharingMode::eConcurrent : vk::SharingMode::eExclusive)
        .setPreTransform(Capabilities.currentTransform)
        .setCompositeAlpha(chooseCompositeAlpha(Capabilities.supportedCompositeAlpha))
        .setPresentMode(SelectedPresentMode)
        .setClipped(Descriptor.Clipped)
        .setOldSwapchain(OldSwapchain);
    if (Concurrent)
        CreateInfo.setQueueFamilyIndices(Families);
    Swapchain = Context->Device->createSwapchainKHRUnique(CreateInfo);
    if (++Generation == 0) ++Generation;
    Status.store(ESwapchainStatus::Ready, std::memory_order_relaxed);

    const auto NativeImages = Context->Device->getSwapchainImagesKHR(Swapchain.get());
    Images.reserve(NativeImages.size());
    ImageViews.reserve(NativeImages.size());
    for (const auto Image : NativeImages)
    {
        RImage::Descriptor_t ImageDesc;
        ImageDesc.Format = Format;
        ImageDesc.Dimension = EImageDimension::Texture2D;
        ImageDesc.Width = Descriptor.Width;
        ImageDesc.Height = Descriptor.Height;
        ImageDesc.Usage = Descriptor.ImageUsage;
        auto Wrapped = VulkanImage::wrapExternal(*Device, ImageDesc, Image);
        RImageView::Descriptor_t ViewDesc;
        ViewDesc.Image = Wrapped;
        ViewDesc.Format = Format;
        ViewDesc.Dimension = EImageViewDimension::Texture2D;
        ViewDesc.Aspect = EImageAspect::Color;
        Images.emplace_back(Wrapped);
        ImageViews.emplace_back(Device->createImageView(ViewDesc));
    }
    if (Descriptor.HDR && !setHDRMetadata(*Descriptor.HDR))
        throw std::runtime_error("HDR metadata was explicitly requested but is not supported.");
}

bool VulkanSwapchain::setHDRMetadata(const HDRMetadata& Metadata)
{
    if (!Swapchain || !Context->EnabledFeatures.HDRMetadata ||
        (ColorSpace != EColorSpace::HDR10_ST2084 && ColorSpace != EColorSpace::ExtendedSRGBLinear))
        return false;
    auto Function = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
        Context->Device->getProcAddr("vkSetHdrMetadataEXT"));
    if (!Function) return false;
    VkHdrMetadataEXT Native { VK_STRUCTURE_TYPE_HDR_METADATA_EXT };
    auto Chromaticity = [](const std::array<float, 2>& Value)
    {
        return VkXYColorEXT { Value[0], Value[1] };
    };
    Native.displayPrimaryRed = Chromaticity(Metadata.DisplayPrimaryRed);
    Native.displayPrimaryGreen = Chromaticity(Metadata.DisplayPrimaryGreen);
    Native.displayPrimaryBlue = Chromaticity(Metadata.DisplayPrimaryBlue);
    Native.whitePoint = Chromaticity(Metadata.WhitePoint);
    Native.maxLuminance = Metadata.MaxLuminanceNits;
    Native.minLuminance = Metadata.MinLuminanceNits;
    Native.maxContentLightLevel = Metadata.MaxContentLightLevelNits;
    Native.maxFrameAverageLightLevel = Metadata.MaxFrameAverageLightLevelNits;
    const VkSwapchainKHR SwapchainHandle = static_cast<VkSwapchainKHR>(Swapchain.get());
    Function(static_cast<VkDevice>(Context->Device.get()), 1, &SwapchainHandle, &Native);
    return true;
}

void VulkanSwapchain::releaseImages()
{
    ImageViews.clear();
    Images.clear();
}

} // namespace rhi
