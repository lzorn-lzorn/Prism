#include "game/Game.hpp"
#include "logger/Logger.hpp"
#include <cstdint>

namespace game
{
namespace
{

// 具体演示游戏: 实现 game::Game 的完整生命周期。
// 它对外不可见 —— 引擎与组合根只通过 game::Game 接口驱动它。
class DemoGame final : public Game
{
public:
    void preinitialize() override
    {
        LOG_INFO(runtime::DefaultCategory, "[Game] preinitialize");
    }

    void initialize() override
    {
        LOG_INFO(runtime::DefaultCategory, "[Game] initialize");
    }

    void postinitialize() override
    {
        LOG_INFO(runtime::DefaultCategory, "[Game] postinitialize (first frame)");
        beginPlay();
    }

    void tick(float DeltaTime) override
    {
        ++FrameIndex;
        if (FrameIndex <= 3 || FrameIndex % 60 == 0)
        {
            LOG_INFO(runtime::DefaultCategory, "[Game] tick frame={} dt={:.4f}", FrameIndex, DeltaTime);
        }
    }

    void destroy() override
    {
        endPlay();
        LOG_INFO(runtime::DefaultCategory, "[Game] destroy");
    }

    void beginPlay() override
    {
        LOG_INFO(runtime::DefaultCategory, "[Game] beginPlay");
    }

    void endPlay() override
    {
        LOG_INFO(runtime::DefaultCategory, "[Game] endPlay");
    }

private:
    std::uint64_t FrameIndex{0};
};

} // namespace

std::unique_ptr<Game> createDemoGame()
{
    return std::make_unique<DemoGame>();
}

} // namespace game
