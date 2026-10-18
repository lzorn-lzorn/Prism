#include <infra/common/Error.hpp>
#include "Window.hpp"
#include <limits>
#include <stdexcept>
#include <utility>

namespace ui
{
namespace
{
bool isSameRect(UIRect Left, UIRect Right) noexcept
{
    return Left.X == Right.X && Left.Y == Right.Y &&
           Left.Width == Right.Width && Left.Height == Right.Height;
}

UISize nearestPreset(UISize Target, const std::vector<UISize>& Presets)
{
    UISize Best = Target;
    double BestDistance = std::numeric_limits<double>::infinity();
    for (const UISize& Preset : Presets)
    {
        const double DX = static_cast<double>(Preset.Width) - Target.Width;
        const double DY = static_cast<double>(Preset.Height) - Target.Height;
        const double Distance = DX * DX + DY * DY;
        if (Distance < BestDistance)
        {
            BestDistance = Distance;
            Best = Preset;
        }
    }
    return Best;
}
}

Window::Window(IGenericPlatform& Platform, const WindowConfigs& Configs,
               std::unique_ptr<Widget> InContentRoot)
    : Native(Platform.createWindow(Configs)),
      ResizePolicy(Configs.ResizePolicy),
      PresetResolutions(Configs.PresetResolutions)
{
    if (!Native)
    {
        Error::raise(EErrorCode::Runtime, "Platform did not create a window");
    }
    setContentRoot(std::move(InContentRoot));
}

Widget* Window::getContentRoot() noexcept
{
    return ContentRoot.get();
}

const Widget* Window::getContentRoot() const noexcept
{
    return ContentRoot.get();
}

void Window::setContentRoot(std::unique_ptr<Widget> InRoot)
{
    if (InRoot && InRoot->getSlot())
    {
        Error::raise(EErrorCode::InvalidArgument, "Window content root must not be attached to a parent slot");
    }
    ContentRoot = std::move(InRoot);
    Hovered = nullptr;
    Pressed = nullptr;
    Focused = nullptr;
    if (ContentRoot)
    {
        ContentRoot->invalidate(EInvalidationReason::All);
    }
    if (Retained)
    {
        Retained->IsListValid = false;
        Retained->HasSubmitted = false;
        Retained->LastRootBounds = {};
    }
    LastUpdate = {};
}

std::unique_ptr<Widget> Window::takeContentRoot() noexcept
{
    Hovered = nullptr;
    Pressed = nullptr;
    Focused = nullptr;
    if (Retained)
    {
        Retained->IsListValid = false;
        Retained->HasSubmitted = false;
        Retained->LastRootBounds = {};
    }
    LastUpdate = {};
    return std::move(ContentRoot);
}

IGenericWindow& Window::getNativeWindow() noexcept
{
    return *Native;
}

const IGenericWindow& Window::getNativeWindow() const noexcept
{
    return *Native;
}

float Window::getDisplayScale() const
{
    return Native->getDisplayScale();
}

EWindowResizePolicy Window::getResizePolicy() const noexcept
{
    return ResizePolicy;
}

const std::vector<UISize>& Window::getPresetResolutions() const noexcept
{
    return PresetResolutions;
}

void Window::setResizePolicy(EWindowResizePolicy Policy)
{
    ResizePolicy = Policy;
    Native->setResizable(Policy != EWindowResizePolicy::Fixed);
}

void Window::setPresetResolutions(std::vector<UISize> Resolutions)
{
    PresetResolutions = std::move(Resolutions);
}

bool Window::setResolution(UISize Size)
{
    if (Size.Width <= 0 || Size.Height <= 0)
    {
        return false;
    }
    if (ResizePolicy == EWindowResizePolicy::Fixed)
    {
        return false;
    }
    if (ResizePolicy == EWindowResizePolicy::Preset && !PresetResolutions.empty())
    {
        Size = nearestPreset(Size, PresetResolutions);
    }
    Native->setSize(Size);
    return true;
}

void Window::setBackgroundColor(Color Value) noexcept
{
    Background = Value;
}

Color Window::getBackgroundColor() const noexcept
{
    return Background;
}

bool Window::drawFrame(IUIRenderer* Renderer)
{
    const auto Size = Native->getSize();
    const auto SafeArea = Native->getSafeArea();
    const UIRect RootBounds{static_cast<float>(SafeArea.X), static_cast<float>(SafeArea.Y),
                            static_cast<float>(SafeArea.Width), static_cast<float>(SafeArea.Height)};

    // 保留模式才持有跨帧缓存; 立即模式每帧使用本地列表. 
    const bool RetainedMode = Renderer && Renderer->getUIMode() == EUIMode::Retained;
    if (RetainedMode && !Retained)
    {
        Retained = std::make_unique<WindowRetainedState>();
    }
    else if (!RetainedMode)
    {
        Retained.reset();
    }

    // 安全区变化等价于 IVisual 失效: 内容根的分配区域变了. 
    if (ContentRoot && Retained && !isSameRect(RootBounds, Retained->LastRootBounds))
    {
        ContentRoot->invalidate(EInvalidationReason::IVisual);
    }

    const EInvalidationReason Reasons = ContentRoot ? ContentRoot->getInvalidationReason() : EInvalidationReason::None;
    bool DidLayout = false;
    bool DidPaint = false;

    // 布局: 立即模式每帧全量; 保留模式只在 IVisual 失效时进行. 
    if (ContentRoot && (!RetainedMode || hasInvalidation(Reasons, EInvalidationReason::IVisual)))
    {
        ContentRoot->arrange(RootBounds);
        if (Retained)
        {
            Retained->LastRootBounds = RootBounds;
        }
        DidLayout = true;
    }

    if (!Renderer)
    {
        LastUpdate = {DidLayout, false, Reasons};
        return false;
    }

    // 绘制列表: 立即模式每帧重建; 保留模式只在绘制相关失效(或尚未构建/刚排版)时重建. 
    const bool IsListValid = Retained && Retained->IsListValid;
    const bool NeedsPaint = !IsListValid || !RetainedMode || hasInvalidation(Reasons, PaintInvalidationReasons) || DidLayout;
    UIDrawList FrameList;
    UIDrawList* Submit = nullptr;
    if (NeedsPaint)
    {
        auto& List = RetainedMode ? Retained->CachedList : FrameList;
        List.clear();
        if (ContentRoot)
        {
            PaintContext Context(List, Renderer);
            ContentRoot->paint(Context);
            DidPaint = true;
        }
        if (Retained)
        {
            Retained->IsListValid = true;
        }
        Submit = &List;
    }
    else
    {
        // 保留模式且列表仍有效 → 复用缓存(此时 Retained 必非空). 
        Submit = &Retained->CachedList;
    }

    LastUpdate = {DidLayout, DidPaint, Reasons};

    if (!Renderer->canBeginFrame({Size, Native->getPixelSize(), Background}))
    {
        return false;
    }

    // 保留模式且本帧未重建内容 -> 复用上次上传的 GPU 缓冲，跳过顶点上传. 
    const bool ReuseBuffers = RetainedMode && Retained->IsListValid && !DidPaint && Retained->HasSubmitted;
    try
    {
        Renderer->submitDrawList(*Submit, ReuseBuffers);
    }
    catch (...)
    {
        try
        {
            Renderer->endFrame();
        }
        catch (...)
        {
        }
        throw;
    }
    Renderer->endFrame();
    if (Retained)
    {
        Retained->HasSubmitted = true;
    }
    return true;
}

Widget* Window::hitTest(UIPoint WindowPoint)
{
    return ContentRoot ? ContentRoot->hitTest(WindowPoint) : nullptr;
}

const Widget* Window::hitTest(UIPoint WindowPoint) const
{
    return ContentRoot ? ContentRoot->hitTest(WindowPoint) : nullptr;
}

const UIUpdateReport& Window::getLastUpdate() const noexcept
{
    return LastUpdate;
}

EventReply Window::dispatchMouseMove(const MouseMoveEvent& Event)
{
    auto* Target = hitTest({Event.X, Event.Y});
    EventReply Reply = EventReply::Unhandled;
    if (Target != Hovered)
    {
        if (Hovered)
        {
            Reply = Hovered->onMouseLeave(Event, Hovered->localizePoint({Event.X, Event.Y}));
        }
        Hovered = Target;
        if (Hovered)
        {
            if (Hovered->onMouseEnter(Event, Hovered->localizePoint({Event.X, Event.Y})) == EventReply::Handled)
            {
                Reply = EventReply::Handled;
            }
        }
    }
    if (Hovered)
    {
        if (Hovered->onMouseMoved(Event, Hovered->localizePoint({Event.X, Event.Y})) == EventReply::Handled)
        {
            Reply = EventReply::Handled;
        }
    }
    return Reply;
}

EventReply Window::dispatchMouseButton(const MouseButtonEvent& Event)
{
    auto* Target = hitTest({Event.X, Event.Y});
    if (Event.Pressed)
    {
        Pressed = Target;
        if (!Target)
        {
            clearFocus();
            return EventReply::Unhandled;
        }
        const auto Reply = Target->onMouseButtonDown(Event, Target->localizePoint({Event.X, Event.Y}));
        if (Target->isFocusable())
        {
            setFocus(Target);
        }
        return Reply;
    }
    EventReply Reply = EventReply::Unhandled;
    if (Target)
    {
        Reply = Target->onMouseButtonUp(Event, Target->localizePoint({Event.X, Event.Y}));
        if (Pressed == Target &&
            Target->onMouseButtonClicked(Event, Target->localizePoint({Event.X, Event.Y})) == EventReply::Handled)
        {
            Reply = EventReply::Handled;
        }
    }
    Pressed = nullptr;
    return Reply;
}

EventReply Window::dispatchMouseWheel(const MouseWheelEvent& Event)
{
    if (auto* Target = hitTest({Event.MouseX, Event.MouseY}))
    {
        return Target->onMouseWheel(Event, Target->localizePoint({Event.MouseX, Event.MouseY}));
    }
    return EventReply::Unhandled;
}

EventReply Window::dispatchKey(const KeyEvent& Event)
{
    if (!Focused)
    {
        return EventReply::Unhandled;
    }
    if (Event.Pressed)
    {
        return Focused->onKeyDown(Event);
    }
    return Focused->onKeyUp(Event);
}

void Window::setFocus(Widget* Target)
{
    Focused = Target;
}

Widget* Window::getFocus() noexcept
{
    return Focused;
}

const Widget* Window::getFocus() const noexcept
{
    return Focused;
}

void Window::clearFocus() noexcept
{
    Focused = nullptr;
}
}
