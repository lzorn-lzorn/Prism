#pragma once

#include <infra/math/Matrix.hpp>
#include <infra/math/Vector.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace runtime::renderer
{

/**
 * @brief 轴对齐包围盒(AABB).
 *
 * 供剔除(视锥/遮挡)与 BVH 加速结构使用; 与 RenderProxy 的包围球(BoundsCenter/R)
 * 互补: 球用于快速粗剔除, AABB 用于更紧的保守剔除与空间划分.
 */
struct Aabb
{
    infra::Vec3f Min { std::numeric_limits<float>::infinity() };
    infra::Vec3f Max { -std::numeric_limits<float>::infinity() };

    /** @brief 由中心 + 半径构造 AABB(包围球 -> AABB 的保守外扩). */
    static Aabb fromCenterRadius(const infra::Vec3f& Center, float Radius) noexcept
    {
        Aabb B;
        B.Min = { Center.x() - Radius, Center.y() - Radius, Center.z() - Radius };
        B.Max = { Center.x() + Radius, Center.y() + Radius, Center.z() + Radius };
        return B;
    }

    /** @brief 并入另一个 AABB(求并集). */
    void merge(const Aabb& Other) noexcept
    {
        for (int I = 0; I < 3; ++I)
        {
            Min[I] = std::min(Min[I], Other.Min[I]);
            Max[I] = std::max(Max[I], Other.Max[I]);
        }
    }

    [[nodiscard]] bool isValid() const noexcept { return Min[0] <= Max[0]; }
    [[nodiscard]] infra::Vec3f center() const noexcept { return (Min + Max) * 0.5f; }
};

/**
 * @brief 视锥平面(半空间): 内侧定义为 Normal·p + Distance >= 0.
 */
struct FrustumPlane
{
    infra::Vec3f Normal { 0.0f, 0.0f, 0.0f };
    float Distance { 0.0f };
};

/** @brief 由 6 个平面构成的视锥(近/远/左/右/上/下). */
struct Frustum
{
    std::array<FrustumPlane, 6> Planes;
};

/**
 * @brief 从 ViewProjection 提取视锥平面(Gribb-Hartmann).
 *
 * 约定: Matrix4x4 为行主序存储 + 列向量(M * v); 深度为 Vulkan 的 [0,1].
 * 提取出的平面朝向视锥内部(Normal·p + Distance >= 0 表示在内侧).
 */
inline Frustum makeFrustum(const infra::Matrix4x4& ViewProjection) noexcept
{
    // 行向量(列向量约定下的 clip = M * v 的各分量).
    const float* R0 = ViewProjection[0].data();
    const float* R1 = ViewProjection[1].data();
    const float* R2 = ViewProjection[2].data();
    const float* R3 = ViewProjection[3].data();

    auto plane = [](const float* A, const float* B) -> FrustumPlane
    {
        FrustumPlane P;
        P.Normal = { A[0] + B[0], A[1] + B[1], A[2] + B[2] };
        P.Distance = A[3] + B[3];
        const float Len = P.Normal.length();
        if (Len > 1e-12f)
        {
            P.Normal = P.Normal / Len;
            P.Distance /= Len;
        }
        return P;
    };
    auto planeMinus = [](const float* A, const float* B) -> FrustumPlane
    {
        FrustumPlane P;
        P.Normal = { A[0] - B[0], A[1] - B[1], A[2] - B[2] };
        P.Distance = A[3] - B[3];
        const float Len = P.Normal.length();
        if (Len > 1e-12f)
        {
            P.Normal = P.Normal / Len;
            P.Distance /= Len;
        }
        return P;
    };

    Frustum F;
    F.Planes[0] = plane(R3, R0);      // 左: x + w >= 0
    F.Planes[1] = planeMinus(R3, R0); // 右: w - x >= 0
    F.Planes[2] = plane(R3, R1);      // 下: y + w >= 0(Vulkan NDC y 向下)
    F.Planes[3] = planeMinus(R3, R1); // 上: w - y >= 0
    F.Planes[4] = planeMinus(R2, R3); // 远: w - z >= 0
    // 近裁剪面: Vulkan 深度 [0,1], 近平面为 z = 0 → z >= 0.
    {
        FrustumPlane Near;
        Near.Normal = { R2[0], R2[1], R2[2] };
        Near.Distance = R2[3];
        const float Len = Near.Normal.length();
        if (Len > 1e-12f)
        {
            Near.Normal = Near.Normal / Len;
            Near.Distance /= Len;
        }
        F.Planes[5] = Near;
    }
    return F;
}

/**
 * @brief 保守测试: 包围球是否与视锥有交集(用于粗剔除).
 *
 * 对每个平面, 若球心完全落在平面外侧(center·N + D < -radius)则判为不可见.
 * @return true 表示"可能可见"(保守), false 表示"必然不可见".
 */
inline bool contains(const Frustum& F, const infra::Vec3f& Center, float Radius) noexcept
{
    for (const FrustumPlane& P : F.Planes)
    {
        if (P.Normal.dot(Center) + P.Distance < -Radius)
            return false;
    }
    return true;
}

/**
 * @brief 保守测试: AABB 是否与视锥有交集.
 *
 * 对每个平面取 AABB 的"正顶点"(沿平面法向最远的角), 若该顶点都在某平面外侧则不可见.
 * @return true 表示"可能可见", false 表示"必然不可见".
 */
inline bool contains(const Frustum& F, const Aabb& Box) noexcept
{
    for (const FrustumPlane& P : F.Planes)
    {
        const infra::Vec3f Positive {
            P.Normal[0] >= 0.0f ? Box.Max[0] : Box.Min[0],
            P.Normal[1] >= 0.0f ? Box.Max[1] : Box.Min[1],
            P.Normal[2] >= 0.0f ? Box.Max[2] : Box.Min[2],
        };
        if (P.Normal.dot(Positive) + P.Distance < 0.0f)
            return false;
    }
    return true;
}

} // namespace runtime::renderer
