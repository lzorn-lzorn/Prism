#pragma once

#include <string>
#include <vector>

namespace ui
{
struct SystemFonts
{
    std::vector<std::string> DefaultFontPaths;
    std::vector<std::string> EmojiFontPaths;
};

SystemFonts discoverSystemFonts();
}