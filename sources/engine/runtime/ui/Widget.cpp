#include <infra/common/Error.hpp>
#include "Widget.hpp"
#include "WidgetGeometry.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace ui
{
std::unique_ptr<PanelSlot> PanelWidget::createSlot(std::unique_ptr<Widget> Child)
{
    return std::make_unique<PanelSlot>(*this, std::move(Child));
}

PanelSlot& PanelWidget::addSlot(std::unique_ptr<Widget> Child)
{
    if (!Child)
    {
        Error::raise(EErrorCode::InvalidArgument, "Cannot add a null widget");
    }
    auto NewSlot = createSlot(std::move(Child));
    Slots.push_back(std::move(NewSlot));
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
    return *Slots.back();
}

Widget& PanelWidget::addChild(std::unique_ptr<Widget> Child)
{
    return *addSlot(std::move(Child)).getChild();
}

void Widget::setBounds(UIRect Value)
{
    validateLayoutBounds(Value);
    Bounds = Value;
    DesiredSize = Value.getSize();
    invalidate(EInvalidationReason::IVisual);
}

UIRect Widget::getBounds() const noexcept
{
    return Bounds;
}

Slot* Widget::getSlot() noexcept
{
    return ParentSlot;
}

const Slot* Widget::getSlot() const noexcept
{
    return ParentSlot;
}

void Widget::setLayout(const Layout& Value)
{
    Value.validate();
    if (auto* Slot = dynamic_cast<PanelSlot*>(ParentSlot))
    {
        Slot->setLayout(Value);
    }
    else if (ParentSlot)
    {
        Error::raise(EErrorCode::Logic, "Parent slot does not support anchors");
    }
    else
    {
        PendingAnchor = Value;
    }
    invalidate(EInvalidationReason::IVisual);
}

const Layout& Widget::getLayout() const noexcept
{
    if (const auto* Slot = dynamic_cast<const PanelSlot*>(ParentSlot))
    {
        return Slot->getLayout();
    }
    static const Layout DefaultLayout;
    return PendingAnchor ? *PendingAnchor : DefaultLayout;
}

bool Widget::hasLayout() const noexcept
{
    if (const auto* Slot = dynamic_cast<const PanelSlot*>(ParentSlot))
    {
        return Slot->hasLayout();
    }
    return PendingAnchor.has_value();
}

void Widget::clearLayout() noexcept
{
    if (auto* Slot = dynamic_cast<PanelSlot*>(ParentSlot))
    {
        Slot->clearLayout();
    }
    PendingAnchor.reset();
    invalidate(EInvalidationReason::IVisual);
}

void Widget::setVisibility(EVisibility Value) noexcept
{
    if (Visibility == Value)
    {
        return;
    }
    Visibility = Value;
    invalidate(EInvalidationReason::Visibility);
}

EVisibility Widget::getVisibility() const noexcept
{
    return Visibility;
}

bool Widget::isVisible() const noexcept
{
    return Visibility == EVisibility::Visible;
}

void Widget::setEnableOuterRender(bool Value) noexcept
{
    if (EnableOuterRender == Value)
    {
        return;
    }
    EnableOuterRender = Value;
    invalidate(EInvalidationReason::Clipping);
}

bool Widget::isEnableOuterRender() const noexcept
{
    return EnableOuterRender;
}

void Widget::setAcceptEvents(bool Value) noexcept
{
    if (AcceptEvents == Value)
    {
        return;
    }
    AcceptEvents = Value;
    invalidate(EInvalidationReason::HitTest);
}

bool Widget::isAcceptEvents() const noexcept
{
    return AcceptEvents;
}

void Widget::setOpacity(float Value)
{
    if (!std::isfinite(Value) || Value < 0 || Value > 1)
    {
        Error::raise(EErrorCode::InvalidArgument, "Opacity must be finite and within [0, 1]");
    }
    if (Opacity == Value)
    {
        return;
    }
    Opacity = Value;
    invalidate(EInvalidationReason::Paint);
}

float Widget::getOpacity() const noexcept
{
    return Opacity;
}

void Widget::setPivot(Vector2 Value) noexcept
{
    RenderTransformValue.Pivot = Value;
}

Vector2 Widget::getPivot() const noexcept
{
    return RenderTransformValue.Pivot;
}

void Widget::setRotation(float Value) noexcept
{
    RenderTransformValue.Rotation = Value;
}

float Widget::getRotation() const noexcept
{
    return RenderTransformValue.Rotation;
}

void Widget::setRenderTransform(const RenderTransform& Value) noexcept
{
    RenderTransformValue = Value;
}

const RenderTransform& Widget::getRenderTransform() const noexcept
{
    return RenderTransformValue;
}

void Widget::setParentSlot(Slot* Parent) noexcept
{
    ParentSlot = Parent;
}

std::optional<Layout> Widget::takePendingAnchor() noexcept
{
    auto Result = std::move(PendingAnchor);
    PendingAnchor.reset();
    return Result;
}

void Widget::clearPendingAnchor() noexcept
{
    PendingAnchor.reset();
}

void Widget::setFocusable(bool Value) noexcept
{
    Focusable = Value;
}

bool Widget::isFocusable() const noexcept
{
    return Focusable;
}

const Widget* Widget::hitTest(UIPoint Point) const
{
    if (const auto* ChildHit = hitTestChildren(Point))
    {
        return ChildHit;
    }
    if (!AcceptEvents || !containsPoint(Point))
    {
        return nullptr;
    }
    return this;
}

Widget* Widget::hitTest(UIPoint Point)
{
    return const_cast<Widget*>(std::as_const(*this).hitTest(Point));
}

UIPoint Widget::localizePoint(UIPoint Point) const noexcept
{
    // 沿祖先链累乘局部->父变换, 再求逆, 把父坐标换算为自身局部坐标.
    UITransform ToRoot{1, 0, 0, 1, 0, 0};
    for (const Widget* Current = this; Current; )
    {
        ToRoot = WidgetGeometry::renderTransform(Current->Bounds, Current->RenderTransformValue)
                     .compose(ToRoot);
        const auto* CurrentSlot = Current->getSlot();
        Current = CurrentSlot ? CurrentSlot->getParent() : nullptr;
    }
    return WidgetGeometry::invert(ToRoot).transformPoint(Point);
}

void Widget::invalidate(EInvalidationReason Reason) noexcept
{
    Reason = normalizeInvalidation(Reason);
    if (Reason == EInvalidationReason::None)
    {
        return;
    }
    const auto Combined = Invalidation | Reason;
    if (Combined == Invalidation)
    {
        return;
    }
    Invalidation = Combined;
    if (ParentSlot)
    {
        ParentSlot->getParent()->invalidate(Reason);
    }
}

EInvalidationReason Widget::getInvalidationReason() const noexcept
{
    return Invalidation;
}

bool Widget::isInvalidated(EInvalidationReason Reason) const noexcept
{
    return hasInvalidation(Invalidation, Reason);
}

bool Widget::containsPoint(UIPoint Point) const noexcept
{
    return Point.X >= Bounds.X && Point.Y >= Bounds.Y &&
           Point.X < Bounds.X + Bounds.Width && Point.Y < Bounds.Y + Bounds.Height;
}

void Widget::consumeInvalidation(EInvalidationReason Reason) const noexcept
{
    Invalidation = Invalidation & ~Reason;
}

Vector2 Widget::measure(Vector2 AvailableSize) const
{
    validateLayoutSize(AvailableSize);
    const auto Desired = onMeasure(AvailableSize);
    validateLayoutSize(Desired);
    return Desired;
}

void Widget::arrange(UIRect Value)
{
    consumeInvalidation(EInvalidationReason::IVisual);
    validateLayoutBounds(Value);
    Bounds = Value;
    const UIRect LocalContent{0, 0, Bounds.Width, Bounds.Height};
    onArrange(LocalContent);
    arrangeChildren(LocalContent);
}

Vector2 Widget::onMeasure(Vector2) const
{
    return DesiredSize;
}

void Widget::onArrange(UIRect)
{
}

void Widget::paint(PaintContext& Context) const
{
    consumeInvalidation(EInvalidationReason::Paint | EInvalidationReason::Visibility |
                        EInvalidationReason::ChildOrder | EInvalidationReason::Clipping |
                        EInvalidationReason::HitTest);
    if (Visibility == EVisibility::Hidden)
    {
        return;
    }
    Context.pushTransform(WidgetGeometry::renderTransform(Bounds, RenderTransformValue));
    const bool ClipsSelf = shouldClipSelf();
    if (ClipsSelf)
    {
        try
        {
            Context.pushClipRect({0, 0, Bounds.Width, Bounds.Height}, EnableOuterRender);
        }
        catch (...)
        {
            Context.popTransform();
            throw;
        }
    }
    try
    {
        Context.pushOpacity(Opacity);
    }
    catch (...)
    {
        if (ClipsSelf)
        {
            Context.popClip();
        }
        Context.popTransform();
        throw;
    }
    try
    {
        onPaint(Context);
        paintChildren(Context);
    }
    catch (...)
    {
        Context.popOpacity();
        if (ClipsSelf)
        {
            Context.popClip();
        }
        Context.popTransform();
        throw;
    }
    Context.popOpacity();
    if (ClipsSelf)
    {
        Context.popClip();
    }
    Context.popTransform();
}

void Widget::paint(IUIRenderer& Renderer) const
{
    UIDrawList List;
    PaintContext Context(List);
    paint(Context);
    Renderer.submitDrawList(List);
}

void Widget::onPaint(PaintContext&) const
{
}

EventReply Widget::onMouseButtonDown(const MouseButtonEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseButtonUp(const MouseButtonEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseButtonClicked(const MouseButtonEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseMoved(const MouseMoveEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseEnter(const MouseMoveEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseLeave(const MouseMoveEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onMouseWheel(const MouseWheelEvent&, UIPoint)
{
    return EventReply::Unhandled;
}

EventReply Widget::onKeyDown(const KeyEvent&)
{
    return EventReply::Unhandled;
}

EventReply Widget::onKeyUp(const KeyEvent&)
{
    return EventReply::Unhandled;
}

size_t LeafWidget::getChildCount() const noexcept
{
    return 0;
}

void LeafWidget::paintChildren(PaintContext&) const
{
}

void LeafWidget::arrangeChildren(UIRect)
{
}

const Widget* LeafWidget::hitTestChildren(UIPoint) const
{
    return nullptr;
}

size_t ContentWidget::getChildCount() const noexcept
{
    return getChild() ? 1 : 0;
}

Widget* ContentWidget::getChild() noexcept
{
    return ChildSlot ? ChildSlot->getChild() : nullptr;
}

const Widget* ContentWidget::getChild() const noexcept
{
    return ChildSlot ? ChildSlot->getChild() : nullptr;
}

LeafSlot* ContentWidget::getChildSlot() noexcept
{
    return ChildSlot.get();
}

const LeafSlot* ContentWidget::getChildSlot() const noexcept
{
    return ChildSlot.get();
}

LeafSlot& ContentWidget::addSlot(std::unique_ptr<Widget> Child)
{
    auto NewSlot = std::make_unique<LeafSlot>(*this, std::move(Child));
    NewSlot->setLayoutEnabled(true);
    ChildSlot = std::move(NewSlot);
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
    return *ChildSlot;
}

void ContentWidget::setChild(std::unique_ptr<Widget> Value)
{
    if (!Value)
    {
        ChildSlot.reset();
        invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
        return;
    }
    auto NewSlot = std::make_unique<LeafSlot>(*this, std::move(Value));
    ChildSlot = std::move(NewSlot);
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
}

std::unique_ptr<Widget> ContentWidget::takeChild() noexcept
{
    if (!ChildSlot)
    {
        return nullptr;
    }
    auto Result = ChildSlot->takeChild();
    ChildSlot.reset();
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
    return Result;
}

Vector2 ContentWidget::onMeasure(Vector2 AvailableSize) const
{
    return ChildSlot ? ChildSlot->measure(AvailableSize) : Vector2{};
}

void ContentWidget::paintChildren(PaintContext& Context) const
{
    if (const auto* Child = getChild())
    {
        Child->paint(Context);
    }
}

void ContentWidget::arrangeChildren(UIRect Content)
{
    if (ChildSlot)
    {
        ChildSlot->arrange(Content);
    }
}

const Widget* ContentWidget::hitTestChildren(UIPoint Point) const
{
    const auto* Child = getChild();
    if (!Child || (!Child->isEnableOuterRender() && !containsPoint(Point)))
    {
        return nullptr;
    }
    return Child->hitTest(Point);
}

size_t PanelWidget::getChildCount() const noexcept
{
    return static_cast<size_t>(std::count_if(Slots.begin(), Slots.end(),
        [](const auto& ChildSlot) { return ChildSlot->getChild() != nullptr; }));
}

size_t PanelWidget::getSlotCount() const noexcept
{
    return Slots.size();
}

size_t PanelWidget::getChildSlotIndex(size_t Index) const
{
    for (size_t SlotIndex = 0; SlotIndex < Slots.size(); ++SlotIndex)
    {
        if (Slots[SlotIndex]->getChild())
        {
            if (Index == 0)
            {
                return SlotIndex;
            }
            --Index;
        }
    }
    Error::raise(EErrorCode::OutOfRange, "Invalid widget child index");
}

Widget& PanelWidget::getChild(size_t Index)
{
    return *Slots[getChildSlotIndex(Index)]->getChild();
}

const Widget& PanelWidget::getChild(size_t Index) const
{
    return *Slots[getChildSlotIndex(Index)]->getChild();
}

PanelSlot& PanelWidget::getChildSlot(size_t Index)
{
    return *Slots[getChildSlotIndex(Index)];
}

const PanelSlot& PanelWidget::getChildSlot(size_t Index) const
{
    return *Slots[getChildSlotIndex(Index)];
}

PanelSlot& PanelWidget::getSlot(size_t Index)
{
    if (Index >= Slots.size())
    {
        Error::raise(EErrorCode::OutOfRange, "Invalid widget slot index");
    }
    return *Slots[Index];
}

const PanelSlot& PanelWidget::getSlot(size_t Index) const
{
    if (Index >= Slots.size())
    {
        Error::raise(EErrorCode::OutOfRange, "Invalid widget slot index");
    }
    return *Slots[Index];
}

PanelSlot* PanelWidget::getSlotForChild(const Widget& Child) noexcept
{
    for (const auto& ChildSlot : Slots)
    {
        if (ChildSlot->getChild() == &Child)
        {
            return ChildSlot.get();
        }
    }
    return nullptr;
}

const PanelSlot* PanelWidget::getSlotForChild(const Widget& Child) const noexcept
{
    for (const auto& ChildSlot : Slots)
    {
        if (ChildSlot->getChild() == &Child)
        {
            return ChildSlot.get();
        }
    }
    return nullptr;
}

std::unique_ptr<Widget> PanelWidget::takeChild(size_t Index)
{
    const auto SlotIndex = getChildSlotIndex(Index);
    auto Result = Slots[SlotIndex]->takeChild();
    Slots.erase(Slots.begin() + static_cast<std::ptrdiff_t>(SlotIndex));
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
    return Result;
}

void PanelWidget::removeSlot(size_t Index)
{
    if (Index >= Slots.size())
    {
        Error::raise(EErrorCode::OutOfRange, "Invalid widget slot index");
    }
    Slots.erase(Slots.begin() + static_cast<std::ptrdiff_t>(Index));
    invalidate(EInvalidationReason::IVisual | EInvalidationReason::ChildOrder);
}

Vector2 PanelWidget::onMeasure(Vector2 AvailableSize) const
{
    Vector2 Desired{};
    for (const auto& ChildSlot : Slots)
    {
        const auto ChildDesired = ChildSlot->measure(AvailableSize);
        Desired.x() = std::max(Desired.x(), ChildDesired.x());
        Desired.y() = std::max(Desired.y(), ChildDesired.y());
    }
    return Desired;
}

void PanelWidget::paintChildren(PaintContext& Context) const
{
    // 按 Z 序绘制：值小的在下层先画，相等时按添加顺序（后添加的覆盖其上）。
    std::vector<size_t> Order(Slots.size());
    std::iota(Order.begin(), Order.end(), 0);
    std::sort(Order.begin(), Order.end(), [this](size_t Left, size_t Right)
    {
        const int ZLeft = Slots[Left]->getZOrder();
        const int ZRight = Slots[Right]->getZOrder();
        return ZLeft != ZRight ? ZLeft < ZRight : Left < Right;
    });
    for (const auto Index : Order)
    {
        if (const auto* Child = Slots[Index]->getChild())
        {
            Child->paint(Context);
        }
    }
}

void PanelWidget::arrangeChildren(UIRect Content)
{
    for (const auto& ChildSlot : Slots)
    {
        ChildSlot->arrange(Content);
    }
}

const Widget* PanelWidget::hitTestChildren(UIPoint Point) const
{
    // 按 Z 序命中：值大的在上层先命中，相等时后添加的优先。
    std::vector<size_t> Order(Slots.size());
    std::iota(Order.begin(), Order.end(), 0);
    std::sort(Order.begin(), Order.end(), [this](size_t Left, size_t Right)
    {
        const int ZLeft = Slots[Left]->getZOrder();
        const int ZRight = Slots[Right]->getZOrder();
        return ZLeft != ZRight ? ZLeft > ZRight : Left > Right;
    });
    for (const auto Index : Order)
    {
        const auto* Child = Slots[Index]->getChild();
        if (!Child || (!Child->isEnableOuterRender() && !containsPoint(Point)))
        {
            continue;
        }
        if (const auto* Hit = Child->hitTest(Point))
        {
            return Hit;
        }
    }
    return nullptr;
}
}
