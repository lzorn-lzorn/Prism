#include "ModuleLoader.hpp"

#include "GenericApplication.hpp"
#include "engine/Engine.hpp"
#include <renderer/RendererServer.hpp>

#include <memory>
#include <stdexcept>

namespace
{
/**
 * @brief 让 Engine 驱动渲染器生命周期: initialize 创建 RHI 设备与交换链, shutdown 回收.
 */
class RendererSubsystem final : public runtime::ISubsystem
{
public:
    RendererSubsystem(ui::GenericApplication& InApp, runtime::renderer::RendererServer& InRenderer)
        : App(InApp), Renderer(InRenderer)
    {
    }

    void initialize() override
    {
        auto& NativeWindow = App.getMainWindow().getNativeWindow();
        // 非拥有 shared_ptr: 窗口生命周期由 GenericApplication 的 MainWindow 管理.
        runtime::platform::GenericWindowPointer WindowHandle(
            &NativeWindow, [](runtime::platform::IGenericWindow*) {});
        Renderer.initialize(rhi::ESupportedBackendAPI::Vulkan, WindowHandle);
        if (!Renderer.isInitialized())
            throw std::runtime_error("Renderer initialization did not complete.");
    }

    void shutdown() override
    {
        Renderer.shutdown();
    }

private:
    ui::GenericApplication& App;
    runtime::renderer::RendererServer& Renderer;
};

/**
 * @brief 让 Engine 主循环驱动 UI: 每帧泵平台事件 + 执行一帧 UI 工作.
 *
 * GenericApplication 不再自持主循环; 本适配器作为 ISubsystem 注册进 Engine,
 * 由 Engine::run() 每帧调用 tick(), 把 UI 纳入 Engine 的统一编排.
 */
class UIApplicationSubsystem final : public runtime::ISubsystem
{
public:
    explicit UIApplicationSubsystem(ui::GenericApplication& InApp, int InFrameLimit)
        : App(InApp), FrameLimit(InFrameLimit)
    {
    }

    void tick(float) override
    {
        if (!App.pumpEvents() || (FrameLimit > 0 && Frames >= FrameLimit))
        {
            runtime::Engine::self().quit();
            return;
        }
        App.tick();
        ++Frames;
    }

    void shutdown() override
    {
        App.shutdown();
    }

private:
    ui::GenericApplication& App;
    int FrameLimit;
    int Frames{0};
};
} // namespace

void ModuleLoader::registerEditorSubsystems(runtime::Engine& Engine, ui::GenericApplication& App,
    runtime::renderer::RendererServer& Renderer, int FrameLimit)
{
    Engine.registerSubsystem(std::make_unique<RendererSubsystem>(App, Renderer));
    Engine.registerSubsystem(std::make_unique<UIApplicationSubsystem>(App, FrameLimit));
}
