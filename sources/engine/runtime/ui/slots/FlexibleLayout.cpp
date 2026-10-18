#include "FlexibleLayout.hpp"
#include "FlexibleSlot.hpp"
#include "Widget.hpp"
#include <infra/common/Error.hpp>
#include <algorithm>
#include <cmath>

namespace ui
{

bool FlexibleLayout::isHorizontal() const noexcept
{
    return Direction == EFlexDirection::Row || Direction == EFlexDirection::RowReverse;
}

bool FlexibleLayout::isReverse() const noexcept
{
    return Direction == EFlexDirection::RowReverse || Direction == EFlexDirection::ColumnReverse;
}

void FlexibleLayout::validate() const
{
    if (!std::isfinite(Spacing) || Spacing < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Flex spacing must be finite and nonnegative");
    }
}

Vector2 FlexibleLayout::measure(const std::vector<const FlexibleSlot*>& Slots, Vector2 AvailableSize) const
{
    const bool Horizontal = isHorizontal();

    float Main = 0;
    float Cross = 0;
    std::size_t Count = 0;

    for (const FlexibleSlot* Slot : Slots)
    {
        if (!Slot)
        {
            continue;
        }
        const Widget* Child = Slot->getChild();
        if (!Child)
        {
            continue;
        }

        const Vector2 Desired = Child->measure(AvailableSize);
        const Padding& Margin = Slot->getMargin();

        const float ChildMain = Horizontal ? Desired.x() : Desired.y();
        const float ChildCross = Horizontal ? Desired.y() : Desired.x();
        const float Basis = Slot->getFlexBasis() > 0 ? Slot->getFlexBasis() : ChildMain;
        const float MainMargin = Horizontal ? Margin.Left + Margin.Right : Margin.Top + Margin.Bottom;
        const float CrossMargin = Horizontal ? Margin.Top + Margin.Bottom : Margin.Left + Margin.Right;

        Main += Basis + MainMargin;
        Cross = std::max(Cross, ChildCross + CrossMargin);
        ++Count;
    }

    if (Count > 1)
    {
        Main += Spacing * static_cast<float>(Count - 1);
    }

    return Horizontal ? Vector2{Main, Cross} : Vector2{Cross, Main};
}

void FlexibleLayout::arrange(const std::vector<FlexibleSlot*>& Slots, UIRect Content) const
{
    validateLayoutBounds(Content);

    const bool Horizontal = isHorizontal();
    const bool Reverse = isReverse();

    const float MainOrigin = Horizontal ? Content.X : Content.Y;
    const float CrossOrigin = Horizontal ? Content.Y : Content.X;
    const float MainTotal = Horizontal ? Content.Width : Content.Height;
    const float CrossTotal = Horizontal ? Content.Height : Content.Width;

    struct Item
    {
        Widget* Child;
        float Basis;
        float CrossNatural;
        float MainMargin;
        float CrossMargin;
        float MainLeadMargin;
        float CrossLeadMargin;
        float Grow;
        float Shrink;
        ECrossAxisAlignment Align;
    };

    std::vector<Item> Items;
    Items.reserve(Slots.size());
    for (FlexibleSlot* Slot : Slots)
    {
        if (!Slot)
        {
            continue;
        }
        Widget* Child = Slot->getChild();
        if (!Child)
        {
            continue;
        }

        const Vector2 Available = Horizontal
            ? Vector2{MainTotal, CrossTotal}
            : Vector2{CrossTotal, MainTotal};
        const Vector2 Desired = Child->measure(Available);
        const Padding& Margin = Slot->getMargin();

        const float ChildMain = Horizontal ? Desired.x() : Desired.y();
        const float ChildCross = Horizontal ? Desired.y() : Desired.x();
        const float Basis = Slot->getFlexBasis() > 0 ? Slot->getFlexBasis() : ChildMain;

        const float MainLeadMargin = Horizontal ? Margin.Left : Margin.Top;
        const float MainTrailMargin = Horizontal ? Margin.Right : Margin.Bottom;
        const float CrossLeadMargin = Horizontal ? Margin.Top : Margin.Left;
        const float CrossTrailMargin = Horizontal ? Margin.Bottom : Margin.Right;

        Items.push_back(Item{
            Child,
            Basis,
            ChildCross,
            MainLeadMargin + MainTrailMargin,
            CrossLeadMargin + CrossTrailMargin,
            MainLeadMargin,
            CrossLeadMargin,
            Slot->getFlexGrow(),
            Slot->getFlexShrink(),
            Slot->getAlignSelf().value_or(CrossAxisAlignment),
        });
    }

    const std::size_t N = Items.size();
    if (N == 0)
    {
        return;
    }

    // 主轴: 自然尺寸、剩余空间与 grow 分配.
    float NaturalMain = 0;
    float TotalGrow = 0;
    for (const Item& I : Items)
    {
        NaturalMain += I.Basis + I.MainMargin;
        TotalGrow += I.Grow;
    }
    NaturalMain += Spacing * static_cast<float>(N - 1);

    float Free = MainTotal - NaturalMain;
    if (Free > 0 && TotalGrow > 0)
    {
        for (Item& I : Items)
        {
            if (I.Grow > 0)
            {
                I.Basis += Free * (I.Grow / TotalGrow);
            }
        }
        Free = 0;
    }
    else if (Free < 0 && Overflow == EFlexOverflow::Shrink)
    {
        float TotalShrink = 0;
        for (const Item& I : Items)
        {
            TotalShrink += I.Basis * I.Shrink;
        }
        if (TotalShrink > 0)
        {
            const float Shortfall = -Free;
            for (Item& I : Items)
            {
                if (I.Shrink > 0)
                {
                    const float Reduction = Shortfall * (I.Basis * I.Shrink / TotalShrink);
                    I.Basis = std::max(0.0f, I.Basis - Reduction);
                }
            }
        }
        Free = 0;
    }

    // 主轴对齐: 计算起始偏移与相邻间距.
    float MainCursor = 0;
    float ItemSpacing = Spacing;
    switch (MainAxisAlignment)
    {
    case EMainAxisAlignment::Start:
        MainCursor = 0;
        break;
    case EMainAxisAlignment::Center:
        MainCursor = std::max(0.0f, Free) * 0.5f;
        break;
    case EMainAxisAlignment::End:
        MainCursor = std::max(0.0f, Free);
        break;
    case EMainAxisAlignment::SpaceBetween:
        if (N > 1)
        {
            ItemSpacing += std::max(0.0f, Free) / static_cast<float>(N - 1);
        }
        break;
    case EMainAxisAlignment::SpaceAround:
        if (Free > 0)
        {
            const float Gap = Free / static_cast<float>(N);
            MainCursor = Gap * 0.5f;
            ItemSpacing += Gap;
        }
        break;
    case EMainAxisAlignment::SpaceEvenly:
        {
            const float Gap = std::max(0.0f, Free) / static_cast<float>(N + 1);
            MainCursor = Gap;
            ItemSpacing += Gap;
        }
        break;
    }

    for (Item& I : Items)
    {
        float CrossSize;
        if (I.Align == ECrossAxisAlignment::Stretch)
        {
            CrossSize = std::max(0.0f, CrossTotal - I.CrossMargin);
        }
        else
        {
            CrossSize = std::min(I.CrossNatural, std::max(0.0f, CrossTotal - I.CrossMargin));
        }

        const float CrossExtent = CrossSize + I.CrossMargin;
        float CrossPos = 0;
        switch (I.Align)
        {
        case ECrossAxisAlignment::Start:
            CrossPos = CrossOrigin + I.CrossLeadMargin;
            break;
        case ECrossAxisAlignment::Center:
            CrossPos = CrossOrigin + (CrossTotal - CrossExtent) * 0.5f + I.CrossLeadMargin;
            break;
        case ECrossAxisAlignment::End:
            CrossPos = CrossOrigin + CrossTotal - CrossExtent + I.CrossLeadMargin;
            break;
        case ECrossAxisAlignment::Stretch:
            CrossPos = CrossOrigin + I.CrossLeadMargin;
            break;
        }

        const float MainSize = std::max(0.0f, I.Basis);
        const float LocalMain = MainCursor + I.MainLeadMargin;
        const float MainPos = Reverse
            ? MainOrigin + MainTotal - (LocalMain + MainSize)
            : MainOrigin + LocalMain;

        UIRect Rect;
        if (Horizontal)
        {
            Rect = UIRect{MainPos, CrossPos, MainSize, CrossSize};
        }
        else
        {
            Rect = UIRect{CrossPos, MainPos, CrossSize, MainSize};
        }

        I.Child->arrange(Rect);

        MainCursor += MainSize + I.MainMargin + ItemSpacing;
    }
}

}
