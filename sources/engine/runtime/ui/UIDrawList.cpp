#include <infra/common/Error.hpp>
#include "UIDrawList.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace ui
{
UIPoint UITransform::transformPoint(UIPoint Point) const noexcept
{
    return {XX * Point.X + XY * Point.Y + TX, YX * Point.X + YY * Point.Y + TY};
}

UITransform UITransform::compose(const UITransform& Local) const noexcept
{
    return {
        XX * Local.XX + XY * Local.YX,
        XX * Local.XY + XY * Local.YY,
        YX * Local.XX + YY * Local.YX,
        YX * Local.XY + YY * Local.YY,
        XX * Local.TX + XY * Local.TY + TX,
        YX * Local.TX + YY * Local.TY + TY};
}

void UIDrawList::clear() noexcept
{
    Vertices.clear();
    Indices.clear();
    Batches.clear();
}

void UIDrawList::validate() const
{
    const auto Limit = std::numeric_limits<std::uint32_t>::max();
    if (Vertices.size() > Limit || Indices.size() > Limit)
    {
        Error::raise(EErrorCode::InvalidArgument, "Draw list exceeds 32-bit addressing limits");
    }
    for (const auto& Vertex : Vertices)
    {
        if (!std::isfinite(Vertex.Position.X) || !std::isfinite(Vertex.Position.Y)
            || !std::isfinite(Vertex.UV.X) || !std::isfinite(Vertex.UV.Y)
            || !std::isfinite(Vertex.Tint.R) || !std::isfinite(Vertex.Tint.G)
            || !std::isfinite(Vertex.Tint.B) || !std::isfinite(Vertex.Tint.A))
        {
            Error::raise(EErrorCode::InvalidArgument, "Draw list vertices must be finite");
        }
    }
    if (Indices.size() % 3 != 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Draw list indices must describe triangles");
    }
    for (const auto Index : Indices)
    {
        if (Index >= Vertices.size())
        {
            Error::raise(EErrorCode::InvalidArgument, "Draw list index is out of bounds");
        }
    }
    std::size_t NextIndex = 0;
    for (const auto& Batch : Batches)
    {
        if (Batch.FirstIndex != NextIndex || Batch.IndexCount == 0
            || Batch.IndexCount % 3 != 0 || Batch.FirstIndex > Indices.size()
            || Batch.IndexCount > Indices.size() - Batch.FirstIndex)
        {
            Error::raise(EErrorCode::InvalidArgument, "Draw batches must cover contiguous nonempty triangle ranges");
        }
        if (Batch.Clip)
        {
            const auto& Clip = *Batch.Clip;
            if (!std::isfinite(Clip.X) || !std::isfinite(Clip.Y)
                || !std::isfinite(Clip.Width) || !std::isfinite(Clip.Height)
                || Clip.Width < 0 || Clip.Height < 0
                || !std::isfinite(Clip.X + Clip.Width)
                || !std::isfinite(Clip.Y + Clip.Height))
            {
                Error::raise(EErrorCode::InvalidArgument, "Draw batch clip must have finite nonnegative extents");
            }
        }
        NextIndex += Batch.IndexCount;
    }
    if (NextIndex != Indices.size())
    {
        Error::raise(EErrorCode::InvalidArgument, "Draw batches must cover all indices");
    }
}
}