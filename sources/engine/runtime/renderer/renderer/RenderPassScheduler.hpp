#pragma once

#include <renderer/RenderPass.hpp>

#include <memory>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - 场景渲染器(PassScheduler)与细粒度子渲染器
 * ============================================================================
 *
 *  [定位]
 *  SceneRenderer 是一层 PassScheduler(对应 UE 的 FDeferredShadingSceneRenderer):
 *  它不亲自画任何东西, 而是
 *  按固定顺序驱动一组 IRenderPass 子渲染器, 让它们向**同一个** RDGBuilder 声明 Pass,
 *  最终由 RendererServer::renderFrameGraph 一次 compile()+execute() 完成整帧.
 *  这样 3D 各阶段与 UI 虽然实现不同, 但最后合并进同一张图、同一条命令流.
 *
 *  [子渲染器清单(参考 UE 的 Pass 顺序)]
 *    ShadowRenderPass        阴影深度
 *    DepthPrePass            主深度预通道
 *    OpaqueRenderPass        不透明(GBuffer 或 Forward+)
 *    TransparentRenderPass   半透明
 *    PostProcessRenderPass   后处理
 *    UIRenderPass            2D/UI 合成
 *
 *  [当前状态(v1)]
 *  各子渲染器的 addPasses 已声明正确的 Pass 结构与附件/读写依赖(架构骨架),
 *  但 Execute 内的实际绘制留空 —— 需要 MaterialSystem 完成 Primitive -> DrawPacket
 *  的管线/绑定组解析, 以及 UIRenderer 批次的顶点上传与 2D 管线后填充.
 * ============================================================================
 */
namespace runtime::renderer
{

/**
 * @brief 场景渲染器(PassScheduler): 驱动一组子渲染器向同一 RDG 声明 Pass.
 */
class RenderPassScheduler
{
public:
	RenderPassScheduler() = default;

	RenderPassScheduler(const RenderPassScheduler&) = delete;
	RenderPassScheduler& operator=(const RenderPassScheduler&) = delete;

	/** @brief 注册一个子渲染器(按添加顺序执行). */
	void addPass(std::unique_ptr<IRenderPass> Pass);

	/** @brief 清空所有子渲染器. */
	void clear();

	/**
	 * @brief 依次驱动所有子渲染器向 Builder 声明 Pass(不含 compile/execute).
	 *
	 * @param Builder 本帧共享的 RDG 构建器(UI 与 3D 合并于此).
	 * @param BaseContext 只读上下文; 内部会复制并填充共享 FrameResources 后传给各子渲染器.
	 */
	void build(RDGBuilder& Builder, const RenderContext& BaseContext);

	/** @brief 子渲染器数量. */
	[[nodiscard]] size_t getPassCount() const noexcept { return Passes.size(); }

private:
	std::vector<std::unique_ptr<IRenderPass>> Passes;
};

/** @brief 向后兼容别名: 新代码请优先使用 RenderPassScheduler. */
using SceneRenderer = RenderPassScheduler;

// ============================================================================
//  具体子渲染器(架构骨架, Execute 待 MaterialSystem/2D 管线接入后填充)
// ============================================================================

/** @brief 阴影深度渲染(仅写阴影图, 不做光照). */
class ShadowRenderPass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "Shadow"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

/** @brief 主深度预通道(提前写深度做 Early-Z, 供后续 HiZ 与 Opaque 复用). */
class DepthPrePass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "DepthPrePass"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

/** @brief 不透明渲染(GBuffer 或 Forward+ 主着色). */
class OpaqueRenderPass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "Opaque"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

/** @brief 半透明渲染(从后向前, 读深度不写深度). */
class TransparentRenderPass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "Transparent"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

/** @brief 后处理(Bloom / ToneMapping / TAA 等). */
class PostProcessRenderPass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "PostProcess"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

/** @brief 2D/UI 合成(消费 UIRenderer 的合批结果, 与 3D 合并到同一 RDG). */
class UIRenderPass final : public IRenderPass
{
public:
	[[nodiscard]] const char* getName() const noexcept override { return "UI"; }
	void addPasses(RDGBuilder& Builder, const RenderContext& Context) override;
};

} // namespace runtime::renderer
