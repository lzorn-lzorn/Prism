#pragma once
#include "MathCommon.hpp"
#include "Vector.hpp"

#include <array>
#include <cstddef>

namespace infra
{

template <arithmetic Ty, std::size_t Rows, std::size_t Columns = Rows>
struct Matrix
{
    static_assert(Rows > 0 && Columns > 0, "A matrix must have at least one row and one column.");

    using value_type = Ty;
    using row_type = std::array<value_type, Columns>;
    using storage_type = std::array<row_type, Rows>;

    static constexpr std::size_t rows = Rows;
    static constexpr std::size_t columns = Columns;
    static constexpr std::size_t cols = Columns;

    storage_type M {};

    [[nodiscard]] constexpr row_type& operator[](std::size_t Row) noexcept
    {
        return M[Row];
    }

    [[nodiscard]] constexpr const row_type& operator[](std::size_t Row) const noexcept
    {
        return M[Row];
    }

    [[nodiscard]] static constexpr Matrix zero() noexcept
    {
        return Matrix {};
    }

    [[nodiscard]] static constexpr Matrix one() noexcept
    {
        Matrix _result {};
        for (row_type& _row : _result.M) {
            _row.fill(value_type {1});
        }
        return _result;
    }

    [[nodiscard]] static constexpr Matrix identity() noexcept
        requires (Rows == Columns)
    {
        Matrix _result {};
        for (std::size_t _index = 0; _index < Rows; ++_index) {
            _result.M[_index][_index] = value_type {1};
        }
        return _result;
    }

    constexpr Matrix& operator+=(const Matrix& Other) noexcept
    {
        for (std::size_t _row = 0; _row < Rows; ++_row) {
            for (std::size_t _column = 0; _column < Columns; ++_column) {
                M[_row][_column] += Other.M[_row][_column];
            }
        }
        return *this;
    }

    constexpr Matrix& operator-=(const Matrix& Other) noexcept
    {
        for (std::size_t _row = 0; _row < Rows; ++_row) {
            for (std::size_t _column = 0; _column < Columns; ++_column) {
                M[_row][_column] -= Other.M[_row][_column];
            }
        }
        return *this;
    }

    constexpr Matrix& operator*=(value_type Scalar) noexcept
    {
        for (row_type& _row : M) {
            for (value_type& _value : _row) {
                _value *= Scalar;
            }
        }
        return *this;
    }

    constexpr Matrix& operator/=(value_type Scalar) noexcept
    {
        for (row_type& _row : M) {
            for (value_type& _value : _row) {
                _value /= Scalar;
            }
        }
        return *this;
    }

    [[nodiscard]] constexpr Matrix transpose() const noexcept
    {
        Matrix _result {};
        if constexpr (Rows == Columns) {
            for (std::size_t _row = 0; _row < Rows; ++_row) {
                for (std::size_t _column = 0; _column < Columns; ++_column) {
                    _result.M[_row][_column] = M[_column][_row];
                }
            }
        }
        return _result;
    }

    // 兼容旧 API. 
    [[nodiscard]] static constexpr Matrix ZeroMatrix() noexcept { return zero(); }
    [[nodiscard]] static constexpr Matrix OneMatrix() noexcept { return one(); }
    [[nodiscard]] static constexpr Matrix Identity() noexcept requires (Rows == Columns) { return identity(); }
};

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr bool operator==(const Matrix<Ty, Rows, Columns>& Left, const Matrix<Ty, Rows, Columns>& Right) noexcept
{
    return Left.M == Right.M;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator+(
    Matrix<Ty, Rows, Columns> Left,
    const Matrix<Ty, Rows, Columns>& Right) noexcept
{
    return Left += Right;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator-(
    Matrix<Ty, Rows, Columns> Left,
    const Matrix<Ty, Rows, Columns>& Right) noexcept
{
    return Left -= Right;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator*(
    Matrix<Ty, Rows, Columns> Left,
    Ty Scalar) noexcept
{
    return Left *= Scalar;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator*(
    Ty Scalar,
    Matrix<Ty, Rows, Columns> Right) noexcept
{
    return Right *= Scalar;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator/(
    Matrix<Ty, Rows, Columns> Left,
    Ty Scalar) noexcept
{
    return Left /= Scalar;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Middle, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> multiply(
    const Matrix<Ty, Rows, Middle>& Left,
    const Matrix<Ty, Middle, Columns>& Right) noexcept
{
    Matrix<Ty, Rows, Columns> _result {};
    for (std::size_t _row = 0; _row < Rows; ++_row) {
        for (std::size_t _middle = 0; _middle < Middle; ++_middle) {
            for (std::size_t _column = 0; _column < Columns; ++_column) {
                _result.M[_row][_column] += Left.M[_row][_middle] * Right.M[_middle][_column];
            }
        }
    }
    return _result;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Middle, std::size_t Columns>
[[nodiscard]] constexpr Matrix<Ty, Rows, Columns> operator*(
    const Matrix<Ty, Rows, Middle>& Left,
    const Matrix<Ty, Middle, Columns>& Right) noexcept
{
    return multiply(Left, Right);
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Vector<Ty, Rows> operator*(
    const Matrix<Ty, Rows, Columns>& Left,
    const Vector<Ty, Columns>& Right) noexcept
{
    Vector<Ty, Rows> _result {};
    for (std::size_t _row = 0; _row < Rows; ++_row) {
        for (std::size_t _column = 0; _column < Columns; ++_column) {
            _result[_row] += Left.M[_row][_column] * Right[_column];
        }
    }
    return _result;
}

template <arithmetic Ty, std::size_t Rows, std::size_t Columns>
[[nodiscard]] constexpr Vector<Ty, Columns> operator*(
    const Vector<Ty, Rows>& Left,
    const Matrix<Ty, Rows, Columns>& Right) noexcept
{
    Vector<Ty, Columns> _result {};
    for (std::size_t _column = 0; _column < Columns; ++_column) {
        for (std::size_t _row = 0; _row < Rows; ++_row) {
            _result[_column] += Left[_row] * Right.M[_row][_column];
        }
    }
    return _result;
}

namespace details
{
template <arithmetic Ty, std::size_t Rows, std::size_t Columns = Rows>
using TMatrix = Matrix<Ty, Rows, Columns>;
}

using Mat2i = Matrix<std::int32_t, 2>;
using Mat2f = Matrix<float, 2>;
using Mat3i = Matrix<std::int32_t, 3>;
using Mat3f = Matrix<float, 3>;
using Mat4i = Matrix<std::int32_t, 4>;
using Mat4f = Matrix<float, 4>;
using Matrix4x4 = Mat4f;

} // namespace infra
