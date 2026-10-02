#pragma once

/**
 * ============================================================================
 *  renderer/rdg/RDG.hpp
 * ============================================================================
 *
 *  [在渲染分层中的位置] <<< 先读这一段 >>>
 *
 *  Renderer 层的 RDG 主干是 `rdg/RDGBuilder.hpp` (`runtime::renderer::RDGBuilder`):
 *  由 `RendererServer::renderFrameGraph()` 驱动, `SceneRenderer` 编排各子渲染器,
 *  是当前**唯一的帧录制路径**. 权威说明见 `documents/rdg_render_data_incremental_zh.md`.
 *
 *  本文件 (`rdg/RDG.hpp`, 命名空间 `renderer::rdg`) 是**算法侧的辅助设施**, 不是主干,
 *  也不是 `RDGBuilder` 的替代品:
 *
 *      RDGBuilder (主干)              RDG.hpp (本文件, 辅助)
 *      ─────────────────────          ────────────────────────────────
 *      Pass 按声明顺序执行              Kahn 拓扑排序(声明顺序无关)
 *      无 Pass 裁剪                    无用 Pass 裁剪(反向存活传播)
 *      TransientCache 按名称复用        显存别名(按堆 + 槽位 + 生命周期复用)
 *      逐 Pass 状态追踪                 按"显存槽位"追踪(别名点自动 Undefined 转换)
 *      importImage/Buffer               import + keepAlive 导出标记
 *
 *  两者的定位是"主干保留编排与录制, 本文件提供它当前尚未具备的图算法":
 *  `RDGBuilder` 的 v1 边界(见其头文件 [当前版本(v1)边界])明确写了不做拓扑排序 /
 *  别名 / 裁剪, 本文件正是这三项的完备实现与验证. 后续应把本文件的算法**收敛进**
 *  `RDGBuilder.compile()`, 而不是让两套 RDG 长期并行.
 *
 *  在此之前, 请按以下规则使用, 避免出现两条帧录制路径:
 *      - 帧录制/编排: 一律用 `RDGBuilder` + `SceneRenderer`;
 *      - 需要别名收益或图的独立验证: 用本文件, 且**不要**与 `RDGBuilder` 同时驱动同一帧.
 *
 *  [OVERVIEW]
 *  RDG (Render Dependency Graph) 是一套"先声明, 后编译, 再执行"的帧图调度系统.
 *  它把一帧的渲染工作拆成若干 Pass, 由每个 Pass 显式声明自己读写了哪些资源,
 *  再由图编译器统一推导:
 *
 *      1. 依赖关系   (RAW / WAR / WAW)
 *      2. 执行顺序   (Kahn 拓扑排序, 稳定最小 PassId 优先)
 *      3. 生命周期   ([FirstUse, LastUse] 区间)
 *      4. 显存别名   (按堆分类 + 区间分配器, 不同 Pass 复用同一段显存)
 *      5. 屏障       (layout / 可见性转换, 同一 Pass 内合并为一次转换)
 *
 *  于是"手工维护每个资源的 layout 与 barrier"这件事从渲染代码中消失, 只留下
 *  "我要读什么, 我要写什么".
 *
 *
 *  [为什么不用 core::HandleManager]
 *
 *  core::HandleManager 是进程级单例注册表, 内部用 shared_mutex 保护, 面向的是
 *  "跨系统长期存在的资源"(材质, 网格, 纹理). RDG 的资源是"帧内瞬态"的:
 *
 *      - 生命周期严格绑定在 RDGGraph 实例上, 不需要进程级注册;
 *      - 编译与执行都在渲染线程, 加锁纯属浪费;
 *      - 需要按 ResourceId 随机访问 + 顺序遍历, 这正是 vector 的强项.
 *
 *  因此这里沿用 RDG 讨论稿的"轻量值句柄"(ResourceId / PassId = uint32 索引),
 *  由 RDGGraph 集中持有数据, 不产生悬垂指针, 也不引入全局状态.
 *
 *
 *  [两阶段构建]
 *
 *      graph.addPass(...)  声明期 : 只记录描述, 不碰设备
 *      graph.compile()     编译期 : 纯 CPU 图算法, 产出可复用的 RDGCompiledPlan
 *      graph.execute(...)  执行期 : 录屏屏障 + 按序跑 Pass 回调
 *
 *  compile() 与 execute() 分离的意义:
 *
 *      - 图算法可以完全脱离 GPU 做单元测试;
 *      - 同一张图可以编译多次(例如窗口尺寸变化后重建), 也可以换设备重编;
 *      - 一帧的编译结果(RDGCompiledPlan)不持有设备指针, 可安全缓存.
 *
 *
 *  [瞬态资源与别名]
 *
 *  Transient (瞬态) 资源不参与跨帧存活, 因此可以复用显存:
 *
 *      ResourcePool::create()  -> 向设备申请"堆"(大块显存)
 *      pool.suballocate()      -> 在堆内按 (Offset, Size) 切一段出来 (placed resource)
 *      pool.createPlacedXxx()  -> 在指定显存区间上就地创建 Buffer/Image
 *
 *  如果后端不支持 placed resource(createHeap/createPlacedBuffer/createPlacedImage
 *  返回空), ResourcePool 自动退化为"每个资源独立申请", 功能不变, 只是没有别名收益.
 *
 * ============================================================================
 */

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <RHI.hpp>

namespace renderer::rdg
{

class RDGGraph;
struct RDGPass;

// ============================================================================
// 1. 句柄与常量
// ============================================================================

/** @brief 无效索引哨兵值. */
inline constexpr uint32_t InvalidId = ~0u;

/** @brief 区间分配器默认对齐(RHI 未提供时使用的保守值). */
inline constexpr uint64_t DefaultResourceAlignment = 256;

/** @brief 轻量资源句柄; 资源本体由 RDGGraph 集中持有. */
struct ResourceId
{
	uint32_t Value { InvalidId };

