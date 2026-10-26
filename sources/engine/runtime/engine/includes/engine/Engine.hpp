#pragma once

#include <atomic>
#include <vector>
#include <memory>
#include <utility>

#include <RTCommon.hpp>
#include "ISessionHost.hpp"
#include "ISubsystem.hpp"

namespace runtime
{

namespace platform { class IGenericPlatform; }

class Engine final
{
public:
    static Engine& self() 
    {
        static Engine Instance;
        return Instance;
    }
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    Engine(Engine&&) = delete;
    Engine& operator=(Engine&&) = delete;
    /**
     * @brief 负责初始化基础设置和与平台相关的设置
     * 	- 日志
     *  - 线程池
     *	- UI: 获取底层窗口, 交换链, 翻译底层事件循环
     * 	- RHI:
     */
    void preinitialize();

    /**
     * @brief 正式初始化引擎各个子系统
     *  - Network:
     *  - Video, Audio:
     *  - Physics
     *  - Viewport/Editor:
     */
    void initialize();
    
    /**
     * @brief 本质上就是引擎执行的第一帧
     */
    void postinitialize();
    void run();
    void render();
    void quit();
    bool shouldQuit() const { return ShouldQuit; }
    void destroy();

    // 托管一个会话宿主(PlayerHost 或 EditorHost): 引擎负责驱动其生命周期,
    // 但不依赖具体宿主类型。宿主的语义(编辑世界/运行世界、Play/Simulate)在宿主内部。
    void attachSessionHost(std::unique_ptr<ISessionHost> Host);
    [[nodiscard]] bool hasSessionHost() const noexcept;

    // 注册一个引擎子系统: Engine 拥有其所有权, 并按注册顺序驱动其生命周期;
    // shutdown 时按注册的逆序调用. 传入空指针会被忽略.
    void registerSubsystem(std::unique_ptr<ISubsystem> Subsystem);

    // 工厂式注册: 返回新子系统的引用, 便于调用方继续配置或按需获取.
    template<class T, class... ArgTypes>
    T& addSubsystem(ArgTypes&&... Args)
    {
        auto Subsystem = std::make_unique<T>(std::forward<ArgTypes>(Args)...);
        T& Result = *Subsystem;
        Subsystems.push_back(std::move(Subsystem));
        return Result;
    }

    // 平台层: Engine 拥有 GenericPlatform 的所有权, 并在 destroy 时最后释放.
    // UI / Renderer 通过 getPlatform() 获取注入的平台引用, 不再各自创建平台.
    void attachPlatform(std::unique_ptr<platform::IGenericPlatform> Platform);
    [[nodiscard]] platform::IGenericPlatform& getPlatform();
    [[nodiscard]] const platform::IGenericPlatform& getPlatform() const;
    [[nodiscard]] bool hasPlatform() const noexcept;
private:
    // 构造函数与析构都定义在 Engine.cpp: PlatformInstance 是不完整类型(前向声明),
    // unique_ptr 的构造/析构必须在 IGenericPlatform 完整定义处实例化.
    Engine();
    void tick(float DeltaTime);

private:
    std::atomic<bool> ShouldQuit { false };
    bool IsFirstFrame { true };
    std::unique_ptr<ISessionHost> SessionHost { nullptr };
    std::unique_ptr<platform::IGenericPlatform> PlatformInstance { nullptr };
    std::vector<std::unique_ptr<ISubsystem>> Subsystems;
};
}