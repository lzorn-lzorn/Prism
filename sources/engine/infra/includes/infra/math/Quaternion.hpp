#pragma once

#include <cmath>

#include "Vector.hpp"
#include "Matrix.hpp"
#include "MathCommon.hpp"
namespace infra
{

struct Quaternion
{
    Vec4f Self; // (x, y, z, w)

    float x() const noexcept { return Self.x(); }
    float y() const noexcept { return Self.y(); }
    float z() const noexcept { return Self.z(); }
    float w() const noexcept { return Self.w(); }

    Quaternion() noexcept : Self(0.f, 0.f, 0.f, 1.f) {}
    Quaternion(float x, float y, float z, float w) noexcept : Self(x, y, z, w) {}

    static Quaternion axis_angle(const Vec3f& Axis, float Radians) {
        float half = Radians * 0.5f;
        float s = std::sin(half);
        return Quaternion(std::cos(half), Axis.x()*s, Axis.y()*s, Axis.z()*s);
    }

    Quaternion operator*(const Quaternion& Q) const {
        return Quaternion(
            Self.w()*Q.w() - Self.x()*Q.x() - Self.y()*Q.y() - Self.z()*Q.z(),
            Self.w()*Q.x() + Self.x()*Q.w() + Self.y()*Q.z() - Self.z()*Q.y(),
            Self.w()*Q.y() - Self.x()*Q.z() + Self.y()*Q.w() + Self.z()*Q.x(),
            Self.w()*Q.z() + Self.x()*Q.y() - Self.y()*Q.x() + Self.z()*Q.w()
        );
    }

    // @brief 共轭: 对于单位四元数, 共轭等价于逆. 共轭四元数表示相反的旋转.
    Quaternion conj() const { return Quaternion(w(), -x(), -y(), -z()); }

        // 旋转向量
    Vec3f rotate(const Vec3f& V) const 
    {
        // v' = q * v * q^{-1}，单位四元数时 q^{-1} = conj(q)
        Quaternion qv(0, V.x(), V.y(), V.z());
        Quaternion result = (*this) * qv * this->conj();
        return Vec3f(result.x(), result.y(), result.z());
    }

    // 归一化
    void normalize() 
    {
        float len = std::sqrt(w()*w() + x()*x() + y()*y() + z()*z());
        if (len > Tiny) 
        {
            Self = Vec4f(x()/len, y()/len, z()/len, w()/len);
        }
    }

    // 点积
    float dot(const Quaternion& Q) const 
    {
        return w()*Q.w() + x()*Q.x() + y()*Q.y() + z()*Q.z();
    }

    // 球面线性插值
    static Quaternion slerp(const Quaternion& A, const Quaternion& B, float T) 
    {
        float cos_omega = A.dot(B);
        Quaternion end = B;
        // 如果点积为负，取相反路径以确保最短弧
        if (cos_omega < 0.0f) 
        {
            end = Quaternion(-B.w(), -B.x(), -B.y(), -B.z());
            cos_omega = -cos_omega;
        }

        float k0, k1;
        if (cos_omega > 0.9999f) { // 线性插值
            k0 = 1.0f - T;
            k1 = T;
        } else {
            float sin_omega = std::sqrt(1.0f - cos_omega*cos_omega);
            float omega = std::atan2(sin_omega, cos_omega);
            float inv_sin = 1.0f / sin_omega;
            k0 = std::sin((1.0f - T)*omega) * inv_sin;
            k1 = std::sin(T*omega) * inv_sin;
        }
        return Quaternion(
            k0*A.w() + k1*end.w(),
            k0*A.x() + k1*end.x(),
            k0*A.y() + k1*end.y(),
            k0*A.z() + k1*end.z()
        );
    }
};



}
