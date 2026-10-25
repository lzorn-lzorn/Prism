#include <renderer/RenderThread.hpp>

#include <cstdio>
#include <exception>
#include <type_traits>
#include <utility>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
#endif

namespace runtime::renderer
{

RenderThread::~RenderThread()
{
    // 析构兜底: 无论宿主是否显式关闭, 都保证线程先停后合.
    requestStop();
    join();
}

void RenderThread::start()
{
#ifdef _WIN32
    if (ThreadHandle != 0)
        return;
#else
    if (Worker.joinable())
        return;
#endif
    StopRequested.store(false, std::memory_order_release);
#ifdef _WIN32
    // 渲染线程调用栈较深(SPIR-V 反射 + 管线创建 + RDG 执行), 用 8MB 栈避免栈溢出.
    ThreadHandle = _beginthreadex(nullptr, 8u * 1024u * 1024u,
        [](void* Self) -> unsigned int {
            static_cast<RenderThread*>(Self)->run();
            return 0;
        },
        this, 0, nullptr);
#else
    Worker = std::thread([this] { run(); });
#endif
}

void RenderThread::requestStop() noexcept
{
    {
        std::lock_guard lock(WakeMutex);
        StopRequested.store(true, std::memory_order_release);
    }
    WakeCondition.notify_all();
}

void RenderThread::join()
{
#ifdef _WIN32
    if (ThreadHandle != 0)
    {
        WaitForSingleObject(reinterpret_cast<HANDLE>(ThreadHandle), INFINITE);
        CloseHandle(reinterpret_cast<HANDLE>(ThreadHandle));
        ThreadHandle = 0;
    }
#else
    if (Worker.joinable())
        Worker.join();
#endif
}

bool RenderThread::isRunning() const noexcept
{
#ifdef _WIN32
    return ThreadHandle != 0 && !StopRequested.load(std::memory_order_acquire);
#else
    return Worker.joinable() && !StopRequested.load(std::memory_order_acquire);
#endif
}

void RenderThread::enqueue(RenderCommand Command)
{
    {
        // 入队与"队列非空"判定共用同一把锁, 消除条件变量的丢失唤醒竞态.
        std::lock_guard lock(WakeMutex);
        // 队列满时丢弃该命令(调用方下帧重发), 故忽略返回值.
        (void)Commands.tryPush(std::move(Command));
    }
    WakeCondition.notify_one();
}

void RenderThread::postTask(Task InTask)
{
    {
        std::lock_guard lock(WakeMutex);
        PendingTasks.push_back(std::move(InTask));
    }
    WakeCondition.notify_one();
}

void RenderThread::run()
{
    while (true)
    {
        std::deque<Task> tasks;
        {
            std::unique_lock lock(WakeMutex);
            WakeCondition.wait(lock, [this] {
                return StopRequested.load(std::memory_order_acquire) ||
                    Commands.getSize() != 0 ||
                    !PendingTasks.empty();
            });
            // 取出全部待执行任务, 在锁外执行, 避免长任务阻塞生产者.
            tasks.swap(PendingTasks);
        }

        // 先处理完本轮工作(命令 + 任务), 再检查停止标志:
        // 保证 requestStop() 前已投递的命令/任务都不会被丢弃.
        while (auto command = Commands.tryPop())
            apply(*command);
        for (Task& task : tasks)
        {
            try
            {
                task();
            }
            catch (const std::exception& Error)
            {
                // 任务异常不应让渲染线程 terminate; 记录后继续, 避免进程崩溃.
                std::fprintf(stderr, "Render thread task failed: %s\n", Error.what());
                std::fflush(stderr);
            }
        }

        if (StopRequested.load(std::memory_order_acquire))
            break;
    }

    // 停止前把剩余命令排空, 保证关闭时无遗漏.
    while (auto command = Commands.tryPop())
        apply(*command);
}

void RenderThread::apply(const RenderCommand& Command)
{
    std::visit([this](const auto& Cmd) {
        using T = std::decay_t<decltype(Cmd)>;
        if constexpr (std::is_same_v<T, RenderCommandAddProxy>)
        {
            ProxyScene.addProxy(Cmd.Proxy);
        }
        else if constexpr (std::is_same_v<T, RenderCommandRemoveProxy>)
        {
            ProxyScene.removeProxy(Cmd.Index);
        }
        else if constexpr (std::is_same_v<T, RenderCommandUpdateTransform>)
        {
            ProxyScene.updateTransform(Cmd.Index, Cmd.Transform);
        }
        else if constexpr (std::is_same_v<T, RenderCommandUpdateMaterial>)
        {
            ProxyScene.updateMaterial(Cmd.Index, Cmd.MaterialId);
        }
        else if constexpr (std::is_same_v<T, RenderCommandSetRenderView>)
        {
            CurrentView = Cmd.View;
        }
    }, Command);
}

} // namespace runtime::renderer
