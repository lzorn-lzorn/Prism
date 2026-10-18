#include "FlexibleBox.hpp"
#include <infra/common/Error.hpp>
#include "FlexibleSlot.hpp"
#include <cmath>
#include <utility>

namespace ui
{

FlexibleBox::FlexibleBox(EFlexDirection Direction)
{
    Layout.Direction = Direction;
}

std::unique_ptr<PanelSlot> FlexibleBox::createSlot(std::unique_ptr<Widget> Child)
{
    return std::make_unique<FlexibleSlot>(*this, std::move(Child));
}

void FlexibleBox::setDirection(EFlexDirection Value) noexcept
{
    Layout.Direction = Value;
    invalidate(EInvalidationReason::IVisual);
}

EFlexDirection FlexibleBox::getDirection() const noexcept
{
    return Layout.Direction;
}

void FlexibleBox::setMainAxisAlignment(EMainAxisAlignment Value) noexcept
{
    Layout.MainAxisAlignment = Value;
    invalidate(EInvalidationReason::IVisual);
}

EMainAxisAlignment FlexibleBox::getMainAxisAlignment() const noexcept
{
    return Layout.MainAxisAlignment;
}

void FlexibleBox::setCrossAxisAlignment(ECrossAxisAlignment Value) noexcept
{
    Layout.CrossAxisAlignment = Value;
    invalidate(EInvalidationReason::IVisual);
}

ECrossAxisAlignment FlexibleBox::getCrossAxisAlignment() const noexcept
{
    return Layout.CrossAxisAlignment;
}

void FlexibleBox::setSpacing(float Value)
{
    if (!std::isfinite(Value) || Value < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Flex spacing must be finite and nonnegative");
    }
    Layout.Spacing = Value;
    invalidate(EInvalidationReason::IVisual);
}

float FlexibleBox::getSpacing() const noexcept
{
    return Layout.Spacing;
}

void FlexibleBox::setOverflow(EFlexOverflow Value) noexcept
{
    Layout.Overflow = Value;
    invalidate(EInvalidationReason::IVisual);
}

EFlexOverflow FlexibleBox::getOverflow() const noexcept
{
    return Layout.Overflow;
}

FlexibleSlot& FlexibleBox::addChild(std::unique_ptr<Widget> Child)
{
    return static_cast<FlexibleSlot&>(PanelWidget::addSlot(std::move(Child)));
}

FlexibleSlot& FlexibleBox::getChildSlot(size_t Index)
{
    return static_cast<FlexibleSlot&>(PanelWidget::getChildSlot(Index));
}

const FlexibleSlot& FlexibleBox::getChildSlot(size_t Index) const
{
    return static_cast<const FlexibleSlot&>(PanelWidget::getChildSlot(Index));
}

Vector2 FlexibleBox::onMeasure(Vector2 AvailableSize) const
{
    std::vector<const FlexibleSlot*> FlexSlots;
    FlexSlots.reserve(getSlotCount());
    for (size_t Index = 0; Index < getSlotCount(); ++Index)
    {
        FlexSlots.push_back(&static_cast<const FlexibleSlot&>(getSlot(Index)));
    }
    return Layout.measure(FlexSlots, AvailableSize);
}

void FlexibleBox::arrangeChildren(UIRect Content)
{
    std::vector<FlexibleSlot*> FlexSlots;
    FlexSlots.reserve(getSlotCount());
    for (size_t Index = 0; Index < getSlotCount(); ++Index)
    {
        FlexSlots.push_back(&static_cast<FlexibleSlot&>(getSlot(Index)));
    }
    Layout.arrange(FlexSlots, Content);
}

}
