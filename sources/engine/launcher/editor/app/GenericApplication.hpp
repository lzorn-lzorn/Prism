#pragma once

#include "Window.hpp"
#include "RHIUIRenderer.hpp"
#include "TextConfig.hpp"
#include "TextWidget.hpp"
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui
{
using runtime::platform::IGenericPlatform;

struct ApplicationOptions
{
#ifdef __APPLE__
    EGpuBackend Backend{EGpuBackend::Metal};
#else
    EGpuBackend Backend{EGpuBackend::Vulkan};
#endif
    std::string FontPath;
    bool DiscoverSystemFonts{true};
    EUIMode UIMode{EUIMode::Retained}; // @note UI 渲染模式：保留模式（默认）或立即模式。
    std::vector<std::string> DefaultFontPaths;
    std::vector<std::string> EmojiFontPaths;
    TextConfig Text;
    int FrameLimit{0}; // @note 零表示持续运行直到关闭。
};
class GenericApplication
{
public:
    static GenericApplication& self();
    GenericApplication(const GenericApplication&) = delete;
    GenericApplication& operator=(const GenericApplication&) = delete;
    GenericApplication(GenericApplication&&) = delete;
    GenericApplication& operator=(GenericApplication&&) = delete;
    void initialize(IGenericPlatform& Platform, ApplicationOptions Options = {});
    bool isInitialized() const noexcept;
    Window& getMainWindow();
    const Window& getMainWindow() const;
    const ApplicationOptions& getOptions() const;
    const std::string& getCurrentFontPath() const;
    const std::vector<std::string>& getFallbackFontPaths() const;
    TextEncoding getCurrentTextEncoding() const;
    void setCurrentTextEncoding(TextEncoding Encoding);
    std::string encodeText(std::string_view UTF8) const;
    std::string encodeText(std::u32string_view UTF32) const;
    IUIRenderer& getRenderer();
    /**
     * @brief 注册每帧回调(在 UI drawFrame 之后调用), 供宿主把 UI 数据提交给渲染器.
     * @param Hook 无参回调; 空表示取消.
     */
    void setFrameHook(std::function<void()> Hook);
    std::unique_ptr<TextWidget> createTextWidget(std::string_view Text, float Size, float MaxWidth = 0, Color Tint = {1, 1, 1, 1});
    template<class Character, class Traits, class Allocator>
    std::unique_ptr<TextWidget> createTextWidget(const std::basic_string<Character, Traits, Allocator>& Text, float Size, float MaxWidth = 0, Color Tint = {1, 1, 1, 1})
    {
        return createTextWidget(getTextBytes(Text), Size, MaxWidth, Tint);
    }
    /**
     * @brief 轮询并派发平台事件; 返回是否应继续运行(false 表示收到退出请求).
     * 由宿主(Engine 主循环)每帧调用.
     */
    bool pumpEvents();
    /** @brief 一次性启动: 开启文本输入、发布 ready 事件等(进入主循环前调用一次). */
    void start();
    /** @brief 执行一帧 UI 工作: 排版/绘制/呈现内容根, 随后调用 FrameHook. */
    void tick();
    /** @brief 释放窗口、字体会话与 UI 渲染器(析构前可显式调用, 幂等). */
    void shutdown();
    int32_t run();

private:
    GenericApplication() = default;
    ~GenericApplication();
    void checkInitialized() const;
    ApplicationOptions Options;
    IGenericPlatform* Platform{nullptr};
    std::shared_ptr<RHIUIRenderer> Renderer;
    std::unique_ptr<Window> MainWindow;
    std::function<void()> FrameHook;
    bool Fullscreen{false};
};
}
