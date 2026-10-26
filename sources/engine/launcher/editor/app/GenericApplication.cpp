#include <infra/common/Error.hpp>
#include "GenericApplication.hpp"
#include "Unicode.hpp"
#include "PlatformEvents.hpp"
#include "FontService.hpp"
#include "SystemFonts.hpp"

#include <print>
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace ui
{
using runtime::platform::CustomEvent;
using runtime::platform::DropEvent;
using runtime::platform::Event;
using runtime::platform::hasModifier;
using runtime::platform::KeyEvent;
using runtime::platform::Modifiers;
using runtime::platform::MouseButtonEvent;
using runtime::platform::MouseMoveEvent;
using runtime::platform::MouseWheelEvent;
using runtime::platform::PhysicalKey;
using runtime::platform::QuitEvent;
using runtime::platform::TextEditingEvent;
using runtime::platform::TextInputEvent;
using runtime::platform::WindowAction;
using runtime::platform::WindowEvent;

GenericApplication& GenericApplication::self()
{
    FontService::self();
    static GenericApplication Instance;
    return Instance;
}

GenericApplication::~GenericApplication()
{
    shutdown();
}

void GenericApplication::shutdown()
{
    MainWindow.reset();
    FontService::self().shutdown();
    Renderer.reset();
    Platform = nullptr;
}

bool GenericApplication::isInitialized() const noexcept
{
    return MainWindow != nullptr;
}

void GenericApplication::checkInitialized() const
{
    if (!isInitialized())
    {
        Error::raise(EErrorCode::Logic, "Application is not initialized");
    }
}

void GenericApplication::initialize(IGenericPlatform& InPlatform, ApplicationOptions InOptions)
{
    if (isInitialized())
    {
        Error::raise(EErrorCode::Logic, "Application is already initialized");
    }
    if (InOptions.FrameLimit < 0)
    {
        Error::raise(EErrorCode::InvalidArgument, "Frame limit must be nonnegative");
    }
    Platform = &InPlatform;
#ifndef EDITOR_TEST_NO_GPU
    {
        if (InOptions.DiscoverSystemFonts &&
            (InOptions.DefaultFontPaths.empty() || InOptions.EmojiFontPaths.empty()))
        {
            auto Fonts = discoverSystemFonts();
            if (InOptions.DefaultFontPaths.empty()) InOptions.DefaultFontPaths = std::move(Fonts.DefaultFontPaths);
            if (InOptions.EmojiFontPaths.empty()) InOptions.EmojiFontPaths = std::move(Fonts.EmojiFontPaths);
        }
        if (InOptions.FontPath.empty())
        {
            for (const auto& Path : InOptions.DefaultFontPaths)
            {
                if (std::filesystem::is_regular_file(Path))
                {
                    InOptions.FontPath = Path;
                    break;
                }
            }
            if (InOptions.FontPath.empty())
                Error::raise(EErrorCode::Runtime, "No default font found; provide --font PATH");
        }
        if (InOptions.Text.FallbackFontPaths.empty())
        {
            const auto AddFallbacks = [&](const std::vector<std::string>& Candidates)
            {
                for (const auto& Path : Candidates)
                {
                    if (Path != InOptions.FontPath && std::filesystem::is_regular_file(Path) &&
                        std::ranges::find(InOptions.Text.FallbackFontPaths, Path) == InOptions.Text.FallbackFontPaths.end())
                        InOptions.Text.FallbackFontPaths.push_back(Path);
                }
            };
            AddFallbacks(InOptions.DefaultFontPaths);
            AddFallbacks(InOptions.EmojiFontPaths);
        }
    }
#endif
    auto NewWindow = std::make_unique<Window>(InPlatform);
    std::shared_ptr<RHIUIRenderer> NewRenderer;
#ifndef EDITOR_TEST_NO_GPU
    NewRenderer = std::make_shared<RHIUIRenderer>();
    NewRenderer->setUIMode(InOptions.UIMode);
    FontService::self().initialize(NewRenderer);
#endif
    Options = std::move(InOptions);
#ifndef EDITOR_TEST_NO_GPU
    // 把 App 的全局配置/字体/回退字体写入 FontService, 供 TextWidget 的默认钩子读取.
    FontService::self().setDefaultTextConfig(Options.Text);
    FontService::self().setDefaultFont(FontService::self().loadFont(Options.FontPath));
    std::vector<FontHandle> DefaultFallbacks;
    DefaultFallbacks.reserve(Options.Text.FallbackFontPaths.size());
    for (const auto& Path : Options.Text.FallbackFontPaths)
    {
        DefaultFallbacks.push_back(FontService::self().loadFont(Path));
    }
    FontService::self().setDefaultFallbackFonts(std::move(DefaultFallbacks));
#endif
    Renderer = std::move(NewRenderer);
    MainWindow = std::move(NewWindow);
}

Window& GenericApplication::getMainWindow()
{
    checkInitialized();
    return *MainWindow;
}

const Window& GenericApplication::getMainWindow() const
{
    checkInitialized();
    return *MainWindow;
}

const ApplicationOptions& GenericApplication::getOptions() const
{
    checkInitialized();
    return Options;
}

std::unique_ptr<TextWidget> GenericApplication::createTextWidget(std::string_view Text, float Size, float MaxWidth, Color Tint)
{
    getRenderer();
    auto Widget = std::make_unique<TextWidget>();
    Widget->setFont(getCurrentFontPath());
    Widget->setFontSize(Size);
    Widget->setPixelScale(MainWindow->getDisplayScale());
    Widget->setMaxWidth(MaxWidth);
    Widget->setColor(Tint);
    Widget->setText(std::move(Text));
    return Widget;
}

const std::string& GenericApplication::getCurrentFontPath() const
{
    return getOptions().FontPath;
}

TextEncoding GenericApplication::getCurrentTextEncoding() const
{
    return getOptions().Text.Encoding;
}

void GenericApplication::setCurrentTextEncoding(TextEncoding Encoding)
{
    checkInitialized();
    switch (Encoding)
    {
    case TextEncoding::UTF8:
    case TextEncoding::UTF16:
    case TextEncoding::UTF32: break;
    case TextEncoding::Custom:
        if (!Options.Text.CustomDecoder) Error::raise(EErrorCode::InvalidArgument, "Custom encoding requires a decoder");
        break;
    default: Error::raise(EErrorCode::InvalidArgument, "Unknown text encoding");
    }
    Options.Text.Encoding = Encoding;
    FontService::self().setDefaultTextConfig(Options.Text);
}

std::string GenericApplication::encodeText(std::string_view UTF8) const
{
    return ui::encodeText(UTF8, getOptions().Text);
}

std::string GenericApplication::encodeText(std::u32string_view UTF32) const
{
    return ui::encodeText(UTF32, getOptions().Text);
}

const std::vector<std::string>& GenericApplication::getFallbackFontPaths() const
{
    return getOptions().Text.FallbackFontPaths;
}

IUIRenderer& GenericApplication::getRenderer()
{
    checkInitialized();
    if (!Renderer)
    {
        Error::raise(EErrorCode::Logic, "Renderer is unavailable in the no-GPU test build");
    }
    return *Renderer;
}

void GenericApplication::setFrameHook(std::function<void()> Hook)
{
    FrameHook = std::move(Hook);
}

void GenericApplication::setKeyHandler(std::function<bool(const runtime::platform::KeyEvent&)> Handler)
{
    KeyHandler = std::move(Handler);
}

void GenericApplication::start()
{
    auto* Window = &MainWindow->getNativeWindow();
    Window->startTextInput();
    Window->setTextInputArea({16, 16, 600, 30});
    const auto Logical = Window->getSize();
    const auto Pixels = Window->getPixelSize();

    std::println("Window {}: {}x{}, pixels {}x{}. Esc: quit, F11: fullscreen, Ctrl+V: clipboard; type, resize, scroll or drop files.", Window->getId(), Logical.Width, Logical.Height, Pixels.Width, Pixels.Height);

    Platform->postEvent({"editor.ready", std::string("Window ready")}, Window->getId());
}

bool GenericApplication::pumpEvents()
{
    auto* Window = &MainWindow->getNativeWindow();
    bool Running = true;
    while (auto Event = Platform->pollEvent())
    {
        if (Event->get<QuitEvent>())
        {
            Running = false;
        }
        if (Event->Window != 0 && Event->Window != Window->getId())
        {
            continue;
        }
        if (const auto* Change = Event->get<WindowEvent>())
        {
            if (Change->Action == WindowAction::CloseRequested)
            {
                Running = false;
            }
            std::println("Window event {}: {}, {}", static_cast<int32_t>(Change->Action), Change->Data1, Change->Data2);

        }
        else if (const auto* Key = Event->get<KeyEvent>())
        {
            std::println("Key {}: {} {} {}", static_cast<int32_t>(Key->Physical), Key->Pressed ? "down" : "up", Key->Repeat ? "repeat" : "", hasModifier(Key->Modifiers, Modifiers::Control) ? "Ctrl" : "");
            const auto KeyReply = MainWindow->dispatchKey(*Key);

            if (KeyReply == EventReply::Unhandled && Key->Pressed && !Key->Repeat)
            {
                if (KeyHandler && KeyHandler(*Key))
                {
                    // 编辑器宿主消费了该键(Play/Stop/Pause/Step/慢放/Simulate 等), 跳过默认处理.
                }
                else
                {
                    if (Key->Physical == PhysicalKey::Escape)
                    {
                        Running = false;
                    }
                    if (Key->Physical == PhysicalKey::F11)
                    {
                        Fullscreen = !Fullscreen;
                        Window->setFullscreen(Fullscreen);
                    }
                    if (Key->Physical == PhysicalKey::V && hasModifier(Key->Modifiers, Modifiers::Control))
                    {
                        std::println("Clipboard: {}", Platform->getClipboardText());
                    }
                }
            }
        }
        else if (const auto* Text = Event->get<TextInputEvent>())
        {
            std::println("Text: {}", Text->Text);
            Window->setTitle("Generic Editor | " + Text->Text);
        }
        else if (const auto* Composition = Event->get<TextEditingEvent>())
        {
            std::println("IME: {}", Composition->Text);
        }
        else if (const auto* Move = Event->get<MouseMoveEvent>())
        {
            MainWindow->dispatchMouseMove(*Move);
            Window->setTitle("Generic Editor | mouse " + std::to_string(static_cast<int32_t>(Move->X))
                             + ", " + std::to_string(static_cast<int32_t>(Move->Y)));
        }
        else if (const auto* Click = Event->get<MouseButtonEvent>())
        {
            MainWindow->dispatchMouseButton(*Click);
            std::println("Mouse button {}{}", static_cast<int32_t>(Click->NativeButton), Click->Pressed ? " down" : " up");
        }
        else if (const auto* Wheel = Event->get<MouseWheelEvent>())
        {
            MainWindow->dispatchMouseWheel(*Wheel);
            std::println("Wheel: {} {}", Wheel->X, Wheel->Y);
        }
        else if (const auto* Drop = Event->get<DropEvent>())
        {
            std::println("Drop {}: {}", static_cast<int32_t>(Drop->Action), Drop->Data);
        }
        else if (const auto* Custom = Event->get<CustomEvent>())
        {
            std::println("Custom: {}", Custom->Name);
        }
    }
    return Running;
}

void GenericApplication::tick()
{
    if (!MainWindow->drawFrame(Renderer.get()))
    {
        Platform->delay(16);
    }
    if (FrameHook)
    {
        FrameHook();
    }
}

int32_t GenericApplication::run()
{
    checkInitialized();
    start();
    int32_t Frames = 0;
    while (pumpEvents() && (Options.FrameLimit == 0 || Frames < Options.FrameLimit))
    {
        tick();
        ++Frames;
    }
    MainWindow->getNativeWindow().stopTextInput();
    std::cout << "Completed " << Frames << " frames\n";
    return 0;
}
}
