#pragma once

namespace runtime
{

/**
 * 会话宿主契约(取代旧的 IGame / IEditor 并列模型)。
 *
 * Engine 只托管一个"会话宿主": 要么是 PlayerHost(独立游戏模式, 驱动一个主世界),
 * 要么是 EditorHost(编辑器模式, 驱动编辑世界 + 零到多个 Play 会话)。
 *
 * 宿主负责编排"谁在何时 tick"(tick 矩阵)、模拟时钟、相机选择与渲染提交;
 * Engine 只在本契约的固定阶段驱动宿主, 不感知具体宿主类型。
 *
 * 依赖方向: 宿主 -> 引擎; 引擎不依赖具体宿主类型。
 */
class ISessionHost
{
public:
    virtual ~ISessionHost() = default;

    virtual void preinitialize() {}
    virtual void initialize() {}
    virtual void postinitialize() {}
    /** @brief 推进一帧; RealDeltaTime 为渲染帧真实步长(秒, 未缩放). */
    virtual void tick(float RealDeltaTime) = 0;
    /** @brief 逆序关闭: 先停所有 Play 会话, 再停编辑世界/自身资源. */
    virtual void destroy() {}
};

} // namespace runtime
