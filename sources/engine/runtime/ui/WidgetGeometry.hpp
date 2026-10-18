#pragma once

#include "UIDrawList.hpp"

namespace ui
{
// WidgetGeometry: 锚点-转轴模型的几何封装, 全部为无状态的静态方法.
// 约定: 角度用弧度, 逆时针为正; Pivot 用归一化 (0..1); 坐标与尺寸用逻辑单位.
class WidgetGeometry
{
public:
    WidgetGeometry() = delete;

    // 轴心在父坐标系的落点 = parentOrigin + anchor * parentSize + position.
    static Vector2 anchorPoint(Vector2 ParentOrigin, Vector2 ParentSize, Vector2 Anchor,
                               Vector2 Position) noexcept;
    // 未旋转矩形原点 = 轴心 - pivot * size.
    static Vector2 originFromPivot(Vector2 PivotPoint, Vector2 PivotNormalized, Vector2 Size) noexcept;
    // 轴心 = 原点 + pivot * size.
    static Vector2 pivotFromOrigin(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size) noexcept;

    // 平移矩阵; 旋转矩阵 (绕原点, 逆时针).
    static UITransform translation(Vector2 Offset) noexcept;
    static UITransform rotation(float AngleRadians) noexcept;

    // 局部 -> 父: T(origin) * T(pivot*size) * R * T(-pivot*size). 旋转绕轴心, 轴心保持不动.
    static UITransform localToParent(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size,
                                     float Rotation) noexcept;
    // 渲染变换矩阵: T(bounds) * T(translation) * T(pivot) * R * S * T(-pivot).
    static UITransform renderTransform(UIRect Bounds, const RenderTransform& RT) noexcept;
    // 父 -> 局部: localToParent 的逆变换.
    static UITransform parentToLocal(Vector2 Origin, Vector2 PivotNormalized, Vector2 Size,
                                     float Rotation) noexcept;
    // 2x2 仿射矩阵求逆; 仅用于可逆的旋转+平移变换 (det 非零).
    static UITransform invert(const UITransform& Transform) noexcept;
};
}
