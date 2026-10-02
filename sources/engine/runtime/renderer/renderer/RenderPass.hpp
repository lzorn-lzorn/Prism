#pragma once

#include <rdg/RDGBuilder.hpp>
#include <renderer/RenderProxyWorld.hpp>
#include <renderer/UIRenderer.hpp>

#include <core/math/Matrix.hpp>
#include <core/math/Vector.hpp>

#include <cstdint>

/**
 * ============================================================================
 *  runtime::renderer - 细粒度子渲染器抽象(参考 UE 的 Pass Renderer)
 * ============================================================================
 *
 *  [动机]
 *  3D 渲染不应把所有逻辑堆在一个函数里. 参考 UE 的 FDeferredShadingSceneRenderer,
 *  把一帧拆成多个"只做一类事"的子渲染器:
 *      - 只渲染阴影的 ShadowRenderPass;
 *      - 只写深度的 DepthPrePass;
 *      - 只画不透明的 OpaqueRenderPass;
 *      - 只画半透明的 TransparentRenderPass;
 *      - 只做后处理的 PostProcessRenderPass;
 *      - 只画 UI 的 UIRenderPass.
 *
 *  每个子渲染器都实现同一个 IRenderPass 接口, 向**同一个** RDGBuilder 声明自己的 Pass.
 *  SceneRenderer(编排器)依次驱动它们, 最终一次 compile()+execute() 完成整帧 —— 这样
 *  UI 与 3D 即使内部实现不同, 最后也合并进同一张图、同一条命令流.
 *
 *  [依赖方向]
 *  IRenderPass / SceneRenderer(PassScheduler) -> RenderProxyWorld + UIRenderer + RDGBuilder -> RHI
 * ============================================================================
 */
namespace runtime::renderer
{

/**
 * @brief 渲染视图: 相机矩阵的渲染器侧最小表示.
 *
 * 完整 Camera 属于 Game Framework; 渲染器只消费已算好的矩阵, 避免反向依赖.
 */
struct RenderView
{
	core::Matrix4x4 View {};
	core::Matrix4x4 Projection {};
	core::Matrix4x4 ViewProjection {};
	core::Vec3f CameraPosition { 0.0f, 0.0f, 0.0f };
};

/**
 * @brief 跨子渲染器共享的 transient 资源句柄.
 *
 * 由先使用该资源的子渲染器在自己的 Pass Setup 里声明(createTexture)并写回;
 * 后续子渲染器直接读取该句柄复用, 避免同名资源被重复声明导致的物理复用但状态追踪分裂.
 */
struct RenderFrameResources
{
	/** 场景主深度(DepthPrePass 写, Opaque 读). */
	RDGResourceId SceneDepth { InvalidRDGResourceId };
	// 未来扩展: GBuffer[0..N]、HDR SceneColor、HiZ、ShadowAtlas...
};

/**
 * @brief 子渲染器声明 Pass 所需的上下文.
 *
 * 所有子渲染器共享同一份 World / View / UI / Backbuffer, 保证它们向同一 RDG 协同声明 Pass.
 * FrameResources 是可变的共享状态: 先声明的子渲染器写入共享句柄, 后续子渲染器读取.
 */
struct RenderContext
{
	const RenderProxyWorld& ProxyWorld;
	const RenderView& View;
	const UIRenderer& UI;
	/**
	 * @brief World 预处理后的帧交换包(可为空, 由调用方控制是否启用统一提交流).
	 */
	const RenderFrameSubmission* Submission { nullptr };
	/** 最终输出目标(由 RendererServer::renderFrameGraph 导入的交换链图). */
	RDGResourceId Backbuffer { InvalidRDGResourceId };
	/** 共享 transient 句柄(可变, 子渲染器按需填充/读取). */
	RenderFrameResources& FrameResources;
	uint32_t FrameIndex { 0 };
	uint32_t Width { 0 };
	uint32_t Height { 0 };
};

/**
 * @brief 细粒度子渲染器接口.
 *
 * 每个实现只负责一类 Pass. addPasses 里只声明资源读写与附件, 并通过 RDGPassBuilder
 * 拿到 transient 资源句柄; 真正的录制发生在 RDGExecuteContext 的 Execute 回调里.
 */
class IRenderPass
{
public:
	virtual ~IRenderPass() = default;

	/** @brief 稳定名称, 用于调试与日志. */
	[[nodiscard]] virtual const char* getName() const noexcept = 0;

	/**
	 * @brief 向 Builder 声明本渲染器负责的 Pass(不执行 compile/execute).
	 * @param Builder 本帧共享的 RDG 构建器(UI 与 3D 合并于此).
	 * @param Context 只读上下文(World/View/UI/尺寸).
	 */
	virtual void addPasses(RDGBuilder& Builder, const RenderContext& Context) = 0;
};

} // namespace runtime::renderer
