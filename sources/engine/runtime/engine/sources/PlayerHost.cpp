#include "engine/PlayerHost.hpp"

#include "engine/Engine.hpp"
#include "engine/IGameInstance.hpp"
#include "engine/IPlaySession.hpp"
#include <logger/Logger.hpp>
#include <renderer/RendererServer.hpp>
#include <renderer/WorldRenderBridge.hpp>

#include <exception>
#include <memory>
#include <utility>

namespace runtime
{

struct PlayerHost::Impl
{
    std::unique_ptr<IGameInstance> Game;
    std::unique_ptr<IPlaySession> Session;
    renderer::WorldRenderBridge Bridge;

    Impl(std::unique_ptr<IGameInstance> InGame,
        std::unique_ptr<IPlaySession> InSession,
        const renderer::IRenderResourceResolver* Resolver)
        : Game(std::move(InGame))
        , Session(std::move(InSession))
        , Bridge(Resolver)
    {
    }
};

PlayerHost::PlayerHost(std::unique_ptr<IGameInstance> Game,
    std::unique_ptr<IPlaySession> Session,
    const renderer::IRenderResourceResolver* Resolver)
    : ImplPtr(std::make_unique<Impl>(std::move(Game), std::move(Session), Resolver))
{
}

PlayerHost::~PlayerHost() = default;

void PlayerHost::initialize()
{
    if (ImplPtr->Game)
        ImplPtr->Game->initialize();
    if (ImplPtr->Session)
        ImplPtr->Session->beginPlay();
}

void PlayerHost::tick(float RealDeltaTime)
{
    if (!ImplPtr->Session)
        return;

    // 故障隔离: 游戏异常不得摧毁宿主/进程 —— 记录后请求引擎优雅退出。
    bool SessionFailed = false;
    Clock.advance(RealDeltaTime, [&](float DeltaTime) {
        if (SessionFailed)
            return;
        try
        {
            ImplPtr->Session->tick(DeltaTime);
        }
        catch (const std::exception& Error)
        {
            LOG_ERROR(DefaultCategory, "[PlayerHost] Play session tick failed: {}", Error.what());
            SessionFailed = true;
        }
        catch (...)
        {
            LOG_ERROR(DefaultCategory, "[PlayerHost] Play session tick failed: unknown error");
            SessionFailed = true;
        }
    });

    if (SessionFailed || ImplPtr->Session->wantsToEnd())
    {
        Engine::self().quit();
        return;
    }

    // 会话世界 -> 渲染器; 会话相机 -> 渲染视图。
    auto& Renderer = renderer::RendererServer::self();
    (void)ImplPtr->Bridge.pullFrom(*ImplPtr->Session, Renderer.getRenderProxyWorld());
    if (const auto Camera = ImplPtr->Session->getActiveCamera())
    {
        renderer::RenderView View;
        View.View = Camera->View;
        View.Projection = Camera->Projection;
        View.ViewProjection = Camera->ViewProjection;
        View.CameraPosition = Camera->Position;
        Renderer.setRenderView(View);
    }

    // 脱离渲染模拟: 只推进仿真、跳过渲染(无头追赶/重放)。
    if (!Clock.isSimulateWithoutRender())
        (void)Renderer.renderFrameGraph();
}

void PlayerHost::destroy()
{
    // 先停会话(游戏), 再停 GameInstance —— 与 initialize 顺序相反。
    if (ImplPtr->Session)
    {
        try
        {
            ImplPtr->Session->endPlay();
        }
        catch (...)
        {
            // 关闭路径不得抛出.
        }
    }
    ImplPtr->Session.reset();
    if (ImplPtr->Game)
        ImplPtr->Game->shutdown();
    ImplPtr->Game.reset();
}

} // namespace runtime
