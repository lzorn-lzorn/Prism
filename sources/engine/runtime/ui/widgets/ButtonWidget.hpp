#pragma once

#include "Widget.hpp"
#include <infra/wrappers/Delegate.hpp>
#include <memory>
#include <utility>

namespace ui
{
/**
 * @brief 按钮控件: 单内容子控件(ContentWidget), 提供悬停/按下/禁用视觉状态,
 *        以及 Click/Down/Up/Hover/Pressed 事件回调.
 *
 * 内容为任意 Widget(通常为 TextWidget); 由 LeafSlot 负责内边距与对齐.
 * 点击语义与框架一致: 按下与释放都命中本按钮时触发(Window::dispatchMouseButton 中
 * 以 Pressed == Target 判定). 获焦后支持 Space / Enter 键盘触发.
 */
class ButtonWidget : public ContentWidget
{
public:
    using ClickCallback   = infra::UniqueDelegate<void()>;
    using DownCallback    = infra::UniqueDelegate<void()>;
    using UpCallback      = infra::UniqueDelegate<void()>;
    using HoverCallback   = infra::UniqueDelegate<void(bool)>;   // 参数 = 是否悬停
    using PressedCallback = infra::UniqueDelegate<void(bool)>;   // 参数 = 是否按下

    ButtonWidget();
    explicit ButtonWidget(std::unique_ptr<Widget> Content);

    // 事件回调: 均接受任意可调用对象(lambda/函数指针/可调用), 支持捕获 move-only 对象的 lambda.
    //   Click    一次完整点击(按下+释放均命中)时触发.
    //   Down     鼠标左键在本按钮上按下时触发.
    //   Up       鼠标左键在本按钮上释放时触发.
    //   Hover    悬停状态变化(进入=true / 离开=false)时触发.
    //   Pressed  按下状态变化(按下=true / 释放或移出=false)时触发; 含键盘 Space/Enter 触发.
    template<class Callable>
    void setOnClick(Callable&& Callback)
    {
        OnClick = ClickCallback(std::forward<Callable>(Callback));
    }
    void clearOnClick() noexcept;

    template<class Callable>
    void setOnDown(Callable&& Callback)
    {
        OnDown = DownCallback(std::forward<Callable>(Callback));
    }
    void clearOnDown() noexcept;

    template<class Callable>
    void setOnUp(Callable&& Callback)
    {
        OnUp = UpCallback(std::forward<Callable>(Callback));
    }
    void clearOnUp() noexcept;

    template<class Callable>
    void setOnHoverChanged(Callable&& Callback)
    {
        OnHoverChanged = HoverCallback(std::forward<Callable>(Callback));
    }
    void clearOnHoverChanged() noexcept;

    template<class Callable>
    void setOnPressedChanged(Callable&& Callback)
    {
        OnPressedChanged = PressedCallback(std::forward<Callable>(Callback));
    }
    void clearOnPressedChanged() noexcept;

    // 视觉样式.
    void setBackgroundColor(Color Value);
    Color getBackgroundColor() const noexcept;
    void setHoverColor(Color Value);
    Color getHoverColor() const noexcept;
    void setPressedColor(Color Value);
    Color getPressedColor() const noexcept;
    void setDisabledColor(Color Value);
    Color getDisabledColor() const noexcept;
    void setBorderColor(Color Value);
    Color getBorderColor() const noexcept;
    // 描边粗细(逻辑单位, 0 表示无描边).
    void setBorderThickness(float Value);
    float getBorderThickness() const noexcept;
    // 圆角半径(逻辑单位, 0 表示直角).
    void setCornerRadius(float Value);
    float getCornerRadius() const noexcept;

    // 内容内边距: 立即应用到已有子插槽; 无子控件时暂存, 挂接后自动同步.
    void setPadding(Padding Value);
    Padding getPadding() const noexcept;

    // 交互状态.
    void setEnabled(bool Value);
    bool isEnabled() const noexcept;
    bool isPressed() const noexcept;
    bool isHovered() const noexcept;

protected:
    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void onArrange(UIRect LocalContent) override;
    void onPaint(PaintContext& Context) const override;

    EventReply onMouseButtonDown(const MouseButtonEvent& Event, UIPoint LocalPoint) override;
    EventReply onMouseButtonUp(const MouseButtonEvent& Event, UIPoint LocalPoint) override;
    EventReply onMouseButtonClicked(const MouseButtonEvent& Event, UIPoint LocalPoint) override;
    EventReply onMouseEnter(const MouseMoveEvent& Event, UIPoint LocalPoint) override;
    EventReply onMouseLeave(const MouseMoveEvent& Event, UIPoint LocalPoint) override;
    EventReply onKeyDown(const KeyEvent& Event) override;
    EventReply onKeyUp(const KeyEvent& Event) override;

private:
    Color getEffectiveFillColor() const noexcept;
    void setPressedState(bool Value);
    void setHoveredState(bool Value);
    void fireClick() const;
    // 使子插槽布局与内边距与本地存储一致(供 onArrange 在排列前同步).
    void syncChildSlot();

    ClickCallback OnClick;
    DownCallback OnDown;
    UpCallback OnUp;
    HoverCallback OnHoverChanged;
    PressedCallback OnPressedChanged;
    // TODO: UI Material: 颜色/描边/圆角/渐变/纹理/阴影等, 由 UIRenderer 绘制.
    Color BackgroundColor{0.20f, 0.45f, 0.90f, 1.0f};
    Color HoverColor{0.30f, 0.60f, 1.00f, 1.0f};
    Color PressedColor{0.12f, 0.30f, 0.65f, 1.0f};
    Color DisabledColor{0.50f, 0.50f, 0.50f, 0.50f};
    Color BorderColor{0.00f, 0.00f, 0.00f, 1.0f};
    float BorderThickness{0.0f};
    float CornerRadius{4.0f};
    Padding PaddingValue{};
    bool Enabled{true};
    bool Hovered{false};
    bool Pressed{false};
    bool KeyPressed{false};
};

}
