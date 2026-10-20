#pragma once

#include <RHI.hpp>
#include <optional>
#include <system_error>
#include <vulkan/vulkan.hpp>

namespace rhi::vulkan_wsi
{

/** The one normalization point for every Vulkan WSI result path. */
[[nodiscard]] constexpr std::optional<ESwapchainStatus> mapStatus(vk::Result Result) noexcept
{
    switch (Result)
    {
    case vk::Result::eSuccess: return ESwapchainStatus::Ready;
    case vk::Result::eSuboptimalKHR: return ESwapchainStatus::Suboptimal;
    case vk::Result::eErrorOutOfDateKHR: return ESwapchainStatus::OutOfDate;
    case vk::Result::eErrorSurfaceLostKHR: return ESwapchainStatus::SurfaceLost;
    case vk::Result::eErrorDeviceLost: return ESwapchainStatus::DeviceLost;
    default: return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<EAcquireStatus> mapAcquire(vk::Result Result) noexcept
{
    switch (Result)
    {
    case vk::Result::eSuccess: return EAcquireStatus::Success;
    case vk::Result::eSuboptimalKHR: return EAcquireStatus::Suboptimal;
    case vk::Result::eErrorOutOfDateKHR: return EAcquireStatus::OutOfDate;
    case vk::Result::eErrorSurfaceLostKHR: return EAcquireStatus::SurfaceLost;
    case vk::Result::eErrorDeviceLost: return EAcquireStatus::DeviceLost;
    case vk::Result::eNotReady:
    case vk::Result::eTimeout: return EAcquireStatus::NotReady;
    default: return std::nullopt;
    }
}

[[nodiscard]] constexpr std::optional<EPresentStatus> mapPresent(vk::Result Result) noexcept
{
    switch (Result)
    {
    case vk::Result::eSuccess: return EPresentStatus::Success;
    case vk::Result::eSuboptimalKHR: return EPresentStatus::Suboptimal;
    case vk::Result::eErrorOutOfDateKHR: return EPresentStatus::OutOfDate;
    case vk::Result::eErrorSurfaceLostKHR: return EPresentStatus::SurfaceLost;
    case vk::Result::eErrorDeviceLost: return EPresentStatus::DeviceLost;
    default: return std::nullopt;
    }
}

[[nodiscard]] inline vk::Result resultFromSystemError(const vk::SystemError& Error) noexcept
{
    return static_cast<vk::Result>(Error.code().value());
}

} // namespace rhi::vulkan_wsi
