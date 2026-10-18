#pragma once

#include <cstdint>

namespace ui
{
struct FontHandle
{
    std::uint64_t Value{};
    bool operator==(const FontHandle&) const = default;
};
}
