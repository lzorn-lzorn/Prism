#pragma once

#include <algorithm>
#include <cmath>
#include <concepts>
#include <limits>
#include <numbers>

namespace infra
{

template <typename Number>
concept arithmetic = std::integral<Number> || std::floating_point<Number>;

inline constexpr float Epsilon = std::numeric_limits<float>::epsilon();
inline constexpr float Tiny = 1.0e-5f;

template <typename NumType>
inline constexpr NumType Infinity = std::numeric_limits<NumType>::infinity();
inline constexpr float NaN = std::numeric_limits<float>::quiet_NaN();
inline constexpr float Pi = std::numbers::pi_v<float>;
inline constexpr float SquareRootOfPi = 1.7724538509055160273f;
inline constexpr float InverseSquareRootOfPi = std::numbers::inv_sqrtpi_v<float>;
inline constexpr float InversePi = std::numbers::inv_pi_v<float>;
inline constexpr float SquareRootOfTwo = std::numbers::sqrt2_v<float>;
inline constexpr float InverseSquareRootOfTwo = 1.0f / SquareRootOfTwo;
inline constexpr float SquareRootOfThree = std::numbers::sqrt3_v<float>;
inline constexpr float InverseSquareRootOfThree = 1.0f / SquareRootOfThree;
inline constexpr float DegreesToRadians = Pi / 180.0f;
inline constexpr float RadiansToDegrees = 180.0f / Pi;

template <std::floating_point Number>
[[nodiscard]] constexpr bool equalNearly(
    Number Left,
    Number Right,
    Number Tolerance = static_cast<Number>(Tiny)) noexcept
{
    const Number difference = std::abs(Left - Right);
    const Number scale = std::max({std::abs(Left), std::abs(Right), Number {1}});
    return difference <= Tolerance * scale;
}

template <std::floating_point Number>
[[nodiscard]] constexpr bool equalZeroNearly(
    Number Value,
    Number Tolerance = static_cast<Number>(Tiny)) noexcept
{
    return equalNearly(Value, Number {}, Tolerance);
}

// @brief 向上取整到指定对齐值的整数倍
template <std::integral Ty>
inline Ty alignUp(Ty Value, Ty Alignment) noexcept
{
    return (Value + Alignment - 1) & ~(Alignment - 1);
}

} // namespace infra
