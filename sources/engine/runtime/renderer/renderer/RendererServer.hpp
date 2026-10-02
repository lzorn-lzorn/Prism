#pragma once

#include <RHI.hpp>

#include <rdg/RDGBuilder.hpp>
#include <renderer/RenderData.hpp>
#include <renderer/RenderProxyWorld.hpp>
#include <renderer/UIRenderer.hpp>

#include <memory>
#include <functional>
#include <optional>

/**
 * @brief RendererServer 是一个单例渲染服务, 其面向外部提供统一的渲染服务. 
 *  1. 负责 RHI 的生命周期管理, 包括设备创建, 资源管理, 渲染循环等.
 *  2. 统一封装了 2D渲染器(UI 渲染) 和 3D渲染器(场景渲染, 角色渲染)
 *  3. 提供两种一帧录制入口:
 *     - renderFrame(FrameRecorder): 单渲染作用域 + 用户回调(历史兼容, 简单工具);
 *     - renderFrameGraph(FrameGraphSetup): RDG 多 Pass 编排(推荐, 3D+UI 全流程).
 */
namespace runtime::renderer
{

/**
 * @brief Renderer facade that owns the application-facing RHI lifecycle.
 *
 * UI/launcher code depends on this service instead of constructing backend objects.
 * Shader reflection and automatic BindGroup resolution stay in Renderer while native
 * descriptor allocation remains inside RHI.
 */
class RendererServer final
{
public:
	enum class EFrameStatus : uint8_t
	{
		Rendered,
		Skipped,
		SwapchainRecreated,
		SurfaceLost,
		DeviceLost
	};

	/** @brief 历史兼容的单渲染作用域录制回调. */
	using FrameRecorder = std::function<void(
		rhi::RCommandList&,
		const std::shared_ptr<rhi::RImageView>&,
		uint32_t,
		uint32_t)>;

	/**
	 * @brief RDG 帧图构建回调: 应用在回调内声明 Pass 与资源(3D + UI).
	 *
	 * 回调内 import 的交换链图像已被 RDG 托管为 Backbuffer, 并作为参数传入.
	 * 应用可用 SceneRenderer 编排各子渲染器向同一 RDG 声明 Pass, 实现 3D 与 UI 合并.
	 */
	using FrameGraphSetup = std::function<void(RDGBuilder&, RDGResourceId Backbuffer)>;

	[[nodiscard]] static RendererServer& self() noexcept;

	RendererServer(const RendererServer&) = delete;
	RendererServer& operator=(const RendererServer&) = delete;

	/**
	 * @brief Initializes the selected backend for a window.
	 * @param BackendAPI Graphics API implementation to create.
	 * @param Window Platform-neutral output window.
	 */
	void initialize(
		rhi::ESupportedBackendAPI BackendAPI,
		const ui::GenericWindowPointer& Window);

	/** @brief Waits for outstanding GPU work and destroys renderer-owned RHI state. */
	void shutdown() noexcept;

	[[nodiscard]] bool isInitialized() const noexcept;
	[[nodiscard]] const std::shared_ptr<rhi::RDevice>& getDevice() const noexcept;
	/** @brief Returns the most recently completed GPU frame interval measured by timestamp queries. @return Nanoseconds, or std::nullopt before the first result or when unsupported. */
	[[nodiscard]] std::optional<double> getLastGPUFrameTimeNanoseconds() const noexcept;

	/** @brief Retains an object until all renderer submissions made so far have completed. */
	[[nodiscard]] bool deferRelease(std::shared_ptr<void> Resource);

	/** @brief 渲染代理世界数据容器(持有 RenderProxy 与变化意图), 供应用/Game Framework 填充. */
	[[nodiscard]] RenderProxyWorld& getRenderProxyWorld() noexcept;

	/** @brief 兼容旧接口; 新代码请改用 getRenderProxyWorld(). */
	[[nodiscard]] RenderProxyWorld& getRenderWorld() noexcept { return getRenderProxyWorld(); }

	/** @brief UI 渲染器(控件树 -> 合批), 供应用提交控件树. */
	[[nodiscard]] UIRenderer& getUIRenderer() noexcept;

	/**
	 * @brief 构建并缓存一份 World -> Renderer 的统一帧交换包.
	 *
	 * 默认会读取并清零 RenderWorld/UIRenderer 的变化意图, 然后把分类代理预处理成
	 * RenderFrameSubmission, 供 SceneRenderer 各 Pass 消费.
	 */
	[[nodiscard]] const RenderFrameSubmission& buildRenderSubmission();

	/** @brief 最近一次 buildRenderSubmission() 的结果快照. */
	[[nodiscard]] const RenderFrameSubmission& getLastRenderSubmission() const noexcept;

	/**
	 * @brief Exercises reflection, automatic resource resolution and descriptor binding.
	 * @throws std::exception if any BindGroup layer is invalid.
	 * @note This deterministic startup smoke test uses the repository SPIR-V fixture.
	 */
	void runBindGroupSmokeTest();

	/**
	 * @brief 录制并提交一帧. 回调在默认颜色渲染作用域内执行. 
	 *
	 * 无回调时执行一次清屏；有回调时可绑定 2D/3D Pipeline, BindGroup 和几何数据. 
	 * 这是单渲染作用域的历史兼容入口; 多 Pass 请使用 renderFrameGraph().
	 */
	[[nodiscard]] EFrameStatus renderFrame(const FrameRecorder& Recorder = {});

	/**
	 * @brief 以 RDG 多 Pass 方式录制并提交一帧(推荐, 兼顾 3D 与 UI).
	 *
	 * 无回调时退化为一次清屏 Pass. 有回调时应用通过 RDGBuilder 声明:
	 *   Depth -> Opaque -> Lighting -> Transparent -> Post -> UI -> Backbuffer.
	 * 交换链图像的 Present 出口 barrier 由 Renderer 自动追加.
	 */
	[[nodiscard]] EFrameStatus renderFrameGraph(const FrameGraphSetup& Setup = {});

	/** @brief 通知 Renderer 输出尺寸变化；0 尺寸表示窗口最小化.  */
	void resize(uint32_t Width, uint32_t Height);

	/** @brief Recreates only the native surface and swapchain after SurfaceLost. DeviceLost requires full application/RHI resource rebuild. */
	[[nodiscard]] bool recoverSurface();

private:
	RendererServer();
	~RendererServer();
	struct Implementation;
	std::unique_ptr<Implementation> Impl;
};

} // namespace runtime::renderer
