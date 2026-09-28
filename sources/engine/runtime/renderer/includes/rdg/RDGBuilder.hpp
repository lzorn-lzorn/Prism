#pragma once

#include <RHI.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - RenderGraph (RDG) 基础设施
 * ============================================================================
 *
 *  [定位]
 *  RDG 是 Renderer 层对"多 Pass 一帧"的编排设施. 它位于 RHI 之上, 只负责:
 *      - 资源(pass 间共享的纹理/缓冲)的声明与生命周期;
 *      - Pass 之间的读写依赖与资源状态(Layout)追踪;
 *      - 自动生成 barrier 与 Dynamic Rendering 的 beginRendering/endRendering;
 *      - 让 RendererServer 摆脱"所有逻辑堆在 renderFrame()"的困境.
 *
 *  [明确不做]
 *  RDG 不产生原生 Vulkan 对象、不绕过 RHI 直接生成 barrier、不做材质/场景/可见性.
 *  它生成的必须是 RHI 公开的 GlobalBarrier / BufferBarrier / ImageBarrier.
 *
 *  [当前版本(v1)边界]
 *      - Pass 按声明顺序执行(拓扑排序留给后续版本);
 *      - 单 Graphics 队列(跨队列所有权转移留给后续版本);
 *      - transient 资源按"名称+描述"缓存复用, 尚未实现内存别名(alias heap);
 *      - 不做无用 Pass 裁剪(dead-code elimination).
 *
 *  [依赖方向]
 *      RendererServer -> RDGBuilder -> RHI -> Vulkan
 *
 *  [典型用法]
 *      RDGBuilder builder(device);
 *      auto sceneColor = builder.importImage(swapchainImage, swapchainView, "SceneColor");
 *
 *      builder.addGraphicsPass("Opaque",
 *          [&](RDGPassBuilder& p) {
 *              p.renderTarget(sceneColor, rhi::ELoadOp::Clear, rhi::EStoreOp::Store);
 *          },
 *          [&](RDGExecuteContext& ctx) {
 *              ctx.getCommandList().drawIndexed(...);   // 仅绑定 + 绘制
 *          });
 *
 *      builder.compile();                       // 状态追踪 + barrier 规划
 *      builder.execute(commandList);            // 录制整帧
 * ============================================================================
 */
namespace runtime::renderer
{

/** @brief RenderGraph 内部资源的稳定句柄; 与 RHI 对象一一对应(import 或 transient). */
using RDGResourceId = uint32_t;
inline constexpr RDGResourceId InvalidRDGResourceId = ~0u;

// ============================================================================
//  资源描述
// ============================================================================

/**
 * @brief transient 纹理的声明描述. 与 RImage::Descriptor_t 对应但刻意独立:
 *        RDG 描述的是"本帧图形语义", 不包含内存策略(DeviceLocal/SharingMode 等).
 */
struct RDGTextureDesc
{
	rhi::EFormat Format { rhi::EFormat::Undefined };
	uint32_t Width { 1 };
	uint32_t Height { 1 };
	uint32_t Depth { 1 };
	uint32_t MipLevels { 1 };
	uint32_t ArrayLayers { 1 };
	rhi::EImageUsage Usage {};
	rhi::ESampleCount Samples { rhi::ESampleCount::Count1 };

	/**
	 * @brief true 表示生命周期仅限本帧图内, RDG 可跨帧复用同一物理资源(别名).
	 *        v1 仅按名称缓存复用, 不做显存别名.
	 */
	bool Transient { true };

	[[nodiscard]] bool operator==(const RDGTextureDesc&) const = default;
};

/**
 * @brief transient 缓冲的声明描述. 与 RBuffer::Descriptor_t 对应.
 */
struct RDGBufferDesc
{
	rhi::DeviceSizeType Size { 0 };
	rhi::EBufferUsage Usage {};

