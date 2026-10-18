#pragma once

#include "FlexibleBox.hpp"

namespace ui
{

// 主轴沿 X 轴的水平布局盒子.
class HorizontalBox final : public FlexibleBox
{
public:
    HorizontalBox()
        : FlexibleBox(EFlexDirection::Row)
    {
    }
};

}
