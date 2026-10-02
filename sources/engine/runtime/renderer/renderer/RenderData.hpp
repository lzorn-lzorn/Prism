#pragma once

#include <material/Material.hpp>
#include <RHI.hpp>

#include <core/math/Color.hpp>
#include <core/math/Matrix.hpp>
#include <core/math/Vector.hpp>
#include <core/wrappers/Flag.hpp>

#include <cstdint>
#include <array>
#include <memory>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - 渲染交换数据格式 (RendererServer 与外部/子系统之间的协议)
 * ============================================================================
 *
 *  本文件定义三类"数据协议", 它们是 Renderer 层与上层(场景/UI/游戏逻辑)解耦的边界:
 *
 *  1. EChangeFlags       —— 增量渲染意图(变化标志). 上层用它声明"本帧什么变了",
 *                            Renderer 据此决定全量重建还是局部更新.
 *
 *  2. DrawPacket/DrawList —— 3D 渲染交换格式. 场景系统把可见对象编码成不可变的
 *                            DrawPacket, RenderPass 只消费 packet 录制命令, 不访问
 *                            ECS/场景图, 从而支持并行收集与未来 RenderThread.
 *
 *  3. UIQuad/UIRenderBatch —— 2D/UI 渲染交换格式. 控件树(RenderTree)经 UIRenderer
 *                            合批后产出 UIBatchList, 2D 渲染器据此一次性绘制.
 *
 *  [设计原则]
 *  - 交换数据是"值语义"的 POD 聚合, 不持有 RHI 所有权之外的裸指针;
 *  - 纹理引用使用整数索引(TextureIndex)而非 RHI 对象, 由上层纹理表/图集统一解析;
 *  - 透明 2D 默认预乘 alpha, 由消费者(Shader 混合态)约定.
 * ============================================================================
 */
namespace runtime::renderer
{

// ============================================================================
//  1. 增量渲染意图: 变化标志
// ============================================================================

/**
 * @brief 一帧内可能发生变化的子系统, 供场景/UI 表达"渲染意图".
 *
 * 上层在每帧结束时调用 markChanged(...) 累积标志, Renderer 在下一帧开始时读取并清零.
 * Renderer 根据标志选择重建策略:
 *   - Transform/Material 变化 → 只重传对应 DrawPacket / 实例数据(增量上传);
 *   - Geometry 变化 → 重建顶点/索引缓冲;
 *   - Layout/Content 变化(UI) → 重新合批受影响区域;
 *   - 无变化 → 复用上一帧命令/画布(持久化离屏 Canvas).
 */
enum class EChangeFlags_t : uint32_t
{
	None = 0,
	/** 对象变换(位置/旋转/缩放)变化. */
	Transform = 1u << 0,
	/** 材质参数或绑定变化. */
	Material = 1u << 1,
	/** 几何(顶点/索引/拓扑)变化. */
	Geometry = 1u << 2,
	/** 可见性/剔除结果变化. */
	Visibility = 1u << 3,
	/** 光照(灯光列表/阴影)变化. */
	Light = 1u << 4,
	/** 相机(视图/投影)变化. */
	Camera = 1u << 5,
	/** UI 布局(控件位置/尺寸)变化. */
	Layout = 1u << 6,
	/** UI 内容(文本/纹理/颜色)变化. */
	Content = 1u << 7,
	/** 全部变化(等价于强制全量重建). */
	All = 0xFFFFFFFFu,
};

/** @brief 可组合的变化标志集合. */
using EChangeFlags = core::Flags<EChangeFlags_t>;
DEFINE_ENUM_OPERATOR(EChangeFlags_t);

// ============================================================================
//  2. 3D 渲染交换格式: DrawPacket
// ============================================================================

/**
 * @brief 一次绘制的不可变描述, 是 RenderPass 与场景系统之间的协议.
 *
 * DrawPacket 只包含"如何把一份几何画出来"的完整信息, 不持有场景对象/材质系统引用.
 * 因此可以在主线程之外并行收集, 也便于排序与缓存.
 *
 * 排序键(SortKey)由上层编码, 约定高位到低位依次为: Pass -> Pipeline -> Material ->
 * Depth -> 稳定提交序号, 保证透明对象顺序可预测.
 */
struct DrawPacket
{
	using SortKey = uint64_t;

	/** 顶点工厂句柄: 决定顶点语义/顶点着色器变体. */
	::renderer::VertexFactoryHandle VertexFactory;

	/** 可执行管线(不可变, 由 PipelineManager 缓存). */
	std::shared_ptr<rhi::RPipeline> Pipeline;
	/** 管线布局(与 BindGroups 的 ABI 对应). */
	std::shared_ptr<rhi::RPipelineLayout> PipelineLayout;

	/** 按 set 顺序排列的绑定组. */
	std::vector<std::shared_ptr<rhi::RBindGroup>> BindGroups;
	/** 与 BindGroups 中 DynamicOffset 资源一一对应的偏移(布局顺序). */
	std::vector<uint32_t> DynamicOffsets;

