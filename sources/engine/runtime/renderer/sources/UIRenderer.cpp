#include <renderer/UIRenderer.hpp>

#include <algorithm>

namespace runtime::renderer
{

namespace
{

/**
 * @brief 合成最终颜色: Base * Tint * Opacity, 并预乘 alpha.
 * @note 2D 合成默认预乘 alpha(SrcColor=One, DstColor=OneMinusSrcAlpha), 可减少边缘色泄漏.
 */
[[nodiscard]] core::LinearColor4D composeColor(const ui::UIColor& Base,
	const ui::UIColor& Tint, float Opacity) noexcept
{
	const float A = Base.a() * Opacity;
	return core::LinearColor4D(Base.r() * Tint.r(), Base.g() * Tint.g(),
		Base.b() * Tint.b(), A).getPremultipliedAlpha();
}

} // namespace

void UIRenderer::submit(const ui::RenderNode& Root)
{
	std::vector<UIQuad> Quads;
	collect(Root, ui::UIColor { 1.0f, 1.0f, 1.0f, 1.0f }, 1.0f, Quads);
	batch(std::move(Quads));
}

void UIRenderer::submitIncremental(const ui::RenderNode& Root, EChangeFlags Flags)
{
	// v1: 增量接口退化为全量重批, 但保留变化意图以便上层统计/未来优化.
	ChangeFlags.Value |= Flags.Value;
	submit(Root);
}

EChangeFlags UIRenderer::consumeChangeFlags() noexcept
{
	EChangeFlags Result = ChangeFlags;
	ChangeFlags = EChangeFlags_t::None;
	return Result;
}

void UIRenderer::clear() noexcept
{
	Batches.clear();
	QuadCount = 0;
}

void UIRenderer::collect(const ui::RenderNode& Node, const ui::UIColor& InheritTint,
	float InheritOpacity, std::vector<UIQuad>& OutQuads)
{
	// 累积 Tint 与 Opacity(沿控件树向下传播).
	const ui::UIColor Tint { InheritTint.r() * Node.Tint.r(),
		InheritTint.g() * Node.Tint.g(), InheritTint.b() * Node.Tint.b(),
		InheritTint.a() * Node.Tint.a() };
	const float Opacity = InheritOpacity * Node.Opacity;

	for (const ui::RenderCommand& Command : Node.Commands)
	{
		// 文本命令需要 FontAtlas 提供 Glyph UV; v1 跳过(占位), 见文档 §UI 文本.
		if (!Command.Text.empty())
			continue;

		UIQuad Quad;
		Quad.Min = { static_cast<float>(Command.Bounds.X),
			static_cast<float>(Command.Bounds.Y) };
		Quad.Max = { static_cast<float>(Command.Bounds.X + Command.Bounds.Width),
			static_cast<float>(Command.Bounds.Y + Command.Bounds.Height) };
		Quad.Color = composeColor(Command.Color, Tint, Opacity);
		Quad.TextureIndex = Command.TextureId;
		Quad.Layer = Command.Layer;
		if (Node.ShouldClipToBounds)
			Quad.Flags.set(EUIQuadFlags_t::Clipped);
		OutQuads.push_back(Quad);
	}

	for (const ui::RenderNode& Child : Node.Children)
		collect(Child, Tint, Opacity, OutQuads);
}

void UIRenderer::batch(std::vector<UIQuad>&& Quads)
{
	Batches.clear();
	QuadCount = Quads.size();
	if (Quads.empty())
		return;

	// 稳定排序: 先按 Layer(z-order), 层内按纹理索引聚合, 保证绘制顺序正确且可合批.
	std::stable_sort(Quads.begin(), Quads.end(), [](const UIQuad& Lhs, const UIQuad& Rhs) {
		if (Lhs.Layer != Rhs.Layer)
			return Lhs.Layer < Rhs.Layer;
		return Lhs.TextureIndex < Rhs.TextureIndex;
	});

	// 聚合: 相同(纹理 + 层)的图元合并进一个批次.
	UIRenderBatch Current;
	Current.Key = makeBatchKey(Quads.front().TextureIndex, /* Transparent */ true);
	Current.TextureIndex = Quads.front().TextureIndex;
	Current.Transparent = true;
	Current.Quads.push_back(Quads.front());

	for (size_t Index = 1; Index < Quads.size(); ++Index)
	{
		const UIQuad& Quad = Quads[Index];
		const UIRenderBatch::BatchKey Key = makeBatchKey(Quad.TextureIndex, true);
		if (Key == Current.Key && Quad.Layer == Current.Quads.back().Layer)
		{
			Current.Quads.push_back(Quad);
		}
		else
		{
			Batches.push_back(std::move(Current));
			Current = UIRenderBatch {};
			Current.Key = Key;
			Current.TextureIndex = Quad.TextureIndex;
			Current.Transparent = true;
			Current.Quads.push_back(Quad);
		}
	}
	Batches.push_back(std::move(Current));
}

UIRenderBatch::BatchKey UIRenderer::makeBatchKey(uint32_t TextureIndex,
	bool Transparent) noexcept
{
	return (Transparent ? (uint64_t { 1 } << 63) : 0) |
		(uint64_t { TextureIndex } & 0x7FFFFFFFFFFFFFFF);
}

} // namespace runtime::renderer
