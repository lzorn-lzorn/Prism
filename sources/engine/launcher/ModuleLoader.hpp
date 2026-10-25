#pragma once

namespace runtime { class Engine; }
namespace ui { class GenericApplication; }
namespace runtime::renderer { class RendererServer; }

// 模块加载器: 编辑模式下的子系统装配点。
//
// 组合根(Main)只负责引导 Engine 与平台; 把 UI / Renderer 包装成引擎子系统的
// 具体装配集中在这里, 使 Engine 保持对具体子系统的解耦, 仅通过 runtime::ISubsystem
// 抽象统一驱动它们(依赖反转: 具体子系统 -> Engine, 而非 Engine -> 具体子系统)。
class ModuleLoader
{
public:
    static ModuleLoader& self()
    {
        static ModuleLoader Instance;
        return Instance;
    }
    ~ModuleLoader() = default;
    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;
    ModuleLoader(ModuleLoader&&) = delete;
    ModuleLoader& operator=(ModuleLoader&&) = delete;

    // 注册编辑模式所需的引擎子系统(UI + Renderer)。
    void registerEditorSubsystems(runtime::Engine& Engine, ui::GenericApplication& App,
        runtime::renderer::RendererServer& Renderer, int FrameLimit);

private:
    ModuleLoader() = default;
};
