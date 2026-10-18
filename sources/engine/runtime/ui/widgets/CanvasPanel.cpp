#include <infra/common/Error.hpp>
#include "CanvasPanel.hpp"
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ui
{
CanvasSlot::CanvasSlot(Widget& Parent, std::unique_ptr<Widget> Child)
    : PanelSlot(Parent, std::move(Child))
{
}

Layout CanvasSlot::layout() const noexcept
{
    return getLayout();
}

void CanvasSlot::setAnchors(const Anchors& Value)
{
    auto IVisual = layout();
    IVisual.Anchor = Value;
    setLayout(IVisual);
}

void CanvasSlot::setAnchors(Vector2 Min, Vector2 Max)
{
    setAnchors(Anchors{Min, Max});
}

Anchors CanvasSlot::getAnchors() const noexcept
{
    return layout().Anchor;
}

void CanvasSlot::setPosition(Vector2 Value)
{
    auto IVisual = layout();
    IVisual.Position = Value;
    setLayout(IVisual);
}

Vector2 CanvasSlot::getPosition() const noexcept
{
    return layout().Position;
}

void CanvasSlot::setSize(Vector2 Value)
{
    auto IVisual = layout();
    IVisual.Size = Value;
    setLayout(IVisual);
}

Vector2 CanvasSlot::getSize() const noexcept
{
    return layout().Size;
}

void CanvasSlot::setAlignment(Vector2 Value)
{
    auto IVisual = layout();
    IVisual.Pivot = Value;
    setLayout(IVisual);
}

Vector2 CanvasSlot::getAlignment() const noexcept
{
    return layout().Pivot;
}

void CanvasSlot::setOffsets(Vector2 StartMargin, Vector2 EndMargin)
{
    auto IVisual = layout();
    IVisual.StartMargin = StartMargin;
    IVisual.EndMargin = EndMargin;
    setLayout(IVisual);
}

void CanvasSlot::setRotation(float Value)
{
    auto IVisual = layout();
    IVisual.Rotation = Value;
    setLayout(IVisual);
}

float CanvasSlot::getRotation() const noexcept
{
    return layout().Rotation;
}

void CanvasSlot::setZOrder(int Value) noexcept
{
    ZOrder = Value;
    getParent()->invalidate(EInvalidationReason::ChildOrder);
}

int CanvasSlot::getZOrder() const noexcept
{
    return ZOrder;
}

std::unique_ptr<PanelSlot> CanvasPanel::createSlot(std::unique_ptr<Widget> Child)
{
    return std::make_unique<CanvasSlot>(*this, std::move(Child));
}

CanvasSlot& CanvasPanel::addChildToCanvas(std::unique_ptr<Widget> Child)
{
    // addSlot 经由 createSlot 创建 CanvasSlot，这里安全地下转。
    return static_cast<CanvasSlot&>(addSlot(std::move(Child)));
}

#ifndef NDEBUG
void CanvasPanel::setEnablePaintBorder(bool Value) noexcept
{
    EnablePaintBorder = Value;
    invalidate(EInvalidationReason::Paint);
}

bool CanvasPanel::isEnablePaintBorder() const noexcept
{
    return EnablePaintBorder;
}

void CanvasPanel::setEnablePaintDashedBorder(bool Value) noexcept
{
    EnablePaintDashedBorder = Value;
    invalidate(EInvalidationReason::Paint);
}

bool CanvasPanel::isEnablePaintDashedBorder() const noexcept
{
    return EnablePaintDashedBorder;
}

void CanvasPanel::setBorderColor(Color Value)
{
    if (!std::isfinite(Value.R) || !std::isfinite(Value.G) ||
        !std::isfinite(Value.B) || !std::isfinite(Value.A))
    {
        Error::raise(EErrorCode::InvalidArgument, "Border color must be finite");
    }
    BorderColor = Value;
    invalidate(EInvalidationReason::Paint);
}

Color CanvasPanel::getBorderColor() const noexcept
{
    return BorderColor;
}

void CanvasPanel::setBorderThickness(float Value)
{
    if (!std::isfinite(Value) || Value <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Border thickness must be positive and finite");
    }
    BorderThickness = Value;
    invalidate(EInvalidationReason::Paint);
}

float CanvasPanel::getBorderThickness() const noexcept
{
    return BorderThickness;
}
#endif

void CanvasPanel::onPaint(PaintContext& Context) const
{
#ifndef NDEBUG
    const auto Bounds = getBounds();
    if (EnablePaintBorder)
    {
        Context.strokeRect({0, 0, Bounds.Width, Bounds.Height}, BorderColor, BorderThickness);
    }
    else if (EnablePaintDashedBorder)
    {
        Context.strokeDashedRect({0, 0, Bounds.Width, Bounds.Height}, BorderColor, BorderThickness);
    }
#else
    (void)Context;
#endif
}
}
