#include "ButtonWidget.hpp"
#include <infra/common/Error.hpp>
#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace ui
{
using runtime::platform::MouseButton;
using runtime::platform::PhysicalKey;

namespace
{
Vector2 getPaddingSize(Padding Insets)
{
    return {Insets.Left + Insets.Right, Insets.Top + Insets.Bottom};
}

Vector2 subtractPadding(Vector2 Size, Padding Insets)
{
    const auto Total = getPaddingSize(Insets);
    return {std::max(0.0f, Size.x() - Total.x()),
            std::max(0.0f, Size.y() - Total.y())};
}

void validateNonNegative(float Value, const char* Name)
{
    if (!std::isfinite(Value) || Value < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, std::string(Name) + " must be finite and nonnegative");
    }
}

bool isActivationKey(const KeyEvent& Event)
{
    return Event.Physical == PhysicalKey::Space || Event.Physical == PhysicalKey::Enter;
}
}

ButtonWidget::ButtonWidget()
{
    setAcceptEvents(true);
    setFocusable(true);
}

ButtonWidget::ButtonWidget(std::unique_ptr<Widget> Content)
    : ButtonWidget()
{
    setChild(std::move(Content));
}

void ButtonWidget::clearOnClick() noexcept
{
    OnClick.reset();
}

void ButtonWidget::clearOnDown() noexcept
{
    OnDown.reset();
}

void ButtonWidget::clearOnUp() noexcept
{
    OnUp.reset();
}

void ButtonWidget::clearOnHoverChanged() noexcept
{
    OnHoverChanged.reset();
}

void ButtonWidget::clearOnPressedChanged() noexcept
{
    OnPressedChanged.reset();
}

