#pragma once

#include "PlatformEvents.hpp"
#include <memory>
#include <string>
#include <vector>

namespace runtime::platform
{
// 窗口分辨率策略: 是否允许(以及如何)调整主窗口尺寸.
enum class EWindowResizePolicy
{
    Fixed,   // 不能设置: 窗口尺寸固定, 不可调整
    Preset,  // 可以设置为开发者指定的若干档位
    Free     // 能任意设置
};

struct UISize
{
    int Width{}, Height{};
};

struct Rect
{
    int X{}, Y{}, Width{}, Height{};
};

struct WindowConfigs
{
    std::string Title{"Generic Editor"};
    int Width{1280}, Height{720};
    EWindowResizePolicy ResizePolicy{EWindowResizePolicy::Free};
    std::vector<UISize> PresetResolutions;
    bool HighPixelDensity{true}, Hidden{false};
};

class IGenericWindow
{
public:
    virtual ~IGenericWindow() = default;
    IGenericWindow(const IGenericWindow&) = delete;
    IGenericWindow& operator=(const IGenericWindow&) = delete;

    virtual WindowId getId() const noexcept = 0;
    virtual UISize getSize() const = 0;
    virtual UISize getPixelSize() const = 0;
    // @note 可安全交互的客户区采用窗口逻辑坐标，而非帧缓冲像素坐标。
    virtual Rect getSafeArea() const = 0;
    virtual float getDisplayScale() const = 0;
    virtual void setTitle(const std::string& Title) = 0;
    virtual void setSize(UISize Size) = 0;
    virtual void setResizable(bool Enabled) = 0;
    virtual void show() = 0;
    virtual void hide() = 0;
    virtual void setFullscreen(bool Enabled) = 0;
    virtual void setRelativeMouse(bool Enabled) = 0;
    virtual void startTextInput() = 0;
    virtual void stopTextInput() = 0;
    virtual void setTextInputArea(Rect Area, int CursorOffset = 0) = 0;
    // @note 渲染后端(RHI)使用的原生窗口句柄(如 SDL_Window*); 由具体平台实现提供。
    virtual void* getNativeHandle() const noexcept = 0;

protected:
    IGenericWindow() = default;
};

using GenericWindowPointer = std::shared_ptr<IGenericWindow>;
}
