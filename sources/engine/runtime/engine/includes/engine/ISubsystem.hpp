#pragma once

namespace runtime
{

// 引擎子系统契约: 所有受 Engine 编排的引擎自身子系统都实现这一窄接口。
//
// Engine 按注册顺序依次驱动 preinitialize -> initialize -> postinitialize,
// 并在每帧调用 tick(DeltaTime); 关闭时按注册的逆序调用 shutdown。
//
// 注意: 这与 ISessionHost 的"宿主契约"是两回事 —— 这里描述的是"引擎自身子系统的
// 生命周期"(常驻、跟随引擎自举), 而 ISessionHost 描述的是"会话宿主的生命周期",
// Play 会话(IPlaySession)在宿主内部可反复经历 beginPlay -> endPlay(例如 Play In Editor)。
class ISubsystem
{
public:
    virtual ~ISubsystem() = default;

    // 基础设置与平台相关设置(日志、线程池、平台运行时、配置加载)。
    virtual void preinitialize() {}
    // 正式初始化(窗口、RHI/渲染器、UI、字体、编辑器; 未来 Network/Audio/Physics)。
    virtual void initialize() {}
    // 第一帧前的收尾(引擎自身子系统就绪后、宿主实体首帧之前)。
    virtual void postinitialize() {}
    // 每帧推进; DeltaTime 单位为秒。
    virtual void tick(float DeltaTime) {}
    // 逆序关闭, 释放自身资源(与 initialize 顺序相反)。
    virtual void shutdown() {}
};

} // namespace runtime
