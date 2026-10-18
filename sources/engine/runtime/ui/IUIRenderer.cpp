#include "IUIRenderer.hpp"
#include "PaintContext.hpp"

namespace ui
{
void IUIRenderer::drawLine(UIPoint Start, UIPoint End, Color Tint, float Thickness)
{
    UIDrawList List;
    PaintContext(List).drawLine(Start, End, Tint, Thickness);
    submitDrawList(List);
}

void IUIRenderer::drawRect(UIRect Rect, Color Tint, float Thickness)
{
    UIDrawList List;
    PaintContext(List).drawRect(Rect, Tint, Thickness);
    submitDrawList(List);
}

void IUIRenderer::drawTriangle(UIPoint First, UIPoint Second, UIPoint Third, Color Tint, float Thickness)
{
    UIDrawList List;
    PaintContext(List).drawTriangle(First, Second, Third, Tint, Thickness);
    submitDrawList(List);
}

void IUIRenderer::drawCircle(UIPoint Center, float Radius, Color Tint, float Thickness)
{
    UIDrawList List;
    PaintContext(List).drawCircle(Center, Radius, Tint, Thickness);
    submitDrawList(List);
}

void IUIRenderer::drawPoint(UIPoint Position, Color Tint, float Thickness)
{
    UIDrawList List;
    PaintContext(List).drawPoint(Position, Tint, Thickness);
    submitDrawList(List);
}

void IUIRenderer::drawDashedLine(UIPoint Start, UIPoint End, Color Tint, float Thickness,
    float DashLength, float GapLength)
{
    UIDrawList List;
    PaintContext(List).drawDashedLine(Start, End, Tint, Thickness, DashLength, GapLength);
    submitDrawList(List);
}

void IUIRenderer::drawDashedRect(UIRect Rect, Color Tint, float Thickness,
    float DashLength, float GapLength)
{
    UIDrawList List;
    PaintContext(List).strokeDashedRect(Rect, Tint, Thickness, DashLength, GapLength);
    submitDrawList(List);
}

void IUIRenderer::drawDashedCircle(UIPoint Center, float Radius, Color Tint, float Thickness,
    float DashLength, float GapLength)
{
    UIDrawList List;
    PaintContext(List).drawDashedCircle(Center, Radius, Tint, Thickness, DashLength, GapLength);
    submitDrawList(List);
}
}