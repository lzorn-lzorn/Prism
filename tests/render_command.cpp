/**
 * ============================================================================
 *  tests/render_command.cpp
 * ============================================================================
 *
 *  渲染命令队列的 CPU 侧验证(Phase 1): 不依赖 GPU.
 *
 *  覆盖:
 *      1. SPSC 无锁队列的入队/出队顺序一致性
 *      2. variant 命令的载荷往返(移动语义)
 *      3. 满队列时 tryPush 返回 false(有界行为)
 */

#include <renderer/RenderCommand.hpp>

#include <cstdio>
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

    RenderCommandQueue Queue;

    // 1) 入队三条不同类型的命令.
    RenderCommandAddProxy Add;
    Add.Proxy = MeshRenderProxy {};
    CHECK(Queue.tryPush(RenderCommand { std::move(Add) }));
    CHECK(Queue.tryPush(RenderCommand { RenderCommandRemoveProxy { 3 } }));
    CHECK(Queue.tryPush(RenderCommand { RenderCommandUpdateMaterial { 1, 42 } }));

    // 2) 出队顺序必须与入队一致, 且载荷往返正确.
    auto C0 = Queue.tryPop();
    CHECK(C0.has_value());
    if (C0)
    {
        CHECK(std::holds_alternative<RenderCommandAddProxy>(*C0));
        CHECK(std::get_if<RenderCommandAddProxy>(&*C0) != nullptr);
    }

    auto C1 = Queue.tryPop();
    CHECK(C1.has_value());
    if (C1)
    {
        const auto* Remove = std::get_if<RenderCommandRemoveProxy>(&*C1);
        CHECK(Remove != nullptr);
        if (Remove)
            CHECK(Remove->Index == 3);
    }

    auto C2 = Queue.tryPop();
    CHECK(C2.has_value());
    if (C2)
    {
        const auto* Material = std::get_if<RenderCommandUpdateMaterial>(&*C2);
        CHECK(Material != nullptr);
        if (Material)
        {
            CHECK(Material->Index == 1);
            CHECK(Material->MaterialId == 42);
        }
    }

    // 3) 队列已空.
    CHECK(!Queue.tryPop().has_value());

    std::printf("--------------------------------------------------------------\n");
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    std::printf("Render command queue tests %s.\n", g_failures == 0 ? "passed" : "failed");
    return g_failures == 0 ? 0 : 1;
}
