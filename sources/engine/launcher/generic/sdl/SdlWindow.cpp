#include <infra/common/Error.hpp>
#include "SdlInternal.hpp"

namespace ui
{
SdlWindow::SdlWindow(std::shared_ptr<detail::UIRuntime> Runtime, const WindowConfigs& Configs)
    : State(std::make_shared<detail::WindowState>())
{
    if (Configs.Width <= 0 || Configs.Height <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Window size must be positive");
    }
    State->Runtime = std::move(Runtime);
    SDL_WindowFlags Flags = 0;
    if (Configs.ResizePolicy != EWindowResizePolicy::Fixed)
    {
        Flags |= SDL_WINDOW_RESIZABLE;
    }
    if (Configs.HighPixelDensity)
    {
        Flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
    }
    if (Configs.Hidden)
    {
        Flags |= SDL_WINDOW_HIDDEN;
    }
    // @note SDL GPU 会自行接管窗口；此处不要强制设置 SDL_WINDOW_VULKAN。
    State->Window = SDL_CreateWindow(Configs.Title.c_str(), Configs.Width, Configs.Height, Flags);
    if (!State->Window)
    {
        throw detail::makeError("SDL_CreateWindow");
    }
}

std::shared_ptr<detail::WindowState> SdlWindow::getState() const noexcept
{
    return State;
}

void* SdlWindow::getNativeHandle() const noexcept
{
    return State->Window;
}

WindowId SdlWindow::getId() const noexcept
{
    return SDL_GetWindowID(State->Window);
}

UISize SdlWindow::getSize() const
{
    UISize Result;
    detail::check(SDL_GetWindowSize(State->Window, &Result.Width, &Result.Height), "SDL_GetWindowSize");
    return Result;
}

UISize SdlWindow::getPixelSize() const
{
    UISize Result;
    detail::check(SDL_GetWindowSizeInPixels(State->Window, &Result.Width, &Result.Height), "SDL_GetWindowSizeInPixels");
    return Result;
}

Rect SdlWindow::getSafeArea() const
{
    SDL_Rect Native{};
    detail::check(SDL_GetWindowSafeArea(State->Window, &Native), "SDL_GetWindowSafeArea");
    return {Native.x, Native.y, Native.w, Native.h};
}

float SdlWindow::getDisplayScale() const
{
    return SDL_GetWindowDisplayScale(State->Window);
}

void SdlWindow::setTitle(const std::string& Title)
{
    detail::check(SDL_SetWindowTitle(State->Window, Title.c_str()), "SDL_SetWindowTitle");
}

void SdlWindow::setSize(UISize Size)
{
    if (Size.Width <= 0 || Size.Height <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Window size must be positive");
    }
    detail::check(SDL_SetWindowSize(State->Window, Size.Width, Size.Height), "SDL_SetWindowSize");
}

void SdlWindow::setResizable(bool Enabled)
{
    detail::check(SDL_SetWindowResizable(State->Window, Enabled), "SDL_SetWindowResizable");
}

void SdlWindow::show()
{
    detail::check(SDL_ShowWindow(State->Window), "SDL_ShowWindow");
}

void SdlWindow::hide()
{
    detail::check(SDL_HideWindow(State->Window), "SDL_HideWindow");
}

void SdlWindow::setFullscreen(bool Enabled)
{
    detail::check(SDL_SetWindowFullscreen(State->Window, Enabled), "SDL_SetWindowFullscreen");
}

void SdlWindow::setRelativeMouse(bool Enabled)
{
    detail::check(SDL_SetWindowRelativeMouseMode(State->Window, Enabled), "SDL_SetWindowRelativeMouseMode");
}

void SdlWindow::startTextInput()
{
    detail::check(SDL_StartTextInput(State->Window), "SDL_StartTextInput");
}

void SdlWindow::stopTextInput()
{
    detail::check(SDL_StopTextInput(State->Window), "SDL_StopTextInput");
}

void SdlWindow::setTextInputArea(Rect Area, int CursorOffset)
{
    const SDL_Rect Native{Area.X, Area.Y, Area.Width, Area.Height};
    detail::check(SDL_SetTextInputArea(State->Window, &Native, CursorOffset), "SDL_SetTextInputArea");
}
}