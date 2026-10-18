#pragma once

#include "MathCommon.hpp"

#include <array>
#include <tuple>
#include <utility>

namespace infra
{

template <arithmetic Ty, std::size_t Dimensions>
struct Vector;

template <typename VectorType>
struct vector_traits;

template <arithmetic Ty, std::size_t Dimensions>
struct vector_traits<Vector<Ty, Dimensions>>
{
    using value_type = Ty;
    static constexpr std::size_t dimensions = Dimensions;
};

template <arithmetic Ty, std::size_t Dimensions>
struct Vector
{
    static_assert(Dimensions > 0, "A vector must have at least one component.");

    using value_type = Ty;
    using reference = value_type&;
    using const_reference = const value_type&;
    using size_type = std::size_t;

    static constexpr size_type dimensions = Dimensions;

    std::array<value_type, dimensions> Coordinates {};

    constexpr Vector() = default;

    explicit constexpr Vector(value_type Value)
    {
        Coordinates.fill(Value);
    }

    template <arithmetic Other>
        requires std::convertible_to<Other, value_type>
    constexpr Vector(const std::array<Other, dimensions>& Values)
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] = static_cast<value_type>(Values[_index]);
        }
    }

    template <arithmetic Other>
        requires std::convertible_to<Other, value_type>
    constexpr Vector(const Other (&Values)[dimensions])
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] = static_cast<value_type>(Values[_index]);
        }
    }

    template <typename... Values>
        requires (sizeof...(Values) == dimensions) && (std::convertible_to<Values, value_type> && ...)
    constexpr Vector(Values&&... InValues)
        : Coordinates {static_cast<value_type>(std::forward<Values>(InValues))...}
    {
    }

    [[nodiscard]] constexpr reference operator[](size_type Index) noexcept
    {
        return Coordinates[Index];
    }

    [[nodiscard]] constexpr const_reference operator[](size_type Index) const noexcept
    {
        return Coordinates[Index];
    }

    [[nodiscard]] constexpr reference x() noexcept
        requires (dimensions >= 1)
    {
        return Coordinates[0];
    }

    [[nodiscard]] constexpr const_reference x() const noexcept
        requires (dimensions >= 1)
    {
        return Coordinates[0];
    }

    [[nodiscard]] constexpr reference y() noexcept
        requires (dimensions >= 2)
    {
        return Coordinates[1];
    }

    [[nodiscard]] constexpr const_reference y() const noexcept
        requires (dimensions >= 2)
    {
        return Coordinates[1];
    }

    [[nodiscard]] constexpr reference z() noexcept
        requires (dimensions >= 3)
    {
        return Coordinates[2];
    }

    [[nodiscard]] constexpr const_reference z() const noexcept
        requires (dimensions >= 3)
    {
        return Coordinates[2];
    }

    [[nodiscard]] constexpr reference w() noexcept
        requires (dimensions >= 4)
    {
        return Coordinates[3];
    }

    [[nodiscard]] constexpr const_reference w() const noexcept
        requires (dimensions >= 4)
    {
        return Coordinates[3];
    }

    // 兼容旧 API；新代码使用小写访问器. 
    [[nodiscard]] constexpr reference X() noexcept requires (dimensions >= 1) { return x(); }
    [[nodiscard]] constexpr const_reference X() const noexcept requires (dimensions >= 1) { return x(); }
    [[nodiscard]] constexpr reference Y() noexcept requires (dimensions >= 2) { return y(); }
    [[nodiscard]] constexpr const_reference Y() const noexcept requires (dimensions >= 2) { return y(); }
    [[nodiscard]] constexpr reference Z() noexcept requires (dimensions >= 3) { return z(); }
    [[nodiscard]] constexpr const_reference Z() const noexcept requires (dimensions >= 3) { return z(); }
    [[nodiscard]] constexpr reference W() noexcept requires (dimensions >= 4) { return w(); }
    [[nodiscard]] constexpr const_reference W() const noexcept requires (dimensions >= 4) { return w(); }

    [[nodiscard]] static constexpr Vector fill(value_type Value) noexcept
    {
        return Vector(Value);
    }

    [[nodiscard]] static constexpr Vector zero() noexcept
    {
        return Vector(value_type {});
    }

    [[nodiscard]] static constexpr Vector one() noexcept
    {
        return Vector(value_type {1});
    }

    [[nodiscard]] static constexpr Vector min() noexcept
    {
        return Vector(std::numeric_limits<value_type>::lowest());
    }

    [[nodiscard]] static constexpr Vector max() noexcept
    {
        return Vector(std::numeric_limits<value_type>::max());
    }

    [[nodiscard]] static constexpr Vector axis(size_type Axis, value_type Magnitude = value_type {1}) noexcept
    {
        Vector _result {};
        if (Axis < dimensions) {
            _result[Axis] = Magnitude;
        }
        return _result;
    }

    [[nodiscard]] static constexpr Vector axisX() noexcept requires (dimensions >= 1) { return axis(0); }
    [[nodiscard]] static constexpr Vector axisY() noexcept requires (dimensions >= 2) { return axis(1); }
    [[nodiscard]] static constexpr Vector axisZ() noexcept requires (dimensions >= 3) { return axis(2); }
    [[nodiscard]] static constexpr Vector axisW() noexcept requires (dimensions >= 4) { return axis(3); }

    [[nodiscard]] constexpr Vector operator+() const noexcept
    {
        return *this;
    }

    [[nodiscard]] constexpr Vector operator-() const noexcept
    {
        Vector _result {};
        for (size_type _index = 0; _index < dimensions; ++_index) {
            _result[_index] = -Coordinates[_index];
        }
        return _result;
    }

    constexpr Vector& operator+=(const Vector& Other) noexcept
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] += Other[_index];
        }
        return *this;
    }

    constexpr Vector& operator-=(const Vector& Other) noexcept
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] -= Other[_index];
        }
        return *this;
    }

    constexpr Vector& operator*=(const Vector& Other) noexcept
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] *= Other[_index];
        }
        return *this;
    }

    constexpr Vector& operator/=(const Vector& Other) noexcept
    {
        for (size_type _index = 0; _index < dimensions; ++_index) {
            Coordinates[_index] /= Other[_index];
        }
        return *this;
    }

    constexpr Vector& operator+=(value_type Value) noexcept
    {
        for (value_type& _component : Coordinates) {
            _component += Value;
        }
        return *this;
    }

    constexpr Vector& operator-=(value_type Value) noexcept
    {
        for (value_type& _component : Coordinates) {
            _component -= Value;
        }
        return *this;
    }

    constexpr Vector& operator*=(value_type Value) noexcept
    {
        for (value_type& _component : Coordinates) {
            _component *= Value;
        }
        return *this;
    }

    constexpr Vector& operator/=(value_type Value) noexcept
    {
        for (value_type& _component : Coordinates) {
            _component /= Value;
        }
        return *this;
    }

    [[nodiscard]] constexpr value_type square() const noexcept
    {
        value_type _result {};
        for (const value_type _component : Coordinates) {
            _result += _component * _component;
        }
        return _result;
    }

    [[nodiscard]] constexpr value_type dot(const Vector& Other) const noexcept
    {
        value_type _result {};
        for (size_type _index = 0; _index < dimensions; ++_index) {
            _result += Coordinates[_index] * Other[_index];
        }
        return _result;
    }

    [[nodiscard]] float length() const noexcept
    {
        return std::sqrt(static_cast<float>(square()));
    }

    [[nodiscard]] Vector normalized(const Vector& Fallback = Vector {}) const noexcept
        requires std::floating_point<value_type>
    {
        const float _length = length();
        if (_length <= Tiny) {
            return Fallback;
        }
        return *this / static_cast<value_type>(_length);
    }

    constexpr Vector& normalize() noexcept
        requires std::floating_point<value_type>
    {
        const float _length = length();
        if (_length > Tiny) {
            *this /= static_cast<value_type>(_length);
        }
        return *this;
    }

    [[nodiscard]] constexpr auto asTuple() const noexcept
    {
        return std::apply([](const auto&... Components) { return std::make_tuple(Components...); }, Coordinates);
    }

    [[nodiscard]] static constexpr value_type dot(const Vector& Left, const Vector& Right) noexcept
    {
        return Left.dot(Right);
    }

    [[nodiscard]] static constexpr Vector cross(const Vector& Left, const Vector& Right) noexcept
        requires (dimensions == 3)
    {
        return Vector {
            Left[1] * Right[2] - Left[2] * Right[1],
            Left[2] * Right[0] - Left[0] * Right[2],
            Left[0] * Right[1] - Left[1] * Right[0],
        };
    }

    [[nodiscard]] static constexpr value_type cross(const Vector& Left, const Vector& Right) noexcept
        requires (dimensions == 2)
    {
        return Left[0] * Right[1] - Left[1] * Right[0];
    }

    // 兼容先前公开 API. 
    [[nodiscard]] constexpr value_type Square() const noexcept { return square(); }
    [[nodiscard]] constexpr value_type Dot(const Vector& Other) const noexcept { return dot(Other); }
    [[nodiscard]] float Length() const noexcept { return length(); }
    [[nodiscard]] Vector Normalized(const Vector& Fallback = Vector {}) const noexcept requires std::floating_point<value_type> { return normalized(Fallback); }
    constexpr Vector& Normalize() noexcept requires std::floating_point<value_type> { return normalize(); }
    [[nodiscard]] static constexpr Vector Fill(value_type Value) noexcept { return fill(Value); }
    [[nodiscard]] static constexpr Vector ZeroVector() noexcept { return zero(); }
    [[nodiscard]] static constexpr Vector OneVector() noexcept { return one(); }
    [[nodiscard]] static constexpr Vector XAxisVector() noexcept requires (dimensions >= 1) { return axisX(); }
    [[nodiscard]] static constexpr Vector YAxisVector() noexcept requires (dimensions >= 2) { return axisY(); }
    [[nodiscard]] static constexpr Vector ZAxisVector() noexcept requires (dimensions >= 3) { return axisZ(); }
};

