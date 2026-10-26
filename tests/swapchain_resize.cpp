/**
 * ============================================================================
 *  tests/swapchain_resize.cpp
 * ============================================================================
 *
 *  验证窗口缩放后交换链能跟随窗口尺寸重建(回归: Windows + Vulkan 下
 *  acquireNextImageKHR 缩放后仍返回 Success, 导致交换链停留在旧尺寸、UI 不跟随放大).
 *
 *  需要: SDL3 + Vulkan + 可用显卡. 初始化失败时打印 SKIP 并以 0 退出,
 *  这样 CI 上没有 GPU 也不会把测试判为失败.
 * ============================================================================
 */

#include <RHIServer.hpp>

#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>
#include <string>

namespace
{
class SdlTestWindow final : public runtime::platform::IGenericWindow
{
public:
    explicit SdlTestWindow(SDL_Window* InWindow) : Window(InWindow) {}
    [[nodiscard]] runtime::platform::WindowId getId() const noexcept override { return 0; }
    [[nodiscard]] runtime::platform::UISize getSize() const override
    {
        int W = 0, H = 0; SDL_GetWindowSize(Window, &W, &H); return {W, H};
    }
    [[nodiscard]] runtime::platform::UISize getPixelSize() const override
    {
        int W = 0, H = 0; SDL_GetWindowSizeInPixels(Window, &W, &H); return {W, H};
    }
    [[nodiscard]] runtime::platform::Rect getSafeArea() const override { const auto S = getSize(); return {0, 0, S.Width, S.Height}; }
    [[nodiscard]] float getDisplayScale() const override { return SDL_GetWindowDisplayScale(Window); }
    [[nodiscard]] void* getNativeHandle() const noexcept override { return Window; }
    void setTitle(const std::string&) override {}
    void setSize(runtime::platform::UISize S) override { SDL_SetWindowSize(Window, S.Width, S.Height); }
    void setResizable(bool) override {}
    void show() override {}
    void hide() override {}
    void setFullscreen(bool) override {}
    void setRelativeMouse(bool) override {}
    void startTextInput() override {}
    void stopTextInput() override {}
    void setTextInputArea(runtime::platform::Rect, int) override {}
private:
    SDL_Window* Window { nullptr };
};

void pump()
{
    SDL_PumpEvents();
    SDL_Event E;
    while (SDL_PollEvent(&E)) {}
}

int g_checks = 0;
int g_failures = 0;

#define CHECK(Expr)                                                        \
    do                                                                     \
    {                                                                      \
        ++g_checks;                                                        \
        if (!(Expr))                                                       \
        {                                                                  \
            std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #Expr); \
            ++g_failures;                                                  \
        }                                                                  \
    } while (false)
} // namespace

int main()
{
    if (!SDL_Init(SDL_INIT_VIDEO))
    {
        std::printf("SKIP: SDL video unavailable: %s\n", SDL_GetError());
        return 0;
    }
    SDL_Window* window = SDL_CreateWindow("SwapchainResize", 640, 480,
        SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window)
    {
        std::printf("SKIP: no Vulkan-capable window: %s\n", SDL_GetError());
        SDL_Quit();
        return 0;
    }

    auto generic = std::make_shared<SdlTestWindow>(window);
    try
    {
        rhi::RHIServer::self().initialize(rhi::ESupportedBackendAPI::Vulkan, generic);
    }
    catch (const std::exception& E)
    {
        std::printf("SKIP: RHI init failed: %s\n", E.what());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }
    auto device = rhi::RHIServer::self().getDevice();
    if (!device)
    {
        std::printf("SKIP: no Vulkan device\n");
        rhi::RHIServer::self().shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    rhi::SwapchainDescriptor desc;
    desc.Width = 640;
    desc.Height = 480;
    desc.MinimumImageCount = 3;
    desc.DebugName = "SwapchainResizeTest";
    auto swapchain = device->createSwapchain(desc);
    if (!swapchain)
    {
        std::printf("SKIP: swapchain creation failed\n");
        rhi::RHIServer::self().shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 0;
    }

    // 模拟生产帧循环: 窗口缩放后 acquire 应报告 OutOfDate, 再用"当前成员旧尺寸"
    // 重建(create 会用 surface 的 currentExtent 覆盖), 最终交换链尺寸 == 窗口像素尺寸.
    auto verifyResize = [&](int Width, int Height)
    {
        SDL_SetWindowSize(window, Width, Height);
        pump();
        for (int Frame = 0; Frame < 4; ++Frame)
        {
            auto sem = device->createSemaphore();
            const auto acq = swapchain->acquireNextImage(sem);
            if (acq.Status == rhi::EAcquireStatus::OutOfDate ||
                acq.Status == rhi::EAcquireStatus::Suboptimal)
            {
                swapchain->recreate(swapchain->getDescriptor().Width,
                    swapchain->getDescriptor().Height);
                continue;
            }
            CHECK(acq.Status == rhi::EAcquireStatus::Success);
            break;
        }
        int PixelWidth = 0, PixelHeight = 0;
        SDL_GetWindowSizeInPixels(window, &PixelWidth, &PixelHeight);
        CHECK(swapchain->getDescriptor().Width == static_cast<uint32_t>(PixelWidth));
        CHECK(swapchain->getDescriptor().Height == static_cast<uint32_t>(PixelHeight));
    };

    verifyResize(1280, 720);
    verifyResize(1920, 1080);

    rhi::RHIServer::self().shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures != 0)
    {
        std::printf("swapchain resize test FAILED.\n");
        return 1;
    }
    std::printf("swapchain resize test passed.\n");
    return 0;
}
