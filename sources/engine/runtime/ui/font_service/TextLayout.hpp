#pragma once

#include "Layout.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace ui
{
struct TextGlyph
{
    TextureHandle Texture;
    UIRect Bounds;
    UIRect UV;
    std::uint32_t Cluster;
    bool Colored{};
};

struct TextMetrics
{
    float Width{}, Height{}, Ascent{}, Descent{}, LineHeight{};
};

struct TextCaret
{
    std::uint32_t Offset{};
    UIRect Bounds;
};

/**
* @brief 拥有 UTF-32 文本、逻辑坐标下的字形四边形和簇边界光标位置. 
* TextGlyph::Cluster 和 TextCaret::Offset 均为文本中的码点索引. 
 * 纹理引用仅在创建布局的服务存活期间有效. 
 * 不额外生成逐码点或连字内部的光标位置. 
 * 测量使用已塑形的度量, 不进行塑形或渲染器操作. 
 * 若换行约束发生变化, 须通过 FontService::createTextLayout 重新创建布局. 
 */
struct TextLayout : IVisual
{
    std::vector<TextGlyph> Glyphs;
    std::u32string Text;
    TextMetrics Metrics;
    std::vector<TextCaret> Carets;

    Vector2 measure(Vector2 AvailableSize) const override;
    // @note 仅缓存分配区域；字形和光标坐标仍使用局部坐标. 
    void arrange(UIRect Bounds) override;
    UIRect getBounds() const noexcept;
    const TextMetrics& getMetrics() const noexcept;
    // @note 返回码点索引表示的簇边界, 也可返回文本末尾. 
    std::uint32_t hitTest(UIPoint Point) const noexcept;
    /**
     * @brief 限制偏移范围, 并向不大于该偏移的簇边界吸附. 
     * 换行处的共享边界采用下游亲和性, 即选择下一行的光标位置. 
     * @param Offset UTF-32 码点索引. 
     * @return 对应簇边界在局部逻辑坐标下的光标矩形. 
     */
    UIRect getCaretRect(std::uint32_t Offset) const noexcept;

private:
    UIRect Allocation{};
};
}