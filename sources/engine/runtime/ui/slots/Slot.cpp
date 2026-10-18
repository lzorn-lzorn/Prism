#include <infra/common/Error.hpp>
#include "Slot.hpp"
#include "Widget.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace ui
{
namespace
{
Vector2 getInsetSize(Padding Insets)
{
    const Vector2 Size{Insets.Left + Insets.Right, Insets.Top + Insets.Bottom};
    validateLayoutSize(Size);
    return Size;
}

Vector2 subtractInsets(Vector2 Size, Padding Insets)
{
    const auto Total = getInsetSize(Insets);
    return {std::max(0.0f, Size.x() - Total.x()),
            std::max(0.0f, Size.y() - Total.y())};
}

void validateAlignment(Alignment Value)
{
    switch (Value)
    {
    case Alignment::Start:
    case Alignment::Center:
    case Alignment::End:
    case Alignment::Fill:
        return;
    }
    Error::raise(EErrorCode::InvalidArgument, "Unknown alignment");
}

float alignPosition(float Start, float Available, float Size, Alignment Value)
{
    if (Value == Alignment::Center)
    {
        return Start + (Available - Size) * 0.5f;
    }
    if (Value == Alignment::End)
    {
        return Start + Available - Size;
    }
    return Start;
}
}

Slot::Slot(Widget& Parent, std::unique_ptr<Widget> Child)
    : Parent(&Parent), Child(std::move(Child))
{
    if (!this->Child)
    {
        Error::raise(EErrorCode::InvalidArgument, "Cannot attach a null widget");
    }
    if (this->Child->getSlot())
    {
        Error::raise(EErrorCode::InvalidArgument, "Widget is already attached");
    }
    // @note 即使祖先控件的所有权由调用者持有，也拒绝形成循环关系。
    for (const Widget* Ancestor = &Parent; Ancestor; )
    {
        if (Ancestor == this->Child.get())
        {
            Error::raise(EErrorCode::InvalidArgument, "Widget attachment would create a cycle");
        }
        const auto* AncestorSlot = Ancestor->getSlot();
        Ancestor = AncestorSlot ? AncestorSlot->getParent() : nullptr;
    }
    this->Child->setParentSlot(this);
}

Slot::~Slot()
{
    if (Child)
    {
        Child->setParentSlot(nullptr);
        Child->clearPendingAnchor();
    }
}

Widget* Slot::getChild() noexcept
{
    return Child.get();
}

const Widget* Slot::getChild() const noexcept
{
    return Child.get();
}

Widget* Slot::getParent() noexcept
{
    return Parent;
}

const Widget* Slot::getParent() const noexcept
{
    return Parent;
}

std::unique_ptr<Widget> Slot::takeChild() noexcept
{
    if (Child)
    {
        Child->setParentSlot(nullptr);
        Child->clearPendingAnchor();
    }
    return std::move(Child);
}

PanelSlot::PanelSlot(Widget& Parent, std::unique_ptr<Widget> Child)
    : Slot(Parent, std::move(Child))
{
    if (auto Pending = getChild()->takePendingAnchor())
    {
        Anchor = std::move(*Pending);
        HasAnchor = true;
    }
}

void PanelSlot::setLayout(const Layout& Value)
{
    Value.validate();
    Anchor = Value;
    HasAnchor = true;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

const Layout& PanelSlot::getLayout() const noexcept
{
    return Anchor;
}

bool PanelSlot::hasLayout() const noexcept
{
    return HasAnchor;
}

void PanelSlot::clearLayout() noexcept
{
    Anchor = Layout{};
    HasAnchor = false;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

void PanelSlot::setAutoSize(bool Value) noexcept
{
    EnableAutoSize = Value;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

bool PanelSlot::isAutoSize() const noexcept
{
    return EnableAutoSize;
}

Vector2 PanelSlot::measure(Vector2 AvailableSize) const
{
    validateLayoutSize(AvailableSize);
    const auto* Child = getChild();
    if (!Child)
    {
        return {};
    }
    Vector2 Desired = Child->measure(AvailableSize);
    Vector2 Position = Child->getBounds().getPosition();
    if (HasAnchor)
    {
        // @note 拉伸时请求固有尺寸加边距，而非可用锚点跨度，避免期望尺寸依赖分配区域。
        if (Anchor.Anchor.Min.x() != Anchor.Anchor.Max.x())
        {
            Desired.x() += Anchor.StartMargin.x() + Anchor.EndMargin.x();
            Position.x() = 0;
        }
        else
        {
            Desired.x() = EnableAutoSize ? Desired.x() : Anchor.Size.x();
            Position.x() = Anchor.Position.x();
        }
        if (Anchor.Anchor.Min.y() != Anchor.Anchor.Max.y())
        {
            Desired.y() += Anchor.StartMargin.y() + Anchor.EndMargin.y();
            Position.y() = 0;
        }
        else
        {
            Desired.y() = EnableAutoSize ? Desired.y() : Anchor.Size.y();
            Position.y() = Anchor.Position.y();
        }
    }
    if (!std::isfinite(Desired.x()) || !std::isfinite(Desired.y()))
    {
        Error::raise(EErrorCode::InvalidArgument, "Slot desired size overflow");
    }
    const Vector2 Result{std::max(0.0f, Desired.x()) + std::max(0.0f, Position.x()),
                         std::max(0.0f, Desired.y()) + std::max(0.0f, Position.y())};
    validateLayoutSize(Result);
    return Result;
}

void PanelSlot::arrange(UIRect ParentContent)
{
    validateLayoutBounds(ParentContent);
    auto* Child = getChild();
    if (!Child)
    {
        return;
    }
    UIRect Allocation = Child->getBounds();
    if (HasAnchor)
    {
        auto Rules = Anchor;
        if (EnableAutoSize)
        {
            // @note 期望尺寸只影响固定轴；拉伸轴保留边距。
            Rules.Size = Child->measure(ParentContent.getSize());
        }
        Allocation = Rules.calculateBounds(ParentContent);
        // 同步轴心与旋转到孩子, 供绘制与命中测试使用.
        Child->setPivot(Anchor.Pivot);
        Child->setRotation(Anchor.Rotation);
    }
    else
    {
        Child->setPivot({0, 0});
        Child->setRotation(0);
        if (EnableAutoSize)
        {
            const auto Desired = Child->measure(ParentContent.getSize());
            Allocation.Width = Desired.x();
            Allocation.Height = Desired.y();
        }
    }
    Child->arrange(Allocation);
}

LeafSlot::LeafSlot(Widget& Parent, std::unique_ptr<Widget> Child)
    : PanelSlot(Parent, std::move(Child))
{
}

void LeafSlot::setPadding(Padding Value)
{
    validateLayoutSize({Value.Left, Value.Top});
    validateLayoutSize({Value.Right, Value.Bottom});
    getInsetSize(Value);
    Insets = Value;
    LayoutEnabled = true;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

Padding LeafSlot::getPadding() const noexcept
{
    return Insets;
}

void LeafSlot::setHorizontalAlignment(Alignment Value)
{
    validateAlignment(Value);
    Horizontal = Value;
    LayoutEnabled = true;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

Alignment LeafSlot::getHorizontalAlignment() const noexcept
{
    return Horizontal;
}

void LeafSlot::setVerticalAlignment(Alignment Value)
{
    validateAlignment(Value);
    Vertical = Value;
    LayoutEnabled = true;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

Alignment LeafSlot::getVerticalAlignment() const noexcept
{
    return Vertical;
}

void LeafSlot::setLayoutEnabled(bool Value) noexcept
{
    LayoutEnabled = Value;
    getParent()->invalidate(EInvalidationReason::IVisual);
}

bool LeafSlot::isLayoutEnabled() const noexcept
{
    return LayoutEnabled;
}

Vector2 LeafSlot::measure(Vector2 AvailableSize) const
{
    validateLayoutSize(AvailableSize);
    const auto* Child = getChild();
    if (!Child)
    {
        return {};
    }
    if (!LayoutEnabled)
    {
        return PanelSlot::measure(AvailableSize);
    }
    const auto InnerSize = subtractInsets(AvailableSize, Insets);
    const auto Desired = hasLayout() ? PanelSlot::measure(InnerSize) : Child->measure(InnerSize);
    const auto Result = Desired + getInsetSize(Insets);
    validateLayoutSize(Result);
    return Result;
}

void LeafSlot::arrange(UIRect ParentContent)
{
    validateLayoutBounds(ParentContent);
    auto* Child = getChild();
    if (!Child)
    {
        return;
    }
    if (!LayoutEnabled)
    {
        PanelSlot::arrange(ParentContent);
        return;
    }
    const auto InnerSize = subtractInsets(ParentContent.getSize(), Insets);
    const UIRect Inner{ParentContent.X + Insets.Left, ParentContent.Y + Insets.Top,
                       InnerSize.x(), InnerSize.y()};
    validateLayoutBounds(Inner);
    if (hasLayout())
    {
        PanelSlot::arrange(Inner);
        return;
    }
    const auto Desired = Child->measure(InnerSize);
    const float Width = Horizontal == Alignment::Fill ? Inner.Width : std::min(Inner.Width, Desired.x());
    const float Height = Vertical == Alignment::Fill ? Inner.Height : std::min(Inner.Height, Desired.y());
    Child->setPivot({0, 0});
    Child->setRotation(0);
    Child->arrange({alignPosition(Inner.X, Inner.Width, Width, Horizontal),
                    alignPosition(Inner.Y, Inner.Height, Height, Vertical), Width, Height});
}
}