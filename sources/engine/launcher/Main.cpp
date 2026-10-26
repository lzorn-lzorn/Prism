#include "GenericApplication.hpp"
#include "ApplicationCommandLine.hpp"
#include "CommandLineParser.hpp"
#include "ModuleLoader.hpp"
#include "RTGlobal.hpp"
#include "EditorApplication.hpp"
#include "game/Game.hpp"
#include <renderer/RendererServer.hpp>
#include <SDL3/SDL_main.h>
#include <exception>
#include <iostream>
#include <memory>
#include <utility>

/**
 * ============================================================================
 *  引擎组合根 + 入口(编辑模式)
 * ============================================================================
 *
 *  本文件只做"引导", 不承载装配与编辑器逻辑:
 *     1. 解析命令行;
 *     2. 创建平台并交给 Engine 持有;
 *     3. 初始化 UI 应用(平台由 Engine 注入);
 *     4. 子系统装配交给 ModuleLoader, 编辑器宿主(EditorHost)经 attachSessionHost 托管,
 *        并注入 GameInstance(由 game::createDemoGameInstance 提供);
 *     5. Engine::run() 成为唯一主循环。
 *
 *  子系统适配(UI/Renderer)在 ModuleLoader, 编辑器实现(界面/场景/渲染)在
 *  editor::EditorApplication, 见 sources/engine/launcher/editor/app/。
 * ============================================================================
 */
int main(int Argc, char** Argv)
{
    try
    {
        // ---- 1. 引擎: 日志/模块/平台 ----
        ModuleLoader::self();
        GSeedEngine.preinitialize();
        // 平台层归 Engine 管理: 由 Engine 持有并注入给 UI / Renderer.
        GSeedEngine.attachPlatform(runtime::platform::createPlatform());

        // ---- 2. 命令行 ----
        ui::ApplicationOptions Options;
        ui::CommandLineParser Parser("Editor", "基础编辑器示例(编辑器界面 + 游戏视口)");
        ui::registerApplicationOptions(Parser, Options);
        const auto ParseResult = Parser.parse(Argc, Argv, std::cout, std::cerr);
        if (ParseResult.ShouldExit)
            return ParseResult.ExitCode;

        // ---- 3. UI 应用(平台由 Engine 注入) ----
        auto& App = ui::GenericApplication::self();
        App.initialize(GSeedEngine.getPlatform(), std::move(Options));

        // ---- 4. 装配: 子系统由 ModuleLoader 注册, 编辑器宿主(EditorHost)注入 GameInstance ----
        auto& Renderer = runtime::renderer::RendererServer::self();
        ModuleLoader::self().registerEditorSubsystems(
            GSeedEngine, App, Renderer, App.getOptions().FrameLimit);
        GSeedEngine.attachSessionHost(std::make_unique<editor::EditorApplication>(
            App, Renderer, game::createDemoGameInstance()));
        GSeedEngine.initialize(); // → Renderer 初始化 → Editor 初始化(界面 + 场景)

        // ---- 5. UI 一次性启动 + 主循环(Engine 驱动) ----
        App.start();
        GSeedEngine.run();

        GSeedEngine.destroy();
        return 0;
    }
    catch (const std::exception& Error)
    {
        GSeedEngine.destroy();
        std::cerr << "Editor error: " << Error.what() << '\n';
        return 1;
    }
}
