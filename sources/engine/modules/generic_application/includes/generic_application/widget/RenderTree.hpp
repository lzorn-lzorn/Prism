#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "generic_application/UICommon.hpp"
#include "generic_application/widget/UIElement.hpp"


namespace ui
{
/**
 * @brief 控件树中的一条绘制命令: 一个矩形 + 颜色 + 可选纹理/文字.
 *
 * 这是控件树(UI 层)与渲染器之间的中间表示, 因此刻意不持有 RHI 对象:
 *   - TextureId 是纹理表/图集索引(0 表示纯色), 由上层 UI 系统解析到具体纹理;
 *   - Text 非空表示文本命令, 由 UIRenderer 依据字体图集转换成 Glyph 图元.
 */
struct RenderCommand
{
	UIRectangle Bounds;
	UIColor Color;
	/** 纹理表/图集索引; 0 = 纯色填充. */
	uint32_t TextureId = 0;
	/** 文本内容; 空串表示非文本命令. */
	std::string Text;
	/** 绘制层级(z-order), 用于稳定排序. */
	uint32_t Layer = 0;
};

struct RenderNode
{
	UIRectangle Bounds;
	UIColor Tint;
	float Opacity = 1.0f;
	std::vector<RenderCommand> Commands;
	std::vector<RenderNode> Children;
	bool ShouldClipToBounds = false;
};

class RenderTreeBuilder
{
public:
	RenderNode build(const UIElement& Root);

private:
	void traverse(const UIElement& Elem, RenderNode& Parent);

};
} // namespace ui
