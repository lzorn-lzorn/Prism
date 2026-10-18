#pragma once
#include <type_traits>

namespace infra
{
template<typename EnumType>
concept enum_flag = std::is_enum_v<EnumType> && std::is_unsigned_v<std::underlying_type_t<EnumType>>;

template<enum_flag EnumType, typename Derived>
struct FlagsBase {
    using enum_type = EnumType;
    using underlying = std::underlying_type_t<EnumType>;
    underlying Value = 0;

    constexpr FlagsBase() noexcept = default;
    constexpr FlagsBase(EnumType E) noexcept : Value(static_cast<underlying>(E)) {}
    explicit constexpr FlagsBase(underlying V) noexcept : Value(V) {}

    constexpr Derived operator|(FlagsBase<EnumType, Derived> Other) const noexcept { return Derived(Value | Other.Value); }
    constexpr Derived operator^(FlagsBase<EnumType, Derived> Other) const noexcept { return Derived(Value ^ Other.Value); }
    constexpr Derived operator&(FlagsBase<EnumType, Derived> Other) const noexcept { return Derived(Value & Other.Value); }
    constexpr Derived operator~() const noexcept { return Derived(~Value); }

    constexpr bool operator==(FlagsBase Other) const noexcept { return Value == Other.Value; }
    constexpr bool operator!=(FlagsBase Other) const noexcept { return Value != Other.Value; }

    constexpr Derived operator|(EnumType Other) const noexcept { return Derived(Value | static_cast<underlying>(Other)); }
    constexpr Derived operator^(EnumType Other) const noexcept { return Derived(Value ^ static_cast<underlying>(Other)); }
    constexpr Derived operator&(EnumType Other) const noexcept { return Derived(Value & static_cast<underlying>(Other)); }

    constexpr bool operator==(EnumType Other) const noexcept { return Value == static_cast<underlying>(Other); }
    constexpr bool operator!=(EnumType Other) const noexcept { return Value != static_cast<underlying>(Other); }


    constexpr explicit operator bool() const noexcept { return Value != 0; }
    constexpr bool has(EnumType E) const noexcept { return (Value & static_cast<underlying>(E)) != 0; }
    constexpr void set(EnumType E) noexcept { Value |= static_cast<underlying>(E); }
    constexpr void clear(EnumType E) noexcept { Value &= ~static_cast<underlying>(E); }
};

template<enum_flag EnumType>
struct Flags : public FlagsBase<EnumType, Flags<EnumType>> {
    using Base = FlagsBase<EnumType, Flags<EnumType>>;
    using Base::Base;
};

// 枚举 | 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator|(EnumType Lhs, EnumType Rhs) noexcept {
    return Flags<EnumType>(Lhs) | Rhs;
}

// 枚举 | Flags
template<enum_flag EnumType>
constexpr Flags<EnumType> operator|(EnumType Lhs, Flags<EnumType> Rhs) noexcept {
    return Flags<EnumType>(Lhs) | Rhs;
}

// Flags | 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator|(Flags<EnumType> Lhs, EnumType Rhs) noexcept {
    return Lhs | Flags<EnumType>(Rhs);
}
// 枚举 & 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator&(EnumType Lhs, EnumType Rhs) noexcept {
    return Flags<EnumType>(Lhs) & Rhs;
}

// 枚举 & Flags
template<enum_flag EnumType>
constexpr Flags<EnumType> operator&(EnumType Lhs, Flags<EnumType> Rhs) noexcept {
    return Flags<EnumType>(Lhs) & Rhs;
}

// Flags & 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator&(Flags<EnumType> Lhs, EnumType Rhs) noexcept {
    return Lhs & Flags<EnumType>(Rhs);
}
// 枚举 ^ 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator^(EnumType Lhs, EnumType Rhs) noexcept {
    return Flags<EnumType>(Lhs) ^ Rhs;
}

// 枚举 ^ Flags
template<enum_flag EnumType>
constexpr Flags<EnumType> operator^(EnumType Lhs, Flags<EnumType> Rhs) noexcept {
    return Flags<EnumType>(Lhs) ^ Rhs;
}

// Flags ^ 枚举
template<enum_flag EnumType>
constexpr Flags<EnumType> operator^(Flags<EnumType> Lhs, EnumType Rhs) noexcept {
    return Lhs ^ Flags<EnumType>(Rhs);
}

template<enum_flag EnumType>
constexpr Flags<EnumType> operator~(Flags<EnumType> Other) noexcept {
    return ~Other;
}


template<enum_flag EnumType>
constexpr Flags<EnumType> operator~(EnumType Other) noexcept {
    return ~Flags<EnumType>(Other);
}

} // namespace infra

#define DEFINE_FLAGS(EnumType)                                                                          \
    template<> struct infra::Flags<EnumType> : public infra::FlagsBase<EnumType, infra::Flags<EnumType>> { \
        using enum_type = EnumType;                                                                     \
        using Base = infra::FlagsBase<EnumType, infra::Flags<EnumType>>;                                  \
        using Base::Base;                                                                               \
    };

#define DEFINE_ENUM_OPERATOR(EnumType)                              \
    constexpr EnumType operator|(EnumType a, EnumType b) noexcept { \
        using U = std::underlying_type_t<EnumType>;                 \
        return static_cast<EnumType>(U(a) | U(b));                  \
    }                                                               \
    constexpr EnumType operator&(EnumType a, EnumType b) noexcept { \
        using U = std::underlying_type_t<EnumType>;                 \
        return static_cast<EnumType>(U(a) & U(b));                  \
    }                                                               \
    constexpr EnumType operator^(EnumType a, EnumType b) noexcept { \
        using U = std::underlying_type_t<EnumType>;                 \
        return static_cast<EnumType>(U(a) ^ U(b));                  \
    }                                                               \
    constexpr EnumType operator~(EnumType a) noexcept {             \
        using U = std::underlying_type_t<EnumType>;                 \
        return static_cast<EnumType>(~U(a));                        \
    }