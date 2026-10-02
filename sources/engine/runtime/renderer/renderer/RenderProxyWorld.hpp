#pragma once

#include <renderer/RenderData.hpp>

#include <core/math/Matrix.hpp>
#include <core/math/Vector.hpp>

#include <cstdint>
#include <variant>
#include <memory>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - 渲染世界与渲染代理(渲染器侧的场景聚合)
 * ============================================================================
 *
 *  [命名与分层]
 *  注意: 渲染模块里刻意**不出现 "Scene" 这个名词** —— "场景" 是 Game Framework 的概念,
 *  它由 World/Engine 持有. 渲染器侧只关心"世界中有哪些可渲染对象", 因此这里叫:
 *
 *      - RenderProxy  —— 单个 3D 可渲染对象在渲染器侧的代理(对应 UE 的 FPrimitiveSceneProxy);
 *      - RenderProxyWorld  —— 所有 RenderProxy 的聚合数据容器(对应 UE 的 FScene).
 *
 *  [与 Game Framework 的关系]
 *  - Game Framework 中的 RenderComponent(可渲染组件)是 RenderProxy 的**生产者/持有者**;
 *    RenderComponent 创建/更新 RenderProxy, 渲染器只**只读消费**.
 *  - 一个可能的更高效率方案(待商榷): 各 RenderComponent 的代理与 World/Engine 持有的
 *    "总渲染代理" 交互, 由总渲染代理统一提交 —— 该决策在 Game Framework 侧, 不在本模块.
 *
 *  [增量渲染意图]
 *  RenderProxyWorld 通过 EChangeFlags 表达"本帧什么变了":
 *      - 游戏逻辑修改代理后 markChanged(...) 累积标志;
 *      - 渲染器帧首 consumeChangeFlags() 读取并清零;
 *      - 依据标志决定局部上传 / 重建几何 / 重新剔除排序 / 复用上一帧.
 * ============================================================================
 */
namespace runtime::renderer
{

/**
 * @brief 静态网格几何: 顶点/索引缓冲 + 顶点布局 + 包围球.
 *
 * 几何数据由 Mesh/资源系统加载后共享, 此处仅持有 RHI 句柄引用.
 * 包围球供后续视锥/遮挡剔除使用(v1 未启用, 仅预留).
 */
struct StaticMesh
{
	std::shared_ptr<rhi::RBuffer> VertexBuffer;
	std::shared_ptr<rhi::RBuffer> IndexBuffer;
	::renderer::VertexFactoryHandle VertexFactory;
	rhi::EIndexFormat IndexFormat { rhi::EIndexFormat::UInt32 };
	uint32_t IndexCount { 0 };

	/** 包围球中心(世界空间), 供剔除使用. */
	core::Vec3f BoundsCenter { 0.0f, 0.0f, 0.0f };
	/** 包围球半径. */
	float BoundsRadius { 0.0f };
};

/** @brief RenderProxy 的主分类(按职责而非系统来源). */
enum class ERenderProxyKind : uint8_t
{
	OpaqueMesh,
	TransparentMesh,
	RigidBodyMesh,
	TerrainPatch,
	Light,
	FluidSurface,
	GasVolume,
};

/** @brief 各类 RenderProxy 共享头信息. */
struct RenderProxyHeader
{
	uint64_t EntityId { 0 };
	ERenderProxyKind Kind { ERenderProxyKind::OpaqueMesh };
	uint32_t MaterialId { 0 };
	core::Matrix4x4 LocalToWorld {};
	core::Vec3f BoundsCenterWS { 0.0f, 0.0f, 0.0f };
	float BoundsRadius { 0.0f };
	uint32_t StableOrder { 0 };
};

/** @brief 几何代理: 网格/透明网格/刚体网格的渲染表示. */
struct MeshRenderProxy
{
	RenderProxyHeader Header;
	std::shared_ptr<StaticMesh> Mesh;
	bool CastShadow { true };
	core::Matrix4x4 PreviousLocalToWorld {};
	core::Vec3f LinearVelocityWS { 0.0f, 0.0f, 0.0f };
	core::Vec3f AngularVelocityWS { 0.0f, 0.0f, 0.0f };
	bool Sleeping { false };
};

/** @brief 地形代理: patch 级 LOD 与材质输入. */
struct TerrainRenderProxy
{
	RenderProxyHeader Header;
	TerrainPatchData Patch;
};

/** @brief 光源代理. */
struct LightRenderProxy
{
	RenderProxyHeader Header;
	LightData Light;
};

/** @brief 流体/水体代理. */
struct FluidRenderProxy
{
	RenderProxyHeader Header;
	FluidSurfaceData Fluid;
};

/** @brief 气体/体积介质代理. */
struct GasRenderProxy
{
	RenderProxyHeader Header;
	GasVolumeData Gas;
};

using RenderProxyVariant = std::variant<MeshRenderProxy, TerrainRenderProxy,
	LightRenderProxy, FluidRenderProxy, GasRenderProxy>;

/**
 * @brief 渲染代理: 一个 3D 可渲染对象在渲染器侧的数据(对应 UE 的 FPrimitiveSceneProxy).
 *
 * 由 Game Framework 的 RenderComponent 创建并更新; 渲染器/渲染线程只读.
 * MaterialId 是材质表索引, 由 RenderScene/MaterialSystem 解析为 Pipeline/BindGroup.
 * Transparent 决定排序策略(不透明按材质聚合, 透明按深度从后向前).
 * StableOrder 提供稳定提交序号, 保证同键对象顺序可预测.
 */
using RenderProxy = MeshRenderProxy;

/** @brief 一个可见代理: 代理索引 + 排序键. 是 RenderProxyWorld -> 子渲染器的交换结构. */
struct VisibleProxy
{
	uint32_t ProxyIndex { 0 };
	DrawPacket::SortKey Key { 0 };
};

/**
 * @brief 渲染代理世界数据容器: 渲染器侧对所有可渲染代理的聚合(对应 UE 的 FScene).
 */
class RenderProxyWorld
{
public:
	RenderProxyWorld() = default;

