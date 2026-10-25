#pragma once

#include <renderer/RenderCommand.hpp>
#include <renderer/RenderProxyWorld.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

/**
 * ============================================================================
 *  runtime::renderer - RenderThread(渲染线程)
 * ============================================================================
 *
 *  [要解决的问题]
 *  把"场景增量消费 + 渲染准备"从 GameThread 上拆下来, 放到一条专用线程上:
 *    - GameThread 只负责 enqueue() 渲染命令(值语义), 然后继续跑游戏逻辑;
 *    - RenderThread 负责: 等待唤醒 → 排空命令队列 → 应用到自己的场景快照
 *      → (Phase 3) 建 RDG → 录制 → 提交.
 *
 *  [实现方式]
 *  - 命令队列复用 infra::MPMCQueue(无锁 SPSC): 数据路径无锁;
 *  - 唤醒用 condition_variable + mutex: 生产者入队时短暂持有 mutex,
 *    与消费者的"队列是否非空"判定形成互斥, 从根本上避免"丢失唤醒"竞态;
 *  - 场景采用"渲染线程私有副本"(ProxyScene), GameThread 永远不直接写它,
 *    从而避免跨线程读写竞争 —— 命令是唯一同步点.
 *
 *  [线程约定]
 *  - start()/enqueue()/requestStop() 由 GameThread 调用;
 *  - join() 由宿主在关闭阶段调用(先 requestStop 再 join).
 *
 *  [改进空间]
 *  @note 入队为与唤醒判定互斥而短暂持锁; 若未来测出该锁成为瓶颈,
 *        可换成 std::counting_semaphore(每入队一条 release 一次、每消费一条
 *        acquire 一次, 保持计数与队列长度 1:1)以彻底去掉入队侧的锁.
 */

namespace runtime::renderer
{

/**
 * @brief 渲染线程: 单消费者消费渲染命令, 维护渲染场景快照.
 *
 * 生命周期: start() → (GameThread 反复 enqueue()) → requestStop() → join().
 */
class RenderThread final
{
public:
    RenderThread() = default;
    ~RenderThread();

    RenderThread(const RenderThread&)            = delete;
    RenderThread& operator=(const RenderThread&) = delete;
    RenderThread(RenderThread&&)                 = delete;
    RenderThread& operator=(RenderThread&&)      = delete;

    /** @brief 启动工作线程. 幂等: 已运行则忽略. */
    void start();

    /** @brief 请求停止(置标志 + 唤醒). 不阻塞. */
    void requestStop() noexcept;

    /** @brief 等待工作线程退出. 必须先 requestStop(). */
    void join();

    /** @brief 工作线程是否正在运行. */
    [[nodiscard]] bool isRunning() const noexcept;

    /**
     * @brief 入队一条渲染命令并唤醒工作线程(GameThread 调用).
     * @param Command 值语义命令; 队列满时被丢弃(调用方下帧重发).
     */
    void enqueue(RenderCommand Command);

    /**
     * @brief 投递一个任务到渲染线程串行执行(GameThread 调用).
     * @param Task 任意可调用体; 在渲染线程上、命令排空之后按 FIFO 顺序执行.
     *
     * @note 这是"渲染线程接管帧录制/提交"的载体: 调用方把一帧的录制工作打包成
     *       Task 投递, 渲染线程在消费完命令后执行它, 从而把录制从 GameThread 挪走.
     *       配合 std::promise/future 可实现"投递后阻塞等待结果"的帧握手.
     *       类型为 move_only_function: 允许捕获 move-only 载荷(如 std::promise).
     */
    using Task = std::move_only_function<void()>;
    void postTask(Task InTask);

    /** @brief 命令队列(GameThread 直接 push 的备选入口). */
    [[nodiscard]] RenderCommandQueue& getQueue() noexcept { return Commands; }

    /**
     * @brief 渲染线程私有的场景快照(只读视图).
     * @return 一致快照; 调用方(渲染线程内)可安全读取.
     */
    [[nodiscard]] const RenderProxyWorld& getProxyScene() const noexcept { return ProxyScene; }

    /** @brief 最近一次 SetRenderView 的相机矩阵. */
    [[nodiscard]] const RenderView& getCurrentView() const noexcept { return CurrentView; }

private:
    /** @brief 工作线程主循环. */
    void run();

    /** @brief 把一条命令应用到场景快照 / 当前视图. */
    void apply(const RenderCommand& Command);

    RenderCommandQueue Commands;
    RenderProxyWorld   ProxyScene;
    RenderView         CurrentView {};

    std::thread        Worker;
    /** @brief Windows 下用 _beginthreadex 创建的大栈线程句柄(栈 8MB, 避免管线创建栈溢出). */
    uintptr_t          ThreadHandle { 0 };
    std::atomic<bool>  StopRequested { false };
    std::mutex         WakeMutex;
    std::condition_variable WakeCondition;
    /** @brief 待执行任务(由 WakeMutex 保护, FIFO). */
    std::deque<Task>   PendingTasks;
};

} // namespace runtime::renderer
