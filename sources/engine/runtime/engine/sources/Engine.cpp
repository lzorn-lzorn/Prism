#include "engine/Engine.hpp"
#include "logger/Logger.hpp"
#include <string_view>

namespace runtime
{
void Engine::preinitialize() 
{
    // 初始化日志系统
    auto& logger = Logger::self();

    Logger::Config config;
    config.Mode = Logger::ERunMode::ManualFramePump;
    config.EnableFileOutput = true;
    config.FilePath = "logs/engine.log";
    config.FlushThresholdBytes = 64 * 1024;
    config.BackgroundBatchMessages = 512; // 后台线程没批处理条数
    config.BackgroundWakeInterval = std::chrono::milliseconds(10); // 后台线程唤醒间隔
    config.MinimumLevel = levels::Info;

    // 屏幕 sink：比如用 ImGui 或引擎自带的调试面板显示
    logger.setScreenSink([](const Message& Msg, std::string_view Formatted){
        // 将 formatted 文本显示到屏幕
        // 例如：DebugOverlay::addLogLine(formatted);
    });

    // 编辑器控制台 sink：发送到编辑器 UI
    logger.setEditorConsoleSink([](const Message& Msg, std::string_view Formatted){
        // 将 formatted 文本显示到编辑器控制台
        // 例如：EditorConsole::addLogLine(formatted);
    });

    logger.registerSink(
        [](const Message& Msg, std::string_view Formatted){
            if (Msg.LevelPriority >= levels::Warning.Priority)
            {
                // 将 formatted 文本写入文件或其他自定义处理
                // 例如：FileLogger::write(formatted);
            }
        },
        LogDestination::enum_type::All
    );
    logger.setMinimumLevel(runtime::levels::Warning);

 	if (config.Mode == runtime::Logger::ERunMode::ManualFramePump)
    {
        if (!logger.startManually(config))
        {
            return ;
        }
    }
    else
    {
        if (!logger.start(config))
        {
            return ;
        }
    }

    LOG_INFO(runtime::DefaultCategory, "Engine initialized successfully");
    LOG_INFO(DefaultCategory, "Engine version {}", "1.0.0");
    // 初始化底层 UI 系统, 获得窗口和事件分发

    // 初始化 TimerManager

    // 初始化 RHI

}
void Engine::initialize() 
{
    // 初始化网络模块

    // 初始化音频视频组件

    // 初始化物理模块

    // 初始化渲染线程

    // 加载整个引擎的配置

    // 加载 Game 或者启动 Editor 模式 

}
void Engine::postinitialize() {}
void Engine::run()
{
    if (!ShouldQuit)
    {
        if (IsFirstFrame)
        {
            postinitialize();
            IsFirstFrame = false;
        }

        // 计算 DeltaTime

        // 处理输入事件, 向 UI 逐层传递, UI 不处理再传递给世界

        // 更新世界

        // 渲染

        
        // 处理日志
        if (Logger::self().getRunMode() == Logger::ERunMode::ManualFramePump)
        {
            if (!Logger::self().pumpFrame(512, std::chrono::milliseconds(10)))
            {
                // 处理泵帧失败的情况
            }
        }
    }
}
void Engine::tick(float DeltaTime) {}
void Engine::render() {}
void Engine::quit() { ShouldQuit = true; }
void Engine::destroy() 
{
}

}