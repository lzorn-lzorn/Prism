#pragma once

#include "FontHandle.hpp"
#include "TextLayout.hpp"
#include "TextConfig.hpp"
#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ui
{
class PaintContext;

// TextLayoutEngine: 文字排版引擎, 由 FontService 统一创建, 统筹字体/字号/排版/测量/绘制.
// 描述变化时惰性重建内部 TextLayout; 文本按 Config 解码, 可适配不同编码.
class TextLayoutEngine
{
public:
    TextLayoutEngine(const TextLayoutEngine&) = delete;
    TextLayoutEngine& operator=(const TextLayoutEngine&) = delete;

    // 运行时描述 (变化后惰性重建布局)
    void setText(std::string_view Value);
    // std::format 风格重载: 编译期检查格式串.
    template<class... Args>
    void setText(std::format_string<Args...> Format, Args&&... Arguments)
    {
        setText(std::format(Format, std::forward<Args>(Arguments)...));
    }
    void setFont(FontHandle Value);
    void setFallbackFonts(std::vector<FontHandle> Value);
    void setFontSize(float Value);
    void setPixelScale(float Value);
    void setMaxWidth(float Value);
    void setConfig(const TextConfig& Value);
    // 运行时显示属性 (不影响排版)
    void setColor(Color Value);
    void setEnableOuterRender(bool Value);

    const std::string& getText() const noexcept;
    FontHandle getFont() const noexcept;
    float getFontSize() const noexcept;
    float getPixelScale() const noexcept;
    float getMaxWidth() const noexcept;
    const TextConfig& getConfig() const noexcept;
    Color getColor() const noexcept;
    bool isEnableOuterRender() const noexcept;

    // 测量/排布/绘制 (首次或描述变化后惰性重建)
    Vector2 measure(Vector2 AvailableSize) const;
    void arrange(UIRect LocalContent) const;
    void paint(PaintContext& Context) const;

private:
    friend class FontService;
    TextLayoutEngine(const TextConfig& Config, FontHandle Font, std::vector<FontHandle> Fallbacks);
    void rebuildIfNeeded() const;

    TextConfig Config;
    FontHandle Font{};
    std::vector<FontHandle> Fallbacks;
    std::string Text;
    float FontSize{16};
    float PixelScale{1};
    float MaxWidth{};
    Color Tint{1, 1, 1, 1};
    bool EnableOuterRender{};
    mutable bool Dirty{true};
    mutable TextLayout IVisual;
};
}
