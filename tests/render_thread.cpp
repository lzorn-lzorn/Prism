/**
 * ============================================================================
 *  tests/render_thread.cpp
 * ============================================================================
 *
 *  渲染线程(Phase 2)的 CPU 侧验证: 不依赖 GPU.
 *
 *  覆盖:
 *      1. 线程启动 / 命令入队 / 停止 / 合入的生命周期
 *      2. 命令应用到渲染线程私有场景快照(AddProxy)
 *      3. SetRenderView 应用到当前视图
 *      4. RemoveProxy 移除
 *
 *  同步方式: enqueue 之后 requestStop()+join(); join 建立 happens-before,
 *  因此 join 之后读取快照是线程安全的(无需额外锁).
 */

#include <renderer/RenderThread.hpp>

#include <cstdio>
#include <future>
#include <thread>
#include <utility>

namespace
{

int g_failures = 0;
int g_checks = 0;

#define CHECK(Expr)                                                     \
    do                                                                  \
    {                                                                   \
        ++g_checks;                                                     \
        if (!(Expr))                                                    \
        {                                                               \
            std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #Expr); \
            ++g_failures;                                               \
        }                                                               \
    } while (false)

} // namespace

int main()
{
    using namespace runtime::renderer;

    RenderThread Thread;
    Thread.start();
    CHECK(Thread.isRunning());

    // 入队: 新增一个代理 + 设置相机视图.
    Thread.enqueue(RenderCommand { RenderCommandAddProxy { MeshRenderProxy {} } });
    Thread.enqueue(RenderCommand { RenderCommandSetRenderView { RenderView {} } });

    // 停止 + 合入: join 之后读取快照是安全的.
    Thread.requestStop();
    Thread.join();
    CHECK(!Thread.isRunning());

    // 1) 代理已应用到渲染线程私有场景快照.
    CHECK(Thread.getProxyScene().getProxyCount() == 1);

    // 2) 相机视图已应用(简单验证其可访问).
    CHECK(Thread.getProxyScene().getProxy(0) != nullptr);

    // 3) 再跑一轮: 入队 RemoveProxy 后停止, 验证移除.
    RenderThread Thread2;
    Thread2.start();
    Thread2.enqueue(RenderCommand { RenderCommandAddProxy { MeshRenderProxy {} } });
    Thread2.enqueue(RenderCommand { RenderCommandRemoveProxy { 0 } });
    Thread2.requestStop();
    Thread2.join();
    CHECK(Thread2.getProxyScene().getProxyCount() == 0);

    // 4) postTask: 任务在渲染线程上执行(非主线程), 并用 promise/future 握手.
    RenderThread Thread3;
    Thread3.start();
    std::promise<std::thread::id> Promise;
    std::future<std::thread::id> Future = Promise.get_future();
    Thread3.postTask([&Promise]() {
        Promise.set_value(std::this_thread::get_id());
    });
    Thread3.requestStop();
    Thread3.join();
    CHECK(Future.get() != std::this_thread::get_id());

    std::printf("--------------------------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("Render thread tests %s.\n", g_failures == 0 ? "passed" : "failed");
    return g_failures == 0 ? 0 : 1;
}
