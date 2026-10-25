#include <infra/common/Error.hpp>
#include "IGenericPlatform.hpp"
#include "SdlInternal.hpp"

namespace runtime::platform
{
std::unique_ptr<IGenericPlatform> createPlatform(PlatformBackend Backend)
{
    switch (Backend)
    {
        case PlatformBackend::Auto:
        case PlatformBackend::Sdl:
            return std::make_unique<SdlPlatform>();
        case PlatformBackend::Win32:
            Error::raise(EErrorCode::Runtime, "Win32 platform backend is not implemented");
        case PlatformBackend::AppKit:
            Error::raise(EErrorCode::Runtime, "AppKit platform backend is not implemented");
    }
    Error::raise(EErrorCode::InvalidArgument, "Unknown platform backend");
}
}