	[[nodiscard]] bool operator==(const RDGBufferDesc&) const = default;
};

/** @brief 一次逻辑 Pass 的执行类别. */
enum class ERDGPassType : uint8_t
{
	Graphics, // 需要 beginRendering/endRendering 作用域
	Compute,  // 无渲染作用域, 执行 dispatch
	Copy      // 无渲染作用域, 执行 copy/blit/fill
};

// 前向声明(内部实现结构会互相引用)
class RDGBuilder;
class RDGPassBuilder;
class RDGExecuteContext;

// ============================================================================
//  内部实现结构(builder 与 builder 的友元可见, 不对外)
// ============================================================================
namespace detail
{

enum class EResourceKind : uint8_t { Texture, Buffer };

struct RDGResource
{
	EResourceKind Kind { EResourceKind::Texture };
	RDGTextureDesc TextureDesc;
	RDGBufferDesc BufferDesc;
	std::string Name;
	bool Imported { false };

	// 运行时 RHI 句柄(import 或懒分配)
	std::shared_ptr<rhi::RImage> Image;
	std::shared_ptr<rhi::RImageView> ImageView;
	std::shared_ptr<rhi::RBuffer> Buffer;

	// 状态追踪(compile 期间使用)
	rhi::EResourceState LastState { rhi::EResourceState::Undefined };
	bool LastAccessWasWrite { false };
};

struct RDGAttachmentDecl
{
	RDGResourceId Resource { InvalidRDGResourceId };
	rhi::ELoadOp LoadOp { rhi::ELoadOp::Load };
	rhi::EStoreOp StoreOp { rhi::EStoreOp::Store };
	rhi::ClearColorValue ClearColor {};
	rhi::ClearDepthStencilValue ClearDepth {};
};

struct RDGAccess
{
	RDGResourceId Resource { InvalidRDGResourceId };
	rhi::EResourceState State { rhi::EResourceState::Undefined };
};

struct RDGPass
{
	std::string Name;
	ERDGPassType Type { ERDGPassType::Graphics };
	std::function<void(RDGPassBuilder&)> Setup;
	std::function<void(RDGExecuteContext&)> Execute;

	// Setup 收集
	std::vector<RDGAttachmentDecl> ColorAttachments;
	std::optional<RDGAttachmentDecl> DepthAttachment;
	std::vector<RDGAccess> Reads;
	std::vector<RDGAccess> Writes;

	// compile 生成
	std::vector<rhi::ImageBarrier> ImageBarriersBefore;
	std::vector<rhi::BufferBarrier> BufferBarriersBefore;
	std::vector<rhi::GlobalBarrier> GlobalBarriersBefore;
};

} // namespace detail

// ============================================================================
//  Pass 构建器: 在 Setup 回调内声明资源与访问
// ============================================================================

/**
 * @brief 单个 Pass 的声明期接口.
 *
 * 用户在 add*Pass 的 Setup 回调里通过它声明:
 *   - 本 Pass 需要创建哪些 transient 资源;
 *   - 本 Pass 读/写哪些资源以及它们的目标状态;
 *   - 本 Pass 的颜色/深度附件(对于 Graphics Pass).
 *
 * 声明结果会在 compile() 阶段被消费, 用于生成 barrier 与 RenderingInfo.
 */
class RDGPassBuilder
{
public:
	RDGPassBuilder(const RDGPassBuilder&) = delete;
	RDGPassBuilder& operator=(const RDGPassBuilder&) = delete;

	/**
	 * @brief 声明一个 transient 纹理. 同名资源跨帧复用同一物理纹理.
	 * @param Desc 图形语义描述; Usage 需覆盖本帧所有访问(如 Target|Sampled).
	 * @param Name 稳定名称, 用于跨帧缓存与调试.
	 */
	[[nodiscard]] RDGResourceId createTexture(RDGTextureDesc Desc, std::string_view Name);

	/** @brief 声明一个 transient 缓冲. 语义同 createTexture. */
	[[nodiscard]] RDGResourceId createBuffer(RDGBufferDesc Desc, std::string_view Name);

	/**
	 * @brief 声明一次只读访问.
	 * @param Resource 目标资源.
	 * @param State 访问时要求的状态(如 PixelShaderResource / NonPixelShaderResource).
	 */
	void read(RDGResourceId Resource, rhi::EResourceState State);

