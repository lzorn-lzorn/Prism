#pragma once

#include <core/math/Color.hpp>
#include <core/math/Matrix.hpp>
#include <core/math/Vector.hpp>

#include <cstdint>
#include <variant>
#include <vector>

namespace runtime::world
{

/**
 * 分层约束:
 *  - runtime/world 仅承载纯逻辑/纯 ECS 数据与接口(不出现 RHI/渲染 API 类型);
 *  - renderer 通过 bridge/adaptor 把本文件中的逻辑快照转换为渲染代理.
 *
 * 迁移判据:
 *  只有当一段数据不再携带渲染专有类型(例如 RBuffer/RImageView/Pipeline 等),
 *  才应放入 runtime/world. 否则应留在 renderer 层.
 */

using EntityId = uint64_t;

/** @brief 逻辑层对象分类; 不携带渲染 API 细节. */
enum class ELogicObjectKind : uint8_t
{
    StaticRenderable,
    DynamicRenderable,
    TerrainPatch,
    Light,
    FluidSurface,
    GasVolume,
    LogicOnly,
};

struct LogicObjectHeader
{
    EntityId Id { 0 };
    ELogicObjectKind Kind { ELogicObjectKind::LogicOnly };
    core::Matrix4x4 LocalToWorld {};
    core::Matrix4x4 PreviousLocalToWorld {};
    core::Vec3f BoundsCenterWS { 0.0f, 0.0f, 0.0f };
    float BoundsRadius { 0.0f };
    uint32_t StableOrder { 0 };
};

/** @brief 渲染无关的几何引用(逻辑资源 ID). */
struct LogicRenderableRef
{
    uint64_t GeometryId { 0 };
    uint32_t MaterialId { 0 };
    bool Transparent { false };
    bool CastShadow { true };
};

struct LogicTerrainRef
{
    uint64_t PatchId { 0 };
    uint32_t MaterialId { 0 };
    uint32_t LodLevel { 0 };
    core::Vec2f PatchSizeMeters { 64.0f, 64.0f };
};

struct LogicLightData
{
    enum class EType : uint8_t
    {
        Directional,
        Point,
        Spot,
    };

    EType Type { EType::Directional };
    core::Vec3f PositionWS { 0.0f, 0.0f, 0.0f };
    core::Vec3f DirectionWS { 0.0f, -1.0f, 0.0f };
    core::LinearColor4D Color { 1.0f, 1.0f, 1.0f, 1.0f };
    float Intensity { 1.0f };
    float Range { 0.0f };
    float SpotInnerCos { 0.9f };
    float SpotOuterCos { 0.8f };
    bool CastShadow { false };
};

struct LogicFluidData
{
    uint64_t SurfaceId { 0 };
    uint32_t MaterialId { 0 };
    float GridWorldSize { 1.0f };
    float FoamThreshold { 0.6f };
    float RefractionStrength { 0.02f };
};

struct LogicGasData
{
    uint64_t VolumeId { 0 };
    core::Vec3f BoundsMinWS { 0.0f, 0.0f, 0.0f };
    core::Vec3f BoundsMaxWS { 0.0f, 0.0f, 0.0f };
    float Scattering { 0.04f };
    float Absorption { 0.01f };
    float AnisotropyG { 0.2f };
    uint32_t StepCount { 64 };
};

using LogicPayload = std::variant<LogicRenderableRef, LogicTerrainRef,
    LogicLightData, LogicFluidData, LogicGasData>;

struct LogicObjectSnapshot
{
    LogicObjectHeader Header;
    LogicPayload Payload;
};

struct WorldSnapshot
{
    std::vector<LogicObjectSnapshot> Objects;
    uint64_t Revision { 0 };

    void clear() noexcept
    {
        Objects.clear();
        Revision = 0;
    }
};

/** @brief 逻辑层快照提供接口, 仅暴露纯逻辑数据. */
class IWorldSnapshotProvider
{
public:
    virtual ~IWorldSnapshotProvider() = default;
    virtual bool buildSnapshot(WorldSnapshot& Out) const = 0;
};

} // namespace runtime::world