	[[nodiscard]] constexpr bool isValid() const noexcept { return Value != InvalidId; }
	constexpr bool operator==(const ResourceId&) const noexcept = default;
	constexpr bool operator<(const ResourceId& Other) const noexcept { return Value < Other.Value; }
};

/** @brief 轻量 Pass 句柄; Pass 本体由 RDGGraph 集中持有. */
struct PassId
{
	uint32_t Value { InvalidId };

	[[nodiscard]] constexpr bool isValid() const noexcept { return Value != InvalidId; }
	constexpr bool operator==(const PassId&) const noexcept = default;
	constexpr bool operator<(const PassId& Other) const noexcept { return Value < Other.Value; }
};

// ============================================================================
// 2. 访问模式 / 资源状态
// ============================================================================

/** @brief RDG 层声明的访问模式, 用于建立依赖边. */
enum class EResourceAccess : uint8_t
{
	Read      = 1 << 0,
	Write     = 1 << 1,
	ReadWrite = Read | Write,
};

/** @brief 同一资源在一个 Pass 上声明的访问集合. */
using EResourceAccessMask = uint8_t;

[[nodiscard]] constexpr EResourceAccessMask toMask(EResourceAccess Access) noexcept
{
	return static_cast<EResourceAccessMask>(Access);
}

[[nodiscard]] constexpr bool hasAccess(EResourceAccessMask Mask, EResourceAccess Test) noexcept
{
	return (Mask & toMask(Test)) != 0;
}

[[nodiscard]] constexpr bool isReadOnlyAccess(EResourceAccessMask Mask) noexcept
{
	return hasAccess(Mask, EResourceAccess::Read) && !hasAccess(Mask, EResourceAccess::Write);
}

/**
 * @brief 屏障使用的资源状态.
 *
 * 刻意不复用 rhi::EResourceState, 而是保留 RDG 自己的枚举再映射:
 * 图编译器只依赖这个枚举做状态推导, 后端状态增减不会改变图算法.
 */
enum class EResourceState : uint8_t
{
	Undefined,
	Common,
	VertexBuffer,
	IndexBuffer,
	UniformBuffer,
	ConstantBuffer,
	StorageBuffer,
	UnorderedAccess,
	DepthWrite,
	DepthRead,
	CopySrc,
	CopyDst,
	Present,
	RenderTarget,
	PixelShaderResource,
	NonPixelShaderResource,
};

/** @brief 把 RDG 状态映射为 RHI 状态. */
[[nodiscard]] constexpr rhi::EResourceState toRHIState(EResourceState State) noexcept
{
	switch (State)
	{
	case EResourceState::Undefined:              return rhi::EResourceState::Undefined;
	case EResourceState::Common:                 return rhi::EResourceState::Common;
	case EResourceState::VertexBuffer:           return rhi::EResourceState::VertexBuffer;
	case EResourceState::IndexBuffer:            return rhi::EResourceState::IndexBuffer;
	case EResourceState::UniformBuffer:          return rhi::EResourceState::UniformBuffer;
	case EResourceState::ConstantBuffer:         return rhi::EResourceState::ConstantBuffer;
	case EResourceState::StorageBuffer:          return rhi::EResourceState::StorageBuffer;
	case EResourceState::UnorderedAccess:        return rhi::EResourceState::UnorderedAccess;
	case EResourceState::DepthWrite:             return rhi::EResourceState::DepthWrite;
	case EResourceState::DepthRead:              return rhi::EResourceState::DepthRead;
	case EResourceState::CopySrc:                return rhi::EResourceState::CopySrc;
	case EResourceState::CopyDst:                return rhi::EResourceState::CopyDst;
	case EResourceState::Present:                return rhi::EResourceState::Present;
	case EResourceState::RenderTarget:           return rhi::EResourceState::RenderTarget;
	case EResourceState::PixelShaderResource:    return rhi::EResourceState::PixelShaderResource;
	case EResourceState::NonPixelShaderResource: return rhi::EResourceState::NonPixelShaderResource;
	}
	return rhi::EResourceState::Common;
}

/** @brief 该状态是否以写访问为主; 用于判断 Pass 内是否需要额外写屏障. */
[[nodiscard]] constexpr bool isWriteState(EResourceState State) noexcept
{
	switch (State)
	{
	case EResourceState::UnorderedAccess:
	case EResourceState::DepthWrite:
	case EResourceState::CopyDst:
	case EResourceState::RenderTarget:
	case EResourceState::Present:
		return true;
	default:
		return false;
	}
}

/** @brief 判断在 State 下执行 Access 是否合法. 编译期即可发现"采样写目标"这类错误. */
[[nodiscard]] constexpr bool isAccessValidInState(EResourceAccess Access, EResourceState State) noexcept
{
	if (State == EResourceState::Undefined)
		return false;
	if (Access == EResourceAccess::Write)
		return isWriteState(State);
	if (Access == EResourceAccess::Read)
		return !isWriteState(State) && State != EResourceState::Present;
	return true;
}

/** @brief 资源生命周期类别. */
enum class EResourceLifetime : uint8_t
{
	Transient, ///< 仅当前帧存活, 参与显存别名
	Imported,  ///< 外部注入(交换链图像 / 持久化 Buffer), 不参与别名
};

/** @brief 堆分类; 不同堆不共享别名空间. */
enum class EPoolKind : uint8_t
{
	RenderTarget,
	DepthStencil,
	Buffer,
	UAV,
	Count,
};

inline constexpr size_t PoolKindCount = static_cast<size_t>(EPoolKind::Count);

[[nodiscard]] constexpr std::string_view getPoolKindName(EPoolKind Kind) noexcept
{
	switch (Kind)
	{
	case EPoolKind::RenderTarget: return "RenderTarget";
	case EPoolKind::DepthStencil: return "DepthStencil";
	case EPoolKind::Buffer:       return "Buffer";
	case EPoolKind::UAV:          return "UAV";
	case EPoolKind::Count:        break;
	}
	return "Unknown";
}

/** @brief 资源物理类别. */
enum class EResourceKind : uint8_t
{
	None,
	Buffer,
	Texture,
};

// ============================================================================
// 3. 资源描述
// ============================================================================

/**
 * @brief 瞬态 Buffer 描述.
 * @note 这里只保留"决定显存"的字段; 名称等元数据由 RDG 自己管理.
 */
struct BufferDesc
{
	rhi::DeviceSizeType  Size { 0 };
	rhi::EBufferUsage    Usage {};
	rhi::EMemoryUsage    MemoryUsage { rhi::EMemoryUsage::Auto };
	rhi::EMemoryProperty MemoryProperty { rhi::EMemoryProperty_t::DeviceLocal };
	rhi::EMemoryProperty PreferredMemoryProperty {};
	float                MemoryPriority { 0.5f };
	bool                 DedicatedAllocation { false };
	bool                 PersistentlyMapped { false };
	std::string          Name;

