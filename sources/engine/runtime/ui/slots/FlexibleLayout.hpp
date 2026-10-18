#pragma once

#include <vector>
#include "Slot.hpp"

namespace ui
{

class FlexibleSlot;

// 主轴方向: Row 主轴沿 X 轴, Column 主轴沿 Y 轴; Reverse 变体反转主轴方向.
enum class EFlexDirection
{
    Row,
    RowReverse,
    Column,
    ColumnReverse
};

// 主轴对齐 (justify-content): 决定剩余空间在主轴上的分配方式.
enum class EMainAxisAlignment
{
    Start,        // 靠主轴起始 (默认)
    Center,       // 居中
    End,          // 靠主轴末尾
    SpaceBetween, // 两端对齐, 剩余空间均分到子项之间
    SpaceAround,  // 每个子项两侧均分
    SpaceEvenly   // 首尾与子项之间间距相等
};

// 交叉轴对齐 (align-items): 决定子项在交叉轴上的尺寸与位置.
enum class ECrossAxisAlignment
{
    Start,   // 靠交叉轴起始
    Center,  // 交叉轴居中
    End,     // 靠交叉轴末尾
    Stretch  // 拉伸填满交叉轴 (默认)
};

// 主轴空间不足时的处理策略.
enum class EFlexOverflow
{
    Clip,   // 裁剪: 保持尺寸不变, 超出部分被容器边界裁剪 (默认)
    Shrink  // 收缩: 按 flex-shrink 权重成比例收缩子项
};

// FlexibleBox 的一维布局计算引擎.
// 依据主轴/交叉轴/方向/间距/对齐等基础属性, 在 measure/arrange 阶段统一计算各子项的尺寸与位置.
class FlexibleLayout : public ILayout
{
public:
    EFlexDirection Direction{EFlexDirection::Row};
    EMainAxisAlignment MainAxisAlignment{EMainAxisAlignment::Start};
    ECrossAxisAlignment CrossAxisAlignment{ECrossAxisAlignment::Stretch};
    EFlexOverflow Overflow{EFlexOverflow::Clip};
    float Spacing{0};

    // 校验方向/对齐/间距等配置合法性.
    void validate() const override;

    // 计算容器自然尺寸: 主轴为各子项主轴尺寸之和(加间距), 交叉轴取子项最大值.
    Vector2 measure(const std::vector<const FlexibleSlot*>& Slots, Vector2 AvailableSize) const;
    // 依据 Content 计算每个子项的外接矩形, 并调用其 arrange.
    void arrange(const std::vector<FlexibleSlot*>& Slots, UIRect Content) const;

private:
    bool isHorizontal() const noexcept;
    bool isReverse() const noexcept;
};

}
