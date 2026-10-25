#pragma once

#include <RHI.hpp>

#include <rdg/RDG.hpp>
#include <renderer/RenderCommand.hpp>
#include <renderer/RenderData.hpp>
#include <renderer/RenderPassScheduler.hpp>
#include <renderer/RenderProxyWorld.hpp>
#include <renderer/Viewport.hpp>

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

/** @brief 帧图算法命名空间别名(::renderer::rdg). */
namespace rdg = ::renderer::rdg;

/**
 * @brief 持有面向应用的 RHI 生命周期的渲染器门面。
 *
 * UI/launcher 代码依赖本服务，而不是自行构造后端对象。
 * 着色器反射与自动 BindGroup 解析留在 Renderer 中，而原生描述符分配仍位于 RHI 内部。
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
    using FrameGraphSetup = std::function<void(rdg::RDGGraph&, rdg::ResourceId Backbuffer)>;

    [[nodiscard]] static RendererServer& self() noexcept;

    RendererServer(const RendererServer&) = delete;
    RendererServer& operator=(const RendererServer&) = delete;

    /**
     * @brief 为窗口初始化选定的后端。
     * @param BackendAPI 要创建的图形 API 实现。
     * @param Window 平台无关的输出窗口。
     */
    void initialize(
        rhi::ESupportedBackendAPI BackendAPI,
        const runtime::platform::GenericWindowPointer& Window);

    /** @brief 等待未完成的 GPU 工作并销毁 Renderer 持有的 RHI 状态。 */
    void shutdown() noexcept;

    [[nodiscard]] bool isInitialized() const noexcept;
    [[nodiscard]] const std::shared_ptr<rhi::RDevice>& getDevice() const noexcept;
    /**
     * @brief 返回由时间戳查询测得、最近完成的 GPU 帧间隔。
     * @return 纳秒；在首个结果之前或不受支持时为 std::nullopt。
     */
    [[nodiscard]] std::optional<double> getLastGPUFrameTimeNanoseconds() const noexcept;

    /** @brief 保留一个对象，直到目前为止所有 Renderer 提交均已完成。 */
    [[nodiscard]] bool deferRelease(std::shared_ptr<void> Resource);

    /** @brief 渲染代理世界数据容器(持有 RenderProxy 与变化意图), 供应用/Game Framework 填充. */
    [[nodiscard]] RenderProxyWorld& getRenderProxyWorld() noexcept;

    /** @brief 兼容旧接口; 新代码请改用 getRenderProxyWorld(). */
    [[nodiscard]] RenderProxyWorld& getRenderWorld() noexcept { return getRenderProxyWorld(); }

    /**
     * @brief 场景渲染器(PassScheduler): 持有一组子 Pass, 供应用注册或自定义 Pass 链.
     *
     * renderFrameGraph 在无用户回调时, 会驱动该调度器声明
     * Shadow → DepthPre → Opaque → Transparent → Post → UI 的完整 Pass 链,
     * 使 3D 与 2D 合并进同一张帧图、同一条命令流.
     */
    [[nodiscard]] RenderPassScheduler& getSceneRenderer() noexcept;

    /**
     * @brief 设置当前帧的渲染视图(相机矩阵), 供 SceneRenderer 各 Pass 消费.
     * @param InView 视图/投影/位置矩阵.
     */
    void setRenderView(const RenderView& InView) noexcept;

    /**
     * @brief 把游戏场景渲染进指定视口(Editor 的 3DView 面板或 standalone 主视口).
     * @param InViewport 视口(相机 + 渲染目标 + 矩形).
     *
     * @note 内部会调用 setRenderView 把该视口的相机投递给渲染线程; 渲染目标(RT)
     *       由 RTT 池按描述复用, 游戏输出到 RT 后由 UI 3DView 控件采样合成.
     */
    void setRenderViewport(const Viewport& InViewport);

    /**
     * @brief 设置 3D 场景在最终画面中的像素矩形(编辑器 Viewport 面板的区域).
     *
     * 3D Pass 只在这个矩形内设置 viewport/scissor, 因此场景会"画进"编辑器面板;
     * 面板外框由 UI 绘制(中间留空即可透出场景). 宽或高为 0 表示整屏渲染.
     *
     * @param Rect 目标像素坐标下的矩形(相对渲染目标左上角).
     * @note GameThread 调用; 值随下一帧交换包传给渲染线程.
     */
    void setSceneViewport(const rhi::RenderArea& Rect) noexcept;

    /** @brief UI 渲染器(控件树 -> 合批), 供应用提交控件树. */

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
     * @brief 提交一帧的 UI 绘制列表(GameThread 调用, 由 RHIUIRenderer 在帧边界提供).
     * @param DrawLists 原始顶点/索引/批次; 内部复制, 随后由 UIRenderPass 消费.
     * @param TextureUploads 与批次纹理句柄配套的脏区数据块; 为空表示纹理内容未变.
     */
    void submitUIDrawLists(std::vector<ui::UIDrawList> DrawLists,
        std::vector<ui::UISampledTextureUpload> TextureUploads = {});

    /**
     * @brief 演练反射、自动资源解析与描述符绑定。
     * @throws std::exception 如果任一 BindGroup 层无效。
     * @note 该确定性启动冒烟测试使用仓库中的 SPIR-V fixture。
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

    /** @brief 在 SurfaceLost 后仅重建原生 surface 与交换链。DeviceLost 需要完全重建应用/RHI 资源。 */
    [[nodiscard]] bool recoverSurface();

private:
    RendererServer();
    ~RendererServer();
    struct Implementation;
    std::unique_ptr<Implementation> Impl;
};

} // namespace runtime::renderer