	/** @brief 描述是否可用于创建资源. */
	[[nodiscard]] bool isValid() const noexcept
	{
		return Size != 0 && static_cast<bool>(Usage);
	}
};

/**
 * @brief 瞬态纹理描述(全部 mip / layer).
 * @note 不保留 DebugName: rhi::RImage::Descriptor_t 没有该字段.
 */
struct TextureDesc
{
	rhi::EFormat         Format { rhi::EFormat::Undefined };
	rhi::EImageDimension Dimension { rhi::EImageDimension::Texture2D };
	uint32_t             Width { 1 };
	uint32_t             Height { 1 };
	uint32_t             Depth { 1 };
	uint32_t             MipLevels { 1 };
	uint32_t             ArrayLayers { 1 };
	rhi::ESharingMode    SharingMode { rhi::ESharingMode::Exclusive };
	rhi::EMemoryProperty MemoryProperty { rhi::EMemoryProperty_t::DeviceLocal };
	rhi::EImageUsage     Usage {};
	rhi::ESampleCount    SampleCount { rhi::ESampleCount::Count1 };
	std::string          Name;

	/** @brief 描述是否可用于创建资源. */
	[[nodiscard]] bool isValid() const noexcept
	{
		return Format != rhi::EFormat::Undefined && Width != 0 && Height != 0 &&
			Depth != 0 && MipLevels != 0 && ArrayLayers != 0 && static_cast<bool>(Usage);
	}
};

// ============================================================================
// 4. 资源记录
// ============================================================================

/** @brief 图内一个资源的全部信息(声明 + 编译结果). */
struct RDGResource
{
	ResourceId        Id {};
	std::string       Name;
	EResourceKind     Kind { EResourceKind::None };
	EResourceLifetime Lifetime { EResourceLifetime::Transient };

	// ---- 声明期 ----
	std::optional<BufferDesc>  BufferDescription;
	std::optional<TextureDesc> TextureDescription;
	bool                       KeepAlive { false }; ///< 标记导出: 不参与别名

	// ---- 外部注入 (Lifetime == Imported) ----
	std::shared_ptr<rhi::RBuffer> ImportedBuffer;
	std::shared_ptr<rhi::RImage>  ImportedImage;

	// ---- 生命周期分析 (拓扑序 Pass 下标) ----
	uint32_t FirstUse { InvalidId };
	uint32_t LastUse { 0 };
	uint32_t UseCount { 0 };
	std::vector<PassId> ReferencingPasses;

	// ---- 别名分析结果 ----
	bool      Aliased { false };
	EPoolKind PoolIndex { EPoolKind::Count };
	/** @brief 计划用的对齐与大小; 实际 RHI 需求在实例化时取得. */
	uint64_t  PlannedSize { 0 };
	uint64_t  PlannedAlignment { 1 };
	/** @brief 计划内的显存偏移与占用大小(仅调试与断言用). */
	uint64_t  AliasOffset { 0 };
	uint64_t  AliasSize { 0 };

	/** @brief 所在显存槽位下标(InvalidId 表示独占分配). */
	uint32_t  MemorySlot { InvalidId };
	/** @brief 计划所在堆下标. */
	uint32_t  HeapIndex { InvalidId };

	[[nodiscard]] bool isTransient() const noexcept { return Lifetime == EResourceLifetime::Transient; }
	[[nodiscard]] bool isImported() const noexcept { return Lifetime == EResourceLifetime::Imported; }
	/** @brief 是否是本帧真正被使用到的瞬态资源(决定是否需要分配显存). */
	[[nodiscard]] bool isUsedTransient() const noexcept
	{
		return isTransient() && FirstUse != InvalidId;
	}

	/** @brief 估算显存占用(按 mip 金字塔逐级累加, 忽略驱动 padding). */
	[[nodiscard]] uint64_t computePlannedSize() const noexcept;
	/** @brief 计划对齐(RHI 未提供时为 DefaultResourceAlignment). */
	[[nodiscard]] uint64_t computePlannedAlignment() const noexcept;
};

/** @brief 一个 Pass 上对某个资源的访问声明. */
struct RDGResourceAccess
{
	ResourceId      Resource {};
	EResourceAccessMask Mask { 0 };
	EResourceState  State { EResourceState::Undefined };
};

/**
 * @brief 绑定到某个 RDG 资源上的真实 RHI 资源. 两种互斥, 由 Kind 决定.
 * @note 执行期由 RDGCompiledPlan 持有; Pass 回调只能拿到裸指针或 shared_ptr.
 */
struct RDGResourceBinding
{
	ResourceId                     Id {};
	EResourceKind                  Kind { EResourceKind::None };
	std::shared_ptr<rhi::RBuffer>  Buffer;
	std::shared_ptr<rhi::RImage>   Image;
	/** @brief placed 资源在该堆内的偏移(独立创建时为 0). */
	uint64_t                       MemoryOffset { 0 };

	[[nodiscard]] bool isValid() const noexcept
	{
		return Kind == EResourceKind::Buffer ? static_cast<bool>(Buffer)
		                                     : static_cast<bool>(Image);
	}
};

/**
 * @brief 传给 Pass 回调的执行上下文.
 *
 * 这是执行期访问 RHI 资源的唯一入口: Pass 只能通过 RDG 句柄取资源,
 * 从而保证"声明的依赖"与"实际使用"一致.
 */
class RDGPassContext
{
public:
	RDGPassContext(
		rhi::RCommandList& InCommandList,
		std::span<const RDGResourceBinding> InBindings,
		const RDGPass& InPass) noexcept
		: CommandList(&InCommandList), Bindings(InBindings), Pass(&InPass)
	{
	}

