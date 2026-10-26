#pragma once

#include "engine/IPlaySession.hpp"
#include "engine/IGameInstance.hpp"

#include <memory>

namespace game
{

// 游戏侧 Play 会话基类。
//
// 它派生自引擎的宿主契约 runtime::IPlaySession, 因此可以被引擎的会话宿主
// (EditorHost / PlayerHost)托管并驱动; 游戏侧仍可在此扩展更丰富的阶段。
// 一次会话对应"一份从编辑世界复制的运行世界", 可反复经历 beginPlay -> endPlay。
class Game : public runtime::IPlaySession
{
public:
    ~Game() override = default;
};

// 演示游戏的 GameInstance 工厂: 具体类型隐藏在实现文件中,
// 组合根(Launcher)只通过 runtime::IGameInstance 这一窄接口看到它。
std::unique_ptr<runtime::IGameInstance> createDemoGameInstance();

} // namespace game
