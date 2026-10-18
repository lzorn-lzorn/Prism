#include <infra/common/Error.hpp>
#include "PaintContext.hpp"
#include "TextLayout.hpp"

#include <mapbox/earcut.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace ui
{
namespace
{
void checkPoint(UIPoint Point)
{
    if (!std::isfinite(Point.X) || !std::isfinite(Point.Y))
    {
        Error::raise(EErrorCode::InvalidArgument, "Point must be finite");
    }
}

void checkRect(UIRect Rect)
{
    checkPoint({Rect.X, Rect.Y});
    if (!std::isfinite(Rect.Width) || !std::isfinite(Rect.Height)
        || Rect.Width < 0 || Rect.Height < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Rectangle must have finite nonnegative extents");
    }
    checkPoint({Rect.X + Rect.Width, Rect.Y + Rect.Height});
}

void checkColor(Color Tint)
{
    if (!std::isfinite(Tint.R) || !std::isfinite(Tint.G)
        || !std::isfinite(Tint.B) || !std::isfinite(Tint.A))
    {
        Error::raise(EErrorCode::InvalidArgument, "Color must be finite");
    }
}

void checkThickness(float Thickness)
{
    if (!std::isfinite(Thickness) || Thickness <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Thickness must be positive and finite");
    }
}

float clampRadius(UIRect Rect, float Radius)
{
    checkRect(Rect);
    if (!std::isfinite(Radius) || Radius < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Radius must be finite and nonnegative");
    }
    return std::min(Radius, std::min(Rect.Width, Rect.Height) * 0.5f);
}

bool isClipEqual(const std::optional<UIRect>& Left, const std::optional<UIRect>& Right)
{
    if (Left.has_value() != Right.has_value())
    {
        return false;
    }
    return !Left || (Left->X == Right->X && Left->Y == Right->Y
        && Left->Width == Right->Width && Left->Height == Right->Height);
}

std::vector<UIPoint> roundedPoints(UIRect Rect, float Radius)
{
    const std::array<UIPoint, 4> Centers{{
        {Rect.X + Rect.Width - Radius, Rect.Y + Radius},
        {Rect.X + Rect.Width - Radius, Rect.Y + Rect.Height - Radius},
        {Rect.X + Radius, Rect.Y + Rect.Height - Radius},
        {Rect.X + Radius, Rect.Y + Radius}}};
    std::vector<UIPoint> Points;
    Points.reserve(36);
    for (std::size_t Corner = 0; Corner < Centers.size(); ++Corner)
    {
        for (int Segment = 0; Segment <= 8; ++Segment)
        {
            const double Angle = (static_cast<double>(Corner) - 1.0) * std::numbers::pi / 2.0
                + static_cast<double>(Segment) * std::numbers::pi / 16.0;
            Points.push_back({Centers[Corner].X + Radius * static_cast<float>(std::cos(Angle)),
                Centers[Corner].Y + Radius * static_cast<float>(std::sin(Angle))});
        }
    }
    return Points;
}

std::vector<UIVertex> makeVertices(std::span<const UIPoint> Points, Color Tint)
{
    std::vector<UIVertex> Vertices;
    Vertices.reserve(Points.size());
    for (const auto Point : Points)
    {
        Vertices.push_back({Point, {}, Tint});
    }
    return Vertices;
}

std::vector<std::uint32_t> ringIndices(std::size_t Count)
{
    std::vector<std::uint32_t> Indices;
    Indices.reserve(Count * 6);
    for (std::uint32_t Index = 0; Index < Count; ++Index)
    {
        const auto Next = static_cast<std::uint32_t>((Index + 1) % Count);
        const auto Inner = static_cast<std::uint32_t>(Count);
        Indices.insert(Indices.end(), {Index, Next, Inner + Next, Index, Inner + Next, Inner + Index});
    }
    return Indices;
}

void appendStrokeSegment(std::vector<UIVertex>& Vertices, std::vector<std::uint32_t>& Indices,
    UIPoint Start, UIPoint End, Color Tint, float Thickness)
{
    const double DeltaX = static_cast<double>(End.X) - Start.X;
    const double DeltaY = static_cast<double>(End.Y) - Start.Y;
    const double Length = std::hypot(DeltaX, DeltaY);
    if (Length == 0)
    {
        return;
    }
    const float OffsetX = static_cast<float>(-DeltaY / Length * Thickness * 0.5);
    const float OffsetY = static_cast<float>(DeltaX / Length * Thickness * 0.5);
    const auto Base = static_cast<std::uint32_t>(Vertices.size());
    const std::array<UIPoint, 4> Quad{{
        {Start.X + OffsetX, Start.Y + OffsetY}, {End.X + OffsetX, End.Y + OffsetY},
        {End.X - OffsetX, End.Y - OffsetY}, {Start.X - OffsetX, Start.Y - OffsetY}}};
    const auto SegmentVertices = makeVertices(Quad, Tint);
    Vertices.insert(Vertices.end(), SegmentVertices.begin(), SegmentVertices.end());
    Indices.insert(Indices.end(), {Base, Base + 1, Base + 2, Base, Base + 2, Base + 3});
}
}

PaintContext::PaintContext(UIDrawList& List, IUIRenderer* InRenderer)
    : DrawList(List), Renderer(InRenderer)
{
    DrawList.validate();
}

IUIRenderer* PaintContext::getRenderer() const noexcept
{
    return Renderer;
}

void PaintContext::appendMesh(std::span<const UIVertex> Vertices,
    std::span<const std::uint32_t> Indices, TextureHandle Texture)
{
    const auto Limit = std::numeric_limits<std::uint32_t>::max();
    if (DrawList.Vertices.size() > Limit || DrawList.Indices.size() > Limit
        || Vertices.size() > Limit - DrawList.Vertices.size()
        || Indices.size() > Limit - DrawList.Indices.size() || Indices.size() % 3 != 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Mesh exceeds triangle or addressing limits");
    }
    for (const auto Index : Indices)
    {
        if (Index >= Vertices.size())
        {
            Error::raise(EErrorCode::InvalidArgument, "Mesh index is out of bounds");
        }
    }
    std::vector<UIVertex> Transformed;
    Transformed.reserve(Vertices.size());
    for (auto Vertex : Vertices)
    {
        checkPoint(Vertex.Position);
        checkPoint(Vertex.UV);
        checkColor(Vertex.Tint);
        Vertex.Position = Transforms.back().transformPoint(Vertex.Position);
        Vertex.Tint.A *= Opacities.back();
        checkPoint(Vertex.Position);
        checkColor(Vertex.Tint);
        Transformed.push_back(Vertex);
    }
    if (Indices.empty())
    {
        return;
    }
    const auto Base = static_cast<std::uint32_t>(DrawList.Vertices.size());
    const auto First = static_cast<std::uint32_t>(DrawList.Indices.size());
    const auto Count = static_cast<std::uint32_t>(Indices.size());
    const bool Merge = !DrawList.Batches.empty() && DrawList.Batches.back().Texture == Texture
        && isClipEqual(DrawList.Batches.back().Clip, Clips.back())
        && static_cast<std::size_t>(DrawList.Batches.back().FirstIndex)
            + DrawList.Batches.back().IndexCount == First;
    DrawList.Vertices.reserve(DrawList.Vertices.size() + Vertices.size());
    DrawList.Indices.reserve(DrawList.Indices.size() + Indices.size());
    if (!Merge)
    {
        DrawList.Batches.reserve(DrawList.Batches.size() + 1);
    }
    DrawList.Vertices.insert(DrawList.Vertices.end(), Transformed.begin(), Transformed.end());
    for (const auto Index : Indices)
    {
        DrawList.Indices.push_back(Base + Index);
    }
    if (Merge)
    {
        DrawList.Batches.back().IndexCount += Count;
    }
    else
    {
        DrawList.Batches.push_back({First, Count, Texture, Clips.back()});
    }
}

void PaintContext::fillRect(UIRect Rect, Color Tint)
{
    drawImage({}, Rect, {0, 0, 1, 1}, Tint);
}

void PaintContext::drawLine(UIPoint Start, UIPoint End, Color Tint, float Thickness)
{
    const std::array Points{Start, End};
    strokePath(Points, Tint, Thickness);
}

void PaintContext::drawRect(UIRect Rect, Color Tint, float Thickness)
{
    strokeRect(Rect, Tint, Thickness);
}

void PaintContext::drawTriangle(UIPoint First, UIPoint Second, UIPoint Third, Color Tint, float Thickness)
{
    const std::array Points{First, Second, Third};
    strokePath(Points, Tint, Thickness, true);
}

void PaintContext::drawCircle(UIPoint Center, float Radius, Color Tint, float Thickness)
{
    checkPoint(Center);
    checkColor(Tint);
    checkThickness(Thickness);
    if (!std::isfinite(Radius) || Radius < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Radius must be finite and nonnegative");
    }
    if (Radius == 0)
    {
        return;
    }
    constexpr std::uint32_t SegmentCount = 64;
    std::vector<UIVertex> Vertices;
    Vertices.reserve(SegmentCount * 2);
    const float InnerRadius = std::max(0.0f, Radius - Thickness);
    for (const float RingRadius : {Radius, InnerRadius})
    {
        for (std::uint32_t Segment = 0; Segment < SegmentCount; ++Segment)
        {
            const double Angle = 2 * std::numbers::pi * Segment / SegmentCount;
            Vertices.push_back({{Center.X + RingRadius * static_cast<float>(std::cos(Angle)),
                Center.Y + RingRadius * static_cast<float>(std::sin(Angle))}, {}, Tint});
        }
    }
    appendMesh(Vertices, ringIndices(SegmentCount));
}

void PaintContext::drawPoint(UIPoint Position, Color Tint, float Thickness)
{
    checkThickness(Thickness);
    drawCircle(Position, Thickness * 0.5f, Tint, Thickness);
}

void PaintContext::drawImage(TextureHandle Texture, UIRect Destination, UIRect SourceUV, Color Tint)
{
    checkRect(Destination);
    checkRect(SourceUV);
    checkColor(Tint);
    if (Destination.Width == 0 || Destination.Height == 0)
    {
        return;
    }
    const float Right = Destination.X + Destination.Width;
    const float Bottom = Destination.Y + Destination.Height;
    const float UVRight = SourceUV.X + SourceUV.Width;
    const float UVBottom = SourceUV.Y + SourceUV.Height;
    const std::array<UIVertex, 4> Vertices{{
        {{Destination.X, Destination.Y}, {SourceUV.X, SourceUV.Y}, Tint},
        {{Right, Destination.Y}, {UVRight, SourceUV.Y}, Tint},
        {{Right, Bottom}, {UVRight, UVBottom}, Tint},
        {{Destination.X, Bottom}, {SourceUV.X, UVBottom}, Tint}}};
    const std::array<std::uint32_t, 6> Indices{0, 1, 2, 0, 2, 3};
    appendMesh(Vertices, Indices, Texture);
}

void PaintContext::strokeRect(UIRect Rect, Color Tint, float Thickness)
{
    checkRect(Rect);
    checkColor(Tint);
    checkThickness(Thickness);
    if (Rect.Width == 0 || Rect.Height == 0)
    {
        return;
    }
    if (Thickness >= std::min(Rect.Width, Rect.Height) * 0.5f)
    {
        fillRect(Rect, Tint);
        return;
    }
    const float Right = Rect.X + Rect.Width;
    const float Bottom = Rect.Y + Rect.Height;
    const std::array<UIPoint, 8> Points{{
        {Rect.X, Rect.Y}, {Right, Rect.Y}, {Right, Bottom}, {Rect.X, Bottom},
        {Rect.X + Thickness, Rect.Y + Thickness}, {Right - Thickness, Rect.Y + Thickness},
        {Right - Thickness, Bottom - Thickness}, {Rect.X + Thickness, Bottom - Thickness}}};
    appendMesh(makeVertices(Points, Tint), ringIndices(4));
}

void PaintContext::fillRoundedRect(UIRect Rect, float Radius, Color Tint)
{
    Radius = clampRadius(Rect, Radius);
    checkColor(Tint);
    if (Radius == 0)
    {
        fillRect(Rect, Tint);
        return;
    }
    const auto Points = roundedPoints(Rect, Radius);
    auto Vertices = makeVertices(Points, Tint);
    const auto Center = static_cast<std::uint32_t>(Vertices.size());
    Vertices.push_back({{Rect.X + Rect.Width * 0.5f, Rect.Y + Rect.Height * 0.5f}, {}, Tint});
    std::vector<std::uint32_t> Indices;
    for (std::uint32_t Index = 0; Index < Points.size(); ++Index)
    {
        Indices.insert(Indices.end(), {Center, Index, static_cast<std::uint32_t>((Index + 1) % Points.size())});
    }
    appendMesh(Vertices, Indices);
}

void PaintContext::strokeRoundedRect(UIRect Rect, float Radius, Color Tint, float Thickness)
{
    Radius = clampRadius(Rect, Radius);
    checkColor(Tint);
    checkThickness(Thickness);
    if (Rect.Width == 0 || Rect.Height == 0)
    {
        return;
    }
    if (Thickness >= std::min(Rect.Width, Rect.Height) * 0.5f)
    {
        fillRoundedRect(Rect, Radius, Tint);
        return;
    }
    if (Radius == 0)
    {
        strokeRect(Rect, Tint, Thickness);
        return;
    }
    const UIRect Inner{Rect.X + Thickness, Rect.Y + Thickness,
        Rect.Width - 2 * Thickness, Rect.Height - 2 * Thickness};
    const auto OuterPoints = roundedPoints(Rect, Radius);
    const auto InnerPoints = roundedPoints(Inner, std::max(Radius - Thickness, 0.0f));
    auto Vertices = makeVertices(OuterPoints, Tint);
    const auto InnerVertices = makeVertices(InnerPoints, Tint);
    Vertices.insert(Vertices.end(), InnerVertices.begin(), InnerVertices.end());
    appendMesh(Vertices, ringIndices(OuterPoints.size()));
}

void PaintContext::fillPath(std::span<const UIPoint> Points, Color Tint)
{
    checkColor(Tint);
    if (Points.size() < 3 || Points.size() > std::numeric_limits<std::uint32_t>::max())
    {
        Error::raise(EErrorCode::InvalidArgument, "Filled path needs at least three addressable points");
    }
    std::vector<std::vector<std::array<double, 2>>> Polygon(1);
    Polygon.front().reserve(Points.size());
    for (const auto Point : Points)
    {
        checkPoint(Point);
        Polygon.front().push_back({Point.X, Point.Y});
    }
    const auto Indices = mapbox::earcut<std::uint32_t>(Polygon);
    appendMesh(makeVertices(Points, Tint), Indices);
}

void PaintContext::strokePath(std::span<const UIPoint> Points, Color Tint, float Thickness, bool Closed)
{
    checkThickness(Thickness);
    checkColor(Tint);
    if (Points.size() < (Closed ? 3u : 2u)
        || Points.size() > std::numeric_limits<std::uint32_t>::max() / 6)
    {
        Error::raise(EErrorCode::InvalidArgument, "Stroke path has an invalid point count");
    }
    for (const auto Point : Points)
    {
        checkPoint(Point);
    }
    std::vector<UIVertex> Vertices;
    std::vector<std::uint32_t> Indices;
    const auto SegmentCount = Closed ? Points.size() : Points.size() - 1;
    for (std::size_t Segment = 0; Segment < SegmentCount; ++Segment)
    {
        appendStrokeSegment(Vertices, Indices, Points[Segment], Points[(Segment + 1) % Points.size()], Tint, Thickness);
    }
    appendMesh(Vertices, Indices);
}

void PaintContext::drawDashedLine(UIPoint Start, UIPoint End, Color Tint, float Thickness,
    float DashLength, float GapLength)
{
    const std::array Points{Start, End};
    strokeDashedPath(Points, Tint, Thickness, DashLength, GapLength, false);
}

void PaintContext::strokeDashedRect(UIRect Rect, Color Tint, float Thickness, float DashLength, float GapLength)
{
    checkRect(Rect);
    if (Rect.Width == 0 || Rect.Height == 0)
    {
        return;
    }
    const float Right = Rect.X + Rect.Width;
    const float Bottom = Rect.Y + Rect.Height;
    const std::array<UIPoint, 4> Points{{
        {Rect.X, Rect.Y}, {Right, Rect.Y}, {Right, Bottom}, {Rect.X, Bottom}}};
    strokeDashedPath(Points, Tint, Thickness, DashLength, GapLength, true);
}

void PaintContext::drawDashedCircle(UIPoint Center, float Radius, Color Tint, float Thickness,
    float DashLength, float GapLength)
{
    checkPoint(Center);
    checkColor(Tint);
    checkThickness(Thickness);
    if (!std::isfinite(Radius) || Radius <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Radius must be positive and finite");
    }
    constexpr std::uint32_t SegmentCount = 64;
    std::vector<UIPoint> Points;
    Points.reserve(SegmentCount);
    for (std::uint32_t Segment = 0; Segment < SegmentCount; ++Segment)
    {
        const double Angle = 2 * std::numbers::pi * Segment / SegmentCount;
        Points.push_back({Center.X + Radius * static_cast<float>(std::cos(Angle)),
                          Center.Y + Radius * static_cast<float>(std::sin(Angle))});
    }
    strokeDashedPath(Points, Tint, Thickness, DashLength, GapLength, true);
}

void PaintContext::strokeDashedPath(std::span<const UIPoint> Points, Color Tint, float Thickness,
    float DashLength, float GapLength, bool Closed)
{
    checkThickness(Thickness);
    checkColor(Tint);
    if (!std::isfinite(DashLength) || DashLength <= 0 || !std::isfinite(GapLength) || GapLength <= 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Dash and gap lengths must be positive and finite");
    }
    if (Points.size() < (Closed ? 3u : 2u)
        || Points.size() > std::numeric_limits<std::uint32_t>::max() / 6)
    {
        Error::raise(EErrorCode::InvalidArgument, "Dashed path has an invalid point count");
    }
    for (const auto Point : Points)
    {
        checkPoint(Point);
    }
    std::vector<UIVertex> Vertices;
    std::vector<std::uint32_t> Indices;
    const auto SegmentCount = Closed ? Points.size() : Points.size() - 1;
    for (std::size_t Segment = 0; Segment < SegmentCount; ++Segment)
    {
        const auto Start = Points[Segment];
        const auto End = Points[(Segment + 1) % Points.size()];
        const double DeltaX = static_cast<double>(End.X) - Start.X;
        const double DeltaY = static_cast<double>(End.Y) - Start.Y;
        const double Length = std::hypot(DeltaX, DeltaY);
        if (Length == 0)
        {
            continue;
        }
        const float DirX = static_cast<float>(DeltaX / Length);
        const float DirY = static_cast<float>(DeltaY / Length);
        const float Total = static_cast<float>(Length);
        for (float Position = 0; Position < Total; Position += DashLength + GapLength)
        {
            const float DashEnd = std::min(Total, Position + DashLength);
            if (DashEnd > Position)
            {
                appendStrokeSegment(Vertices, Indices,
                    {Start.X + DirX * Position, Start.Y + DirY * Position},
                    {Start.X + DirX * DashEnd, Start.Y + DirY * DashEnd}, Tint, Thickness);
            }
        }
    }
    appendMesh(Vertices, Indices);
}

void PaintContext::pushClipRect(UIRect Rect, bool IgnoreParentClip)
{
    checkRect(Rect);
    const auto& Transform = Transforms.back();
    // 变换矩形四角并取轴对齐包围盒 (AABB), 以支持旋转变换下的保守裁剪.
    const std::array<UIPoint, 4> Corners{{
        {Rect.X, Rect.Y},
        {Rect.X + Rect.Width, Rect.Y},
        {Rect.X + Rect.Width, Rect.Y + Rect.Height},
        {Rect.X, Rect.Y + Rect.Height}}};
    float MinX = std::numeric_limits<float>::infinity();
    float MinY = std::numeric_limits<float>::infinity();
    float MaxX = -std::numeric_limits<float>::infinity();
    float MaxY = -std::numeric_limits<float>::infinity();
    for (const auto& Corner : Corners)
    {
        const auto Mapped = Transform.transformPoint(Corner);
        checkPoint(Mapped);
        MinX = std::min(MinX, Mapped.X);
        MaxX = std::max(MaxX, Mapped.X);
        MinY = std::min(MinY, Mapped.Y);
        MaxY = std::max(MaxY, Mapped.Y);
    }
    Rect = {MinX, MinY, MaxX - MinX, MaxY - MinY};
    checkRect(Rect);
    if (!IgnoreParentClip && Clips.back())
    {
        const auto Parent = *Clips.back();
        const float Left = std::max(Rect.X, Parent.X);
        const float Top = std::max(Rect.Y, Parent.Y);
        const float Right = std::min(Rect.X + Rect.Width, Parent.X + Parent.Width);
        const float Bottom = std::min(Rect.Y + Rect.Height, Parent.Y + Parent.Height);
        Rect = {Left, Top, std::max(0.0f, Right - Left), std::max(0.0f, Bottom - Top)};
        checkRect(Rect);
    }
    Clips.push_back(Rect);
}

void PaintContext::popClip()
{
    if (Clips.size() == 1)
    {
        Error::raise(EErrorCode::Logic, "Clip stack underflow");
    }
    Clips.pop_back();
}

void PaintContext::pushTransform(const UITransform& Transform)
{
    const std::array<float, 6> Components{Transform.XX, Transform.XY, Transform.YX,
        Transform.YY, Transform.TX, Transform.TY};
    for (const auto Component : Components)
    {
        if (!std::isfinite(Component))
        {
            Error::raise(EErrorCode::InvalidArgument, "Transform must be finite");
        }
    }
    const auto Composed = Transforms.back().compose(Transform);
    const std::array<float, 6> Result{Composed.XX, Composed.XY, Composed.YX,
        Composed.YY, Composed.TX, Composed.TY};
    for (const auto Component : Result)
    {
        if (!std::isfinite(Component))
        {
            Error::raise(EErrorCode::InvalidArgument, "Composed transform must be finite");
        }
    }
    Transforms.push_back(Composed);
}

void PaintContext::popTransform()
{
    if (Transforms.size() == 1)
    {
        Error::raise(EErrorCode::Logic, "Transform stack underflow");
    }
    Transforms.pop_back();
}

void PaintContext::pushOpacity(float Opacity)
{
    if (!std::isfinite(Opacity) || Opacity < 0 || Opacity > 1)
    {
        Error::raise(EErrorCode::InvalidArgument, "Opacity must be finite and within [0, 1]");
    }
    Opacities.push_back(Opacities.back() * Opacity);
}

void PaintContext::popOpacity()
{
    if (Opacities.size() == 1)
    {
        Error::raise(EErrorCode::Logic, "Opacity stack underflow");
    }
    Opacities.pop_back();
}

void PaintContext::drawTextLayout(const TextLayout& IVisual, UIPoint Origin, Color Tint)
{
    checkPoint(Origin);
    checkColor(Tint);
    for (const auto& Glyph : IVisual.Glyphs)
    {
        auto Bounds = Glyph.Bounds;
        Bounds.X += Origin.X;
        Bounds.Y += Origin.Y;
        drawImage(Glyph.Texture, Bounds, Glyph.UV, Glyph.Colored ? Color{1, 1, 1, Tint.A} : Tint);
    }
}
}