	[[nodiscard]] rhi::RCommandList& getCommandList() const noexcept { return *CommandList; }
	[[nodiscard]] const RDGPass& getPass() const noexcept { return *Pass; }

	/** @brief 取 Buffer; 未绑定或类型不符时返回 nullptr. */
	[[nodiscard]] rhi::RBuffer* getBuffer(ResourceId Id) const noexcept
	{
		if (!Id.isValid() || Id.Value >= Bindings.size())
			return nullptr;
		const RDGResourceBinding& Binding = Bindings[Id.Value];
		return Binding.Kind == EResourceKind::Buffer ? Binding.Buffer.get() : nullptr;
	}

	/** @brief 取 Image; 未绑定或类型不符时返回 nullptr. */
	[[nodiscard]] rhi::RImage* getTexture(ResourceId Id) const noexcept
	{
		if (!Id.isValid() || Id.Value >= Bindings.size())
			return nullptr;
		const RDGResourceBinding& Binding = Bindings[Id.Value];
		return Binding.Kind == EResourceKind::Texture ? Binding.Image.get() : nullptr;
	}

	/** @brief 取共享所有权(用于创建 BindGroup 等需要 shared_ptr 的场景). */
	[[nodiscard]] const std::shared_ptr<rhi::RBuffer>& getBufferShared(ResourceId Id) const noexcept
	{
		static const std::shared_ptr<rhi::RBuffer> Empty;
		if (!Id.isValid() || Id.Value >= Bindings.size())
			return Empty;
		return Bindings[Id.Value].Buffer;
	}

	[[nodiscard]] const std::shared_ptr<rhi::RImage>& getTextureShared(ResourceId Id) const noexcept
	{
		static const std::shared_ptr<rhi::RImage> Empty;
		if (!Id.isValid() || Id.Value >= Bindings.size())
			return Empty;
		return Bindings[Id.Value].Image;
	}

	/** @brief 带断言的 Buffer 访问. */
	[[nodiscard]] rhi::RBuffer& buffer(ResourceId Id) const
	{
		rhi::RBuffer* Found = getBuffer(Id);
		assert(Found && "RDG: buffer is not bound to this pass");
		return *Found;
	}

	/** @brief 带断言的 Image 访问. */
	[[nodiscard]] rhi::RImage& texture(ResourceId Id) const
	{
		rhi::RImage* Found = getTexture(Id);
		assert(Found && "RDG: texture is not bound to this pass");
		return *Found;
	}

private:
	rhi::RCommandList*                  CommandList { nullptr };
	std::span<const RDGResourceBinding> Bindings;
	const RDGPass*                      Pass { nullptr };
};

/** @brief Pass 执行回调签名. */
using RDGExecuteFn = std::function<void(RDGPassContext&)>;

/** @brief Pass 类别(仅用于自描述与调试). */
enum class ERDGPassType : uint8_t
{
	Raster,
	Compute,
	Copy,
	Present,
	Generic,
};

/** @brief 一个 Pass 的声明与编译结果. */
struct RDGPass
{
	PassId      Id {};
	std::string Name;
	ERDGPassType Type { ERDGPassType::Generic };

	std::vector<RDGResourceAccess> Accesses;
	std::vector<PassId>            Dependencies; ///< 拓扑排序后的直接前驱
	uint32_t                       TopoIndex { InvalidId };

	RDGExecuteFn Execute;

	/** @brief 查找本 Pass 对某资源的访问; 未声明时返回 nullptr. */
	[[nodiscard]] const RDGResourceAccess* findAccess(ResourceId Resource) const noexcept
	{
		for (const RDGResourceAccess& Access : Accesses)
			if (Access.Resource == Resource)
				return &Access;
		return nullptr;
	}

	[[nodiscard]] bool reads(ResourceId Resource) const noexcept
	{
		const RDGResourceAccess* Access = findAccess(Resource);
		return Access && hasAccess(Access->Mask, EResourceAccess::Read);
	}

	[[nodiscard]] bool writes(ResourceId Resource) const noexcept
	{
		const RDGResourceAccess* Access = findAccess(Resource);
		return Access && hasAccess(Access->Mask, EResourceAccess::Write);
	}
};

// ============================================================================
// 5. 屏障计划
// ============================================================================

/**
 * @brief 一次显存布局/可见性转换.
 *
 * 条目以"显存槽位"为单位而不是以资源为单位:
 * 被别名复用的两个资源共享同一段显存, 对驱动来说就是同一个对象, 状态必须连续.
 */
struct RDGBarrier
{
	/** @brief 触发该屏障的访问者. */
	ResourceId      Resource {};
	/** @brief 该访问者占用或独占的显存槽位. */
	uint32_t        MemorySlot { InvalidId };
	/** @brief 转换前的状态(首次使用时为 Undefined). */
	EResourceState  Before { EResourceState::Undefined };
	/** @brief 转换后的状态. */
	EResourceState  After { EResourceState::Undefined };
	/** @brief 显存被复用时为 true: 说明 Before 是"推断值", 需要将整段布局重置为 Undefined. */
	bool            AliasedMemory { false };
};

/** @brief 每个 Pass 执行前必须插入的屏障. */
struct RDGPassBarriers
{
	PassId                 Pass {};
	std::vector<RDGBarrier> Barriers;
};

// ============================================================================
// 6. 显存槽位与别名计划
// ============================================================================

/** @brief 一段可被多个资源复用的显存槽位. */
struct RDGMemorySlot
{
	uint32_t Slot { InvalidId };
	EPoolKind Pool { EPoolKind::Count };
	uint32_t HeapIndex { InvalidId };
	uint64_t Offset { 0 };
	uint64_t Size { 0 };
	uint64_t Alignment { 1 };
	/** @brief 逐次复用该槽位的资源, 按 FirstUse 升序. */
	std::vector<ResourceId> Owners;
};

/** @brief 一个堆的布局结果. */
struct RDGHeapLayout
{
	uint32_t HeapIndex { InvalidId };
	EPoolKind Pool { EPoolKind::Count };
	/** @brief 分配器实际增长到的峰值. */
	uint64_t PeakSize { 0 };
	/** @brief 实际请求设备创建的堆大小(对齐/上限修正后). */
	uint64_t ReservedSize { 0 };
	uint32_t SlotCount { 0 };
	/**
	 * @brief 该堆允许的内存类型位: 堆内全部资源需求类型位的交集.
	 * @note 由编译期向设备查询得到. 为 0 表示无法确定(此时池会拒绝别名).
	 */
	uint32_t MemoryTypeBits { 0 };
};

/** @brief 别名计划是否自洽: 任意两个共享槽位的资源, 生命周期不得重叠. */
struct RDGAliasValidation
{
	bool     isValid { true };
	uint32_t FirstConflictSlot { InvalidId };
	ResourceId First { };
	ResourceId Second { };
};

// ============================================================================
// 8. 资源池: 把别名计划翻译成真实 RHI 资源
// ============================================================================

/** @brief 池内一处已申请的显存区间. */
struct RDGHeapAllocation
{
	std::shared_ptr<rhi::RTransientHeap> Heap;
	uint64_t                             Offset { 0 };
	uint64_t                             Size { 0 };
};

/**
 * @brief 瞬态资源池接口.
 *
 * compile() 只产出"计划", 由池把计划落到真实资源上. 分离二者的好处:
 *   - 图算法可脱离 GPU 测试;
 *   - 同一计划可以换设备重新实例化;
 *   - 后端不支持 placed resource 时自动退化.
 */
class IRDGResourcePool
{
public:
	virtual ~IRDGResourcePool() = default;

