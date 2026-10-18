#pragma once

#include <optional>
#include "Slot.hpp"
#include "FlexibleLayout.hpp"

namespace ui
{

// FlexibleBox 的插槽: 持有每个子项的一维 flex 配置, 由 FlexibleLayout 统一消费.
class FlexibleSlot : public PanelSlot
{
public:
    FlexibleSlot(Widget& Parent, std::unique_ptr<Widget> Child);

    // 主轴拉伸权重; 0 表示不拉伸 (默认).
    void setFlexGrow(float Value) noexcept;
    float getFlexGrow() const noexcept;

    // 主轴基准尺寸; 0 表示自动(依据子项测量结果).
    void setFlexBasis(float Value) noexcept;
    float getFlexBasis() const noexcept;

    // 主轴收缩权重; 空间不足且容器 Overflow=Shrink 时按权重收缩 (默认 1).
    void setFlexShrink(float Value) noexcept;
    float getFlexShrink() const noexcept;

    // 覆盖容器的交叉轴对齐; 空表示跟随容器设置.
    void setAlignSelf(std::optional<ECrossAxisAlignment> Value) noexcept;
    std::optional<ECrossAxisAlignment> getAlignSelf() const noexcept;

    // 子项四周的额外间距.
    void setMargin(Padding Value) noexcept;
    const Padding& getMargin() const noexcept;

private:
    float FlexGrow{0};
    float FlexBasis{0};
    float FlexShrink{1};
    std::optional<ECrossAxisAlignment> AlignSelf;
    Padding Margin;
};

}
