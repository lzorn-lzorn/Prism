#pragma once

#include <world/WorldSnapshot.hpp>

#include <infra/math/Matrix.hpp>
#include <infra/math/Vector.hpp>

#include <optional>

namespace runtime
{

/**
 * @brief 会话相机的引擎层最小表示(与渲染器解耦; 渲染器侧对应 runtime::renderer::RenderView)。
 *
 * 投影矩阵(Projection / ViewProjection)依赖视口宽高比, 因此通常由宿主按实际视口
 * 重算并覆写; 会话至少提供 View 与 Position(游戏自身的取景/眼睛位置)。
 */
struct SessionCamera
{
    infra::Matrix4x4 View {};
    infra::Matrix4x4 Projection {};
    infra::Matrix4x4 ViewProjection {};
    infra::Vec3f Position { 0.0f, 0.0f, 0.0f };
};

/**
 * 可播放的"运行世界"契约(对应 UE 的 World / PlaySession 的简化)。
 *
 * 一次 Play 会话 = 一份从编辑世界复制出的逻辑世界 + 驱动它的生命周期:
 *   beginPlay -> tick(dt)* -> endPlay。
 *
 * 会话同时是一个 world::IWorldSnapshotProvider: 每帧由宿主拉取其世界快照并提交给
 * 渲染器(见 renderer::WorldRenderBridge), 从而实现"编辑世界与运行世界分离"
 * (copy-on-play)。运行世界是编辑世界的独立副本, 会话对其的演化不影响编辑世界。
 *
 * 依赖方向: 游戏 -> 引擎; 引擎只依赖本窄接口, 不依赖具体游戏类型。
 */
class IPlaySession : public world::IWorldSnapshotProvider
{
public:
    ~IPlaySession() override = default;

    virtual void beginPlay() {}
    /** @brief 推进一个仿真步; SimDeltaTime 为该仿真步长(秒, 已含时间缩放). */
    virtual void tick(float SimDeltaTime) = 0;
    virtual void endPlay() {}
    /** @brief 会话自身请求结束(如游戏内部触发退出), 由宿主决定如何响应. */
    [[nodiscard]] virtual bool wantsToEnd() const { return false; }
    /** @brief 会话的"游戏相机"; 无相机时返回 nullopt(宿主回退到编辑器相机). */
    [[nodiscard]] virtual std::optional<SessionCamera> getActiveCamera() const { return std::nullopt; }
};

} // namespace runtime