	/** @brief 该池是否真的支持显存别名(堆 + placed resource). */
	[[nodiscard]] virtual bool supportsAliasing() const noexcept = 0;

	/**
	 * @brief 在申请堆之前登记一个瞬态资源的显存需求.
	 *
	 * 池需要提前知道"这个资源能用哪些内存类型", 才能在 beginFrame() 里创建出
	 * 一个真正能容纳它的堆. 实现方应把需求按堆汇总到对应的 RDGHeapLayout 上.
	 *
	 * @param Resource 资源句柄
	 * @param Kind 资源类别
	 * @param Heap 该资源所在的堆布局(可由实现回填 MemoryTypeBits)
	 * @param Buffer Buffer 描述(Kind == Buffer 时有效)
	 * @param Texture Texture 描述(Kind == Texture 时有效)
	 */
	virtual void prepareResource(
		ResourceId Resource,
		EResourceKind Kind,
		RDGHeapLayout& Heap,
		const BufferDesc* Buffer,
		const TextureDesc* Texture)
	{
		(void)Resource; (void)Kind; (void)Heap; (void)Buffer; (void)Texture;
	}

	/** @brief 为新一帧准备堆布局; 同一帧内的资源分配必须在 beginFrame 之后进行. */
	virtual void beginFrame(std::span<const RDGHeapLayout> Heaps) = 0;

	/**
	 * @brief 在指定堆内申请一段区间.
	 * @param HeapIndex 目标堆
	 * @param Size 需要的字节数
	 * @param Alignment 需要的对齐
	 * @param ResourceIndex 发起申请的资源下标(仅用于诊断)
	 * @return 区间; 不支持别名或堆内放不下时返回 std::nullopt
	 */
	[[nodiscard]] virtual std::optional<RDGHeapAllocation> allocateMemory(
		uint32_t HeapIndex,
		uint64_t Size,
		uint64_t Alignment,
		uint32_t ResourceIndex) = 0;

	/** @brief 在已有区间上就地创建 Buffer. */
	[[nodiscard]] virtual std::shared_ptr<rhi::RBuffer> createPlacedBuffer(
		const RDGHeapAllocation& Allocation,
		const BufferDesc& Description) = 0;

	/** @brief 在已有区间上就地创建 Image. */
	[[nodiscard]] virtual std::shared_ptr<rhi::RImage> createPlacedImage(
		const RDGHeapAllocation& Allocation,
		const TextureDesc& Description) = 0;

	/** @brief 独立创建一个 Buffer(退化路径). */
	[[nodiscard]] virtual std::shared_ptr<rhi::RBuffer> createBuffer(
		const BufferDesc& Description) = 0;

	/** @brief 独立创建一个 Image(退化路径). */
	[[nodiscard]] virtual std::shared_ptr<rhi::RImage> createImage(
		const TextureDesc& Description) = 0;

	/** @brief 释放所有堆; 调用前必须保证 GPU 已经空闲. */
	virtual void releaseHeaps() {}
};

// ============================================================================
// 9. 编译计划
// ============================================================================

/**
 * @brief 编译结果: 不含 Pass 回调与设备指针, 只描述"怎么执行".
 *
 * 可以重复执行, 也可以在设备重建后重新实例化资源再执行.
 */
class RDGCompiledPlan
{
public:
	[[nodiscard]] std::span<const PassId> getExecutionOrder() const noexcept { return ExecutionOrder; }
	[[nodiscard]] std::span<const RDGPassBarriers> getPassBarriers() const noexcept { return PassBarriers; }
	[[nodiscard]] std::span<const RDGMemorySlot> getMemorySlots() const noexcept { return MemorySlots; }
	[[nodiscard]] std::span<const RDGHeapLayout> getHeapLayouts() const noexcept { return HeapLayouts; }
	[[nodiscard]] std::span<const RDGResourceBinding> getBindings() const noexcept { return Bindings; }
	[[nodiscard]] uint64_t getTotalHeapBytes() const noexcept { return TotalHeapBytes; }
	[[nodiscard]] uint32_t getCulledPassCount() const noexcept { return CulledPassCount; }
	[[nodiscard]] bool isAliasingEnabled() const noexcept { return AliasingEnabled; }

	/** @brief 别名计划自洽性检查(同一槽位内生命周期不得重叠). */
	[[nodiscard]] RDGAliasValidation validateAliasing(
		std::span<const RDGResource> Resources) const;

