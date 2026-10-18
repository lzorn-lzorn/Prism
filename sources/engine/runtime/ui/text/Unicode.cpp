#include <infra/common/Error.hpp>
#include "Unicode.hpp"

#include <bit>
#include <cstdint>
#include <stdexcept>

namespace
{
void validateScalar(std::uint32_t Scalar)
{
    if (Scalar > 0x10ffff || (Scalar >= 0xd800 && Scalar <= 0xdfff))
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid Unicode scalar");
    }
}

bool isLittleEndian(ui::TextByteOrder ByteOrder)
{
    switch (ByteOrder)
    {
    case ui::TextByteOrder::Native:
        if constexpr (std::endian::native == std::endian::little) return true;
        if constexpr (std::endian::native == std::endian::big) return false;
        Error::raise(EErrorCode::InvalidArgument, "Unsupported native byte order");
    case ui::TextByteOrder::LittleEndian: return true;
    case ui::TextByteOrder::BigEndian: return false;
    }
    Error::raise(EErrorCode::InvalidArgument, "Unknown byte order");
}

std::uint32_t readUnit(std::string_view Text, std::size_t Offset,
    std::size_t UnitSize, bool LittleEndian)
{
    std::uint32_t Value = 0;
    for (std::size_t Byte = 0; Byte < UnitSize; ++Byte)
    {
        const auto Shift = LittleEndian ? Byte : UnitSize - 1 - Byte;
        Value |= static_cast<std::uint32_t>(static_cast<unsigned char>(Text[Offset + Byte])) << (Shift * 8);
    }
    return Value;
}

void appendUnit(std::string& Result, std::uint32_t Unit, std::size_t UnitSize, bool LittleEndian)
{
    for (std::size_t Byte = 0; Byte < UnitSize; ++Byte)
    {
        const auto Shift = LittleEndian ? Byte : UnitSize - 1 - Byte;
        Result.push_back(static_cast<char>((Unit >> (Shift * 8)) & 0xff));
    }
}
}

