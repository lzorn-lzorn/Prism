#include "TextWidget.hpp"
#include "FontService.hpp"
#include <stdexcept>
#include <utility>

namespace ui
{
TextWidget::TextWidget()
    : Engine(FontService::self().createTextLayoutEngine())
{
}

void TextWidget::setupTextLayoutEngine(TextLayoutEngine&) const
{
    // 默认: 引擎已由 FontService 用全局默认配置/字体/回退初始化, 无需额外设置.
}

void TextWidget::ensureConfigured() const
{
    if (!IsConfigured)
    {
        setupTextLayoutEngine(*Engine);
        IsConfigured = true;
    }
}

void TextWidget::setText(std::string_view Text)
{
    Engine->setText(Text);
    invalidate(EInvalidationReason::IVisual);
}

void TextWidget::setFont(const std::string& Path)
{
    FontPath = Path;
    Engine->setFont(FontService::self().loadFont(Path));
    invalidate(EInvalidationReason::IVisual);
}

void TextWidget::setColor(Color Tint)
{
    Engine->setColor(Tint);
    invalidate(EInvalidationReason::Paint);
}

void TextWidget::setFontSize(float Size)
{
    Engine->setFontSize(Size);
    invalidate(EInvalidationReason::IVisual);
}

void TextWidget::setPixelScale(float Scale)
{
    Engine->setPixelScale(Scale);
    invalidate(EInvalidationReason::IVisual);
}

void TextWidget::setMaxWidth(float MaxWidth)
{
    Engine->setMaxWidth(MaxWidth);
    invalidate(EInvalidationReason::IVisual);
}

void TextWidget::setEnableTextOuter(bool IsOuterRender)
{
    Engine->setEnableOuterRender(IsOuterRender);
    invalidate(EInvalidationReason::Paint);
}

const std::string& TextWidget::getText() const noexcept
{
    return Engine->getText();
}

const std::string& TextWidget::getFontPath() const noexcept
{
    return FontPath;
}

Color TextWidget::getColor() const noexcept
{
    return Engine->getColor();
}

float TextWidget::getFontSize() const noexcept
{
    return Engine->getFontSize();
}

float TextWidget::getPixelScale() const noexcept
{
    return Engine->getPixelScale();
}

float TextWidget::getMaxWidth() const noexcept
{
    return Engine->getMaxWidth();
}

bool TextWidget::isEnableTextOuter() const noexcept
{
    return Engine->isEnableOuterRender();
}

Vector2 TextWidget::onMeasure(Vector2 AvailableSize) const
{
    ensureConfigured();
    return Engine->measure(AvailableSize);
}

void TextWidget::onArrange(UIRect LocalContent)
{
    ensureConfigured();
    Engine->arrange(LocalContent);
}

void TextWidget::onPaint(PaintContext& Context) const
{
    ensureConfigured();
    Engine->paint(Context);
}

bool TextWidget::shouldClipSelf() const noexcept
{
    return !Engine->isEnableOuterRender();
}
}