	/**
	 * @brief 检查同一堆内所有资源的显存区间互不重叠.
	 *
	 * 这是别名正确性的核心不变量: 堆内任意两个资源的 [AliasOffset, +AliasSize)
	 * 都不允许相交. 生命周期不重叠由 validateAliasing() 负责, 两者合起来
	 * 才能保证"复用同一段显存"是安全的.
	 */
	[[nodiscard]] bool validateNoOverlap(std::span<const RDGResource> Resources) const;

	/** @brief 查找某个 Pass 的屏障列表; 没有则返回空 span. */
	[[nodiscard]] std::span<const RDGBarrier> findBarriers(PassId Id) const noexcept
	{
		for (const RDGPassBarriers& Entry : PassBarriers)
			if (Entry.Pass == Id)
				return Entry.Barriers;
		return {};
	}

private:
	friend class RDGGraph;

	std::vector<PassId>               ExecutionOrder;
	std::vector<RDGPassBarriers>      PassBarriers;
	std::vector<RDGMemorySlot>        MemorySlots;
	std::vector<RDGHeapLayout>        HeapLayouts;
	std::vector<RDGResourceBinding>   Bindings;
	/** @brief 每个资源在堆内的对齐要求(RHI 需求驱动, 与计划用的对齐无关). */
	std::vector<uint64_t>             AllocationAlignments;
	uint64_t                          TotalHeapBytes { 0 };
	uint32_t                          CulledPassCount { 0 };
	bool                              AliasingEnabled { false };
};

// ============================================================================
// 10. RDGBuilder: Pass 内的资源声明 API
// ============================================================================

/**
 * @brief 声明期传给 Setup 回调的 Builder.
 *
 * 所有 Setter 都返回 *this 以便链式书写; 资源创建/导入会立即在图内注册并返回句柄.
 */
class RDGBuilder
{
public:
	RDGBuilder(RDGGraph& InGraph, RDGPass& InPass) noexcept
		: Graph(&InGraph), Pass(&InPass) {}

	// ---- 资源创建 ----
	/** @brief 创建瞬态纹理. @return 资源句柄; 描述非法时抛出 std::invalid_argument. */
	[[nodiscard]] ResourceId createTexture(std::string_view Name, const TextureDesc& Description);
	/** @brief 创建瞬态 Buffer. */
	[[nodiscard]] ResourceId createBuffer(std::string_view Name, const BufferDesc& Description);

	/** @brief 注入外部图像(交换链 / 持久化 RTT), 不参与别名. */
	[[nodiscard]] ResourceId importTexture(std::string_view Name, std::shared_ptr<rhi::RImage> Image);
	/** @brief 注入外部 Buffer, 不参与别名. */
	[[nodiscard]] ResourceId importBuffer(std::string_view Name, std::shared_ptr<rhi::RBuffer> Buffer);

	/**
	 * @brief 标记资源必须导出到图外(读回 / 跨帧), 从而退出别名复用.
	 * @note 可用于本 Pass 或之前 Pass 创建的资源.
	 */
	RDGBuilder& keepAlive(ResourceId Id);

	// ---- 依赖声明 ----
	/** @brief 声明只读访问(如采样、顶点输入). */
	RDGBuilder& read(ResourceId Id, EResourceState State);
	/** @brief 声明只写访问(如渲染目标、UAV). */
	RDGBuilder& write(ResourceId Id, EResourceState State);
	/** @brief 声明读改写访问(如颜色混合、原子操作). */
	RDGBuilder& readWrite(ResourceId Id, EResourceState State);

	/**
	 * @brief 声明一次访问; 重复声明同一资源会合并, 后声明的状态覆盖先声明的读状态.
	 * @return 实际生效的访问集合.
	 */
	EResourceAccessMask access(ResourceId Id, EResourceAccess Access, EResourceState State);

	// ---- 渲染目标便捷声明 ----
	/**
	 * @brief 便捷声明"渲染目标".
	 * @param Color 颜色附件(写 RenderTarget)
	 * @param Depth 深度附件(写 DepthWrite), 传无效句柄表示无深度
	 */
	RDGBuilder& setRenderTargets(std::span<const ResourceId> Color, ResourceId Depth = {});

	/** @brief 便捷声明"计算 Pass"的 UAV 读写. */
	RDGBuilder& setUnorderedAccess(ResourceId Id) { return readWrite(Id, EResourceState::UnorderedAccess); }

	// ---- 执行体 ----
	/** @brief 设置本 Pass 的执行回调. */
	RDGBuilder& setExecute(RDGExecuteFn Fn) &
	{
		Pass->Execute = std::move(Fn);
		return *this;
	}

	/** @brief 设置 Pass 类别(仅自描述/调试用途). */
	RDGBuilder& setType(ERDGPassType InType)
	{
		Pass->Type = InType;
		return *this;
	}

	[[nodiscard]] RDGGraph&       getGraph() noexcept { return *Graph; }
	[[nodiscard]] const RDGGraph& getGraph() const noexcept { return *Graph; }
	[[nodiscard]] PassId          getPassId() const noexcept { return Pass->Id; }

private:
	RDGGraph* Graph { nullptr };
	RDGPass*  Pass { nullptr };
};

// ============================================================================
// 11. RDGGraph
// ============================================================================

/**
 * @brief 帧图主体: 持有资源与 Pass 数据, 负责编译与执行.
 *
 * 线程约定: 一个 RDGGraph 实例同一时刻只应被一个线程构建/编译/录制.
 * 多帧并行请使用多个实例(通常每帧在飞一份).
 */
class RDGGraph
{
public:
	RDGGraph() = default;

	RDGGraph(const RDGGraph&)            = delete;
	RDGGraph& operator=(const RDGGraph&) = delete;

	// ------------------------------------------------------------------
	// 构建
	// ------------------------------------------------------------------

