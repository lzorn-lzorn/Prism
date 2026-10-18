#include "FlexibleSlot.hpp"
#include "Widget.hpp"
#include <algorithm>
#include <utility>

namespace ui
{

FlexibleSlot::FlexibleSlot(Widget& Parent, std::unique_ptr<Widget> Child)
    : PanelSlot(Parent, std::move(Child))
{
}

void FlexibleSlot::setFlexGrow(float Value) noexcept
{
    FlexGrow = std::max(0.0f, Value);
}

float FlexibleSlot::getFlexGrow() const noexcept
{
    return FlexGrow;
}

void FlexibleSlot::setFlexBasis(float Value) noexcept
{
    FlexBasis = std::max(0.0f, Value);
}

float FlexibleSlot::getFlexBasis() const noexcept
{
    return FlexBasis;
}

void FlexibleSlot::setFlexShrink(float Value) noexcept
{
    FlexShrink = std::max(0.0f, Value);
}

float FlexibleSlot::getFlexShrink() const noexcept
{
    return FlexShrink;
}

void FlexibleSlot::setAlignSelf(std::optional<ECrossAxisAlignment> Value) noexcept
{
    AlignSelf = Value;
}

std::optional<ECrossAxisAlignment> FlexibleSlot::getAlignSelf() const noexcept
{
    return AlignSelf;
}

void FlexibleSlot::setMargin(Padding Value) noexcept
{
    Margin = Value;
}

const Padding& FlexibleSlot::getMargin() const noexcept
{
    return Margin;
}

}