template <arithmetic First, arithmetic... Rest>
Vector(First, Rest...) -> Vector<std::common_type_t<First, Rest...>, 1 + sizeof...(Rest)>;

template <arithmetic Ty, std::size_t Dimensions>
Vector(const std::array<Ty, Dimensions>&) -> Vector<Ty, Dimensions>;

template <arithmetic Ty, std::size_t Dimensions>
Vector(const Ty (&)[Dimensions]) -> Vector<Ty, Dimensions>;

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr bool operator==(const Vector<Ty, Dimensions>& Left, const Vector<Ty, Dimensions>& Right) noexcept
{
    return Left.Coordinates == Right.Coordinates;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator+(Vector<Ty, Dimensions> Left, const Vector<Ty, Dimensions>& Right) noexcept
{
    return Left += Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator-(Vector<Ty, Dimensions> Left, const Vector<Ty, Dimensions>& Right) noexcept
{
    return Left -= Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator*(Vector<Ty, Dimensions> Left, const Vector<Ty, Dimensions>& Right) noexcept
{
    return Left *= Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator/(Vector<Ty, Dimensions> Left, const Vector<Ty, Dimensions>& Right) noexcept
{
    return Left /= Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator+(Vector<Ty, Dimensions> Left, Ty Right) noexcept
{
    return Left += Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator+(Ty Left, Vector<Ty, Dimensions> Right) noexcept
{
    return Right += Left;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator-(Vector<Ty, Dimensions> Left, Ty Right) noexcept
{
    return Left -= Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator-(Ty Left, Vector<Ty, Dimensions> Right) noexcept
{
    for (std::size_t _index = 0; _index < Dimensions; ++_index) {
        Right[_index] = Left - Right[_index];
    }
    return Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator*(Vector<Ty, Dimensions> Left, Ty Right) noexcept
{
    return Left *= Right;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator*(Ty Left, Vector<Ty, Dimensions> Right) noexcept
{
    return Right *= Left;
}

template <arithmetic Ty, std::size_t Dimensions>
[[nodiscard]] constexpr Vector<Ty, Dimensions> operator/(Vector<Ty, Dimensions> Left, Ty Right) noexcept
{
    return Left /= Right;
}

template <arithmetic Ty>
using Vec1D = Vector<Ty, 1>;
template <arithmetic Ty>
using Vec2D = Vector<Ty, 2>;
template <arithmetic Ty>
using Vec3D = Vector<Ty, 3>;
template <arithmetic Ty>
using Vec4D = Vector<Ty, 4>;

template <arithmetic Ty>
using Vector1D = Vector<Ty, 1>;
template <arithmetic Ty>
using Vector2D = Vector<Ty, 2>;
template <arithmetic Ty>
using Vector3D = Vector<Ty, 3>;
template <arithmetic Ty>
using Vector4D = Vector<Ty, 4>;

using Vec1f = Vector<float, 1>;
using Vec2f = Vector<float, 2>;
using Vec3f = Vector<float, 3>;
using Vec4f = Vector<float, 4>;
using Vec1i = Vector<std::int32_t, 1>;
using Vec2i = Vector<std::int32_t, 2>;
using Vec3i = Vector<std::int32_t, 3>;
using Vec4i = Vector<std::int32_t, 4>;

[[nodiscard]] inline bool isParallel(const Vec3f& Left, const Vec3f& Right, float Tolerance = Tiny) noexcept
{
    return equalZeroNearly(Vec3f::cross(Left, Right).length(), Tolerance);
}

[[nodiscard]] inline bool isParallel(const Vec2f& Left, const Vec2f& Right, float Tolerance = Tiny) noexcept
{
    return equalZeroNearly(Vec2f::cross(Left, Right), Tolerance);
}

[[nodiscard]] inline bool isVertical(const Vec3f& Left, const Vec3f& Right, float Tolerance = Tiny) noexcept
{
    return equalZeroNearly(Left.dot(Right), Tolerance);
}

[[nodiscard]] inline bool isVertical(const Vec2f& Left, const Vec2f& Right, float Tolerance = Tiny) noexcept
{
    return equalZeroNearly(Left.dot(Right), Tolerance);
}

} // namespace infra
