#pragma once

// @note 仅供后端使用的定义；公共头文件不得包含此文件。
#include "IGenericPlatform.hpp"
#include <infra/common/Error.hpp>
#include <SDL3/SDL.h>
#include <deque>
#include <stdexcept>

namespace runtime::platform::detail
{
inline Error makeError(const char* Operation)
{
    return Error(EErrorCode::Runtime, std::string(Operation) + ": " + SDL_GetError());
}

inline void check(bool Success, const char* Operation)
{
    if (!Success)
    {
        throw makeError(Operation);
    }
}

struct UIRuntime
{
    UIRuntime();
    ~UIRuntime();
};

struct WindowState
{
    std::shared_ptr<UIRuntime> Runtime;
    SDL_Window* Window{};
    bool GPUClaimed{};

    ~WindowState()
    {
        if (Window)
        {
            SDL_DestroyWindow(Window);
        }
    }
};

Event translateEvent(const SDL_Event& Source);
}

namespace runtime::platform
{
class SdlPlatform final : public IGenericPlatform
{
public:
    SdlPlatform();
    ~SdlPlatform() override = default;
    PlatformBackend getBackend() const noexcept override;
    std::unique_ptr<IGenericWindow> createWindow(const WindowConfigs& Configs = {}) override;
    std::optional<Event> pollEvent() override;
    void postEvent(CustomEvent InEvent, WindowId InWindowId = 0) override;
    std::string getClipboardText() const override;
    void setClipboardText(const std::string& Text) override;
    void delay(std::uint32_t Milliseconds) override;

private:
    std::shared_ptr<detail::UIRuntime> Runtime;
    std::deque<Event> Posted;
};

class SdlWindow final : public IGenericWindow
{
public:
    SdlWindow(std::shared_ptr<detail::UIRuntime> Runtime, const WindowConfigs& Configs);
    ~SdlWindow() override = default;
    WindowId getId() const noexcept override;
    UISize getSize() const override;
    UISize getPixelSize() const override;
    Rect getSafeArea() const override;
    float getDisplayScale() const override;
    void setTitle(const std::string& Title) override;
    void setSize(UISize Size) override;
    void setResizable(bool Enabled) override;
    void show() override;
    void hide() override;
    void setFullscreen(bool Enabled) override;
    void setRelativeMouse(bool Enabled) override;
    void startTextInput() override;
    void stopTextInput() override;
    void setTextInputArea(Rect Area, int CursorOffset = 0) override;
    void* getNativeHandle() const noexcept override;
    std::shared_ptr<detail::WindowState> getState() const noexcept;

private:
    std::shared_ptr<detail::WindowState> State;
};
}
