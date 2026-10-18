#pragma once

#include "UIDrawList.hpp"
#include <array>
#include <cstddef>

namespace ui
{
// 语义颜色角色: 底层框架只定义"有哪些角色", 不决定具体颜色.
// 具体颜色由上层调用者填充 Palette 后传入.
enum class EColorRole
{
    Window,     // 窗口/根背景
    Text,       // 正文
    Border,     // 边框
    Accent,     // 强调/选中
    Hover,      // 悬停
    Pressed,    // 按下
    Disabled,   // 禁用
    Count       // 角色数量 (哨兵)
};

// 调色板工具: 角色 -> 颜色 的映射表, 由上层调用者填充.
// 底层框架不关心调用者如何选择颜色, 只提供这个容器供其使用.
class Palette
{
public:
    Palette() = default;

    void setColor(EColorRole Role, Color Value) noexcept
    {
        Colors[index(Role)] = Value;
    }

    Color color(EColorRole Role) const noexcept
    {
        return Colors[index(Role)];
    }

private:
    static std::size_t index(EColorRole Role) noexcept
    {
        return static_cast<std::size_t>(Role);
    }

    std::array<Color, static_cast<std::size_t>(EColorRole::Count)> Colors{};
};

}