	/**
	 * @brief 声明一次写访问(非附件, 如 Storage 缓冲 / Storage 纹理).
	 * @param Resource 目标资源.
	 * @param State 写入状态(如 UnorderedAccess).
	 */
	void write(RDGResourceId Resource, rhi::EResourceState State);

	/**
	 * @brief 声明一个颜色附件(仅 Graphics Pass 有效).
	 * @param Resource 目标纹理(必须含 Target 用法).
	 * @param LoadOp 载入策略(Clear/Load/DontCare).
	 * @param StoreOp 保存策略(Store/DontCare).
	 * @param Clear 当 LoadOp 为 Clear 时的清除值.
	 */
	void renderTarget(RDGResourceId Resource, rhi::ELoadOp LoadOp, rhi::EStoreOp StoreOp,
		rhi::ClearColorValue Clear = {});

	/**
	 * @brief 声明深度/模板附件(仅 Graphics Pass 有效).
	 * @param Resource 目标深度纹理(必须为深度格式且含 DepthStencil 用法).
	 */
	void depthStencil(RDGResourceId Resource, rhi::ELoadOp LoadOp, rhi::EStoreOp StoreOp,
		rhi::ClearDepthStencilValue Clear = {});

private:
	friend class RDGBuilder;

	RDGPassBuilder(detail::RDGPass& Pass, RDGBuilder& Builder) noexcept
		: Pass(Pass), Builder(Builder)
	{
	}

	detail::RDGPass& Pass;
	RDGBuilder& Builder;
};

// ============================================================================
//  执行上下文: 在 Execute 回调内解析资源并录制
// ============================================================================

/**
 * @brief 单个 Pass 的执行期接口.
 *
 * 在 Execute 回调内通过它解析 RDGResourceId 为真实 RHI 对象, 并拿到命令列表录制命令.
 * Graphics Pass 的回调已经位于 RDG 自动创建的 beginRendering 作用域内.
 */
class RDGExecuteContext
{
public:
	RDGExecuteContext(const RDGExecuteContext&) = delete;
	RDGExecuteContext& operator=(const RDGExecuteContext&) = delete;

	/** @brief 用于录制命令的命令列表(已处于正确作用域). */
	[[nodiscard]] rhi::RCommandList& getCommandList() const noexcept;

	/** @brief 解析为 RImage; 资源不是纹理时返回空. */
	[[nodiscard]] const std::shared_ptr<rhi::RImage>& getImage(RDGResourceId Resource) const;

	/** @brief 解析为默认全范围 RImageView; 资源不是纹理时返回空. */
	[[nodiscard]] const std::shared_ptr<rhi::RImageView>& getView(RDGResourceId Resource) const;

	/** @brief 解析为 RBuffer; 资源不是缓冲时返回空. */
	[[nodiscard]] const std::shared_ptr<rhi::RBuffer>& getBuffer(RDGResourceId Resource) const;

private:
	friend class RDGBuilder;

	RDGExecuteContext(rhi::RCommandList& CommandList, const RDGBuilder& Builder) noexcept
		: CommandList(CommandList), Builder(Builder)
	{
	}

	rhi::RCommandList& CommandList;
	const RDGBuilder& Builder;
};

// ============================================================================
//  图构建器
// ============================================================================

/**
 * @brief RenderGraph 构建器: 声明 Pass → compile → execute 的三段式编排.
 *
 * 生命周期:
 *   1. import* / create*(在 Pass Setup 内) 声明资源;
 *   2. add*Pass 声明 Pass 及依赖;
 *   3. compile() 做状态追踪与 barrier 规划;
 *   4. execute(CommandList) 录制整帧命令;
 *   5. clear() 清空 Pass 与资源声明(但保留跨帧 transient 缓存), 可进入下一帧.
 */
class RDGBuilder
{
public:
	/** @brief 构造空图. @param Device 用于创建 transient 资源的设备, 必须比 builder 活得久. */
	explicit RDGBuilder(rhi::RDevice& Device) noexcept : Device(Device) {}

	RDGBuilder(const RDGBuilder&) = delete;
	RDGBuilder& operator=(const RDGBuilder&) = delete;

	/** @brief 析构时释放 transient 缓存. */
	~RDGBuilder();

