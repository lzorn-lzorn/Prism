#include <infra/common/Error.hpp>
#include "Layout.hpp"
#include "WidgetGeometry.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace ui
{
namespace
{
bool isFinite(Vector2 Value)
{
    return std::isfinite(Value.x()) && std::isfinite(Value.y());
}

bool isNormalized(Vector2 Value)
{
    return isFinite(Value) && Value.x() >= 0 && Value.x() <= 1 && Value.y() >= 0 && Value.y() <= 1;
}
}

void validateLayoutSize(Vector2 Size)
{
    if (!isFinite(Size) || Size.x() < 0 || Size.y() < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Layout size must be finite and nonnegative");
    }
}

void validateLayoutBounds(UIRect Bounds)
{
    validateLayoutSize(Bounds.getSize());
    if (!isFinite(Bounds.getPosition()))
    {
        Error::raise(EErrorCode::InvalidArgument, "Layout position must be finite");
    }
}

void Layout::setPreset(LayoutPreset Preset)
{
    constexpr std::array<Anchors, 16> Values{{
        {{0, 0}, {0, 0}}, {{0.5f, 0}, {0.5f, 0}}, {{1, 0}, {1, 0}},
        {{0, 0.5f}, {0, 0.5f}}, {{0.5f, 0.5f}, {0.5f, 0.5f}}, {{1, 0.5f}, {1, 0.5f}},
        {{0, 1}, {0, 1}}, {{0.5f, 1}, {0.5f, 1}}, {{1, 1}, {1, 1}},
        {{0, 0}, {1, 0}}, {{0, 0.5f}, {1, 0.5f}}, {{0, 1}, {1, 1}},
        {{0, 0}, {0, 1}}, {{0.5f, 0}, {0.5f, 1}}, {{1, 0}, {1, 1}},
        {{0, 0}, {1, 1}}
    }};
    const auto Index = static_cast<std::size_t>(Preset);
    if (Index >= Values.size())
    {
        Error::raise(EErrorCode::InvalidArgument, "Unknown anchor preset");
    }
    Anchor = Values[Index];
    Pivot = (Anchor.Min + Anchor.Max) * 0.5f;
}

void Layout::validate() const
{
    if (!isNormalized(Anchor.Min) || !isNormalized(Anchor.Max) ||
        Anchor.Min.x() > Anchor.Max.x() || Anchor.Min.y() > Anchor.Max.y() ||
        !isNormalized(Pivot) || !isFinite(Position) || !isFinite(Size) ||
        Size.x() < 0 || Size.y() < 0 || !isFinite(StartMargin) || !isFinite(EndMargin) ||
        !std::isfinite(Rotation))
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid anchor layout");
    }
}

UIRect Layout::calculateBounds(UIRect ParentContent) const
{
    validate();
    validateLayoutBounds(ParentContent);
    const auto ParentPosition = ParentContent.getPosition();
    const auto ParentSize = ParentContent.getSize();
    const Vector2 Start = ParentPosition + Anchor.Min * ParentSize;
    const Vector2 End = ParentPosition + Anchor.Max * ParentSize;
    // 固定轴: 原点 = 轴心(锚点+偏移) - pivot*size, 由 WidgetGeometry 统一计算.
    Vector2 ResultPosition = WidgetGeometry::originFromPivot(
        WidgetGeometry::anchorPoint(ParentPosition, ParentSize, Anchor.Min, Position), Pivot, Size);
    Vector2 ResultSize = Size;
    if (Anchor.Min.x() != Anchor.Max.x())
    {
        ResultPosition.x() = Start.x() + StartMargin.x();
        ResultSize.x() = std::max(0.0f, End.x() - Start.x() - StartMargin.x() - EndMargin.x());
    }
    if (Anchor.Min.y() != Anchor.Max.y())
    {
        ResultPosition.y() = Start.y() + StartMargin.y();
        ResultSize.y() = std::max(0.0f, End.y() - Start.y() - StartMargin.y() - EndMargin.y());
    }
    if (!isFinite(ResultPosition) || !isFinite(ResultSize))
    {
        Error::raise(EErrorCode::InvalidArgument, "Anchor layout overflow");
    }
    return {ResultPosition.x(), ResultPosition.y(), ResultSize.x(), ResultSize.y()};
}
}