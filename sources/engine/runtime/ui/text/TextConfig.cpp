#include "TextConfig.hpp"
#include "Unicode.hpp"

namespace ui
{
std::string encodeText(std::string_view UTF8, const TextConfig& Config)
{
    return encodeText(utf8ToUTF32(UTF8), Config);
}

std::string decodeText(std::string_view Text, const TextConfig& Config)
{
    return utf32ToUTF8(decodeTextToUTF32(Text, Config));
}
}