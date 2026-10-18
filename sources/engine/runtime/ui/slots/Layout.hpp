
#pragma once

#include "UIDrawList.hpp"

namespace ui
{
class IVisual
{
public:
    virtual ~IVisual() = default;
    virtual Vector2 measure(Vector2 AvailableSize) const = 0;
    virtual void arrange(UIRect Bounds) = 0;
};

void validateLayoutSize(Vector2 Size);
void validateLayoutBounds(UIRect Bounds);

enum class LayoutPreset
{
    TopLeft, TopCenter, TopRight,
    CenterLeft, Center, CenterRight,
    BottomLeft, BottomCenter, BottomRight,
    StretchTop, StretchCenter, StretchBottom,
    StretchLeft, StretchMiddle, StretchRight,
    StretchAll
};

struct Anchors
{
    Vector2 Min;
    Vector2 Max;
};

// 布局抽象基类: 描述"如何将内容摆放到父矩形内"的布局配置.
// 具体实现目前有锚点布局(Layout)与弹性布局(FlexibleLayout)两种.
class ILayout
{
public:
    virtual ~ILayout() = default;

    // 校验布局配置是否合法; 非法时抛出 Error.
    virtual void validate() const = 0;
};

// 锚点布局: 基于归一化锚点/轴心/偏移/旋转的布局模型.
struct Layout : public ILayout
{
    Anchors Anchor;
    Vector2 Pivot;
    Vector2 Position;
    Vector2 Size;
    Vector2 StartMargin;
    Vector2 EndMargin;
    // 绕轴心 (Pivot) 的旋转, 弧度, 逆时针为正.
    float Rotation{};

    void setPreset(LayoutPreset Preset);
    void validate() const override;
    UIRect calculateBounds(UIRect ParentContent) const;
};
}