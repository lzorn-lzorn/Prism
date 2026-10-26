#pragma once

#include <world/WorldSnapshot.hpp>

#include <memory>

namespace runtime
{

class IPlaySession;

/**
 * 游戏实例契约(对应 UE 的 UGameInstance 的"身份 / 会话工厂"侧): 进程级、常驻,
 * 描述"这是哪一款游戏", 并能用一份初始关卡(逻辑世界)创建可反复 begin/end 的
 * Play 会话。
 *
 * 分层:
 *  - GameInstance(身份/设置/存档上下文)      —— 由 Engine 持有, 进程级;
 *  - PlaySession(一次可玩的运行世界)         —— 由宿主(EditorHost / PlayerHost)持有,
 *                                               可反复经历 beginPlay -> endPlay。
 *
 * 依赖方向: 游戏 -> 引擎; 引擎只依赖本窄接口。
 */
class IGameInstance
{
public:
    virtual ~IGameInstance() = default;

    [[nodiscard]] virtual const char* getName() const noexcept = 0;

    virtual void initialize() {}
    virtual void shutdown() {}

    /**
     * @brief 用给定的初始关卡创建一个 Play 会话。
     * @param InitialWorld 宿主提供的初始世界(编辑器关卡)。会话持有其独立副本,
     *        之后的所有演化都发生在副本上, 不触碰传入的关卡数据。
     */
    virtual std::unique_ptr<IPlaySession> createPlaySession(const world::WorldSnapshot& InitialWorld) = 0;
};

} // namespace runtime