	/**
	 * @brief 添加一个 Pass.
	 * @param Name Pass 名称(调试标签)
	 * @param Setup 声明回调, 形如 void(RDGBuilder&)
	 * @return 新 Pass 的句柄
	 */
	template <typename SetupFn>
	PassId addPass(std::string_view Name, SetupFn&& Setup)
	{
		RDGPass& Pass = beginPass(Name);
		RDGBuilder Builder(*this, Pass);
		std::invoke(std::forward<SetupFn>(Setup), Builder);
		endPass(Pass);
		return Pass.Id;
	}

	/** @brief 添加仅用于资源声明/导入的 Pass(无执行体, 编译期会被裁剪). */
	template <typename SetupFn>
	PassId addSetupPass(std::string_view Name, SetupFn&& Setup)
	{
		return addPass(Name, std::forward<SetupFn>(Setup));
	}

	// ------------------------------------------------------------------
	// 编译 / 执行
	// ------------------------------------------------------------------

	/**
	 * @brief 编译: 依赖构建 -> 拓扑排序 -> 生命周期 -> 别名 -> 屏障.
	 * @param Pool 资源池; 非空时同时实例化瞬态资源
	 * @param EnableAliasing 是否启用显存别名(默认启用, 便于对照测试)
	 * @return 编译计划, 供 execute() 使用
	 */
	[[nodiscard]] RDGCompiledPlan compile(
		IRDGResourcePool* Pool = nullptr,
		bool EnableAliasing = true);

	/**
	 * @brief 执行: 按拓扑序插入屏障并依次调用 Pass 回调.
	 * @param CommandList 目标命令列表(必须已 begin())
	 * @param Plan 之前 compile() 产出的计划
	 */
	void execute(rhi::RCommandList& CommandList, const RDGCompiledPlan& Plan) const;

	/** @brief 清空全部资源与 Pass, 以便复用同一个图对象构建下一帧. */
	void reset();

	// ------------------------------------------------------------------
	// 查询
	// ------------------------------------------------------------------

	[[nodiscard]] const std::vector<RDGResource>& getResources() const noexcept { return Resources; }
	[[nodiscard]] const std::vector<RDGPass>&     getPasses() const noexcept { return Passes; }

	[[nodiscard]] const RDGResource* findResource(ResourceId Id) const noexcept
	{
		return Id.isValid() && Id.Value < Resources.size() ? &Resources[Id.Value] : nullptr;
	}

	[[nodiscard]] RDGResource* findResourceMut(ResourceId Id) noexcept
	{
		return Id.isValid() && Id.Value < Resources.size() ? &Resources[Id.Value] : nullptr;
	}

	/** @brief 按名称查找资源; 找不到返回无效句柄. */
	[[nodiscard]] ResourceId findResourceByName(std::string_view Name) const noexcept;
	/** @brief 按名称查找 Pass; 找不到返回无效句柄. */
	[[nodiscard]] PassId findPassByName(std::string_view Name) const noexcept;

	[[nodiscard]] bool isAliasingEnabled() const noexcept { return AliasingEnabled; }

	// ------------------------------------------------------------------
	// 内部: 供 Builder 使用
	// ------------------------------------------------------------------

	/** @brief 注册一个资源记录并返回句柄(供 RDGBuilder 调用). */
	ResourceId registerResource(RDGResource Resource);

	/** @brief 取下一个资源申请到的显存槽位下标(仅编译期有效). */
	RDGPass& beginPass(std::string_view Name);
	void     endPass(RDGPass& Pass);

private:
	// 编译子阶段
	void buildDependencyGraph();
	void topologicalSort();
	void analyzeLifetimes();
	void cullPasses();
	void buildAliasPlan(bool EnableAliasing);
	void generateBarriers();
	void instantiateResources(IRDGResourcePool* Pool, RDGCompiledPlan& Plan);

	RDGPass* findPassMut(PassId Id) noexcept
	{
		return Id.isValid() && Id.Value < Passes.size() ? &Passes[Id.Value] : nullptr;
	}

	/** @brief 复制到 RDGCompiledPlan 的编译产物. */
	std::vector<RDGResource> Resources;
	std::vector<RDGPass>     Passes;
	std::vector<PassId>      SortedPasses;

	std::unordered_map<std::string, ResourceId> NameToResource;
	std::unordered_map<std::string, PassId>     NameToPass;

	// 编译中间结果(每次 compile() 重建).
	std::vector<RDGMemorySlot>                            MemorySlotScratch;
	std::vector<RDGHeapLayout>                            HeapLayoutScratch;
	std::unordered_map<uint32_t, std::vector<RDGBarrier>> PassBarrierScratch;
	/** @brief 资源 -> 显存槽位下标. */
	std::vector<uint32_t>                                 ResourceSlot;

	bool AliasingEnabled { true };
};

// ============================================================================
// 12. 惰性创建的同帧图门面
// ============================================================================

/**
 * @brief 一次 AddPass 的延迟配置对象; 析构时提交.
 *
 * 相比 lambda 版 addPass(), 该门面更适合"在宿主代码里分多行写一段 Pass"
 * 或者从返回的 Builder 里再创建资源的场景.
 */
class RDGPassBuilder
{
public:
	RDGPassBuilder(RDGGraph& InGraph, std::string_view Name)
		: Graph(&InGraph), Pass(InGraph.beginPass(Name)), Builder(InGraph, Pass) {}

	~RDGPassBuilder() { Graph->endPass(Pass); }

	RDGPassBuilder(const RDGPassBuilder&)            = delete;
	RDGPassBuilder& operator=(const RDGPassBuilder&) = delete;

