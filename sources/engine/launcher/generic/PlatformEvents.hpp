#pragma once

#include <any>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

namespace ui
{
using WindowId = uint32_t; // @note 不透明的窗口标识；零表示无窗口。
using DeviceId = uint32_t;

// @note 使用 USB HID 键盘用途编码而非 SDL 枚举；未命名的用途编码仍可表示。
enum class PhysicalKey : uint32_t
{
    Unknown = 0, A = 4, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P,
    Q, R, S, T, U, V, W, X, Y, Z,
    Digit1 = 30, Digit2, Digit3, Digit4, Digit5, Digit6, Digit7, Digit8, Digit9, Digit0,
    Enter = 40, Escape, Backspace, Tab, Space,
    F1 = 58, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
    Insert = 73, Home, PageUp, Delete, End, PageDown, Right, Left, Down, Up,
    LeftControl = 224, LeftShift, LeftAlt, LeftSuper,
    RightControl, RightShift, RightAlt, RightSuper
};
enum class Modifiers : uint16_t
{
    None = 0,
    Shift = 1,
    Control = 2,
    Alt = 4,
    Super = 8,
    CapsLock = 16,
    NumLock = 32,
    AltGraph = 64
};
constexpr Modifiers operator|(Modifiers A, Modifiers B)
{
    return static_cast<Modifiers>(static_cast<uint16_t>(A) | static_cast<uint16_t>(B));
}
constexpr bool hasModifier(Modifiers Value, Modifiers Flag)
{
    return (static_cast<uint16_t>(Value) & static_cast<uint16_t>(Flag)) != 0;
}

struct QuitEvent {};
enum class WindowAction
{
    Shown, Hidden, Exposed, Moved, Resized, PixelSizeChanged, Minimized,
    Maximized, Restored, MouseEntered, MouseLeft, FocusGained, FocusLost,
    CloseRequested, DisplayChanged, ScaleChanged, EnterFullscreen,
    LeaveFullscreen, Destroyed, Other, SafeAreaChanged
};
struct WindowEvent
{
    WindowAction Action;
    int Data1{}, Data2{};
};
struct KeyEvent
{
    PhysicalKey Physical{PhysicalKey::Unknown};
    char32_t Character{};
    ui::Modifiers Modifiers{ui::Modifiers::None};
    DeviceId Device{};
    bool Pressed{}, Repeat{};
};
struct TextInputEvent
{
    std::string Text;
};
struct TextEditingEvent
{
    std::string Text;
    int Start{}, Length{};
};
struct TextCandidatesEvent
{
    std::vector<std::string> Candidates;
    int Selected{-1};
    bool Horizontal{};
};
enum class MouseButton : uint8_t
{
    Unknown,
    Left,
    Middle,
    Right,
    Back,
    Forward
};
enum class MouseButtons : uint8_t
{
    None = 0,
    Left = 1,
    Middle = 2,
    Right = 4,
    Back = 8,
    Forward = 16
};
constexpr bool hasButton(MouseButtons Value, MouseButtons Flag)
{
    return (static_cast<uint8_t>(Value) & static_cast<uint8_t>(Flag)) != 0;
}
struct MouseMoveEvent
{
    DeviceId Device{};
    MouseButtons Buttons{MouseButtons::None};
    float X{}, Y{}, DX{}, DY{}; // @note 使用窗口逻辑坐标，而非帧缓冲像素坐标。
};
struct MouseButtonEvent
{
    DeviceId Device{};
    MouseButton Button{MouseButton::Unknown};
    uint8_t NativeButton{}, Clicks{}; // @note 保留额外鼠标按键的原生标识。
    bool Pressed{};
    float X{}, Y{};
};
struct MouseWheelEvent
{
    DeviceId Device{};
    float X{}, Y{}, MouseX{}, MouseY{}; // @note 已统一滚动方向，并保留小数滚动量。
    bool WasFlipped{};
};
enum class DropAction { Begin, File, Text, Position, Complete };
struct DropEvent
{
    DropAction Action;
    std::string Data, Source;
    float X{}, Y{};
};
enum class DisplayAction { Added, Removed, Moved, Orientation, DesktopMode, CurrentMode, Scale, Bounds };
struct DisplayEvent
{
    DisplayAction Action;
    uint32_t Display{};
    int Data1{}, Data2{};
};
struct ClipboardEvent {};
enum class DeviceKind { Keyboard, Mouse };
struct DeviceEvent
{
    DeviceKind Kind;
    DeviceId Device{};
    bool Added{};
};
struct KeymapChangedEvent {};
struct CustomEvent
{
    std::string Name;
    std::any Payload;
};
struct UnhandledEvent
{
    uint32_t BackendType{};
};
using EventPayload = std::variant<QuitEvent, WindowEvent, KeyEvent, TextInputEvent,
TextEditingEvent, TextCandidatesEvent, MouseMoveEvent, MouseButtonEvent,
MouseWheelEvent, DropEvent, DisplayEvent, ClipboardEvent, DeviceEvent,
KeymapChangedEvent, CustomEvent, UnhandledEvent>;
struct Event
{
    uint64_t TimestampNs{};
    WindowId Window{};
    EventPayload Payload{UnhandledEvent{}};

    template<class T>
    const T* get() const noexcept
    {
        return std::get_if<T>(&Payload);
    }
};
}
