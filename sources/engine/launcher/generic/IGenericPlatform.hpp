#pragma once

#include "IGenericWindow.hpp"
#include <memory>
#include <optional>

namespace ui
{
enum class PlatformBackend
{
    Auto,
    Sdl,
    Win32,
    AppKit
};

class IGenericPlatform
{
public:
    virtual ~IGenericPlatform() = default;
    IGenericPlatform(const IGenericPlatform&) = delete;
    IGenericPlatform& operator=(const IGenericPlatform&) = delete;

    virtual PlatformBackend getBackend() const noexcept = 0;
    virtual std::unique_ptr<IGenericWindow> createWindow(const WindowConfigs& Configs = {}) = 0;
    virtual std::optional<Event> pollEvent() = 0;
    virtual void postEvent(CustomEvent InEvent, WindowId InWindowId = 0) = 0;
    virtual std::string getClipboardText() const = 0;
    virtual void setClipboardText(const std::string& Text) = 0;
    virtual void delay(std::uint32_t Milliseconds) = 0;

protected:
    IGenericPlatform() = default;
};

std::unique_ptr<IGenericPlatform> createPlatform(PlatformBackend Backend = PlatformBackend::Auto);
}