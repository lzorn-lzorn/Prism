#pragma once

#include "engine/IGame.hpp"
#include <memory>

namespace game
{

// 游戏自己定义的生命周期基类。
//
// 它派生自引擎的宿主契约 runtime::IGame, 因此可以被引擎托管并驱动;
// 同时它能在游戏侧扩展更丰富的阶段(如 beginPlay/endPlay), 这些阶段引擎
// 一无所知、也不驱动, 由游戏在自己的逻辑里调用。这正体现了"游戏生命周期
// 独立定义, 与引擎自身生命周期并非同一套"。
class Game : public runtime::IGame
{
public:
    ~Game() override = default;

    // 游戏侧扩展的、引擎不参与驱动的阶段。
    virtual void beginPlay() {}
    virtual void endPlay() {}
};

// 演示游戏的工厂: 具体类型隐藏在实现文件中,
// 组合根(Launcher)与引擎只通过 game::Game 这一窄接口看到它。
std::unique_ptr<Game> createDemoGame();

} // namespace game
