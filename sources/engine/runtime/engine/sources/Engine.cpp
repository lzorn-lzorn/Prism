#include "engine/Engine.hpp"
#include "logger/Logger.hpp"
#include "IGenericPlatform.hpp"
#include <chrono>
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

    // 驱动引擎自身子系统的基础设置阶段(日志、平台运行时、配置加载等)。
    for (auto& Subsystem : Subsystems)
    {
        Subsystem->preinitialize();
    }
}
void Engine::initialize() 
{
    // 初始化网络模块

    // 初始化音频视频组件

    // 初始化物理模块

    // 初始化渲染线程

    // 加载整个引擎的配置

    // 加载 Game 或者启动 Editor 模式 
    // 引擎自身子系统就绪后, 才轮到"游戏生命周期"起步.
    for (auto& Subsystem : Subsystems)
    {
        Subsystem->initialize();
    }
    if (GameInstance)
    {
        GameInstance->preinitialize();
        GameInstance->initialize();
    }
    if (EditorInstance)
    {
        EditorInstance->preinitialize();
        EditorInstance->initialize();
    }
}
void Engine::postinitialize()
{
    for (auto& Subsystem : Subsystems)
    {
        Subsystem->postinitialize();
    }
}
void Engine::run()
{
    using Clock = std::chrono::steady_clock;
    auto LastTick = Clock::now();
    while (!ShouldQuit)
    {
        if (IsFirstFrame)
        {
            // 引擎自己的第一帧, 随后才是游戏的第一帧.
            postinitialize();
            if (GameInstance)
            {
                GameInstance->postinitialize();
            }
            if (EditorInstance)
            {
                EditorInstance->postinitialize();
            }
            IsFirstFrame = false;
        }

        // 计算 DeltaTime
        const auto Now = Clock::now();
        const float DeltaTime = std::chrono::duration<float>(Now - LastTick).count();
        LastTick = Now;

        // 处理输入事件、更新世界、渲染: 交由各子系统在 tick 内完成.
        // 引擎只负责按固定顺序驱动: 先子系统(UI 事件泵/帧工作、渲染), 再游戏.
        tick(DeltaTime);

        // 处理日志(手动泵帧模式)
        if (Logger::self().getRunMode() == Logger::ERunMode::ManualFramePump)
        {
            if (!Logger::self().pumpFrame(512, std::chrono::milliseconds(10)))
            {
                // 处理泵帧失败的情况
            }
        }
    }
}
void Engine::tick(float DeltaTime)
{
    // 引擎主循环内, 先驱动引擎自身子系统, 再驱动被托管游戏的一帧.
    for (auto& Subsystem : Subsystems)
    {
        Subsystem->tick(DeltaTime);
    }
    if (GameInstance)
    {
        GameInstance->tick(DeltaTime);
    }
    if (EditorInstance)
    {
        EditorInstance->tick(DeltaTime);
    }
}
void Engine::render() {}
void Engine::quit() { ShouldQuit = true; }
void Engine::destroy() 
{
    // 先销毁游戏/编辑器(托管实体), 再拆引擎自身子系统 —— 与 initialize 的顺序相反.
    if (GameInstance)
    {
        GameInstance->destroy();
    }
    GameInstance.reset();
    if (EditorInstance)
    {
        EditorInstance->destroy();
    }
    EditorInstance.reset();

    // 引擎自身子系统按注册逆序关闭(后初始化先关闭)。
    for (auto It = Subsystems.rbegin(); It != Subsystems.rend(); ++It)
    {
        (*It)->shutdown();
    }
    Subsystems.clear();

    // 平台最后释放: 所有依赖窗口/平台的子系统都已关闭.
    PlatformInstance.reset();

    // TODO: 停止日志、释放渲染线程等引擎自身资源.
}

void Engine::attachGame(std::unique_ptr<IGame> Game)
{
    GameInstance = std::move(Game);
}

bool Engine::hasGame() const noexcept
{
    return GameInstance != nullptr;
}

void Engine::attachEditor(std::unique_ptr<IEditor> Editor)
{
    EditorInstance = std::move(Editor);
}

bool Engine::hasEditor() const noexcept
{
    return EditorInstance != nullptr;
}

void Engine::registerSubsystem(std::unique_ptr<ISubsystem> Subsystem)
{
    if (Subsystem)
    {
        Subsystems.push_back(std::move(Subsystem));
    }
}

Engine::Engine() = default;

Engine::~Engine() = default;

void Engine::attachPlatform(std::unique_ptr<platform::IGenericPlatform> Platform)
{
    PlatformInstance = std::move(Platform);
}

platform::IGenericPlatform& Engine::getPlatform()
{
    return *PlatformInstance;
}

const platform::IGenericPlatform& Engine::getPlatform() const
{
    return *PlatformInstance;
}

bool Engine::hasPlatform() const noexcept
{
    return PlatformInstance != nullptr;
}

}