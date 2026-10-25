#pragma once

#include <infra/containers/MPMCQueue.hpp>
#include <infra/math/Matrix.hpp>
#include <renderer/RenderPass.hpp>
#include <renderer/RenderProxyWorld.hpp>

#include <cstdint>
#include <variant>

/**
 * ============================================================================
 *  runtime::renderer - 渲染命令(Render Command)
 * ============================================================================
 *
 *  [要解决的问题]
 *  GameThread 与 RenderThread 是两条线程, 它们之间需要一个"值语义、无锁、增量"
 *  的边界: GameThread 只负责"声明场景发生了什么变化", RenderThread 只负责"消费
 *  这些变化并更新自己的渲染场景副本". 任何 RHI 对象都不允许越过这条边界.
 *
 *  [实现方式]
 *  命令采用 std::variant(每个命令只携带自己的载荷), 队列直接复用
 *  infra::MPMCQueue(Vyukov 有界无锁队列, SPSC 是其特例). 这样:
 *    - GameThread 生产: queue.tryPush(RenderCommand{...})
 *    - RenderThread 消费: queue.tryPop() -> std::optional<RenderCommand>
 *    - 无锁、有界、Cache-Line 对齐, 避免伪共享.
 *
 *  [与快照的关系]
 *  命令是"增量意图", 快照是"渲染线程私有场景副本". RenderThread 把命令应用到
 *  自己的 RenderProxyWorld 上, 从而永远读到一致状态, 不与 GameThread 的写入竞争.
 */

namespace runtime::renderer
{

/**
 * @brief 新增渲染代理的命令载荷.
 * @note Proxy 以值方式移动进队列; 内部 shared_ptr 移动开销为常数.
 */
struct RenderCommandAddProxy
{
    RenderProxyVariant Proxy;
};

/** @brief 按索引移除渲染代理的命令载荷. */
struct RenderCommandRemoveProxy
{
    uint32_t Index { 0 };
};

/** @brief 更新代理变换的命令载荷. */
struct RenderCommandUpdateTransform
{
    uint32_t Index { 0 };
    infra::Matrix4x4 Transform {};
};

/** @brief 更新代理材质引用的命令载荷. */
struct RenderCommandUpdateMaterial
{
    uint32_t Index { 0 };
    uint32_t MaterialId { 0 };
};

/** @brief 更新相机矩阵的命令载荷. */
struct RenderCommandSetRenderView
{
    RenderView View {};
};

/**
 * @brief 一条渲染命令: 值语义、跨线程安全、nothrow 移动.
 *
 * @note 采用 variant 而非平铺结构, 使每个命令只携带自身载荷;
 *       载荷均为 POD / shared_ptr, 满足 infra::MPMCQueue 的 nothrow 移动要求.
 */
using RenderCommand = std::variant<
    RenderCommandAddProxy,
    RenderCommandRemoveProxy,
    RenderCommandUpdateTransform,
    RenderCommandUpdateMaterial,
    RenderCommandSetRenderView>;

/**
 * @brief 渲染命令队列容量(必须为 2 的幂, 满足 infra::MPMCQueue 的约束).
 * @note 256 为保守默认; 每帧增量命令通常远小于此.
 */
inline constexpr size_t RenderCommandQueueCapacity = 256;

/**
 * @brief 渲染命令队列: 单生产者(GameThread)单消费者(RenderThread)的无锁有界队列.
 *
 * 直接复用 infra::MPMCQueue:
 *   - 生产者(GameThread)  : tryPush(RenderCommand) -> bool(满则 false, 调用方下帧重发)
 *   - 消费者(RenderThread): tryPop() -> std::optional<RenderCommand>
 *
 * @note 若在极端场景下爆满, tryPush 返回 false 时应视为"本帧略过该增量、下帧补发".
 */
using RenderCommandQueue = infra::MPMCQueue<RenderCommand, RenderCommandQueueCapacity>;

} // namespace runtime::renderer
