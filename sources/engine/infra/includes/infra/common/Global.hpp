#pragma once 

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
namespace infra
{

enum class ESystemType : uint8_t
{
	Global = 0,
	Game = 1,
	Renderer = 2,
	Physics = 3,
	Audio = 4,
	Video = 5,
	Network = 6,
	AI = 7,
	Script = 8,
	Animation = 9,
	Editor = 10
};

using HandleIdType = uint64_t;
using HandleGenerationType  = uint64_t;

inline constexpr HandleIdType         InvalidHandleId         = 0ull;
inline constexpr HandleGenerationType InvalidHandleGeneration = 0ull;
struct Global_t 
{
    constexpr static uint32_t value = 0; // System Number
    constexpr static std::string_view name = "Global"; 
};
struct Game_t 
{
    constexpr static uint32_t value = 1; // System Number
    constexpr static std::string_view name = "Game"; 
};
struct Renderer_t 
{
    constexpr static uint32_t value = 2; // System Number
    constexpr static std::string_view name = "Renderer"; 
};
struct Physics_t 
{
    constexpr static uint32_t value = 3; // System Number
    constexpr static std::string_view name = "Physics"; 
};
struct Audio_t 
{
    constexpr static uint32_t value = 4; // System Number
    constexpr static std::string_view name = "Audio"; 
};
struct Video_t 
{
    constexpr static uint32_t value = 5; // System Number
    constexpr static std::string_view name = "Video"; 
};
struct Network_t 
{
    constexpr static uint32_t value = 6; // System Number
    constexpr static std::string_view name = "Network"; 
};
struct AI_t 
{
    constexpr static uint32_t value = 7; // System Number
    constexpr static std::string_view name = "AI"; 
};
struct Script_t 
{
    constexpr static uint32_t value = 8; // System Number
    constexpr static std::string_view name = "Script"; 
};
struct Animation_t 
{
    constexpr static uint32_t value = 9; // System Number
    constexpr static std::string_view name = "Animation"; 
};
struct Editor_t 
{
    constexpr static uint32_t value = 10; // System Number
    constexpr static std::string_view name = "Editor"; 
};

inline std::atomic<uint64_t> GNextHandleId { 0 };

#define CORE_CONCAT_IMPL(a, b) a##b
#define CORE_CONCAT(a, b)      CORE_CONCAT_IMPL(a, b)

// GId: 128 位全局唯一标识符, 参考 UE FGuid 设计.
// 布局: A/B/C/D 四个 uint32, 与 Windows GUID 的 Data1 / Data2 / Data3 / Data4 一一对应.
struct GId
{
    uint32_t A{0};
    uint32_t B{0};
    uint32_t C{0};
    uint32_t D{0};

    constexpr GId() noexcept = default;
    constexpr GId(uint32_t InA, uint32_t InB, uint32_t InC, uint32_t InD) noexcept
        : A(InA), B(InB), C(InC), D(InD) {}

    // 生成一个新的唯一 GId(Windows: CoCreateGuid; Linux/macOS: uuid_generate; 其余平台回退随机数).
    static GId makeGuid();

    // 非全零即为有效.
    [[nodiscard]] constexpr bool isValid() const noexcept { return (A | B | C | D) != 0; }
    void invalidate() noexcept { A = B = C = D = 0; }

    // 转为 "XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX"(可选大括号).
    [[nodiscard]] std::string toString(bool WithBraces = false) const;

    // 从字符串解析(容忍可选大括号与连字符); 失败返回 false 且不改动 OutGuid.
    static bool parse(std::string_view Text, GId& OutGuid);

    [[nodiscard]] constexpr bool operator==(const GId& Other) const noexcept
    {
        return A == Other.A && B == Other.B && C == Other.C && D == Other.D;
    }
    [[nodiscard]] constexpr bool operator!=(const GId& Other) const noexcept { return !(*this == Other); }

    // 字典序比较, 供有序容器使用.
    [[nodiscard]] constexpr bool operator<(const GId& Other) const noexcept
    {
        if (A != Other.A) return A < Other.A;
        if (B != Other.B) return B < Other.B;
        if (C != Other.C) return C < Other.C;
        return D < Other.D;
    }

    [[nodiscard]] constexpr uint32_t operator[](std::size_t Index) const noexcept
    {
        switch (Index)
        {
        case 0: return A;
        case 1: return B;
        case 2: return C;
        default: return D;
        }
    }
    [[nodiscard]] uint32_t& operator[](std::size_t Index) noexcept
    {
        switch (Index)
        {
        case 0: return A;
        case 1: return B;
        case 2: return C;
        default: return D;
        }
    }

    // 哈希: 与 UE GetTypeHash(FGuid) 一致, 四个分量的异或.
    [[nodiscard]] constexpr uint32_t getHash() const noexcept { return A ^ B ^ C ^ D; }
};

} // namespace infra

namespace std
{
template <>
struct hash<infra::GId>
{
    std::size_t operator()(const infra::GId& Id) const noexcept
    {
        return static_cast<std::size_t>(Id.getHash());
    }
};
} // namespace std