namespace ui
{
std::u32string utf8ToUTF32(std::string_view Text)
{
    std::u32string Result;
    for (std::size_t Index = 0; Index < Text.size();)
    {
        const auto Lead = static_cast<unsigned char>(Text[Index++]);
        std::size_t Continuations = 0;
        std::uint32_t Scalar = Lead;
        std::uint32_t Minimum = 0;
        if (Lead <= 0x7f)
        {
            Result.push_back(static_cast<char32_t>(Scalar));
            continue;
        }
        if (Lead >= 0xc2 && Lead <= 0xdf)
        {
            Continuations = 1;
            Scalar = Lead & 0x1f;
            Minimum = 0x80;
        }
        else if (Lead >= 0xe0 && Lead <= 0xef)
        {
            Continuations = 2;
            Scalar = Lead & 0x0f;
            Minimum = 0x800;
        }
        else if (Lead >= 0xf0 && Lead <= 0xf4)
        {
            Continuations = 3;
            Scalar = Lead & 0x07;
            Minimum = 0x10000;
        }
        else
        {
            Error::raise(EErrorCode::InvalidArgument, "Invalid UTF-8 leading byte");
        }

        if (Text.size() - Index < Continuations)
        {
            Error::raise(EErrorCode::InvalidArgument, "Truncated UTF-8 sequence");
        }
        for (std::size_t Offset = 0; Offset < Continuations; ++Offset)
        {
            const auto Byte = static_cast<unsigned char>(Text[Index++]);
            if ((Byte & 0xc0) != 0x80)
            {
                Error::raise(EErrorCode::InvalidArgument, "Invalid UTF-8 continuation byte");
            }
            Scalar = (Scalar << 6) | (Byte & 0x3f);
        }
        if (Scalar < Minimum)
        {
            Error::raise(EErrorCode::InvalidArgument, "Overlong UTF-8 sequence");
        }
        validateScalar(Scalar);
        Result.push_back(static_cast<char32_t>(Scalar));
    }
    return Result;
}

std::u32string utf16ToUTF32(std::u16string_view Text)
{
    std::u32string Result;
    for (std::size_t Index = 0; Index < Text.size(); ++Index)
    {
        auto Scalar = static_cast<std::uint32_t>(Text[Index]);
        if (Scalar >= 0xd800 && Scalar <= 0xdbff)
        {
            if (++Index == Text.size())
            {
                Error::raise(EErrorCode::InvalidArgument, "Truncated UTF-16 surrogate pair");
            }
            const auto Low = static_cast<std::uint32_t>(Text[Index]);
            if (Low < 0xdc00 || Low > 0xdfff)
            {
                Error::raise(EErrorCode::InvalidArgument, "Invalid UTF-16 surrogate pair");
            }
            Scalar = 0x10000 + ((Scalar - 0xd800) << 10) + Low - 0xdc00;
        }
        validateScalar(Scalar);
        Result.push_back(static_cast<char32_t>(Scalar));
    }
    return Result;
}

void validateUTF32(std::u32string_view Text)
{
    for (const auto Scalar : Text)
    {
        validateScalar(static_cast<std::uint32_t>(Scalar));
    }
}

std::string utf32ToUTF8(std::u32string_view Text)
{
    validateUTF32(Text);
    std::string Result;
    for (const auto Character : Text)
    {
        const auto Scalar = static_cast<std::uint32_t>(Character);
        if (Scalar <= 0x7f)
        {
            Result.push_back(static_cast<char>(Scalar));
        }
        else if (Scalar <= 0x7ff)
        {
            Result.push_back(static_cast<char>(0xc0 | (Scalar >> 6)));
            Result.push_back(static_cast<char>(0x80 | (Scalar & 0x3f)));
        }
        else if (Scalar <= 0xffff)
        {
            Result.push_back(static_cast<char>(0xe0 | (Scalar >> 12)));
            Result.push_back(static_cast<char>(0x80 | ((Scalar >> 6) & 0x3f)));
            Result.push_back(static_cast<char>(0x80 | (Scalar & 0x3f)));
        }
        else
        {
            Result.push_back(static_cast<char>(0xf0 | (Scalar >> 18)));
            Result.push_back(static_cast<char>(0x80 | ((Scalar >> 12) & 0x3f)));
            Result.push_back(static_cast<char>(0x80 | ((Scalar >> 6) & 0x3f)));
            Result.push_back(static_cast<char>(0x80 | (Scalar & 0x3f)));
        }
    }
    return Result;
}

std::u16string utf32ToUTF16(std::u32string_view Text)
{
    validateUTF32(Text);
    std::u16string Result;
    for (const auto Character : Text)
    {
        auto Scalar = static_cast<std::uint32_t>(Character);
        if (Scalar >= 0x10000)
        {
            Scalar -= 0x10000;
            Result.push_back(static_cast<char16_t>(0xd800 + (Scalar >> 10)));
            Result.push_back(static_cast<char16_t>(0xdc00 + (Scalar & 0x3ff)));
        }
        else
        {
            Result.push_back(static_cast<char16_t>(Scalar));
        }
    }
    return Result;
}

std::u16string utf8ToUTF16(std::string_view Text)
{
    return utf32ToUTF16(utf8ToUTF32(Text));
}

std::string utf16ToUTF8(std::u16string_view Text)
{
    return utf32ToUTF8(utf16ToUTF32(Text));
}

std::u32string decodeTextToUTF32(std::string_view Text, const TextConfig& Config)
{
    switch (Config.Encoding)
    {
    case TextEncoding::UTF8:
        return utf8ToUTF32(Text);
    case TextEncoding::UTF16:
    {
        if (Text.size() % 2 != 0) Error::raise(EErrorCode::InvalidArgument, "Truncated Unicode code unit");
        const bool LittleEndian = isLittleEndian(Config.ByteOrder);
        std::u16string Units;
        Units.reserve(Text.size() / 2);
        for (std::size_t Offset = 0; Offset < Text.size(); Offset += 2)
        {
            Units.push_back(static_cast<char16_t>(readUnit(Text, Offset, 2, LittleEndian)));
        }
        return utf16ToUTF32(Units);
    }
    case TextEncoding::UTF32:
    {
        if (Text.size() % 4 != 0) Error::raise(EErrorCode::InvalidArgument, "Truncated Unicode code unit");
        const bool LittleEndian = isLittleEndian(Config.ByteOrder);
        std::u32string Result;
        Result.reserve(Text.size() / 4);
        for (std::size_t Offset = 0; Offset < Text.size(); Offset += 4)
        {
            Result.push_back(static_cast<char32_t>(readUnit(Text, Offset, 4, LittleEndian)));
        }
        validateUTF32(Result);
        return Result;
    }
    case TextEncoding::Custom:
        if (!Config.CustomDecoder) Error::raise(EErrorCode::InvalidArgument, "Custom encoding requires a decoder");
        return utf8ToUTF32(Config.CustomDecoder(Text));
    }
    Error::raise(EErrorCode::InvalidArgument, "Unknown text encoding");
}

std::string encodeText(std::u32string_view Text, const TextConfig& Config)
{
    validateUTF32(Text);
    if (Config.Encoding == TextEncoding::UTF8) return utf32ToUTF8(Text);
    if (Config.Encoding != TextEncoding::UTF16 && Config.Encoding != TextEncoding::UTF32)
    {
        Error::raise(EErrorCode::InvalidArgument, "Encoding text requires a supported Unicode target encoding");
    }
    const bool LittleEndian = isLittleEndian(Config.ByteOrder);
    std::string Result;
    if (Config.Encoding == TextEncoding::UTF16)
    {
        for (const auto Unit : utf32ToUTF16(Text))
        {
            appendUnit(Result, static_cast<std::uint32_t>(Unit), 2, LittleEndian);
        }
    }
    else
    {
        for (const auto Unit : Text)
        {
            appendUnit(Result, static_cast<std::uint32_t>(Unit), 4, LittleEndian);
        }
    }
    return Result;
}
}