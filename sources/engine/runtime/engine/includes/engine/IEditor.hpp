#pragma once

namespace runtime
{

// 引擎托管的"编辑器宿主契约", 与 IGame 并列。
//
// 引擎加载一个"执行实体": 要么是游戏(IGame), 要么是编辑器(IEditor)。
// 两者都由 Engine 驱动生命周期, 但接口相互独立 —— 编辑器常驻引擎,
// 而游戏实例可反复经历 initialize -> destroy(例如 Play In Editor)。
//
// 与 IGame 一样, 这是窄契约: 引擎不依赖具体编辑器类型, 依赖方向保持
// editor -> engine。编辑器所需的 UI / Renderer / Platform 在编辑器模块内
// 组合, 引擎只通过本契约驱动其生命周期。
class IEditor
{
public:
    virtual ~IEditor() = default;

    virtual void preinitialize() {}
    virtual void initialize() {}
    virtual void postinitialize() {}   // 编辑器的第一帧
    virtual void tick(float DeltaTime) = 0;
    virtual void destroy() {}
};

} // namespace runtime
