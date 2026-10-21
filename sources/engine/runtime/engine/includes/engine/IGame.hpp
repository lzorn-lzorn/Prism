#pragma once

namespace runtime
{

// 引擎驱动的"游戏宿主契约"。
//
// 这是引擎为了托管并驱动一个游戏所需的最小接口。引擎只依赖这一窄接口,
// 不依赖任何具体游戏类型 —— 依赖方向因此保持为 game -> engine。
//
// 注意: 这不是引擎自身的生命周期。runtime::Engine 的 preinitialize/initialize/
// postinitialize/tick/destroy 描述的是"引擎如何自举"；而 IGame 的同名方法描述的是
// "游戏如何自举"。两者是不同对象上的不同契约, 触发时机与循环次数都相互独立
// (例如 Play In Editor 中, 引擎常驻, 而游戏实例会反复经历完整的 initialize -> destroy)。
class IGame
{
public:
    virtual ~IGame() = default;

    virtual void preinitialize() {}
    virtual void initialize() {}
    virtual void postinitialize() {}   // 游戏的第一帧
    virtual void tick(float DeltaTime) = 0;
    virtual void destroy() {}
};

} // namespace runtime
