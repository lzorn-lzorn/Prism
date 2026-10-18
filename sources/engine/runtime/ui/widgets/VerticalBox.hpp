#pragma once

#include "FlexibleBox.hpp"

namespace ui
{

// 主轴沿 Y 轴的垂直布局盒子.
class VerticalBox final : public FlexibleBox
{
public:
    VerticalBox()
        : FlexibleBox(EFlexDirection::Column)
    {
    }
};

}
