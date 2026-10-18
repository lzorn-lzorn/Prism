#pragma once

#include <cstddef>
#include "Widget.hpp"
#include "FlexibleLayout.hpp"

namespace ui
{

class FlexibleSlot;

// FlexibleBox: 一维 flex 容器.
// 依据主轴/交叉轴/方向/间距/对齐等基础属性, 对内部子项做自动一维布局.
// 派生类 HorizontalBox/VerticalBox 分别固定主轴为水平/垂直方向.
class FlexibleBox : public PanelWidget
{
public:
    FlexibleBox() = default;

    void setDirection(EFlexDirection Value) noexcept;
    EFlexDirection getDirection() const noexcept;

    void setMainAxisAlignment(EMainAxisAlignment Value) noexcept;
    EMainAxisAlignment getMainAxisAlignment() const noexcept;

    void setCrossAxisAlignment(ECrossAxisAlignment Value) noexcept;
    ECrossAxisAlignment getCrossAxisAlignment() const noexcept;

    void setSpacing(float Value);
    float getSpacing() const noexcept;

    // 主轴空间不足时的策略: Clip 裁剪(默认) / Shrink 收缩.
    void setOverflow(EFlexOverflow Value) noexcept;
    EFlexOverflow getOverflow() const noexcept;

    // 添加子控件并返回其插槽, 便于随后配置 grow/basis/align/margin.
    // 该函数隐藏了 PanelWidget::addChild(返回 Widget&).
    FlexibleSlot& addChild(std::unique_ptr<Widget> Child);
    // 按子控件索引访问插槽, 隐藏了 PanelWidget::getChildSlot(返回 PanelSlot&).
    FlexibleSlot& getChildSlot(size_t Index);
    const FlexibleSlot& getChildSlot(size_t Index) const;

protected:
    explicit FlexibleBox(EFlexDirection Direction);

    std::unique_ptr<PanelSlot> createSlot(std::unique_ptr<Widget> Child) override;
    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void arrangeChildren(UIRect Content) override;

private:
    FlexibleLayout Layout;
};

}