	RenderProxyWorld(const RenderProxyWorld&) = delete;
	RenderProxyWorld& operator=(const RenderProxyWorld&) = delete;

	/** @brief 注册任意类型代理, 返回稳定索引. */
	uint32_t addProxy(RenderProxyVariant InProxy);

	/** @brief 注册网格代理(历史兼容入口). */
	uint32_t addProxy(RenderProxy InProxy);

	/** @brief 移除代理(交换到末尾弹出, 旧索引失效). 置 Geometry|Visibility 变化标志. */
	void removeProxy(uint32_t Index);

	/** @brief 更新代理变换. 置 Transform 变化标志. */
	void updateTransform(uint32_t Index, const core::Matrix4x4& LocalToWorld);

	/** @brief 更新代理材质引用. 置 Material 变化标志. */
	void updateMaterial(uint32_t Index, uint32_t MaterialId);

	/** @brief 累积渲染意图(变化标志). */
	void markChanged(EChangeFlags Flags) noexcept;

	/**
	 * @brief 读取并清零本帧变化标志(渲染意图).
	 * @return 自上一次次调用以来累积的变化.
	 */
	[[nodiscard]] EChangeFlags consumeChangeFlags() noexcept;

	/**
	 * @brief 收集可见代理并按 SortKey 排序.
	 *
	 * v1 不做视锥/遮挡剔除(仅排序), 剔除见后续 P4 路线. 返回列表按 Key 升序稳定排序.
	 * @param Out 输出可见代理列表(会先清空).
	 */
	void collectVisible(std::vector<VisibleProxy>& Out) const;

	/**
	 * @brief 按代理分类构建帧交换包, 供 RenderServer / SceneRenderer 消费.
	 *
	 * v1 先构建分类数据列表(光源/流体/气体/地形/刚体), DrawPacket 的实际管线解析由
	 * MaterialSystem 后续填充.
	 */
	void buildSubmission(RenderFrameSubmission& Out, EChangeFlags WorldChanges,
		EChangeFlags UIChanges = EChangeFlags_t::None) const;

	/** @brief 清空所有代理并复位变化标志. */
	void clear() noexcept;

	[[nodiscard]] size_t getProxyCount() const noexcept { return Proxies.size(); }
	[[nodiscard]] const RenderProxyVariant* getProxy(uint32_t Index) const noexcept;
	[[nodiscard]] RenderProxyVariant* getProxy(uint32_t Index) noexcept;

	/**
	 * @brief 构造排序键.
	 *
	 * 编码(高位到低位): Transparent 标志 | MaterialId | StableOrder.
	 * 不透明与透明被天然分到两个连续区间, 便于子渲染器分段消费.
	 */
	[[nodiscard]] static DrawPacket::SortKey makeSortKey(bool Transparent, uint32_t MaterialId,
		uint32_t StableOrder) noexcept;

private:
	std::vector<RenderProxyVariant> Proxies;
	EChangeFlags ChangeFlags { EChangeFlags_t::All };
};

/**
 * @brief 向后兼容别名: 历史代码仍可使用 RenderWorld.
 * @note 新代码请优先使用 RenderProxyWorld 名称.
 */
using RenderWorld = RenderProxyWorld;

} // namespace runtime::renderer