	[[nodiscard]] RDGBuilder&  builder() noexcept { return Builder; }
	[[nodiscard]] RDGBuilder*  operator->() noexcept { return &Builder; }
	[[nodiscard]] RDGBuilder&  operator*() noexcept { return Builder; }
	[[nodiscard]] PassId       getPassId() const noexcept { return Pass.Id; }

private:
	RDGGraph*  Graph { nullptr };
	RDGPass&   Pass;
	RDGBuilder Builder;
};

/** @brief 以链式门面开始一个 Pass. 用法: `auto P = graph.pass("GBuffer"); P->write(...);` */
[[nodiscard]] inline RDGPassBuilder beginPass(RDGGraph& Graph, std::string_view Name)
{
	return RDGPassBuilder(Graph, Name);
}

// ============================================================================
// 13. 池实现
// ============================================================================

/** @brief 堆布局参数. */
struct RDGPoolOptions
{
	/** @brief 每个堆的最小预留大小(驱动对最小分配有隐式下限). */
	uint64_t MinimumHeapSize { 64ull * 1024ull };
	/** @brief 堆大小向上取整粒度. */
	uint64_t HeapSizeGranularity { 64ull * 1024ull };
	/** @brief 单个堆预留上限; 超过时按资源独立创建, 避免一次申请过大的显存. */
	uint64_t MaximumHeapSize { 256ull * 1024ull * 1024ull };
	/** @brief 强制堆为私有, 便于 profiler 观察. */
	bool DedicatedHeaps { true };
};

/**
 * @brief 默认资源池: 后端支持 placed resource 时走堆 + 别名, 否则逐资源独立创建.
 */
class RDGResourcePool final : public IRDGResourcePool
{
public:
	explicit RDGResourcePool(rhi::RDevice& InDevice, RDGPoolOptions Options = {}) noexcept
		: Device(&InDevice), PoolOptions(Options)
	{
	}

	[[nodiscard]] bool supportsAliasing() const noexcept override
	{
		return AliasingSupported;
	}

	void prepareResource(
		ResourceId Resource,
		EResourceKind Kind,
		RDGHeapLayout& Heap,
		const BufferDesc* Buffer,
		const TextureDesc* Texture) override;

	void beginFrame(std::span<const RDGHeapLayout> Heaps) override;
	[[nodiscard]] std::optional<RDGHeapAllocation> allocateMemory(
		uint32_t HeapIndex,
		uint64_t Size,
		uint64_t Alignment,
		uint32_t ResourceIndex) override;
	[[nodiscard]] std::shared_ptr<rhi::RBuffer> createPlacedBuffer(
		const RDGHeapAllocation& Allocation,
		const BufferDesc& Description) override;
	[[nodiscard]] std::shared_ptr<rhi::RImage> createPlacedImage(
		const RDGHeapAllocation& Allocation,
		const TextureDesc& Description) override;
	[[nodiscard]] std::shared_ptr<rhi::RBuffer> createBuffer(const BufferDesc& Description) override;
	[[nodiscard]] std::shared_ptr<rhi::RImage> createImage(const TextureDesc& Description) override;
	void releaseHeaps() override;

	/** @brief 禁用别名(用于 A/B 对照: 关掉就走独立创建路径). */
	void setAliasingEnabled(bool Enabled) noexcept { AliasingEnabled = Enabled; }

	/** @brief 实际创建的堆数量(测试/诊断用). */
	[[nodiscard]] size_t getHeapCount() const noexcept { return Heaps.size(); }

	/** @brief 取某个堆; 越界或未创建时返回空(测试/诊断用). */
	[[nodiscard]] const std::shared_ptr<rhi::RTransientHeap>& getHeap(size_t Index) const noexcept
	{
		static const std::shared_ptr<rhi::RTransientHeap> Empty;
		return Index < Heaps.size() ? Heaps[Index] : Empty;
	}

private:
	rhi::RDevice*  Device { nullptr };
	RDGPoolOptions PoolOptions;
	bool           AliasingEnabled { true };
	bool           AliasingSupported { false };
	/** @brief 每个堆的 RHI 对象, 与计划中的堆下标一一对应. */
	std::vector<std::shared_ptr<rhi::RTransientHeap>> Heaps;
	/** @brief 本帧实际可用的堆(申请失败的堆不会参与子分配). */
	std::vector<bool> CreatedHeaps;
	/** @brief 本帧切出去的区间; 下一帧 beginFrame 时统一归还. */
	struct ReleasedRange
	{
		std::shared_ptr<rhi::RTransientHeap> Heap;
		uint64_t                             Offset { 0 };
		uint64_t                             Size { 0 };
	};
	std::vector<ReleasedRange> Released;
};

// ============================================================================
// 14. RDGResource 辅助实现
// ============================================================================

namespace detail
{
/** @brief 单张 mip 的字节数估算(不含行/层对齐 padding). */
[[nodiscard]] inline uint64_t estimateMipBytes(
	uint32_t Width,
	uint32_t Height,
	uint32_t Depth,
	uint32_t BytesPerPixel) noexcept
{
	return static_cast<uint64_t>(Width) * Height * Depth * BytesPerPixel;
}
} // namespace detail

inline uint64_t RDGResource::computePlannedSize() const noexcept
{
	if (Kind == EResourceKind::Buffer && BufferDescription)
		return BufferDescription->Size;

	if (Kind == EResourceKind::Texture && TextureDescription)
	{
		const TextureDesc& Texture = *TextureDescription;
		const rhi::EImageAspect Aspect = rhi::isDepthFormat(Texture.Format)
			? rhi::EImageAspect::Depth
			: rhi::EImageAspect::Color;
		const uint32_t BytesPerPixel =
			rhi::calPixelSizeFormEFormat(Texture.Format) +
			(Aspect == rhi::EImageAspect::Depth && rhi::hasStencilAspect(Texture.Format) ? 1u : 0u);
		if (BytesPerPixel == 0)
			return 0;

		uint64_t Total = 0;
		uint32_t Width = Texture.Width;
		uint32_t Height = Texture.Height;
		uint32_t Depth = Texture.Depth;
		const uint32_t Layers = std::max(1u, Texture.ArrayLayers);
		for (uint32_t Mip = 0; Mip < std::max(1u, Texture.MipLevels); ++Mip)
		{
			Total += detail::estimateMipBytes(Width, Height, Depth, BytesPerPixel) * Layers;
			Width = std::max(1u, Width >> 1);
			Height = std::max(1u, Height >> 1);
			Depth = std::max(1u, Depth >> 1);
		}
		return Total;
	}
	return 0;
}

inline uint64_t RDGResource::computePlannedAlignment() const noexcept
{
	return DefaultResourceAlignment;
}

} // namespace renderer::rdg
