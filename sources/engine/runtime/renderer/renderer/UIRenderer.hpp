#pragma once

#include <renderer/RenderData.hpp>

#include <generic_application/widget/RenderTree.hpp>

#include <cstdint>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - UI 渲染器(控件树 -> 合批交换格式)
 * ============================================================================
 *
 *  [职责]
 *  UIRenderer 是纯 CPU 的数据变换层, 不持有任何 RHI 对象:
 *      - 输入: ui::RenderNode 控件树(由 RenderTreeBuilder 从 UIElement 构建);
 *      - 输出: UIBatchList 合批后的渲染交换格式(UIQuad 按合批键聚合).
 *
 *  [合批]
 *  相同"纹理索引 + 透明性"的 UIQuad 被合并进同一个 UIRenderBatch, 使得 2D 渲染器
 *  可以一次性上传 instance 数据并 draw, 避免逐图元 draw call.
 *
 *  [增量渲染]
 *  submitIncremental 接收变化标志. v1 内部退化为全量重批(每次 collect 全部图元),
 *  但接口已为"脏矩形 + 持久化离屏 Canvas"的增量方案预留(见文档 §增量渲染):
 *      - Layout 变化: 仅重新布局受影响区域;
 *      - Content 变化: 仅重批内容变化的子树;
 *      - 无变化: 复用上一帧 UIBatchList, 零 CPU 开销.
 *
 *  [与 2D 渲染器的边界]
 *  UIRenderer 只产出"画什么"(UIBatchList), "怎么画"(Pipeline 解析/纹理表/顶点上传/
 *  绘制)由 RendererServer 内的 2D 渲染路径负责.
 * ============================================================================
 */
namespace runtime::renderer
{

/**
 * @brief UI 渲染器: 控件树 -> 合批交换格式.
 */
class UIRenderer
{
public:
	UIRenderer() = default;

	UIRenderer(const UIRenderer&) = delete;
	UIRenderer& operator=(const UIRenderer&) = delete;

	/**
	 * @brief 全量提交控件树, 重新合批.
	 * @param Root 控件树根节点.
	 */
	void submit(const ui::RenderNode& Root);

	/**
	 * @brief 增量提交控件树.
	 * @param Root 控件树根节点.
	 * @param Flags 本帧变化意图; v1 退化为全量重批, 但接口保留增量语义.
	 */
	void submitIncremental(const ui::RenderNode& Root, EChangeFlags Flags);

	/** @brief 读取并清零本帧变化意图. */
	[[nodiscard]] EChangeFlags consumeChangeFlags() noexcept;

	/** @brief 上次提交产生的合批结果(只读). */
	[[nodiscard]] const UIBatchList& getBatches() const noexcept { return Batches; }

	/** @brief 上次提交产生的图元总数(诊断用). */
	[[nodiscard]] size_t getQuadCount() const noexcept { return QuadCount; }

	/** @brief 清空所有批次. */
	void clear() noexcept;

private:
	/** @brief 递归收集控件树, 产出未合批的 UIQuad 列表. */
	void collect(const ui::RenderNode& Node, const ui::UIColor& InheritTint,
		float InheritOpacity, std::vector<UIQuad>& OutQuads);

	/** @brief 按合批键稳定排序并聚合为 UIBatchList. */
	void batch(std::vector<UIQuad>&& Quads);

	/** @brief 构造合批键: Transparent 位 | 纹理索引. */
	[[nodiscard]] static UIRenderBatch::BatchKey makeBatchKey(uint32_t TextureIndex,
		bool Transparent) noexcept;

	UIBatchList Batches;
	EChangeFlags ChangeFlags { EChangeFlags_t::All };
	size_t QuadCount { 0 };
};

} // namespace runtime::renderer
