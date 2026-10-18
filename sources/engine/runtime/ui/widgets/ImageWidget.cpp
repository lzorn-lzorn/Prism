#include "ImageWidget.hpp"
#include "IUIRenderer.hpp"
#include <cmath>
#include <utility>

namespace ui
{
ImageWidget::ImageWidget(std::shared_ptr<Image> InImage)
    : ImageValue(std::move(InImage))
{
}

void ImageWidget::setImage(std::shared_ptr<Image> Image)
{
    ImageValue = std::move(Image);
    Texture = {};
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::Paint);
}

std::shared_ptr<Image> ImageWidget::getImage() const noexcept
{
    return ImageValue;
}

void ImageWidget::setSize(Vector2 Size) noexcept
{
    ExplicitSize = Size;
    invalidate(EInvalidationReason::IVisual);
}

void ImageWidget::clearSize() noexcept
{
    ExplicitSize.reset();
    invalidate(EInvalidationReason::IVisual);
}

Vector2 ImageWidget::getSize() const noexcept
{
    if (ExplicitSize)
    {
        return *ExplicitSize;
    }
    if (!ImageValue)
    {
        return Vector2{};
    }
    return Vector2{static_cast<float>(ImageValue->getWidth()),
                   static_cast<float>(ImageValue->getHeight())};
}

void ImageWidget::setPosition(Vector2 Value) noexcept
{
    Position = Value;
    invalidate(EInvalidationReason::Paint);
}

Vector2 ImageWidget::getPosition() const noexcept
{
    return Position;
}

void ImageWidget::setFillMode(EImageFillMode Mode) noexcept
{
    FillMode = Mode;
    invalidate(EInvalidationReason::Paint);
}

EImageFillMode ImageWidget::getFillMode() const noexcept
{
    return FillMode;
}

void ImageWidget::setSourceUV(UIRect UV) noexcept
{
    SourceUV = UV;
    invalidate(EInvalidationReason::Paint);
}

UIRect ImageWidget::getSourceUV() const noexcept
{
    return SourceUV;
}

void ImageWidget::setClipToBounds(bool Value) noexcept
{
    ClipToBounds = Value;
    invalidate(EInvalidationReason::Paint);
}

bool ImageWidget::isClipToBounds() const noexcept
{
    return ClipToBounds;
}

Vector2 ImageWidget::onMeasure(Vector2) const
{
    return getSize();
}

bool ImageWidget::shouldClipSelf() const noexcept
{
    return ClipToBounds;
}

void ImageWidget::onPaint(PaintContext& Context) const
{
    if (!ImageValue || ImageValue->isEmpty())
    {
        return;
    }
    if (Texture.Value == 0)
    {
        if (IUIRenderer* Renderer = Context.getRenderer())
        {
            Texture = Renderer->createTexture(ImageValue->toTextureData());
        }
    }
    const auto Bounds = getBounds();
    if (Texture.Value == 0)
    {
        Context.fillRect({0, 0, Bounds.Width, Bounds.Height}, {0.55f, 0.55f, 0.55f, 1});
        return;
    }

    const float ImageW = static_cast<float>(ImageValue->getWidth());
    const float ImageH = static_cast<float>(ImageValue->getHeight());

    switch (FillMode)
    {
    case EImageFillMode::Stretch:
        Context.drawImage(Texture, {Position.x(), Position.y(), Bounds.Width, Bounds.Height}, SourceUV, {1, 1, 1, 1});
        break;

    case EImageFillMode::Center:
    {
        const float W = ImageW * SourceUV.Width;
        const float H = ImageH * SourceUV.Height;
        const float X = Position.x() + (Bounds.Width - W) * 0.5f;
        const float Y = Position.y() + (Bounds.Height - H) * 0.5f;
        Context.drawImage(Texture, {X, Y, W, H}, SourceUV, {1, 1, 1, 1});
        break;
    }

    case EImageFillMode::Repeat:
    {
        const float TileW = ImageW * SourceUV.Width;
        const float TileH = ImageH * SourceUV.Height;
        if (TileW <= 0 || TileH <= 0)
        {
            break;
        }
        const int Columns = static_cast<int>(std::ceil(Bounds.Width / TileW));
        const int Rows = static_cast<int>(std::ceil(Bounds.Height / TileH));
        for (int Row = 0; Row < Rows; ++Row)
        {
            for (int Column = 0; Column < Columns; ++Column)
            {
                const float X = Position.x() + Column * TileW;
                const float Y = Position.y() + Row * TileH;
                Context.drawImage(Texture, {X, Y, TileW, TileH}, SourceUV, {1, 1, 1, 1});
            }
        }
        break;
    }
    }
}

}
