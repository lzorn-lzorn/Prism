#include <exception>
#include <iostream>

#include "EngineMain.hpp"
#include <Application.hpp>
#include "Engine.hpp"
#include "RTGlobal.hpp"
#include "CommandLine.hpp"
#include "ModuleLoader.hpp"
#include <renderer/RendererServer.hpp>

int EngineMain(int argc, char** argv)
{
    try
    {
        CommandLine cmd(argc, argv);
        // 初始化内存分配器

        // 初始化模块加载器
        ModuleLoader::self();
        
        // 初始化引擎
        GSeedEngine.preinitialize();

        // 初始化通用窗口系统；当前工厂选择 SDL3 实现. 
        auto generic_application = app::createGenericApplication();
        ui::WindowDescriptor window_descriptor {
            .Title = "SeedEngine RHI Test",
            .LeftTopPoint = ui::UIVector(100, 100),
            .Width = 1280,
            .Height = 720,
            .WindowType = ui::EWindowType::Windowed,
            .IsResizable = true,
            .IsVisiable = true,
            .HasBorder = true,
            .AcceptsInputs = true,
            .IsDialog = false
        };
        ui::GenericWindowPointer generic_window = generic_application->makeWindow(window_descriptor);
        generic_window->show();

        // UI 只依赖 Renderer；后端创建, 反射与 BindGroup 均由下层管理. 
        auto& renderer = runtime::renderer::RendererServer::self();
        renderer.initialize(rhi::ESupportedBackendAPI::Vulkan, generic_window);
        if (!renderer.isInitialized())
        {
            throw std::runtime_error("Renderer initialization did not complete.");
        }
        renderer.runBindGroupSmokeTest();
        // WSI smoke: minimized surfaces own no swapchain images, then restore without device idle.
        renderer.resize(0, 0);
        renderer.resize(
            static_cast<uint32_t>(generic_window->getWidth()),
            static_cast<uint32_t>(generic_window->getHeight()));
        (void)renderer.renderFrame();

        GSeedEngine.initialize();
        GSeedEngine.run();
        GSeedEngine.destroy();

        renderer.shutdown();
        generic_window.reset();
        generic_application.reset();
        return 0;
    }
    catch (const std::exception& exception)
    {
        runtime::renderer::RendererServer::self().shutdown();
        GSeedEngine.destroy();
        std::cerr << "SeedEngine startup failed: " << exception.what() << '\n';
        return 1;
    }
}