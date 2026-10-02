#include <renderer/WorldRenderBridge.hpp>

#include <type_traits>

namespace runtime::renderer
{

namespace
{

[[nodiscard]] ELightType convertLightType(world::LogicLightData::EType Type) noexcept
{
    switch (Type)
    {
    case world::LogicLightData::EType::Directional: return ELightType::Directional;
    case world::LogicLightData::EType::Point: return ELightType::Point;
    case world::LogicLightData::EType::Spot: return ELightType::Spot;
    }
    return ELightType::Directional;
}

[[nodiscard]] RenderProxyHeader makeHeader(const world::LogicObjectHeader& In, ERenderProxyKind Kind,
    uint32_t MaterialId)
{
    return RenderProxyHeader {
        .EntityId = In.Id,
        .Kind = Kind,
        .MaterialId = MaterialId,
        .LocalToWorld = In.LocalToWorld,
        .BoundsCenterWS = In.BoundsCenterWS,
        .BoundsRadius = In.BoundsRadius,
        .StableOrder = In.StableOrder,
    };
}

} // namespace

bool WorldRenderBridge::importSnapshot(const world::WorldSnapshot& Snapshot,
    RenderProxyWorld& OutWorld) const
{
    if (!Resolver)
        return false;

    OutWorld.clear();

    for (const world::LogicObjectSnapshot& Object : Snapshot.Objects)
    {
        const world::LogicObjectHeader& Header = Object.Header;
        std::visit([&](const auto& Payload) {
            using PayloadType = std::decay_t<decltype(Payload)>;

            if constexpr (std::is_same_v<PayloadType, world::LogicRenderableRef>)
            {
                auto mesh = Resolver->resolveStaticMesh(Payload.GeometryId);
                if (!mesh)
                    return;

                ERenderProxyKind kind = ERenderProxyKind::OpaqueMesh;
                if (Payload.Transparent)
                    kind = ERenderProxyKind::TransparentMesh;
                else if (Header.Kind == world::ELogicObjectKind::DynamicRenderable)
                    kind = ERenderProxyKind::RigidBodyMesh;

                MeshRenderProxy proxy;
                proxy.Header = makeHeader(Header, kind, Payload.MaterialId);
                proxy.Mesh = std::move(mesh);
                proxy.CastShadow = Payload.CastShadow;
                proxy.PreviousLocalToWorld = Header.PreviousLocalToWorld;
                OutWorld.addProxy(RenderProxyVariant { std::move(proxy) });
            }
            else if constexpr (std::is_same_v<PayloadType, world::LogicTerrainRef>)
            {
                TerrainPatchData patch;
                if (!Resolver->resolveTerrainPatch(Payload.PatchId, patch))
                    return;

                patch.MaterialId = Payload.MaterialId;
                patch.LodLevel = Payload.LodLevel;
                patch.LocalToWorld = Header.LocalToWorld;
                patch.PatchSizeMeters = Payload.PatchSizeMeters;

                TerrainRenderProxy proxy;
                proxy.Header = makeHeader(Header, ERenderProxyKind::TerrainPatch, Payload.MaterialId);
                proxy.Patch = std::move(patch);
                OutWorld.addProxy(RenderProxyVariant { std::move(proxy) });
            }
            else if constexpr (std::is_same_v<PayloadType, world::LogicLightData>)
            {
                LightRenderProxy proxy;
                proxy.Header = makeHeader(Header, ERenderProxyKind::Light, 0);
                proxy.Light.Type = convertLightType(Payload.Type);
                proxy.Light.PositionWS = Payload.PositionWS;
                proxy.Light.DirectionWS = Payload.DirectionWS;
                proxy.Light.Color = Payload.Color;
                proxy.Light.Intensity = Payload.Intensity;
                proxy.Light.Range = Payload.Range;
                proxy.Light.SpotInnerCos = Payload.SpotInnerCos;
                proxy.Light.SpotOuterCos = Payload.SpotOuterCos;
                proxy.Light.CastShadow = Payload.CastShadow;
                OutWorld.addProxy(RenderProxyVariant { std::move(proxy) });
            }
            else if constexpr (std::is_same_v<PayloadType, world::LogicFluidData>)
            {
                FluidSurfaceData fluid;
                if (!Resolver->resolveFluidSurface(Payload.SurfaceId, fluid))
                    return;

                fluid.MaterialId = Payload.MaterialId;
                fluid.GridWorldSize = Payload.GridWorldSize;
                fluid.FoamThreshold = Payload.FoamThreshold;
                fluid.RefractionStrength = Payload.RefractionStrength;

                FluidRenderProxy proxy;
                proxy.Header = makeHeader(Header, ERenderProxyKind::FluidSurface, Payload.MaterialId);
                proxy.Fluid = std::move(fluid);
                OutWorld.addProxy(RenderProxyVariant { std::move(proxy) });
            }
            else if constexpr (std::is_same_v<PayloadType, world::LogicGasData>)
            {
                GasVolumeData gas;
                if (!Resolver->resolveGasVolume(Payload.VolumeId, gas))
                    return;

                gas.BoundsMinWS = Payload.BoundsMinWS;
                gas.BoundsMaxWS = Payload.BoundsMaxWS;
                gas.Scattering = Payload.Scattering;
                gas.Absorption = Payload.Absorption;
                gas.AnisotropyG = Payload.AnisotropyG;
                gas.StepCount = Payload.StepCount;

                GasRenderProxy proxy;
                proxy.Header = makeHeader(Header, ERenderProxyKind::GasVolume, 0);
                proxy.Gas = std::move(gas);
                OutWorld.addProxy(RenderProxyVariant { std::move(proxy) });
            }
        }, Object.Payload);
    }

    return true;
}

bool WorldRenderBridge::pullFrom(const world::IWorldSnapshotProvider& Provider,
    RenderProxyWorld& OutWorld) const
{
    world::WorldSnapshot snapshot;
    if (!Provider.buildSnapshot(snapshot))
        return false;
    return importSnapshot(snapshot, OutWorld);
}

} // namespace runtime::renderer
