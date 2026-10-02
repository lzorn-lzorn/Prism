#include <renderer/RenderPassScheduler.hpp>

namespace runtime::renderer
{

// ============================================================================
//  SceneRenderer(编排器)
// ============================================================================

void RenderPassScheduler::addPass(std::unique_ptr<IRenderPass> Pass)
{
	if (Pass)
		Passes.push_back(std::move(Pass));
}

void RenderPassScheduler::clear()
{
	Passes.clear();
}

void RenderPassScheduler::build(RDGBuilder& Builder, const RenderContext& BaseContext)
{
	// 复制上下文(引用成员指向同一对象), 让子渲染器可通过 FrameResources 填充/读取共享句柄.
	RenderContext Context = BaseContext;
	for (auto& Pass : Passes)
		Pass->addPasses(Builder, Context);
}

// ============================================================================
//  具体子渲染器
//
//  说明: 以下 addPasses 已声明正确的 Pass 结构与附件/读写依赖(架构骨架),
//  但 Execute 内实际绘制留空 —— 需要 MaterialSystem 完成 RenderProxy -> DrawPacket
//  的管线/绑定组解析, 以及 UIRenderer 批次的顶点上传与 2D 管线后填充.
// ============================================================================

namespace
{

/** @brief 构造全屏尺寸的深度纹理描述(供子渲染器复用). */
[[nodiscard]] RDGTextureDesc makeDepthDesc(uint32_t Width, uint32_t Height)
{
	return RDGTextureDesc {
		.Format = rhi::EFormat::D32_Float,
		.Width = Width,
		.Height = Height,
		.Depth = 1,
		.MipLevels = 1,
		.ArrayLayers = 1,
		.Usage = rhi::EImageUsage_t::DepthStencil,
		.Samples = rhi::ESampleCount::Count1,
		.Transient = true,
	};
}

} // namespace

void ShadowRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	// 阴影图集: 每个灯/级联一个区域, 深度-only. 实际渲染需要灯光视角矩阵(后续接入).
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			RDGResourceId shadowAtlas = PassBuilder.createTexture(
				makeDepthDesc(Context.Width, Context.Height), "ShadowAtlas");
			PassBuilder.depthStencil(shadowAtlas, rhi::ELoadOp::Clear, rhi::EStoreOp::Store);
		},
		[](RDGExecuteContext&) {
			// TODO: 以每盏灯的视图矩阵绘制阴影投射体(深度-only 管线).
		});
}

void DepthPrePass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			// 声明共享主深度并写回共享句柄, 供 Opaque 等后续 Pass 复用.
			RDGResourceId depth = PassBuilder.createTexture(
				makeDepthDesc(Context.Width, Context.Height), "SceneDepth");
			Context.FrameResources.SceneDepth = depth;
			PassBuilder.depthStencil(depth, rhi::ELoadOp::Clear, rhi::EStoreOp::Store);
		},
		[](RDGExecuteContext&) {
			// TODO: 以纯深度 shader 绘制全部不透明代理, 建立 Early-Z.
		});
}

void OpaqueRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Clear,
				rhi::EStoreOp::Store);
			if (Context.FrameResources.SceneDepth != InvalidRDGResourceId)
				PassBuilder.depthStencil(Context.FrameResources.SceneDepth, rhi::ELoadOp::Load,
					rhi::EStoreOp::Store);
		},
		[&Context](RDGExecuteContext& ExecuteContext) {
			// TODO: 优先消费 Context.Submission->OpaqueDraws;
			//       无 Submission 时回退到 Context.ProxyWorld.collectVisible() 路径.
			(void)ExecuteContext;
		});
}

void TransparentRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Load, rhi::EStoreOp::Store);
		},
		[](RDGExecuteContext&) {
			// TODO: 消费 Context.Submission->TransparentDraws,
			//       或回退到透明代理排序路径(读深度, 不写深度).
		});
}

void PostProcessRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Load, rhi::EStoreOp::Store);
		},
		[](RDGExecuteContext&) {
			// TODO: Bloom -> ToneMapping -> (TAA), 全屏三角形采样场景色.
		});
}

void UIRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			// UI 合成到最终输出: Load 已有场景色, Store 保留.
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Load, rhi::EStoreOp::Store);
		},
		[&Context](RDGExecuteContext& ExecuteContext) {
			// TODO: 遍历 Context.UI.getBatches(), 经 2D 管线解析 BatchKey 为 Pipeline/纹理,
			//       上传 UIQuad instance 数据并一次性绘制每个批次.
			(void)ExecuteContext;
		});
}

} // namespace runtime::renderer