	// -------------------------------------------------------------------------
	//  外部资源导入
	// -------------------------------------------------------------------------

	/**
	 * @brief 导入外部纹理(交换链图、历史缓冲、持久 RT 等).
	 * @param Image 底层图像; 不得为空.
	 * @param View 默认视图; 为空时 RDG 将创建覆盖全范围的视图.
	 * @param Name 调试名.
	 * @param InitialState 该资源进入本帧时的状态(如交换链图首次为 Undefined, 之后为 Present).
	 * @return 资源句柄.
	 */
	[[nodiscard]] RDGResourceId importImage(const std::shared_ptr<rhi::RImage>& Image,
		const std::shared_ptr<rhi::RImageView>& View, std::string_view Name,
		rhi::EResourceState InitialState = rhi::EResourceState::Undefined);

	/** @brief 导入外部缓冲(逐帧重用的上传/常量/Indirect 缓冲等). */
	[[nodiscard]] RDGResourceId importBuffer(const std::shared_ptr<rhi::RBuffer>& Buffer,
		std::string_view Name,
		rhi::EResourceState InitialState = rhi::EResourceState::Undefined);

	// -------------------------------------------------------------------------
	//  Pass 声明
	// -------------------------------------------------------------------------

	/** @brief 声明一个 Graphics Pass. */
	void addGraphicsPass(std::string_view Name,
		std::function<void(RDGPassBuilder&)> Setup,
		std::function<void(RDGExecuteContext&)> Execute);

	/** @brief 声明一个 Compute Pass. */
	void addComputePass(std::string_view Name,
		std::function<void(RDGPassBuilder&)> Setup,
		std::function<void(RDGExecuteContext&)> Execute);

	/** @brief 声明一个 Copy Pass. */
	void addCopyPass(std::string_view Name,
		std::function<void(RDGPassBuilder&)> Setup,
		std::function<void(RDGExecuteContext&)> Execute);

	// -------------------------------------------------------------------------
	//  编译与执行
	// -------------------------------------------------------------------------

	/**
	 * @brief 编译图: 收集访问 → 计算资源首末使用 → 生成逐 Pass 的 barrier 计划.
	 *
	 * 当前按声明顺序执行; 不做拓扑排序/裁剪/别名. 必须在 execute() 之前调用.
	 */
	void compile();

	/**
	 * @brief 执行图: 逐 Pass 录制命令到 CommandList.
	 * @param CommandList 已 begin() 的 Primary 命令列表, RDG 只在其上追加命令.
	 */
	void execute(rhi::RCommandList& CommandList);

	/** @brief 清空 Pass 与资源声明; 保留 transient 缓存以便下一帧复用. */
	void clear();

	// -------------------------------------------------------------------------
	//  查询
	// -------------------------------------------------------------------------

	[[nodiscard]] rhi::RDevice& getDevice() const noexcept { return Device; }
	[[nodiscard]] size_t getPassCount() const noexcept { return Passes.size(); }
	[[nodiscard]] size_t getResourceCount() const noexcept { return Resources.size(); }

private:
	friend class RDGPassBuilder;
	friend class RDGExecuteContext;

	// 内部实现
	[[nodiscard]] RDGResourceId declareTexture(RDGTextureDesc Desc, std::string_view Name);
	[[nodiscard]] RDGResourceId declareBuffer(RDGBufferDesc Desc, std::string_view Name);
	[[nodiscard]] detail::RDGResource& getResource(RDGResourceId Id);
	[[nodiscard]] const detail::RDGResource& getResource(RDGResourceId Id) const;
	void ensureAllocated(detail::RDGResource& Resource);
	void planAccess(detail::RDGPass& Pass, detail::RDGResource& Resource,
		rhi::EResourceState State, bool IsWrite);

	rhi::RDevice& Device;

	std::vector<detail::RDGResource> Resources;
	std::vector<detail::RDGPass> Passes;

	// 跨帧 transient 缓存: 名称 -> 上次分配的物理资源(含 RHI 句柄), 供下一帧同名资源复用.
	std::unordered_map<std::string, detail::RDGResource> TransientCache;
};

} // namespace runtime::renderer
