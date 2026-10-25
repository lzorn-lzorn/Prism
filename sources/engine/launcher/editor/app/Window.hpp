#pragma once

#include "IGenericPlatform.hpp"
#include "Widget.hpp"
#include <memory>
#include <vector>

namespace ui
{
using runtime::platform::EWindowResizePolicy;
using runtime::platform::IGenericPlatform;
using runtime::platform::IGenericWindow;
using runtime::platform::KeyEvent;
using runtime::platform::MouseButtonEvent;
using runtime::platform::MouseMoveEvent;
using runtime::platform::MouseWheelEvent;
using runtime::platform::UISize;
using runtime::platform::WindowConfigs;

// 保留模式（增量渲染）的跨帧缓存；立即模式下 Window 不持有它（为 nullptr）。
struct WindowRetainedState
{
    UIDrawList CachedList;
    UIRect LastRootBounds{};
    bool IsListValid{};
    bool HasSubmitted{};
};

class Window
{
public:
    Window(IGenericPlatform& Platform, const WindowConfigs& Configs = {}, std::unique_ptr<Widget> ContentRoot = nullptr);
    /**
     * @brief 窗口内容根控件：可空；非空时每帧排列到安全区，作为绘制、命中测试与事件派发的树根。
     * 为空时窗口只呈现清屏背景。
     */
    Widget* getContentRoot() noexcept;
    const Widget* getContentRoot() const noexcept;
    void setContentRoot(std::unique_ptr<Widget> Root);
    std::unique_ptr<Widget> takeContentRoot() noexcept;
    IGenericWindow& getNativeWindow() noexcept;
    const IGenericWindow& getNativeWindow() const noexcept;
    float getDisplayScale() const;
    // 分辨率策略与预设档位.
    EWindowResizePolicy getResizePolicy() const noexcept;
    const std::vector<UISize>& getPresetResolutions() const noexcept;
    void setResizePolicy(EWindowResizePolicy Policy);
    void setPresetResolutions(std::vector<UISize> Resolutions);
    // 请求切换到指定分辨率: Fixed 下拒绝, Preset 下吸附到最近档位. 返回是否成功.
    bool setResolution(UISize Size);
    // 窗口背景清屏色; 由上层调用者传入.
    void setBackgroundColor(Color Value) noexcept;
    Color getBackgroundColor() const noexcept;
    /**
     * @brief 实际排版, 绘制并呈现一帧, 不是能力查询.
     * 保留模式下(渲染器 EUIMode::Retained)只在失效时重排/重建绘制列表, 
     * 否则复用上一帧的结果并提示渲染器复用 GPU 缓冲; 立即模式每帧全量重算.
     * 无 Renderer 时只排版; 最小化或像素尺寸为零时返回 false.
     */
    bool drawFrame(IUIRenderer* Renderer = nullptr);
    // 命中测试：WindowPoint 使用窗口逻辑坐标; 返回最深命中的可交互控件.
    Widget* hitTest(UIPoint WindowPoint);
    const Widget* hitTest(UIPoint WindowPoint) const;
    // 上一帧因为失效实际做了哪些工作(用于观察与测试).
    const UIUpdateReport& getLastUpdate() const noexcept;
    // 交互派发：由应用事件循环调用, 内部维护悬停/按下/焦点状态.
    // 返回被命中/聚焦控件的事件处理结果(Handled 表示消费了事件, 调用方应停止默认处理).
    EventReply dispatchMouseMove(const MouseMoveEvent& Event);
    EventReply dispatchMouseButton(const MouseButtonEvent& Event);
    EventReply dispatchMouseWheel(const MouseWheelEvent& Event);
    EventReply dispatchKey(const KeyEvent& Event);
    // 键盘焦点：键盘事件派发给聚焦控件; 点击可聚焦控件会自动聚焦.
    void setFocus(Widget* Target);
    Widget* getFocus() noexcept;
    const Widget* getFocus() const noexcept;
    void clearFocus() noexcept;

private:
    std::unique_ptr<IGenericWindow> Native;
    std::unique_ptr<Widget> ContentRoot;
    EWindowResizePolicy ResizePolicy{EWindowResizePolicy::Free};
    std::vector<UISize> PresetResolutions;
    Color Background{0.94f, 0.95f, 0.94f, 1};
    // 保留模式的增量渲染状态；立即模式下为 nullptr（不持有跨帧缓存）。
    std::unique_ptr<WindowRetainedState> Retained;
    UIUpdateReport LastUpdate{};
    Widget* Hovered{};
    Widget* Pressed{};
    Widget* Focused{};
};
}
