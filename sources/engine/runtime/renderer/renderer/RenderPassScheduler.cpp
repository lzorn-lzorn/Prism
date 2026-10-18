#include <renderer/RenderPassScheduler.hpp>

#include <array>

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
//  录制工具: 把已解析的 DrawPacket / UI 批次翻译成 RHI 绘制命令
// ============================================================================

namespace
{

/** @brief 构造全屏尺寸的深度纹理描述(供子渲染器复用). */
[[nodiscard]] RDGTextureDesc makeDepthDesc(uint32_t width, uint32_t height)
{
	return RDGTextureDesc {
		.Format = rhi::EFormat::D32_Float,
		.Width = width,
		.Height = height,
		.Depth = 1,
		.MipLevels = 1,
		.ArrayLayers = 1,
		.Usage = rhi::EImageUsage_t::DepthStencil,
		.Samples = rhi::ESampleCount::Count1,
		.Transient = true,
	};
}

/** @brief 设置覆盖整个渲染目标的全尺寸视口与裁剪矩形. */
void setFullViewport(rhi::RCommandList& command_list, uint32_t width, uint32_t height)
{
	const std::array viewports { rhi::Viewport {
		.Width = static_cast<float>(width),
		.Height = static_cast<float>(height),
	} };
	const std::array scissors { rhi::RenderArea {
		.Width = width,
		.Height = height,
	} };
	command_list.setViewports(viewports);
	command_list.setScissors(scissors);
}

/**
 * @brief 录制一个已解析的 DrawPacket: 绑定管线 / 常量 / 绑定组 / 顶点 / 索引并绘制.
 *
 * - Pipeline 为空表示该 packet 尚未经 MaterialSystem 解析, 直接跳过(安全降级);
 * - Push Constant 约定作用于 Vertex|Pixel 阶段(逐对象变换 + 材质参数);
 * - 仅索引绘制(DrawPacket 未携带非索引顶点数).
 */
void recordDrawPacket(rhi::RCommandList& command_list, const DrawPacket& packet)
{
	if (!packet.Pipeline)
		return;

	command_list.bindPipeline(packet.Pipeline);

	if (!packet.PushConstants.empty() && packet.PipelineLayout)
	{
		command_list.pushConstants(packet.PipelineLayout,
			rhi::EShaderStage_t::Vertex | rhi::EShaderStage_t::Pixel,
			0, packet.PushConstants);
	}

	if (!packet.BindGroups.empty())
	{
		command_list.bindBindGroups(rhi::EPipelineType::Graphics, packet.PipelineLayout,
			0, packet.BindGroups, packet.DynamicOffsets);
	}

	if (!packet.VertexBindings.empty())
		command_list.bindVertexBuffers(0, packet.VertexBindings);

	if (packet.IndexBuffer)
		command_list.bindIndexBuffer(packet.IndexBuffer, packet.IndexOffset, packet.IndexFormat);

	if (packet.IndexCount > 0)
	{
		command_list.drawIndexed(packet.IndexCount, packet.InstanceCount, packet.FirstIndex,
			packet.VertexOffset, packet.FirstInstance);
	}
}

/** @brief 录制一个已按 SortKey 排序的绘制列表. */
void recordDrawList(rhi::RCommandList& command_list, const DrawList& draw_list)
{
	for (const DrawPacket& packet : draw_list)
		recordDrawPacket(command_list, packet);
}

/**
 * @brief 录制 2D/UI 批次.
 *
 * v1 先绑定批次管线并校验数据; 完整绘制还缺两块基础资源(由 UIRenderer/RendererServer 持有):
 *   1. 一块共享 Unit Quad 顶点/索引缓冲;
 *   2. 把 Quads 编码为 instance 数据(Min/Max/Uv/Color/CornerRadius)上传到实例缓冲,
 *      然后以 InstanceCount=Quads.size() 绘制 6 个索引顶点.
 * 这两块接入后, 2D 才能与 3D 在同一条命令流里合批.
 */
void recordUIBatches(rhi::RCommandList& command_list, const UIBatchList& batches)
{
	for (const UIRenderBatch& batch : batches)
	{
		if (!batch.Pipeline || batch.Quads.empty())
			continue;
		command_list.bindPipeline(batch.Pipeline);
		// TODO: 绑定 Unit Quad 缓冲 + 上传 instance 数据 + drawIndexed(6, Quads.size()).
	}
}

} // namespace

// ============================================================================
//  具体子渲染器
// ============================================================================

void ShadowRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			// 阴影图集: 每个灯/级联一个区域, 深度-only. 灯光视角矩阵由后续接入.
			RDGResourceId shadow_atlas = PassBuilder.createTexture(
				makeDepthDesc(Context.Width, Context.Height), "ShadowAtlas");
			PassBuilder.depthStencil(shadow_atlas, rhi::ELoadOp::Clear, rhi::EStoreOp::Store);
		},
		[&Context](RDGExecuteContext& ExecuteContext) {
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			if (Context.Submission)
				recordDrawList(command_list, Context.Submission->ShadowDraws);
			// TODO: 逐光源上传阴影视角矩阵(ShadowDraws 已按灯光/级联排序).
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
		[&Context](RDGExecuteContext& ExecuteContext) {
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			// 深度预通道复用不透明绘制列表, 由深度-only 管线建立 Early-Z.
			if (Context.Submission)
				recordDrawList(command_list, Context.Submission->OpaqueDraws);
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
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			// 优先消费预处理的交换包; 无 Submission 时回退到 collectVisible(需 MaterialSystem 解析管线).
			if (Context.Submission)
				recordDrawList(command_list, Context.Submission->OpaqueDraws);
		});
}

void TransparentRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Load, rhi::EStoreOp::Store);
		},
		[&Context](RDGExecuteContext& ExecuteContext) {
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			// 透明列表已按深度从后向前排序; 只读深度不写深度.
			if (Context.Submission)
				recordDrawList(command_list, Context.Submission->TransparentDraws);
		});
}

void PostProcessRenderPass::addPasses(RDGBuilder& Builder, const RenderContext& Context)
{
	Builder.addGraphicsPass(getName(),
		[&Context](RDGPassBuilder& PassBuilder) {
			PassBuilder.renderTarget(Context.Backbuffer, rhi::ELoadOp::Load, rhi::EStoreOp::Store);
		},
		[&Context](RDGExecuteContext& ExecuteContext) {
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			// TODO: Bloom -> ToneMapping -> (TAA), 全屏三角形采样场景色. 需要后处理管线.
			(void)command_list;
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
			auto& command_list = ExecuteContext.getCommandList();
			setFullViewport(command_list, Context.Width, Context.Height);
			if (Context.Submission)
				recordUIBatches(command_list, Context.Submission->UIBatches);
		});
}

} // namespace runtime::renderer
