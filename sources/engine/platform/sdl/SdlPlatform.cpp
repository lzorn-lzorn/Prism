#include <infra/common/Error.hpp>
#include "SdlInternal.hpp"

namespace runtime::platform
{
namespace
{
std::weak_ptr<detail::UIRuntime> ActiveRuntime;
}

detail::UIRuntime::UIRuntime()
{
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        const auto Failure = detail::makeError("SDL_Init");
        SDL_Quit();
        throw Failure;
    }
}

detail::UIRuntime::~UIRuntime()
{
    SDL_Quit();
}

SdlPlatform::SdlPlatform()
{
    if (!ActiveRuntime.expired())
    {
        Error::raise(EErrorCode::Runtime, "Only one SDL platform runtime may be active");
    }
    Runtime = std::make_shared<detail::UIRuntime>();
    ActiveRuntime = Runtime;
}

PlatformBackend SdlPlatform::getBackend() const noexcept
{
    return PlatformBackend::Sdl;
}

std::unique_ptr<IGenericWindow> SdlPlatform::createWindow(const WindowConfigs& Configs)
{
    return std::make_unique<SdlWindow>(Runtime, Configs);
}

std::optional<Event> SdlPlatform::pollEvent()
{
    if (!Posted.empty())
    {
        Event Result = std::move(Posted.front());
        Posted.pop_front();
        return Result;
    }
    SDL_Event Source{};
    if (!SDL_PollEvent(&Source))
    {
        return std::nullopt;
    }
    return detail::translateEvent(Source);
}

void SdlPlatform::postEvent(CustomEvent InEvent, WindowId InWindowId)
{
    Posted.push_back(Event{SDL_GetTicksNS(), InWindowId, std::move(InEvent)});
}

std::string SdlPlatform::getClipboardText() const
{
    char* Text = SDL_GetClipboardText();
    if (!Text)
    {
        throw detail::makeError("SDL_GetClipboardText");
    }
    const std::unique_ptr<char, decltype(&SDL_free)> Owned(Text, &SDL_free);
    return std::string(Owned.get());
}

void SdlPlatform::setClipboardText(const std::string& Text)
{
    detail::check(SDL_SetClipboardText(Text.c_str()), "SDL_SetClipboardText");
}

void SdlPlatform::delay(std::uint32_t Milliseconds)
{
    SDL_Delay(Milliseconds);
}
}
