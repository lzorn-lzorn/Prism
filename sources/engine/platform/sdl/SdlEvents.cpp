#include "SdlInternal.hpp"

#include <utility>

namespace runtime::platform::detail
{
namespace
{
std::string copyText(const char* Text)
{
return Text ? std::string(Text) : std::string{};
}

Modifiers translateModifiers(SDL_Keymod Source)
{
Modifiers Result = Modifiers::None;
if (Source & SDL_KMOD_SHIFT)
{
Result = Result | Modifiers::Shift;
}
if (Source & SDL_KMOD_CTRL)
{
Result = Result | Modifiers::Control;
}
if (Source & SDL_KMOD_ALT)
{
Result = Result | Modifiers::Alt;
}
if (Source & SDL_KMOD_GUI)
{
Result = Result | Modifiers::Super;
}
if (Source & SDL_KMOD_CAPS)
{
Result = Result | Modifiers::CapsLock;
}
if (Source & SDL_KMOD_NUM)
{
Result = Result | Modifiers::NumLock;
}
if (Source & SDL_KMOD_MODE)
{
Result = Result | Modifiers::AltGraph;
}
return Result;
}

char32_t translateCharacter(SDL_Keycode Source)
{
// @note SDL 中由扫描码派生的键码和扩展键码不是 Unicode 字符。
if ((Source & (SDLK_SCANCODE_MASK | SDLK_EXTENDED_MASK)) != 0
|| Source > 0x10FFFFu || (Source >= 0xD800u && Source <= 0xDFFFu))
{
return 0;
}
return static_cast<char32_t>(Source);
}

MouseButton translateButton(Uint8 Source)
{
switch (Source)
{
case SDL_BUTTON_LEFT: return MouseButton::Left;
case SDL_BUTTON_MIDDLE: return MouseButton::Middle;
case SDL_BUTTON_RIGHT: return MouseButton::Right;
case SDL_BUTTON_X1: return MouseButton::Back;
case SDL_BUTTON_X2: return MouseButton::Forward;
default: return MouseButton::Unknown;
}
}

MouseButtons translateButtons(SDL_MouseButtonFlags Source)
{
unsigned Result = 0;
if (Source & SDL_BUTTON_LMASK)
{
Result |= static_cast<unsigned>(MouseButtons::Left);
}
if (Source & SDL_BUTTON_MMASK)
{
Result |= static_cast<unsigned>(MouseButtons::Middle);
}
if (Source & SDL_BUTTON_RMASK)
{
Result |= static_cast<unsigned>(MouseButtons::Right);
}
if (Source & SDL_BUTTON_X1MASK)
{
Result |= static_cast<unsigned>(MouseButtons::Back);
}
if (Source & SDL_BUTTON_X2MASK)
{
Result |= static_cast<unsigned>(MouseButtons::Forward);
}
return static_cast<MouseButtons>(Result);
}

bool canTranslateWindowAction(Uint32 Source, WindowAction& Action)
{
switch (Source)
{
case SDL_EVENT_WINDOW_SHOWN: Action = WindowAction::Shown; break;
case SDL_EVENT_WINDOW_HIDDEN: Action = WindowAction::Hidden; break;
case SDL_EVENT_WINDOW_EXPOSED: Action = WindowAction::Exposed; break;
case SDL_EVENT_WINDOW_MOVED: Action = WindowAction::Moved; break;
case SDL_EVENT_WINDOW_RESIZED: Action = WindowAction::Resized; break;
case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: Action = WindowAction::PixelSizeChanged; break;
case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED: Action = WindowAction::SafeAreaChanged; break;
case SDL_EVENT_WINDOW_MINIMIZED: Action = WindowAction::Minimized; break;
case SDL_EVENT_WINDOW_MAXIMIZED: Action = WindowAction::Maximized; break;
case SDL_EVENT_WINDOW_RESTORED: Action = WindowAction::Restored; break;
case SDL_EVENT_WINDOW_MOUSE_ENTER: Action = WindowAction::MouseEntered; break;
case SDL_EVENT_WINDOW_MOUSE_LEAVE: Action = WindowAction::MouseLeft; break;
case SDL_EVENT_WINDOW_FOCUS_GAINED: Action = WindowAction::FocusGained; break;
case SDL_EVENT_WINDOW_FOCUS_LOST: Action = WindowAction::FocusLost; break;
case SDL_EVENT_WINDOW_CLOSE_REQUESTED: Action = WindowAction::CloseRequested; break;
case SDL_EVENT_WINDOW_DISPLAY_CHANGED: Action = WindowAction::DisplayChanged; break;
case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: Action = WindowAction::ScaleChanged; break;
case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: Action = WindowAction::EnterFullscreen; break;
case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: Action = WindowAction::LeaveFullscreen; break;
case SDL_EVENT_WINDOW_DESTROYED: Action = WindowAction::Destroyed; break;
default: return false;
}
return true;
}

DisplayAction translateDisplayAction(Uint32 Source)
{
switch (Source)
{
case SDL_EVENT_DISPLAY_ADDED: return DisplayAction::Added;
case SDL_EVENT_DISPLAY_REMOVED: return DisplayAction::Removed;
case SDL_EVENT_DISPLAY_MOVED: return DisplayAction::Moved;
case SDL_EVENT_DISPLAY_ORIENTATION: return DisplayAction::Orientation;
case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED: return DisplayAction::DesktopMode;
case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED: return DisplayAction::CurrentMode;
case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED: return DisplayAction::Scale;
default: return DisplayAction::Bounds;
}
}

DropAction translateDropAction(Uint32 Source)
{
switch (Source)
{
case SDL_EVENT_DROP_BEGIN: return DropAction::Begin;
case SDL_EVENT_DROP_FILE: return DropAction::File;
case SDL_EVENT_DROP_TEXT: return DropAction::Text;
case SDL_EVENT_DROP_POSITION: return DropAction::Position;
default: return DropAction::Complete;
}
}

WindowId getEventWindow(const SDL_Event& Source)
{
if (Source.type >= SDL_EVENT_WINDOW_FIRST && Source.type <= SDL_EVENT_WINDOW_LAST)
{
return Source.window.windowID;
}
if (Source.type >= SDL_EVENT_USER && Source.type < SDL_EVENT_LAST)
{
return Source.user.windowID;
}
switch (Source.type)
{
case SDL_EVENT_KEY_DOWN:
case SDL_EVENT_KEY_UP:
    return Source.key.windowID;
case SDL_EVENT_TEXT_EDITING:
    return Source.edit.windowID;
case SDL_EVENT_TEXT_INPUT:
    return Source.text.windowID;
case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    return Source.edit_candidates.windowID;
case SDL_EVENT_SCREEN_KEYBOARD_SHOWN:
case SDL_EVENT_SCREEN_KEYBOARD_HIDDEN:
    return Source.window.windowID;
case SDL_EVENT_MOUSE_MOTION:
    return Source.motion.windowID;
case SDL_EVENT_MOUSE_BUTTON_DOWN:
case SDL_EVENT_MOUSE_BUTTON_UP:
    return Source.button.windowID;
case SDL_EVENT_MOUSE_WHEEL:
    return Source.wheel.windowID;
case SDL_EVENT_DROP_BEGIN:
case SDL_EVENT_DROP_FILE:
case SDL_EVENT_DROP_TEXT:
case SDL_EVENT_DROP_POSITION:
case SDL_EVENT_DROP_COMPLETE:
    return Source.drop.windowID;
case SDL_EVENT_FINGER_DOWN:
case SDL_EVENT_FINGER_UP:
case SDL_EVENT_FINGER_MOTION:
case SDL_EVENT_FINGER_CANCELED:
    return Source.tfinger.windowID;
case SDL_EVENT_PINCH_BEGIN:
case SDL_EVENT_PINCH_UPDATE:
case SDL_EVENT_PINCH_END:
    return Source.pinch.windowID;
case SDL_EVENT_PEN_PROXIMITY_IN:
case SDL_EVENT_PEN_PROXIMITY_OUT:
    return Source.pproximity.windowID;
case SDL_EVENT_PEN_DOWN:
case SDL_EVENT_PEN_UP:
    return Source.ptouch.windowID;
case SDL_EVENT_PEN_BUTTON_DOWN:
case SDL_EVENT_PEN_BUTTON_UP:
    return Source.pbutton.windowID;
case SDL_EVENT_PEN_MOTION:
    return Source.pmotion.windowID;
case SDL_EVENT_PEN_AXIS:
    return Source.paxis.windowID;
case SDL_EVENT_RENDER_TARGETS_RESET:
case SDL_EVENT_RENDER_DEVICE_RESET:
case SDL_EVENT_RENDER_DEVICE_LOST:
    return Source.render.windowID;
default:
    return 0;
}
}
}

Event translateEvent(const SDL_Event& Source)
{
Event Result{Source.common.timestamp, getEventWindow(Source), UnhandledEvent{Source.type}};
WindowAction Action = WindowAction::Other;
if (canTranslateWindowAction(Source.type, Action))
{
    Result.Payload = WindowEvent{Action, Source.window.data1, Source.window.data2};
    return Result;
}

switch (Source.type)
{
    case SDL_EVENT_QUIT:
        Result.Payload = QuitEvent{};
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        Result.Payload = KeyEvent {
            static_cast<PhysicalKey>(Source.key.scancode),
            translateCharacter(Source.key.key), 
            translateModifiers(Source.key.mod),
            Source.key.which, 
            Source.type == SDL_EVENT_KEY_DOWN,
            Source.key.repeat
        };
        break;
    case SDL_EVENT_TEXT_INPUT:
        Result.Payload = TextInputEvent{copyText(Source.text.text)};
        break;
    case SDL_EVENT_TEXT_EDITING:
        Result.Payload = TextEditingEvent{
            copyText(Source.edit.text), Source.edit.start, Source.edit.length};
        break;
    case SDL_EVENT_TEXT_EDITING_CANDIDATES:
    {
        TextCandidatesEvent Candidates;
        Candidates.Selected = Source.edit_candidates.selected_candidate;
        Candidates.Horizontal = Source.edit_candidates.horizontal;
        if (Source.edit_candidates.candidates && Source.edit_candidates.num_candidates > 0)
        {
            Candidates.Candidates.reserve(
                static_cast<std::size_t>(Source.edit_candidates.num_candidates));
            for (int Index = 0; Index < Source.edit_candidates.num_candidates; ++Index)
            {
                Candidates.Candidates.push_back(copyText(Source.edit_candidates.candidates[Index]));
            }
        }
        Result.Payload = std::move(Candidates);
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        Result.Payload = MouseMoveEvent{
            Source.motion.which, translateButtons(Source.motion.state),
            Source.motion.x, Source.motion.y, Source.motion.xrel, Source.motion.yrel};
        break;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        Result.Payload = MouseButtonEvent{
            Source.button.which, translateButton(Source.button.button),
            Source.button.button, Source.button.clicks,
            Source.type == SDL_EVENT_MOUSE_BUTTON_DOWN, Source.button.x, Source.button.y};
        break;
    case SDL_EVENT_MOUSE_WHEEL:
    {
        const bool WasFlipped = Source.wheel.direction == SDL_MOUSEWHEEL_FLIPPED;
        const float Direction = WasFlipped ? -1.0f : 1.0f;
        Result.Payload = MouseWheelEvent{
            Source.wheel.which, Source.wheel.x * Direction, Source.wheel.y * Direction,
            Source.wheel.mouse_x, Source.wheel.mouse_y, WasFlipped};
        break;
    }
    case SDL_EVENT_DROP_BEGIN:
    case SDL_EVENT_DROP_FILE:
    case SDL_EVENT_DROP_TEXT:
    case SDL_EVENT_DROP_POSITION:
    case SDL_EVENT_DROP_COMPLETE:
        // @note 这些指针由 SDL 拥有；只复制内容，不释放指针。
        Result.Payload = DropEvent{
            translateDropAction(Source.type), copyText(Source.drop.data),
            copyText(Source.drop.source), Source.drop.x, Source.drop.y};
        break;
    case SDL_EVENT_DISPLAY_ADDED:
    case SDL_EVENT_DISPLAY_REMOVED:
    case SDL_EVENT_DISPLAY_MOVED:
    case SDL_EVENT_DISPLAY_ORIENTATION:
    case SDL_EVENT_DISPLAY_DESKTOP_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
    case SDL_EVENT_DISPLAY_CONTENT_SCALE_CHANGED:
    case SDL_EVENT_DISPLAY_USABLE_BOUNDS_CHANGED:
        Result.Payload = DisplayEvent{
            translateDisplayAction(Source.type), Source.display.displayID,
            Source.display.data1, Source.display.data2};
        break;
    case SDL_EVENT_CLIPBOARD_UPDATE:
        Result.Payload = ClipboardEvent{};
        break;
    case SDL_EVENT_KEYBOARD_ADDED:
    case SDL_EVENT_KEYBOARD_REMOVED:
        Result.Payload = DeviceEvent{
            DeviceKind::Keyboard, Source.kdevice.which, Source.type == SDL_EVENT_KEYBOARD_ADDED};
        break;
    case SDL_EVENT_MOUSE_ADDED:
    case SDL_EVENT_MOUSE_REMOVED:
        Result.Payload = DeviceEvent{
            DeviceKind::Mouse, Source.mdevice.which, Source.type == SDL_EVENT_MOUSE_ADDED};
        break;
    case SDL_EVENT_KEYMAP_CHANGED:
        Result.Payload = KeymapChangedEvent{};
        break;
    default:
        break;
}
return Result;
}
}
