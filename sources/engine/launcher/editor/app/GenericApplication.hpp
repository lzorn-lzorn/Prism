#pragma once

#include "Window.hpp"
#include "UIRenderer.hpp"
#include "TextConfig.hpp"
#include "TextWidget.hpp"
#include <memory>
#include <string>
#include <vector>

namespace ui
{
struct ApplicationOptions
{
#ifdef __APPLE__
    GpuBackend Backend{GpuBackend::Metal};
#else
    GpuBackend Backend{GpuBackend::Vulkan};
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
    void initialize(ApplicationOptions Options = {});
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
    std::unique_ptr<TextWidget> createTextWidget(std::string_view Text, float Size, float MaxWidth = 0, Color Tint = {1, 1, 1, 1});
    template<class Character, class Traits, class Allocator>
    std::unique_ptr<TextWidget> createTextWidget(const std::basic_string<Character, Traits, Allocator>& Text, float Size, float MaxWidth = 0, Color Tint = {1, 1, 1, 1})
    {
        return createTextWidget(getTextBytes(Text), Size, MaxWidth, Tint);
    }
    int32_t run();

private:
    GenericApplication() = default;
    ~GenericApplication();
    void checkInitialized() const;
    ApplicationOptions Options;
    std::unique_ptr<IGenericPlatform> Platform;
    std::shared_ptr<UIRenderer> Renderer;
    std::unique_ptr<Window> MainWindow;
};
}
