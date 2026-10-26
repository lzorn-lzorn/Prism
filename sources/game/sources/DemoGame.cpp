#include "game/Game.hpp"
#include "logger/Logger.hpp"

#include <infra/math/Matrix.hpp>
#include <infra/math/Vector.hpp>

#include <cmath>
#include <cstdint>
#include <memory>

namespace game
{
namespace
{

constexpr float Pi = 3.14159265358979f;

// ----------------------------------------------------------------------------
//  数学辅助(行主序矩阵 + 列向量约定, 与渲染器/编辑器一致)
// ----------------------------------------------------------------------------

[[nodiscard]] infra::Matrix4x4 makeTranslation(infra::Vec3f Translation)
{
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][3] = Translation.x();
    Result[1][3] = Translation.y();
    Result[2][3] = Translation.z();
    return Result;
}

[[nodiscard]] infra::Matrix4x4 makeRotationY(float Radians)
{
    const float C = std::cos(Radians);
    const float S = std::sin(Radians);
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][0] = C;
    Result[0][2] = S;
    Result[2][0] = -S;
    Result[2][2] = C;
    return Result;
}

[[nodiscard]] infra::Matrix4x4 makeRotationX(float Radians)
{
    const float C = std::cos(Radians);
    const float S = std::sin(Radians);
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[1][1] = C;
    Result[1][2] = -S;
    Result[2][1] = S;
    Result[2][2] = C;
    return Result;
}

[[nodiscard]] infra::Matrix4x4 makeScale(float UniformScale)
{
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][0] = UniformScale;
    Result[1][1] = UniformScale;
    Result[2][2] = UniformScale;
    return Result;
}

/** @brief 构造右手系视图矩阵(相机看向 Target). */
[[nodiscard]] infra::Matrix4x4 makeLookAt(infra::Vec3f Eye, infra::Vec3f Target, infra::Vec3f Up)
{
    const infra::Vec3f Backward = (Eye - Target).normalized({ 0.0f, 0.0f, 1.0f });
    const infra::Vec3f Right = infra::Vec3f::cross(Up, Backward).normalized({ 1.0f, 0.0f, 0.0f });
    const infra::Vec3f CameraUp = infra::Vec3f::cross(Backward, Right);

    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    for (std::size_t Column = 0; Column < 3; ++Column)
    {
        Result[0][Column] = Right[Column];
        Result[1][Column] = CameraUp[Column];
        Result[2][Column] = Backward[Column];
    }
    Result[0][3] = -Right.dot(Eye);
    Result[1][3] = -CameraUp.dot(Eye);
    Result[2][3] = -Backward.dot(Eye);
    return Result;
}

// ----------------------------------------------------------------------------
//  演示游戏: 会话(运行世界)与实例(身份/会话工厂)
// ----------------------------------------------------------------------------

/**
 * @brief 演示 Play 会话: 持有编辑世界的独立副本, 在其上推进动画。
 *
 * 这是 copy-on-play 的关键: 编辑器传入的关卡(WorldSnapshot)在这里被复制一份,
 * 会话的所有演化(轨道/自转)只发生在这份副本上; 结束 Play 后副本被丢弃,
 * 编辑世界的原始数据不受影响。
 */
class DemoGame final : public Game
{
public:
    explicit DemoGame(runtime::world::WorldSnapshot InitialWorld)
        : World(std::move(InitialWorld))
    {
    }

    void beginPlay() override
    {
        Elapsed = 0.0f;
        LOG_INFO(runtime::DefaultCategory, "[DemoGame] beginPlay (world copy with {} objects)", World.Objects.size());
    }

    void tick(float DeltaTime) override
    {
        Elapsed += DeltaTime;
        animate();
    }

    void endPlay() override
    {
        LOG_INFO(runtime::DefaultCategory, "[DemoGame] endPlay (world copy discarded)");
    }

    bool buildSnapshot(runtime::world::WorldSnapshot& Out) const override
    {
        Out = World; // 值拷贝: 把当前运行世界快照交给宿主/渲染器.
        return true;
    }

    std::optional<runtime::SessionCamera> getActiveCamera() const override
    {
        // 游戏相机: 绕原点缓慢环绕, 明显区别于编辑器的固定视角.
        const float Radius = 10.0f;
        const float Height = 4.0f;
        const float AngularSpeed = 0.25f;
        const infra::Vec3f Eye {
            std::cos(Elapsed * AngularSpeed) * Radius,
            Height,
            std::sin(Elapsed * AngularSpeed) * Radius,
        };

        runtime::SessionCamera Camera;
        Camera.View = makeLookAt(Eye, { 0.0f, 0.1f, 0.0f }, { 0.0f, 1.0f, 0.0f });
        Camera.Position = Eye;
        // Projection / ViewProjection 依赖视口宽高比, 由宿主按实际视口重算.
        return Camera;
    }

private:
    /** @brief 推进"动态可渲染"物体的轨道/自转动画. */
    void animate()
    {
        for (auto& Object : World.Objects)
        {
            if (Object.Header.Kind != runtime::world::ELogicObjectKind::DynamicRenderable)
                continue;

            const float Index = static_cast<float>(Object.Header.StableOrder) - 1.0f;
            const float Radius = 2.1f;
            const float Height = -0.55f;
            const float SpinSpeed = 0.9f + 0.35f * Index;
            const float OrbitSpeed = 0.35f + 0.12f * Index;
            const float Phase = 2.0f * Pi * Index / 3.0f;
            const float Scale = 1.15f;
            const float Tilt = 0.15f * Index;

            const float Orbit = Phase + Elapsed * OrbitSpeed;
            const infra::Vec3f Position {
                std::cos(Orbit) * Radius, Height, std::sin(Orbit) * Radius,
            };
            Object.Header.PreviousLocalToWorld = Object.Header.LocalToWorld;
            Object.Header.LocalToWorld =
                makeTranslation(Position) * makeRotationY(Elapsed * SpinSpeed)
                * makeRotationX(Tilt) * makeScale(Scale);
        }
    }

    runtime::world::WorldSnapshot World;
    float Elapsed { 0.0f };
};

/**
 * @brief 演示 GameInstance: 进程级身份 + Play 会话工厂。
 *
 * 引擎持有它(身份), 宿主在 Play 时用编辑器关卡创建 DemoGame 会话。
 */
class DemoGameInstance final : public runtime::IGameInstance
{
public:
    const char* getName() const noexcept override { return "DemoGame"; }

    void initialize() override
    {
        LOG_INFO(runtime::DefaultCategory, "[DemoGame] instance initialize");
    }

    void shutdown() override
    {
        LOG_INFO(runtime::DefaultCategory, "[DemoGame] instance shutdown");
    }

    std::unique_ptr<runtime::IPlaySession> createPlaySession(
        const runtime::world::WorldSnapshot& InitialWorld) override
    {
        return std::make_unique<DemoGame>(InitialWorld);
    }
};

} // namespace

std::unique_ptr<runtime::IGameInstance> createDemoGameInstance()
{
    return std::make_unique<DemoGameInstance>();
}

} // namespace game
