#pragma once

#include "engine/ISessionHost.hpp"
#include <memory>

namespace ui { class GenericApplication; }
namespace runtime { class IGameInstance; }
namespace runtime::renderer { class RendererServer; }

namespace editor
{

/**
 * @brief 编辑器宿主(Editor Host): 实现 runtime::ISessionHost, 是"编辑器模式"的会话宿主。
 *
 * 它持有:
 *  - 引擎级服务引用(UI 应用 GenericApplication + 渲染服务 RendererServer), 由子系统管理,
 *    编辑器自身不拥有;
 *  - 一个常驻 GameInstance(进程级身份 + Play 会话工厂), 由组合根注入;
 *  - 编辑世界(可编辑的关卡数据, 编辑器所有);
 *  - 零个或一个 Play 会话(运行世界, Play/Simulate 时创建, 由编辑器持有并驱动);
 *  - 一个模拟时钟(暂停 / 单步 / 慢放 / 固定步长 / 脱离渲染模拟)。
 *
 * 编辑器负责编排"谁在何时 tick"(tick 矩阵)、相机选择(编辑器相机 vs 游戏相机),
 * 并实现 copy-on-play(编辑世界与运行世界分离)、拆解顺序与故障隔离。
 *
 * 组合根(Main)瘦身为纯引导: 注入 GameInstance 后, 把本宿主交给 Engine 驱动。
 */
class EditorApplication final : public runtime::ISessionHost
{
public:
    EditorApplication(ui::GenericApplication& App, runtime::renderer::RendererServer& Renderer,
        std::unique_ptr<runtime::IGameInstance> GameInstance);
    ~EditorApplication() override;

    void preinitialize() override;
    void initialize() override;
    void postinitialize() override;
    void tick(float RealDeltaTime) override;
    void destroy() override;

private:
    struct Impl;
    std::unique_ptr<Impl> ImplPtr;
};

} // namespace editor
