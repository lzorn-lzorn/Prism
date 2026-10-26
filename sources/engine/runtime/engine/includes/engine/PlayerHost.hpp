#pragma once

#include "ISessionHost.hpp"
#include "SimulationClock.hpp"

#include <memory>

namespace runtime
{

class IGameInstance;
class IPlaySession;
namespace renderer { class IRenderResourceResolver; }

/**
 * @brief 独立游戏模式宿主(对应 UE 的 UGameEngine 的简化)。
 *
 * 拥有一个常驻 GameInstance 与一个主 PlaySession, 用 SimulationClock 驱动该会话,
 * 并把会话世界提交给渲染器、用会话相机渲染。
 *
 * 与 EditorHost 的区别: PlayerHost 没有"编辑世界/运行世界分离"与 Play/Simulate 切换,
 * 它始终驱动那一个会话直到其请求结束。二者都实现 ISessionHost, 由 Engine 统一编排。
 */
class PlayerHost final : public ISessionHost
{
public:
    PlayerHost(std::unique_ptr<IGameInstance> Game,
        std::unique_ptr<IPlaySession> Session,
        const renderer::IRenderResourceResolver* Resolver = nullptr);
    ~PlayerHost() override;

    void initialize() override;
    void tick(float RealDeltaTime) override;
    void destroy() override;

    [[nodiscard]] SimulationClock& getClock() noexcept { return Clock; }

private:
    struct Impl;
    std::unique_ptr<Impl> ImplPtr;
    SimulationClock Clock;
};

} // namespace runtime