	/** 顶点缓冲绑定(slot 0..N). */
	std::vector<rhi::VertexBufferBinding> VertexBindings;
	/** 索引缓冲; 为空表示非索引绘制. */
	std::shared_ptr<rhi::RBuffer> IndexBuffer;
	rhi::DeviceSizeType IndexOffset { 0 };
	rhi::EIndexFormat IndexFormat { rhi::EIndexFormat::None };

	/** Push Constant 字节; 空则跳过. */
	std::vector<std::byte> PushConstants;

	/** 绘制参数. */
	uint32_t IndexCount { 0 };
	uint32_t InstanceCount { 1 };
	uint32_t FirstIndex { 0 };
	int32_t VertexOffset { 0 };
	uint32_t FirstInstance { 0 };

	/** 稳定排序键. */
	SortKey Key { 0 };
};

/** @brief 有序的绘制包列表, 通常按 SortKey 稳定排序后交给 RenderPass 消费. */
using DrawList = std::vector<DrawPacket>;

// ============================================================================
//  2.1 World -> Renderer 帧交换格式: 环境与仿真数据
// ============================================================================

/** @brief 光源类型. */
enum class ELightType : uint8_t
{
	Directional,
	Point,
	Spot,
};

/** @brief 光源的渲染侧快照(与游戏逻辑对象解耦). */
struct LightData
{
	ELightType Type { ELightType::Directional };
	float Intensity { 1.0f };
	float Range { 0.0f };
	float SpotInnerCos { 0.9f };
	float SpotOuterCos { 0.8f };

	core::Vec3f PositionWS { 0.0f, 0.0f, 0.0f };
	core::Vec3f DirectionWS { 0.0f, -1.0f, 0.0f };
	core::LinearColor4D Color { 1.0f, 1.0f, 1.0f, 1.0f };

	bool CastShadow { false };
	uint32_t ShadowSlice { 0 };
};

using LightList = std::vector<LightData>;

/** @brief 流体/水体渲染快照: 仿真纹理 + 绘制参数. */
struct FluidSurfaceData
{
	std::shared_ptr<rhi::RImageView> HeightField;
	std::shared_ptr<rhi::RImageView> NormalField;
	std::shared_ptr<rhi::RImageView> FlowField;

	std::shared_ptr<rhi::RBuffer> VertexBuffer;
	std::shared_ptr<rhi::RBuffer> IndexBuffer;
	uint32_t IndexCount { 0 };
	uint32_t MaterialId { 0 };

	float GridWorldSize { 1.0f };
	float FoamThreshold { 0.6f };
	float RefractionStrength { 0.02f };
};

using FluidList = std::vector<FluidSurfaceData>;

/** @brief 气体/体积介质渲染快照. */
struct GasVolumeData
{
	core::Vec3f BoundsMinWS { 0.0f, 0.0f, 0.0f };
	core::Vec3f BoundsMaxWS { 0.0f, 0.0f, 0.0f };

	std::shared_ptr<rhi::RImageView> Density3D;
	std::shared_ptr<rhi::RImageView> Temperature3D;
	std::shared_ptr<rhi::RImageView> Velocity3D;

	float Scattering { 0.04f };
	float Absorption { 0.01f };
	float AnisotropyG { 0.2f };
	uint32_t StepCount { 64 };
};

using GasVolumeList = std::vector<GasVolumeData>;

/** @brief 地形 Patch 渲染快照(按块管理 LOD 与材质). */
struct TerrainPatchData
{
	core::Matrix4x4 LocalToWorld {};
	uint32_t LodLevel { 0 };
	core::Vec2f PatchSizeMeters { 64.0f, 64.0f };

	std::shared_ptr<rhi::RBuffer> VertexBuffer;
	std::shared_ptr<rhi::RBuffer> IndexBuffer;
	uint32_t IndexCount { 0 };

	std::shared_ptr<rhi::RImageView> HeightMap;
	std::shared_ptr<rhi::RImageView> NormalMap;
	std::shared_ptr<rhi::RImageView> SplatMap;
	std::array<std::shared_ptr<rhi::RImageView>, 4> LayerAlbedo {};

	uint32_t MaterialId { 0 };
};

using TerrainPatchList = std::vector<TerrainPatchData>;

/** @brief 刚体渲染快照: 当前/上一帧变换用于运动矢量与 TAA. */
struct RigidBodyRenderData
{
	std::shared_ptr<rhi::RBuffer> VertexBuffer;
	std::shared_ptr<rhi::RBuffer> IndexBuffer;
	::renderer::VertexFactoryHandle VertexFactory;
	rhi::EIndexFormat IndexFormat { rhi::EIndexFormat::UInt32 };
	uint32_t IndexCount { 0 };

