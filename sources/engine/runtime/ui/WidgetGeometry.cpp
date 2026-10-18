#include "WidgetGeometry.hpp"
#include <cmath>

namespace ui
{
Vector2 WidgetGeometry::anchorPoint(Vector2 ParentOrigin, Vector2 ParentSize, Vector2 Anchor,
                                    Vector2 Position) noexcept
{
    return ParentOrigin + Anchor * ParentSize + Position;
}

Vector2 WidgetGeometry::originFromPivot(Vector2 PivotPoint, Vector2 PivotNormalized, Vector2 Size) noexcept
{
    return PivotPoint - PivotNormalized * Size;
}

Vector2 WidgetGeometry::pivotFromOrigin(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size) noexcept
{
    return Origin + PivotNormalized * Size;
}

UITransform WidgetGeometry::translation(Vector2 Offset) noexcept
{
    return {1, 0, 0, 1, Offset.x(), Offset.y()};
}

UITransform WidgetGeometry::rotation(float AngleRadians) noexcept
{
    const float C = std::cos(AngleRadians), S = std::sin(AngleRadians);
    return {C, -S, S, C, 0, 0};
}

UITransform WidgetGeometry::localToParent(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size,
                                          float Rotation) noexcept
{
    const float PivotX = PivotNormalized.x() * Size.x();
    const float PivotY = PivotNormalized.y() * Size.y();
    const float C = std::cos(Rotation), S = std::sin(Rotation);
    // M = T(origin) * T(pivot) * R * T(-pivot)
    //   2x2 = R; 平移 = origin + pivot - R*pivot.
    return {C, -S, S, C,
            Origin.x() + (1 - C) * PivotX + S * PivotY,
            Origin.y() - S * PivotX + (1 - C) * PivotY};
}

UITransform WidgetGeometry::renderTransform(UIRect Bounds, const RenderTransform& RT) noexcept
{
    const float PivotX = RT.Pivot.x() * Bounds.Width;
    const float PivotY = RT.Pivot.y() * Bounds.Height;
    const float C = std::cos(RT.Rotation), S = std::sin(RT.Rotation);
    const float XX = RT.Scale.x() * C;
    const float XY = -RT.Scale.y() * S;
    const float YX = RT.Scale.x() * S;
    const float YY = RT.Scale.y() * C;
    return {XX, XY, YX, YY,
            Bounds.X + RT.Translation.x() + PivotX - (XX * PivotX + XY * PivotY),
            Bounds.Y + RT.Translation.y() + PivotY - (YX * PivotX + YY * PivotY)};
}

UITransform WidgetGeometry::parentToLocal(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size,
                                          float Rotation) noexcept
{
    return invert(localToParent(Origin, PivotNormalized, Size, Rotation));
}

UITransform WidgetGeometry::invert(const UITransform& Transform) noexcept
{
    // [XX XY; YX YY] 的逆 = [YY -XY; -YX XX] / det; 平移逆 = -A^-1 * T.
    const float Det = Transform.XX * Transform.YY - Transform.XY * Transform.YX;
    const float InvDet = Det != 0 ? 1.0f / Det : 0.0f;
    const float IXX = Transform.YY * InvDet;
    const float IXY = -Transform.XY * InvDet;
    const float IYX = -Transform.YX * InvDet;
    const float IYY = Transform.XX * InvDet;
    return {IXX, IXY, IYX, IYY, -(IXX * Transform.TX + IXY * Transform.TY),
            -(IYX * Transform.TX + IYY * Transform.TY)};
}
}
