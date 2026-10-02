#pragma once

#include <renderer/RenderProxyWorld.hpp>

#include <world/WorldSnapshot.hpp>

#include <cstdint>

namespace runtime::renderer
{

/**
 * @brief 资源解析接口: 把逻辑层资源 ID 解析为渲染层资源.
 *
 * 该接口由 renderer 侧实现(或注入), world 模块不依赖 RHI 类型.
 */
class IRenderResourceResolver
{
public:
    virtual ~IRenderResourceResolver() = default;

    virtual std::shared_ptr<StaticMesh> resolveStaticMesh(uint64_t GeometryId) const = 0;
    virtual bool resolveTerrainPatch(uint64_t PatchId, TerrainPatchData& OutPatch) const = 0;
    virtual bool resolveFluidSurface(uint64_t SurfaceId, FluidSurfaceData& OutFluid) const = 0;
    virtual bool resolveGasVolume(uint64_t VolumeId, GasVolumeData& OutGas) const = 0;
};

/**
 * @brief World -> Renderer 适配器: 纯逻辑快照转 RenderProxyWorld.
 */
class WorldRenderBridge
{
public:
    explicit WorldRenderBridge(const IRenderResourceResolver* Resolver = nullptr) noexcept
        : Resolver(Resolver)
    {
    }

    void setResolver(const IRenderResourceResolver* InResolver) noexcept { Resolver = InResolver; }

    [[nodiscard]] const IRenderResourceResolver* getResolver() const noexcept { return Resolver; }

    /** @brief 直接导入已有逻辑快照. */
    bool importSnapshot(const world::WorldSnapshot& Snapshot, RenderProxyWorld& OutWorld) const;

    /** @brief 通过逻辑层接口拉取快照并导入. */
    bool pullFrom(const world::IWorldSnapshotProvider& Provider, RenderProxyWorld& OutWorld) const;

private:
    const IRenderResourceResolver* Resolver { nullptr };
};

} // namespace runtime::renderer
