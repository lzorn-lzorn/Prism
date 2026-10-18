#pragma once

#include "Widget.hpp"
#include "TextLayoutEngine.hpp"
#include "TextConfig.hpp"
#include <format>
#include <memory>
#include <string>
#include <print>
#include <string_view>
#include <utility>

namespace ui
{
/**
* @brief 文本控件: 只存描述信息(文字/字体/颜色/字号/自适配), 排版/测量/绘制交给内部 TextLayoutEngine(由 FontService 创建).
*/
class TextWidget : public LeafWidget
{
public:
    TextWidget();

    void setText(std::string_view Text);
    template<class Character, class Traits, class Allocator>
    void setText(const std::basic_string<Character, Traits, Allocator>& Text)
    {
        setText(getTextBytes(Text));
    }

    template<class... Args>
    void setText(std::format_string<Args...> Format, Args&&... Arguments)
    {
        setText(std::format(Format, std::forward<Args>(Arguments)...));
    }
    void setFont(const std::string& Path);
    void setColor(Color Tint);
    void setFontSize(float Size);
    void setPixelScale(float Scale);
    void setMaxWidth(float MaxWidth);

    /**
    * @param IsOuterRender false: 字体超出控件边界时被裁剪; true: 允许渲染出自身边界.
    */
    void setEnableTextOuter(bool IsOuterRender);

    const std::string& getText() const noexcept;
    const std::string& getFontPath() const noexcept;
    Color getColor() const noexcept;
    float getFontSize() const noexcept;
    float getPixelScale() const noexcept;
    float getMaxWidth() const noexcept;
    bool isEnableTextOuter() const noexcept;

protected:
    // @note: 直接配置内部排版引擎; 基类默认全局(创建时已设), 派生类覆盖以自定义.
    virtual void setupTextLayoutEngine(TextLayoutEngine& Engine) const;

    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void onArrange(UIRect LocalContent) override;
    void onPaint(PaintContext& Context) const override;
    bool shouldClipSelf() const noexcept override;

private:
    void ensureConfigured() const;

    std::unique_ptr<TextLayoutEngine> Engine;
    std::string FontPath;
    mutable bool IsConfigured{};
};
}
