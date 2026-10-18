#include <infra/common/Global.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <random>

#if defined(_WIN32)
    #define NOMINMAX
    #include <objbase.h>
#elif defined(__APPLE__) || defined(__linux__)
    #include <uuid/uuid.h>
#endif

namespace infra
{
namespace
{
// 将 16 字节(RFC 4122 顺序, 大端)打包为 GId 的 A/B/C/D 布局.
GId packBytes(const std::uint8_t* Bytes) noexcept
{
    return GId{
        (uint32_t(Bytes[0]) << 24) | (uint32_t(Bytes[1]) << 16) | (uint32_t(Bytes[2]) << 8) | uint32_t(Bytes[3]),
        (uint32_t(Bytes[4]) << 24) | (uint32_t(Bytes[5]) << 16) | (uint32_t(Bytes[6]) << 8) | uint32_t(Bytes[7]),
        (uint32_t(Bytes[8]) << 24) | (uint32_t(Bytes[9]) << 16) | (uint32_t(Bytes[10]) << 8) | uint32_t(Bytes[11]),
        (uint32_t(Bytes[12]) << 24) | (uint32_t(Bytes[13]) << 16) | (uint32_t(Bytes[14]) << 8) | uint32_t(Bytes[15]),
    };
}

// 回退实现(不具 uuid_generate / CoCreateGuid 的平台): 加密级随机数生成 v4 UUID.
GId makeUniqueGuidFromPlatform()
{
    std::random_device Random;
    std::array<std::uint8_t, 16> Bytes{};
    for (auto& Byte : Bytes)
    {
        Byte = static_cast<std::uint8_t>(Random() & 0xFF);
    }
    // RFC 4122: version = 4, variant = 10xx.
    Bytes[6] = static_cast<std::uint8_t>((Bytes[6] & 0x0F) | 0x40);
    Bytes[8] = static_cast<std::uint8_t>((Bytes[8] & 0x3F) | 0x80);
    return packBytes(Bytes.data());
}

// 解析一个十六进制字符, 失败返回 -1.
int getHexValue(char Character) noexcept
{
    if (Character >= '0' && Character <= '9') return Character - '0';
    if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
    if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
    return -1;
}
}

GId GId::makeGuid()
{
#if defined(_WIN32)
    // Windows: CoCreateGuid(ole32), 生成 v4 GUID.
    GUID Guid{};
    if (SUCCEEDED(::CoCreateGuid(&Guid)))
    {
        return GId
        {
            static_cast<uint32_t>(Guid.Data1),
            (static_cast<uint32_t>(Guid.Data2) << 16) | static_cast<uint32_t>(Guid.Data3),
            (static_cast<uint32_t>(Guid.Data4[0]) << 24) | (static_cast<uint32_t>(Guid.Data4[1]) << 16) |
                (static_cast<uint32_t>(Guid.Data4[2]) << 8) | static_cast<uint32_t>(Guid.Data4[3]),
            (static_cast<uint32_t>(Guid.Data4[4]) << 24) | (static_cast<uint32_t>(Guid.Data4[5]) << 16) |
                (static_cast<uint32_t>(Guid.Data4[6]) << 8) | static_cast<uint32_t>(Guid.Data4[7]),
        };
    }
#elif defined(__APPLE__) || defined(__linux__)
    uuid_t Bytes{};
    uuid_generate(Bytes);
    return packBytes(Bytes);
#endif
    return makeUniqueGuidFromPlatform();
}

std::string GId::toString(bool WithBraces) const
{
    char Buffer[64];
    std::snprintf(Buffer, sizeof(Buffer),
                  WithBraces ? "{%08X-%04X-%04X-%04X-%04X%08X}"
                             : "%08X-%04X-%04X-%04X-%04X%08X",
                  static_cast<unsigned int>(A),
                  static_cast<unsigned int>((B >> 16) & 0xFFFF),
                  static_cast<unsigned int>(B & 0xFFFF),
                  static_cast<unsigned int>((C >> 16) & 0xFFFF),
                  static_cast<unsigned int>(C & 0xFFFF),
                  static_cast<unsigned int>(D));
    return std::string(Buffer);
}

bool GId::parse(std::string_view Text, GId& OutGuid)
{
    // 1. 收集 32 个十六进制字符(忽略连字符 / 大括号 / 空白).
    char Hex[32];
    std::size_t Count = 0;
    for (const char Character : Text)
    {
        switch (Character)
        {
        case '-':
        case '{':
        case '}':
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            continue;
        default:
            break;
        }
        if (Count >= 32)
        {
            return false;
        }
        Hex[Count++] = Character;
    }
    if (Count != 32)
    {
        return false;
    }

    // 2. 两两解析为 16 字节.
    std::array<std::uint8_t, 16> Bytes{};
    for (std::size_t Index = 0; Index < 16; ++Index)
    {
        const int High = getHexValue(Hex[Index * 2]);
        const int Low = getHexValue(Hex[Index * 2 + 1]);
        if (High < 0 || Low < 0)
        {
            return false;
        }
        Bytes[Index] = static_cast<std::uint8_t>((High << 4) | Low);
    }

    // 3. 打包为 A/B/C/D(与 toString 的布局一致).
    OutGuid = packBytes(Bytes.data());
    return true;
}

} // namespace infra