void ButtonWidget::setBackgroundColor(Color Value)
{
    BackgroundColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color ButtonWidget::getBackgroundColor() const noexcept
{
    return BackgroundColor;
}

void ButtonWidget::setHoverColor(Color Value)
{
    HoverColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color ButtonWidget::getHoverColor() const noexcept
{
    return HoverColor;
}

void ButtonWidget::setPressedColor(Color Value)
{
    PressedColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color ButtonWidget::getPressedColor() const noexcept
{
    return PressedColor;
}

void ButtonWidget::setDisabledColor(Color Value)
{
    DisabledColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color ButtonWidget::getDisabledColor() const noexcept
{
    return DisabledColor;
}

void ButtonWidget::setBorderColor(Color Value)
{
    BorderColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color ButtonWidget::getBorderColor() const noexcept
{
    return BorderColor;
}

void ButtonWidget::setBorderThickness(float Value)
{
    validateNonNegative(Value, "BorderThickness");
    BorderThickness = Value;
    invalidate(EInvalidationReason::Paint);
}

float ButtonWidget::getBorderThickness() const noexcept
{
    return BorderThickness;
}

void ButtonWidget::setCornerRadius(float Value)
{
    validateNonNegative(Value, "CornerRadius");
    CornerRadius = Value;
    invalidate(EInvalidationReason::Paint);
}

float ButtonWidget::getCornerRadius() const noexcept
{
    return CornerRadius;
}

void ButtonWidget::setPadding(Padding Value)
{
    PaddingValue = Value;
    if (auto* Slot = getChildSlot())
    {
        Slot->setPadding(Value);
    }
}

Padding ButtonWidget::getPadding() const noexcept
{
    return PaddingValue;
}

void ButtonWidget::setEnabled(bool Value)
{
    if (Enabled == Value)
    {
        return;
    }
    Enabled = Value;
    if (!Enabled)
    {
        // 用状态变更助手复位, 使 Hover/Pressed 变化回调保持同步.
        setHoveredState(false);
        setPressedState(false);
        KeyPressed = false;
    }
    invalidate(EInvalidationReason::Paint);
}

bool ButtonWidget::isEnabled() const noexcept
{
    return Enabled;
}

bool ButtonWidget::isPressed() const noexcept
{
    return Pressed;
}

bool ButtonWidget::isHovered() const noexcept
{
    return Hovered;
}

Vector2 ButtonWidget::onMeasure(Vector2 AvailableSize) const
{
    const Vector2 Inner = subtractPadding(AvailableSize, PaddingValue);
    Vector2 Desired{};
    if (const auto* Child = getChild())
    {
        Desired = Child->measure(Inner);
    }
    return Desired + getPaddingSize(PaddingValue);
}

void ButtonWidget::onArrange(UIRect)
{
    syncChildSlot();
}

void ButtonWidget::onPaint(PaintContext& Context) const
{
    const UIRect Rect{0, 0, getBounds().Width, getBounds().Height};
    Context.fillRoundedRect(Rect, CornerRadius, getEffectiveFillColor());
    if (BorderThickness > 0)
    {
        Context.strokeRoundedRect(Rect, CornerRadius, BorderColor, BorderThickness);
    }
}

EventReply ButtonWidget::onMouseButtonDown(const MouseButtonEvent& Event, UIPoint)
{
    if (!Enabled || Event.Button != MouseButton::Left)
    {
        return EventReply::Unhandled;
    }
    setPressedState(true);
    if (OnDown)
    {
        OnDown();
    }
    return EventReply::Handled;
}

EventReply ButtonWidget::onMouseButtonUp(const MouseButtonEvent& Event, UIPoint)
{
    if (Event.Button != MouseButton::Left)
    {
        return EventReply::Unhandled;
    }
    setPressedState(false);
    if (!Enabled)
    {
        return EventReply::Unhandled;
    }
    if (OnUp)
    {
        OnUp();
    }
    return EventReply::Handled;
}

EventReply ButtonWidget::onMouseButtonClicked(const MouseButtonEvent& Event, UIPoint)
{
    if (!Enabled || Event.Button != MouseButton::Left)
    {
        return EventReply::Unhandled;
    }
    fireClick();
    return EventReply::Handled;
}

EventReply ButtonWidget::onMouseEnter(const MouseMoveEvent&, UIPoint)
{
    if (!Enabled)
    {
        return EventReply::Unhandled;
    }
    setHoveredState(true);
    return EventReply::Handled;
}

EventReply ButtonWidget::onMouseLeave(const MouseMoveEvent&, UIPoint)
{
    setHoveredState(false);
    // 指针移出时取消按下视觉, 避免拖出后释放导致状态滞留.
    setPressedState(false);
    return Enabled ? EventReply::Handled : EventReply::Unhandled;
}

EventReply ButtonWidget::onKeyDown(const KeyEvent& Event)
{
    if (!Enabled || !isActivationKey(Event))
    {
        return EventReply::Unhandled;
    }
    if (Event.Repeat)
    {
        return EventReply::Handled;
    }
    KeyPressed = true;
    setPressedState(true);
    return EventReply::Handled;
}

EventReply ButtonWidget::onKeyUp(const KeyEvent& Event)
{
    if (!isActivationKey(Event))
    {
        return EventReply::Unhandled;
    }
    const bool WasPressed = KeyPressed;
    KeyPressed = false;
    setPressedState(false);
    if (Enabled && WasPressed)
    {
        fireClick();
        return EventReply::Handled;
    }
    return EventReply::Unhandled;
}

Color ButtonWidget::getEffectiveFillColor() const noexcept
{
    if (!Enabled)
    {
        return DisabledColor;
    }
    if (Pressed)
    {
        return PressedColor;
    }
    if (Hovered)
    {
        return HoverColor;
    }
    return BackgroundColor;
}

void ButtonWidget::setPressedState(bool Value)
{
    if (Pressed == Value)
    {
        return;
    }
    Pressed = Value;
    invalidate(EInvalidationReason::Paint);
    if (OnPressedChanged)
    {
        OnPressedChanged(Pressed);
    }
}

void ButtonWidget::setHoveredState(bool Value)
{
    if (Hovered == Value)
    {
        return;
    }
    Hovered = Value;
    invalidate(EInvalidationReason::Paint);
    if (OnHoverChanged)
    {
        OnHoverChanged(Hovered);
    }
}

void ButtonWidget::fireClick() const
{
    if (OnClick)
    {
        OnClick();
    }
}

void ButtonWidget::syncChildSlot()
{
    if (auto* Slot = getChildSlot())
    {
        if (!Slot->isLayoutEnabled())
        {
            Slot->setLayoutEnabled(true);
        }
        const Padding Current = Slot->getPadding();
        if (Current.Left != PaddingValue.Left || Current.Top != PaddingValue.Top ||
            Current.Right != PaddingValue.Right || Current.Bottom != PaddingValue.Bottom)
        {
            Slot->setPadding(PaddingValue);
        }
    }
}

}