	uint32_t MaterialId { 0 };
	core::Matrix4x4 CurrentLocalToWorld {};
	core::Matrix4x4 PreviousLocalToWorld {};
	core::Vec3f LinearVelocityWS { 0.0f, 0.0f, 0.0f };
	core::Vec3f AngularVelocityWS { 0.0f, 0.0f, 0.0f };
	bool Sleeping { false };
	bool Transparent { false };
	uint32_t StableOrder { 0 };
};

using RigidBodyList = std::vector<RigidBodyRenderData>;

/**
 * @brief World 在帧边界提交给 Renderer 的统一交换包.
 *
 * DrawLists 是 RenderProxy 预处理后可直接录制命令的几何队列;
 * 其余列表是光照/体积/仿真系统的参数快照.
 */
struct RenderFrameSubmission
{
	DrawList ShadowDraws;
	DrawList OpaqueDraws;
	DrawList TransparentDraws;

	LightList Lights;
	FluidList Fluids;
	GasVolumeList Gases;
	TerrainPatchList Terrains;
	RigidBodyList RigidBodies;

	EChangeFlags WorldChanges {};
	EChangeFlags UIChanges {};

	void clear() noexcept
	{
		ShadowDraws.clear();
		OpaqueDraws.clear();
		TransparentDraws.clear();
		Lights.clear();
		Fluids.clear();
		Gases.clear();
		Terrains.clear();
		RigidBodies.clear();
		WorldChanges = EChangeFlags_t::None;
		UIChanges = EChangeFlags_t::None;
	}
};

// ============================================================================
//  3. 2D/UI 渲染交换格式: UIQuad / UIRenderBatch
// ============================================================================

/** @brief UI 四边形的几何/样式标志(合批前由控件树转换而来). */
enum class EUIQuadFlags_t : uint32_t
{
	None = 0,
	/** 九宫格(Sliced), UV 需携带 border 信息. */
	Sliced = 1u << 0,
	/** 圆角矩形(由 Shader 按 CornerRadius 处理). */
	Rounded = 1u << 1,
	/** 需要 Scissor 裁剪(使用 Bounds 作为裁剪矩形). */
	Clipped = 1u << 2,
};

/** @brief 可组合的 UI 四边形标志. */
using EUIQuadFlags = core::Flags<EUIQuadFlags_t>;
DEFINE_ENUM_OPERATOR(EUIQuadFlags_t);

/**
 * @brief 一个 2D/UI 图元, 由控件树的一条 RenderCommand 或一个 Glyph 转换而来.
 *
 * 采用屏幕空间包围盒 + UV + 预乘颜色的表示, 与最终顶点缓冲的 instance 数据一一对应.
 * 所有图元共享一个静态 Unit Quad 顶点/索引缓冲, 每图元只写一份 instance 数据.
 */
struct UIQuad
{
	/** 屏幕空间包围盒(左上角与右下角). */
	core::Vec2f Min { 0.0f, 0.0f };
	core::Vec2f Max { 0.0f, 0.0f };

	/** 纹理 UV 范围. */
	core::Vec2f UvMin { 0.0f, 0.0f };
	core::Vec2f UvMax { 1.0f, 1.0f };

	/** 预乘 alpha 颜色(RGB 已乘 A). */
	core::LinearColor4D Color { 1.0f, 1.0f, 1.0f, 1.0f };

	/** 纹理索引(指向纹理表/图集); 0 表示纯色填充. */
	uint32_t TextureIndex { 0 };

	/** 圆角半径(仅当 Flags 含 Rounded 时生效). */
	float CornerRadius { 0.0f };

	/** 层级/排序(深度), 用于稳定排序. */
	uint32_t Layer { 0 };

	/** 几何/样式标志. */
	EUIQuadFlags Flags {};
};

/**
 * @brief 一个可一次 draw 的 UI 批次: 相同(管线/混合/纹理)的图元被合并到一起.
 *
 * 合批键(BatchKey)由上层编码: Pipeline 变体 + 混合模式 + 纹理索引. 相同键的 UIQuad
 * 被合并进同一个 UIRenderBatch, 由 2D 渲染器一次性上传并绘制, 避免逐图元 draw.
 */
struct UIRenderBatch
{
	using BatchKey = uint64_t;

	/** 该批次的稳定合批键(决定是否可合并). */
	BatchKey Key { 0 };

	/** 本批次图元(共享同一管线与纹理). */
	std::vector<UIQuad> Quads;

	/** 解析后的管线(由 2D 渲染器/材质系统解析 BatchKey 得到, 收集期可为空). */
	std::shared_ptr<rhi::RPipeline> Pipeline;

	/** 纹理索引(本批次图元的 TextureIndex 必须一致). */
	uint32_t TextureIndex { 0 };

	/** 是否透明(决定绘制顺序与混合态). */
	bool Transparent { true };
};

/** @brief 有序的 UI 批次列表, 由 UIRenderer 从控件树合批产出. */
using UIBatchList = std::vector<UIRenderBatch>;

} // namespace runtime::renderer
