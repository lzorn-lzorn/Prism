#include <infra/common/Error.hpp>
#include "TextLayoutEngine.hpp"
#include "FontService.hpp"
#include "PaintContext.hpp"
#include "Unicode.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ui
{
TextLayoutEngine::TextLayoutEngine(const TextConfig& Config, FontHandle Font, std::vector<FontHandle> Fallbacks)
    : Config(Config), Font(Font), Fallbacks(std::move(Fallbacks))
{
}

void TextLayoutEngine::setText(std::string_view Value)
{
    Text.assign(Value);
    Dirty = true;
}

void TextLayoutEngine::setFont(FontHandle Value)
{
    Font = Value;
    Dirty = true;
}

void TextLayoutEngine::setFallbackFonts(std::vector<FontHandle> Value)
{
    Fallbacks = std::move(Value);
    Dirty = true;
}

void TextLayoutEngine::setFontSize(float Value)
{
    if (!std::isfinite(Value) || Value <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Font size must be finite and positive");
    }
    FontSize = Value;
    Dirty = true;
}

void TextLayoutEngine::setPixelScale(float Value)
{
    if (!std::isfinite(Value) || Value <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Pixel scale must be finite and positive");
    }
    PixelScale = Value;
    Dirty = true;
}

void TextLayoutEngine::setMaxWidth(float Value)
{
    if (!std::isfinite(Value) || Value < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Max width must be finite and nonnegative");
    }
    MaxWidth = Value;
    Dirty = true;
}

void TextLayoutEngine::setConfig(const TextConfig& Value)
{
    Config = Value;
    Dirty = true;
}

void TextLayoutEngine::setColor(Color Value)
{
    if (!std::isfinite(Value.R) || !std::isfinite(Value.G) || !std::isfinite(Value.B) || !std::isfinite(Value.A))
    {
        Error::raise(EErrorCode::InvalidArgument, "Text color must be finite");
    }
    Tint = Value;
}

void TextLayoutEngine::setEnableOuterRender(bool Value)
{
    EnableOuterRender = Value;
}

const std::string& TextLayoutEngine::getText() const noexcept
{
    return Text;
}

FontHandle TextLayoutEngine::getFont() const noexcept
{
    return Font;
}

float TextLayoutEngine::getFontSize() const noexcept
{
    return FontSize;
}

float TextLayoutEngine::getPixelScale() const noexcept
{
    return PixelScale;
}

float TextLayoutEngine::getMaxWidth() const noexcept
{
    return MaxWidth;
}

const TextConfig& TextLayoutEngine::getConfig() const noexcept
{
    return Config;
}

Color TextLayoutEngine::getColor() const noexcept
{
    return Tint;
}

bool TextLayoutEngine::isEnableOuterRender() const noexcept
{
    return EnableOuterRender;
}

void TextLayoutEngine::rebuildIfNeeded() const
{
    if (!Dirty)
    {
        return;
    }
    const auto UTF32 = decodeTextToUTF32(Text, Config);
    FontService::self().setFallbackFonts(Font, Fallbacks);
    IVisual = FontService::self().createTextLayout(Font, UTF32, FontSize, PixelScale, MaxWidth);
    Dirty = false;
}

Vector2 TextLayoutEngine::measure(Vector2 AvailableSize) const
{
    rebuildIfNeeded();
    return IVisual.measure(AvailableSize);
}

void TextLayoutEngine::arrange(UIRect LocalContent) const
{
    rebuildIfNeeded();
    IVisual.arrange(LocalContent);
}

void TextLayoutEngine::paint(PaintContext& Context) const
{
    rebuildIfNeeded();
    Context.drawTextLayout(IVisual, {0, 0}, Tint);
}
}
