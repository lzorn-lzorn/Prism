#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>
#include <core/wrappers/Flag.hpp>
#include <core/math/MathCommon.hpp>
#include <generic_application/window/GenericWindow.hpp>

namespace rhi
{

/** @brief Device-visible memory size and byte-offset type. */
using DeviceSizeType = uint64_t;
/** @brief Opaque GPU virtual address; zero is always invalid. */
using DeviceAddress = uint64_t;
// Vk: typedef uint64_t VkDeviceSize;

/** @brief Selects a concrete graphics backend supported by this RHI build. */
enum class ESupportedBackendAPI
{
	Vulkan
};
/** @brief Identifies the lifetime-tracked category of an RHI object. */
enum class EResourceType
{
	Buffer,
	Image,
	Texture,
	Sampler,
	Shader,
	Pipeline,
	DescriptorSet,
	DescriptorSetLayout,
	Fence,
	Swapchain
};

/** @brief Portable resource access/layout state used by explicit barriers. */
enum class EResourceState {
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
	NonPixelShaderResource
};

/** @brief Reports whether a logical device remains safe to use. */
enum class EDeviceStatus : uint8_t
{
	Ready,
	Lost,
	Removed,
	Reset,
	UnknownFailure
};

/**
 * @brief Portable texel, render-target, and depth/stencil formats.
 * @note
 * > _UNorm: 表示线性颜色空间, 纹理采样时不会自动进行 gamma 校正, 适合后处理或需要线性混合的场景
 * > _sRGB: 表示 sRGB 非线性空间, GPU 在写入时自动将线性值转换为 sRGB 编码, 在读取时反向转换, 
 * >		符合标准显示器的 gamma 特性, 能避免颜色过暗或过亮
 */
enum class EFormat : uint32_t
{
	Undefined,
	// 每通道 8 位无符号归一化整数, 取值范围 [0,1], 存储时直接映射, 适用于普通颜色纹理(如漫反射贴图),无 HDR 要求的渲染目标
	RGBA8_UNorm,
	// 与 RGBA8_UNorm 相同位宽, 但纹理采样时 GPU 会自动将数据从 sRGB 色彩空间转换到线性空间, 写入时自动反向转换. 适合用于最终输出到显示器的交换链, 以及存储人眼感知颜色的纹理(如照片、UI)
	RGBA8_sRGB,
	// 与 RGBA8_UNorm 类似, 但通道顺序为 B,G,R,A. 这是许多平台(尤其是 Windows)交换链的默认格式, 因为桌面合成器使用这种通道顺序
	BGRA8_UNorm,
	BGRA8_sRGB,
	// 每通道 16 位浮点数 RGBA, 用于 HDR 中间渲染目标, 支持高动态范围
	RGBA16_Float, 
	// 每通道 32 位浮点数 RGBA, 用于高精度 HDR 渲染或科学计算
	RGBA32_Float, 
	// 10-bit RGB 加 2-bit alpha 的归一化 HDR 交换链格式，常与 HDR10_ST2084 配对
	RGB10A2_UNorm,
	// 16 位深度格式(无符号归一化), 通常用于深度缓冲, 精度较低
	D16_UNorm,
	// 24 位深度(无符号归一化)+ 8 位模板(无符号整数), 经典深度模板格式, 兼容性好
	D24_UNorm_S8_UInt, 
	// 32 位浮点深度格式, 提供更高精度, 常用于现代渲染管线
	D32_Float,    
	// ...
};

/** @brief Tests whether a format contains a depth aspect. @param Format Format to inspect. @return True for depth-capable formats. */
[[nodiscard]] inline bool isDepthFormat(EFormat Format)
{
	return Format == EFormat::D16_UNorm ||
		Format == EFormat::D24_UNorm_S8_UInt ||
		Format == EFormat::D32_Float;
}

/** @brief Tests whether a format contains depth and no stencil. @param Format Format to inspect. @return True for depth-only formats. */
[[nodiscard]] inline bool isDepthOnlyFormat(EFormat Format)
{
	switch (Format) {
        case EFormat::D16_UNorm:
        case EFormat::D32_Float:
            return true;
        default:
            return false;
    }
}

/** @brief Tests whether a format contains stencil and no depth. @param Format Format to inspect. @return True for stencil-only formats. */
[[nodiscard]] inline bool isStencilOnlyFormat(EFormat Format)
{
	return false;
}

/** @brief Tests whether a format contains both depth and stencil. @param Format Format to inspect. @return True for combined formats. */
[[nodiscard]] inline bool isDepthStencilFormat(EFormat Format)
{
	return Format == EFormat::D24_UNorm_S8_UInt;
}

/** @brief Tests for a usable depth aspect. @param Format Format to inspect. @return True when depth can be addressed. */
[[nodiscard]] inline bool hasDepthAspect(EFormat Format)
{
	return isDepthFormat(Format);
}

/** @brief Tests for a usable stencil aspect. @param Format Format to inspect. @return True when stencil can be addressed. */
[[nodiscard]] inline bool hasStencilAspect(EFormat Format)
{
	return Format == EFormat::D24_UNorm_S8_UInt;
}

/** @brief Returns the packed texel size for a format. @param Format Format to inspect. @return Bytes per texel, or zero when undefined/unknown. */
[[nodiscard]] inline uint32_t calPixelSizeFormEFormat(EFormat Format)
{
	switch (Format)
	{
	case EFormat::RGBA8_UNorm:
	case EFormat::RGBA8_sRGB:
	case EFormat::BGRA8_UNorm:
	case EFormat::BGRA8_sRGB:
	case EFormat::RGB10A2_UNorm:
		return 4;
	case EFormat::RGBA16_Float:
		return 8;
	case EFormat::RGBA32_Float:
		return 16;
	case EFormat::D16_UNorm:
		return 2;
	case EFormat::D24_UNorm_S8_UInt:
		return 4;
	case EFormat::D32_Float:
		return 4;
	default:
		return 0;
	}
}


/** @brief Bit values describing required or preferred physical memory properties. */
enum class EMemoryProperty_t : uint8_t
{
    None           = 0,
    DeviceLocal    = 1 << 0,  // 位于 GPU 显存, 访问最快
    HostVisible    = 1 << 1,  // CPU 可映射访问(必须配合 HostVisible 才能用 vkMapMemory)
    HostCoherent   = 1 << 2,  // 自动同步 CPU/GPU 缓存(免去手动 Flush/Invalidate)
    HostCached     = 1 << 3,  // CPU 缓存中保留副本(适合频繁读回的场景)
    LazilyAllocated = 1 << 4, // 惰性分配(用于深度/模板缓冲, 节省显存)
};

/** @brief Composable set of physical memory properties. */
using EMemoryProperty = core::Flags<EMemoryProperty_t>;
DEFINE_ENUM_OPERATOR(EMemoryProperty_t);

/** @brief High-level allocation intent used to rank otherwise valid memory types. */
enum class EMemoryUsage : uint8_t
{
	Auto,
	GPUOnly,
	CPUToGPU,
	GPUToCPU,
	CPUOnly
};
/** @brief Bit values describing all legal uses declared when a buffer is created. */
enum class EBufferUsage_t : uint32_t
{
	None        = 0,
	Vertex      = 1 << 0,
	Index       = 1 << 1,
	Uniform     = 1 << 2,
	Storage     = 1 << 3,
	Indirect    = 1 << 4,
	TransferSrc = 1 << 5,
	TransferDst = 1 << 6,
	UniformTexel = 1 << 7,
	StorageTexel = 1 << 8,
	DeviceAddress = 1 << 9,
	AccelerationStructureBuildInput = 1 << 10,
	AccelerationStructureStorage = 1 << 11,
	ShaderBindingTable = 1 << 12,
};

/** @brief Composable set of buffer usage capabilities. */
using EBufferUsage = core::Flags<EBufferUsage_t>;
DEFINE_ENUM_OPERATOR(EBufferUsage_t);
/** @brief Selects the intended CPU access direction for a mapped buffer. */
enum class EBufferMapMode : uint8_t
{
	Read,
	Write,
	ReadWrite,
	WriteDiscard
};

/** @brief Bit values describing all legal uses declared when an image is created. */
enum class EImageUsage_t : uint32_t
{
	None         = 0,
	Sampled      = 1 << 0,
	Storage      = 1 << 1,
	Target       = 1 << 2,
	DepthStencil = 1 << 3,
	TransferSrc  = 1 << 4,
	TransferDst  = 1 << 5,
	Present      = 1 << 6,
};

/** @brief Composable set of image usage capabilities. */
using EImageUsage = core::Flags<EImageUsage_t>;
DEFINE_ENUM_OPERATOR(EImageUsage_t);

/** @brief Defines image dimensionality and array/cube interpretation. */
enum class EImageDimension : uint8_t
{
    Texture1D,          // 1D 纹理
    Texture1DArray,     // 1D 纹理数组
    Texture2D,          // 2D 纹理
    Texture2DArray,     // 2D 纹理数组
    Texture3D,          // 3D 纹理(整个 Depth 当作第三维, 不能单独切片)
    Cube,               // 立方体贴图(6 个面, 不可独立扩展)
    CubeArray           // 立方体贴图数组(6 的整数倍面)
};

/** @brief Defines how an image subresource is interpreted through a view. */
enum class EImageViewDimension : uint8_t
{
	Auto,
	Texture1D,
    Texture1DArray,
    Texture2D,
    Texture2DArray,
    Texture3D,
    Cube,
    CubeArray
};

/** @brief Selects exclusive or concurrent queue-family resource ownership. */
enum class ESharingMode
{
	Auto,        
	Exclusive,   // GPU 独占模式, 性能更高
	Concurrent   // GPU 并发模式, 允许多个队列同时访问资源, 但性能较低
};

/** @brief Bit values identifying programmable shader stages. */
enum class EShaderStage_t : uint32_t
{
	Vertex        = 1 << 0,
	Pixel         = 1 << 1,
	Compute       = 1 << 2,
	Geometry      = 1 << 3,
	Hull		  = 1 << 4,
	Domain		  = 1 << 5,
	Mesh	      = 1 << 6,
	Amplification = 1 << 7,
	Task		  = 1 << 8,
	RayGeneration = 1 << 9,
	AnyHit        = 1 << 10,
	ClosestHit    = 1 << 11,
	Miss          = 1 << 12,
	Intersection  = 1 << 13,
	Callable      = 1 << 14,
};

/** @brief Composable visibility mask of shader stages. */
using EShaderStage = core::Flags<EShaderStage_t>;
DEFINE_ENUM_OPERATOR(EShaderStage_t);

/** @brief Describes the presentation surface transform applied by the compositor. */
enum class ESurfaceTransform
{
	Identity,
	Rotate90,
	Rotate180,
	Rotate270,
	HorizontalMirror,
	HorizontalMirrorRotate90,
	HorizontalMirrorRotate180,
	HorizontalMirrorRotate270
};


/** @brief Selects the execution capability required from a command queue. */
enum class ECommandQueueType
{
	Graphics,
	Compute,
	Copy
};

/** @brief Reports image acquisition outcome without requiring exceptions. */
enum class EAcquireStatus : uint8_t
{
	Success,
	Suboptimal,
	OutOfDate,
	SurfaceLost,
	DeviceLost,
	NotReady
};

/** @brief Reports presentation outcome and recoverable swapchain/surface failures. */
enum class EPresentStatus : uint8_t
{
	Success,
	Suboptimal,
	OutOfDate,
	SurfaceLost,
	DeviceLost
};

/** @brief Persistent health of swapchain presentation resources between acquire/present calls. */
enum class ESwapchainStatus : uint8_t
{
	Ready,
	Suboptimal,
	OutOfDate,
	SurfaceLost,
	DeviceLost
};

/** @brief Selects the measurement represented by each query slot. */
enum class EQueryType : uint8_t
{
	Timestamp,
	Occlusion,
	PipelineStatistics
};

/** @brief Bit values selecting counters captured by a pipeline-statistics query. */
enum class EPipelineStatistic_t : uint32_t
{
	None = 0,
	InputAssemblyVertices = 1 << 0,
	InputAssemblyPrimitives = 1 << 1,
	VertexShaderInvocations = 1 << 2,
	GeometryShaderInvocations = 1 << 3,
	GeometryShaderPrimitives = 1 << 4,
	ClippingInvocations = 1 << 5,
	ClippingPrimitives = 1 << 6,
	PixelShaderInvocations = 1 << 7,
	HullShaderPatches = 1 << 8,
	DomainShaderInvocations = 1 << 9,
	ComputeShaderInvocations = 1 << 10
};

/** @brief Composable pipeline counter mask; result words follow ascending bit order. */
using EPipelineStatistics = core::Flags<EPipelineStatistic_t>;
DEFINE_ENUM_OPERATOR(EPipelineStatistic_t);

/** @brief Bit values controlling query result width, waiting, availability, and partial results. */
enum class EQueryResultFlag_t : uint8_t
{
	None = 0,
	Wait = 1 << 0,
	WithAvailability = 1 << 1,
	Partial = 1 << 2,
	Result64 = 1 << 3
};

/** @brief Composable query readback/copy policy flags. */
using EQueryResultFlags = core::Flags<EQueryResultFlag_t>;
DEFINE_ENUM_OPERATOR(EQueryResultFlag_t);

/** @brief Converts timestamp ticks to nanoseconds. @param TickDelta Raw tick delta masked to the device's valid timestamp bits. @param TimestampPeriodNanoseconds Nanoseconds represented by one tick from DeviceLimits. @return Converted duration, or zero when the period is non-positive. */
[[nodiscard]] constexpr double timestampTicksToNanoseconds(
	uint64_t TickDelta,
	double TimestampPeriodNanoseconds) noexcept
{
	return TimestampPeriodNanoseconds > 0.0
		? static_cast<double>(TickDelta) * TimestampPeriodNanoseconds
		: 0.0;
}

/** @brief Identifies the programmable pipeline family bound for execution. */
enum class EPipelineType
{
	None,
	Graphics,
	Compute,
	RayTracing
};

/**
 * @brief Pipeline 创建策略. 
 *
 * 这些标志只表达跨后端的编译意图. Vulkan Graphics Pipeline Library、
 * D3D12 Pipeline Library 和 Metal Binary Archive 等后端优化不会直接暴露到 RHI. 
 */
enum class EPipelineCompileFlag_t : uint32_t
{
	None = 0,
	AllowAsync = 1 << 0,
	FailIfCompileNeeded = 1 << 1,
	Optimize = 1 << 2,
	CaptureStatistics = 1 << 3
};

/** @brief Composable pipeline compilation policy flags. */
using EPipelineCompileFlags = core::Flags<EPipelineCompileFlag_t>;
DEFINE_ENUM_OPERATOR(EPipelineCompileFlag_t);

/** @brief Defines how an attachment obtains its value at rendering start. */
enum class ELoadOp
{
	Load,
	Clear,
	DontCare
};

/** @brief Defines whether an attachment value is preserved after rendering. */
enum class EStoreOp
{
	Store,
	DontCare
};

/** @brief Selects multisample resolve behavior for an attachment aspect. */
enum class EResolveMode : uint8_t
{
	None,
	SampleZero,
	Average,
	Min,
	Max
};

/** @brief Selects the scalar interpretation of a clear-color payload. */
enum class EClearColorType : uint8_t
{
	Float,
	UInt,
	SInt
};

/** @brief Defines the packed scalar/vector format of a vertex attribute. */
enum class EVertexFormat {
    Float1,
    Float2,
    Float3,
    Float4,
    UInt1,
    UInt2,
    UInt3,
    UInt4,
    Short2,
    Short4,
    // 更多...
};

/** @brief Selects index element width or disables indexed input. */
enum class EIndexFormat
{
	None,
	UInt16,
	UInt32
};

/** @brief Defines primitive assembly topology. */
enum class EPrimitiveTopology
{
	PointList,
	LineList,
	LineStrip,
	TriangleList,
	TriangleStrip,
	PatchList
};

/** @brief Identifies the resource interpretation of a bind-group entry. */
enum class EDescriptorType
{
	UniformBuffer,
	ReadOnlyStorageBuffer,
	ReadWriteStorageBuffer,
	Sampler,
	ComparisonSampler,
	SampledTexture,
	StorageTexture,
	UniformTexelBuffer,
	StorageTexelBuffer,
	CombinedImageSampler,
	InputAttachment,
	AccelerationStructure
};

/** @brief Bit values enabling dynamic or descriptor-indexing binding behavior. */
enum class EDescriptorBindingFlag_t : uint8_t
{
	None = 0,
	DynamicOffset = 1 << 0,
	PartiallyBound = 1 << 1,
	UpdateAfterBind = 1 << 2,
	VariableArrayCount = 1 << 3
};

/** @brief Composable descriptor binding behavior flags. */
using EDescriptorBindingFlags = core::Flags<EDescriptorBindingFlag_t>;
DEFINE_ENUM_OPERATOR(EDescriptorBindingFlag_t);

/** @brief Declares the image layout/usage expected by a descriptor. */
enum class EDescriptorImageLayout : uint8_t
{
	ShaderReadOnly,
	General,
	DepthStencilReadOnly
};

/** @brief Selects nearest or linear filtering. */
enum class EFilterMode : uint8_t
{
	Nearest,
	Linear
};

/** @brief Defines sampler addressing outside normalized texture coordinates. */
enum class ESamplerAddressMode : uint8_t
{
	Repeat,
	MirroredRepeat,
	ClampToEdge,
	ClampToBorder
};

/** @brief Selects a source or destination blend multiplier. */
enum class EBlendFactor
{
	Zero,
	One,
	SrcColor,
	SrcAlpha,
	OneMinusSrcAlpha,
	DstAlpha,
    OneMinusDstAlpha,
    DstColor,
    OneMinusDstColor
};

/** @brief Selects the arithmetic operation combining blend terms. */
enum class EBlendOp
{
	Add,
	Subtract,
	ReverseSubtract,
	Min,
	Max
};

/** @brief Selects depth, stencil, or sampler comparison behavior. */
enum class ECompareOp
{
	Less,
	LessEqual,
	Equal,
	NotEqual,
	GreaterEqual,
	Greater,
	Always,
	Never
};

/** @brief Selects the operation applied to a stencil value. */
enum class EStencilOp
{
	Keep,
	Zero,
	Replace,
	IncrementAndClamp,
	DecrementAndClamp,
	Invert,
	IncrementAndWrap,
	DecrementAndWrap
};

/** @brief Selects which triangle faces are discarded before rasterization. */
enum class ECullMode
{
	None,
	Front,
	Back
};

/** @brief Selects solid or wireframe polygon rasterization. */
enum class EFillMode
{
	Solid,
	Wireframe
};

/** @brief Defines the winding order interpreted as front-facing. */
enum class EFrontFace : uint32_t
{
	CounterClockwise,
	Clockwise
};

/** @brief Selects per-vertex or per-instance binding advancement. */
enum class EVertexInputRate : uint32_t
{
	PerVertex,
	PerInstance
};

/**
 * @brief 可由命令列表在 Pipeline 创建后覆盖的状态. 
 *
 * 动态状态不会参与 Pipeline 语义缓存键. 请求后端不支持的动态状态时，
 * Pipeline 创建必须失败，而不能静默退化为静态状态. 
 */
enum class EDynamicState_t : uint64_t
{
	None = 0,
	Viewport = 1ull << 0,
	Scissor = 1ull << 1,
	BlendConstants = 1ull << 2,
	StencilReference = 1ull << 3,
	DepthBias = 1ull << 4,
	LineWidth = 1ull << 5,
	CullMode = 1ull << 6,
	FrontFace = 1ull << 7,
	PrimitiveTopology = 1ull << 8,
	DepthTestEnable = 1ull << 9,
	DepthWriteEnable = 1ull << 10,
	DepthCompareOp = 1ull << 11,
	StencilTestEnable = 1ull << 12,
	StencilOperations = 1ull << 13,
	StencilCompareMask = 1ull << 14,
	StencilWriteMask = 1ull << 15,
	VertexInput = 1ull << 16
};

/** @brief Composable set of pipeline states supplied during command recording. */
using EDynamicStates = core::Flags<EDynamicState_t>;


/** @brief Bit values selecting writable render-target color channels. */
enum class EColorWriteMask_t : uint8_t
{
	None = 0,
	R = 1 << 0,
	G = 1 << 1,
	B = 1 << 2,
	A = 1 << 3,
	All = 0x0F
};

/** @brief Composable render-target channel write mask. */
using EColorWriteMask = core::Flags<EColorWriteMask_t>;
DEFINE_ENUM_OPERATOR(EColorWriteMask_t);

/** @brief Enumerates supported rasterization sample counts. */
enum class ESampleCount
{
	Count1 = 1,
	Count2 = 2,
	Count4 = 4,
	Count8 = 8,
	Count16 = 16,
	Count32 = 32,
	Count64 = 64
};

/** @brief Selects image queueing and vertical synchronization behavior. */
enum class EPresentMode {
    Immediate,
    Fifo,
    FifoRelaxed,
    Mailbox
};

// note: 交换链图像通常带有 alpha 通道(例如 VK_FORMAT_B8G8R8A8_SRGB), 
// note: 但最终显示到屏幕时, 窗口系统需要将这些图像与桌面或其他窗口进行合成
/** @brief Defines how swapchain alpha participates in desktop composition. */
enum class ECompositeAlpha
{
	Opaque,         // 图像视为不透明, 忽略 alpha 通道
	PreMultiplied,	// alpha 通道按预乘方式参与合成, 即 RGB 分量已经乘上了 alpha
	PostMultiplied, // alpha 通道按非预乘方式参与合成, RGB 与 alpha 独立
	Inherit         // 表示由窗口系统继承之前的 alpha 设置
};

/** @brief Identifies the transfer function and gamut used for presentation. */
enum class EColorSpace
{
	SRGB_Nonlinear, // sRGB 非线性空间, 适合最终输出到显示器的图像
	AdobeRGB,       // Adobe RGB 色彩空间, 提供更广的色域
	DCIP3,          // DCI-P3 色彩空间, 常用于数字电影投影
	Rec2020,        // Rec. 2020 色彩空间, 用于超高清电视和 HDR 内容
	HDR10_ST2084,
	ExtendedSRGBLinear
};

/** @brief Selects the image aspect addressed by a view, barrier, or copy. */
enum class EImageAspect : uint8_t
{
	Auto,
	Color,
	Depth,
	Stencil,
	DepthStencil
};

/** @brief Bit values describing operations supported by a format. */
enum class EFormatFeature_t : uint32_t
{
	None = 0,
	Sampled = 1 << 0,
	Storage = 1 << 1,
	ColorAttachment = 1 << 2,
	DepthStencilAttachment = 1 << 3,
	LinearFiltering = 1 << 4,
	BlitSource = 1 << 5,
	BlitDestination = 1 << 6,
	VertexBuffer = 1 << 7
};

/** @brief Composable format operation capability mask. */
using EFormatFeatures = core::Flags<EFormatFeature_t>;
DEFINE_ENUM_OPERATOR(EFormatFeature_t);

/** @brief Forward declaration of the buffer resource wrapper. */ class RBuffer;
/** @brief Forward declaration of the image resource wrapper. */ class RImage;
/** @brief Forward declaration of the sampler resource wrapper. */ class RSampler;
/** @brief Forward declaration of the shader module wrapper. */ class RShader;
/** @brief Forward declaration of the pipeline wrapper. */ class RPipeline;
/** @brief Forward declaration of the pipeline cache wrapper. */ class RPipelineCache;
/** @brief Forward declaration of the pipeline layout wrapper. */ class RPipelineLayout;
/** @brief Forward declaration of the bind-group layout wrapper. */ class RBindGroupLayout;
/** @brief Forward declaration of the immutable bind-group wrapper. */ class RBindGroup;
/** @brief Forward declaration of the logical device wrapper. */ class RDevice;
/** @brief Forward declaration of the command-list wrapper. */ class RCommandList;
/** @brief Forward declaration of the execution queue wrapper. */ class RQueue;
/** @brief Forward declaration of the CPU-waitable fence wrapper. */ class RFence;
/** @brief Forward declaration of the GPU semaphore wrapper. */ class RSemaphore;
/** @brief Forward declaration of the query-pool wrapper. */ class RQueryPool;
/** @brief Forward declaration of the presentation swapchain wrapper. */ class RSwapchain;
/** @brief Forward declaration of the device-memory allocator interface. */ class DeviceMemoryAllocator;
/** @brief Forward declaration of the sampled-texture wrapper. */ class RTexture;
/** @brief Forward declaration of the image-view wrapper. */ class RImageView;
/** @brief Forward declaration of the device-memory allocation wrapper. */ class DeviceMemory;
/** @brief Forward declaration of the ray-tracing acceleration-structure wrapper. */ class RAccelerationStructure;
/** @brief Forward declaration of device memory requirements. */ struct MemoryRequirements;

/** @brief Maximum portable number of simultaneous color attachments. */
inline constexpr uint32_t MaxColorAttachments = 8;

/** @brief Type-safe clear payload for a color attachment. */
struct ClearColorValue
{
	EClearColorType Type { EClearColorType::Float };
	std::array<float, 4> Float32 { 0.0f, 0.0f, 0.0f, 0.0f };
	std::array<uint32_t, 4> UInt32 {};
	std::array<int32_t, 4> SInt32 {};
};

/** @brief Clear payload for depth and stencil attachment aspects. */
struct ClearDepthStencilValue
{
	float Depth { 1.0f };
	uint32_t Stencil { 0 };
};

/** @brief Integer framebuffer rectangle used for rendering and scissors. */
struct RenderArea
{
	int32_t X { 0 };
	int32_t Y { 0 };
	uint32_t Width { 0 };
	uint32_t Height { 0 };
};

/** @brief Floating-point viewport transform and normalized depth range. */
struct Viewport
{
	float X { 0.0f };
	float Y { 0.0f };
	float Width { 0.0f };
	float Height { 0.0f };
	float MinDepth { 0.0f };
	float MaxDepth { 1.0f };
};

/** @brief Tracks the legal recording/submission lifecycle of a command list. */
enum class ECommandListState : uint8_t
{
	Initial,
	Recording,
	Executable,
	Pending,
	Completed,
	Invalid
};

/** @brief Selects directly submit-able primary or inherited secondary recording. */
enum class ECommandListLevel : uint8_t
{
	Primary,
	Secondary
};

/** @brief Dynamic-rendering state inherited by a secondary command list. */
struct SecondaryCommandListInheritance
{
	std::array<EFormat, MaxColorAttachments> ColorFormats {};
	uint32_t ColorAttachmentCount { 0 };
	EFormat DepthFormat { EFormat::Undefined };
	EFormat StencilFormat { EFormat::Undefined };
	ESampleCount SampleCount { ESampleCount::Count1 };
	uint32_t ViewMask { 0 };
};

/** @brief Command-list creation policy including optional secondary rendering inheritance. */
struct CommandListDescriptor
{
	ECommandQueueType QueueType { ECommandQueueType::Graphics };
	ECommandListLevel Level { ECommandListLevel::Primary };
	bool OneTimeSubmit { true };
	std::optional<SecondaryCommandListInheritance> RenderingInheritance;
};

/** @brief Numeric hardware limits guaranteed by the selected logical device. */
struct DeviceLimits
{
	uint32_t MaxColorAttachments { 1 };
	uint32_t MaxViewports { 1 };
	uint32_t MaxFramebufferWidth { 1 };
	uint32_t MaxFramebufferHeight { 1 };
	uint32_t MaxVertexInputBindings { 1 };
	uint32_t MaxVertexInputAttributes { 1 };
	uint32_t MaxPushConstantSize { 0 };
	uint32_t MaxBoundBindGroups { 0 };
	uint64_t MinUniformBufferOffsetAlignment { 1 };
	uint64_t MinStorageBufferOffsetAlignment { 1 };
	uint64_t MinTexelBufferOffsetAlignment { 1 };
	uint64_t MinAccelerationStructureScratchOffsetAlignment { 1 };
	uint64_t ShaderBindingTableAlignment { 1 };
	uint64_t ShaderGroupHandleAlignment { 1 };
	uint32_t ShaderGroupHandleSize { 0 };
	uint64_t MaxShaderGroupStride { 0 };
	uint32_t MaxRayRecursionDepth { 0 };
	uint64_t MaxRayDispatchInvocationCount { 0 };
	uint32_t TimestampValidBits { 0 };
	double TimestampPeriodNanoseconds { 0.0 };
};

/** @brief Optional capabilities; callers must test a feature before using its associated API. */
struct DeviceFeatures
{
	bool DynamicRendering { false };
	bool Synchronization2 { false };
	bool TimelineSemaphore { false };
	bool Multiview { false };
	bool SeparateDepthStencilLayouts { false };
	bool GeometryShader { false };
	bool TessellationShader { false };
	bool FillModeNonSolid { false };
	bool WideLines { false };
	bool DepthClamp { false };
	bool DepthBounds { false };
	bool SampleRateShading { false };
	bool SamplerAnisotropy { false };
	bool AlphaToOne { false };
	bool IndependentBlend { false };
	bool ExtendedDynamicState { false };
	bool ExtendedDynamicState2 { false };
	bool ExtendedDynamicState3 { false };
	bool DynamicVertexInput { false };
	bool DescriptorIndexing { false };
	bool RuntimeDescriptorArray { false };
	bool PartiallyBoundDescriptors { false };
	bool VariableDescriptorCount { false };
	bool UpdateAfterBind { false };
	bool MeshShader { false };
	bool TaskShader { false };
	bool RayTracingPipeline { false };
	bool PipelineExecutableProperties { false };
	bool TimestampQueries { false };
	bool OcclusionQueries { false };
	bool PipelineStatisticsQueries { false };
	bool QueryResultCopy { false };
	bool BufferDeviceAddress { false };
	bool AccelerationStructure { false };
	bool RayQuery { false };
	bool DebugLabels { false };
	bool SecondaryCommandLists { false };
	bool DeviceLossReporting { false };
	bool DeferredRelease { false };
	bool SwapchainStatus { false };
	bool HDRSwapchain { false };
	bool HDRMetadata { false };
};

/** @brief Size, alignment, memory-type, and dedicated-allocation constraints for a resource. */
struct MemoryRequirements {
    uint64_t Size;           // 内存大小
    uint64_t Alignment;      // 对齐要求
	uint32_t MemoryTypeBits; 
	bool PrefersDedicatedAllocation { false };
	bool RequiresDedicatedAllocation { false };
};

/** @brief Metadata controlling modern explicit device-memory allocation. */
struct MemoryAllocationDescriptor
{
	MemoryRequirements Requirements {};
	EMemoryUsage Usage { EMemoryUsage::Auto };
	EMemoryProperty RequiredProperties {};
	EMemoryProperty PreferredProperties {};
	float Priority { 0.5f };
	bool PersistentlyMapped { false };
	std::string DebugName;
};

/** @brief Observable properties of a completed device-memory allocation. */
struct MemoryAllocationInfo
{
	DeviceSizeType Size { 0 };
	DeviceSizeType Offset { 0 };
	uint32_t MemoryTypeIndex { 0 };
	EMemoryProperty Properties {};
	bool Dedicated { false };
	bool PersistentlyMapped { false };
};

/** @brief Owns or references a backend device-memory allocation and its host mapping. */
class DeviceMemory
{
public:
	/** @brief Describes ownership and permitted access to the wrapped allocation. */
	enum class EState {
		None,
		InValid, // 持有内存已经释放
		ReadOnly, // 未持有所有权, 仅读取
		Writeable,// 未持有所有权, 可读写
		OwnsMemory // 持有内存所有权, 可读写
	};

	/** @brief Constructs an empty backend allocation wrapper. */
	DeviceMemory() = default;
	/** @brief Releases wrapper ownership; concrete implementations must not outlive their device. */
	virtual ~DeviceMemory() = default;
	DeviceMemory(const DeviceMemory&) = delete;
	DeviceMemory& operator=(const DeviceMemory&) = delete;
	DeviceMemory(DeviceMemory&&) = default;
	DeviceMemory& operator=(DeviceMemory&&) = default;

	/** 
     * @brief 映射可见内存区间 
     * @param Offset 分配内字节偏移
     * @param Size 字节数；0 表示剩余区间.  
     * @return 映射地址 
     */
	virtual void* map(DeviceSizeType Offset = 0, DeviceSizeType Size = 0) = 0;
	
    /** 
     * @brief 结束调用方对当前映射的访问
     */
	virtual void unmap() = 0;

	// note: 当内存被映射到主机地址空间(通过 map)后, CPU 和 GPU 对同一块内存的访问并不是自动同步的, 
    // note: 尤其是对于非主机一致性(non-coherent)内存
	/** 
     * @brief Makes CPU writes visible to device accesses for a non-coherent mapped range.
     * @param Offset Allocation-relative byte offset.
     * @param Size Byte count; zero means the remaining allocation.
     */
	virtual void flush(DeviceSizeType Offset, DeviceSizeType Size) = 0;
	
    /** 
     * @brief Makes device writes visible to CPU reads for a non-coherent mapped range.
     * @param Offset Allocation-relative byte offset.
     * @param Size Byte count; zero means the remaining allocation.
     */
	virtual void invalidate(DeviceSizeType Offset, DeviceSizeType Size) = 0;
	
    /** 
     * @brief 释放或归还底层分配; 调用方必须先证明 GPU 不再使用它 
     */
	virtual void release() = 0;

	/** 
     * @brief 返回创建该分配所满足的要求
     * @return 大小、对齐和内存类型约束 
     */
	virtual MemoryRequirements getMemoryRequirements() const = 0;

	/** 
     * @brief 返回实际内存属性.  
     * @return 可见性、一致性及局部性标志.  
     */
	virtual EMemoryProperty getMemoryProperty() const = 0;
	
    /** 
     * @brief Returns modern allocation metadata when exposed by the backend. 
     * @return Metadata, or std::nullopt for legacy allocations. 
     */
	[[nodiscard]] virtual std::optional<MemoryAllocationInfo> getAllocationInfo() const { return std::nullopt; }

	/** 
     * @brief 查询包装器的所有权状态.  
     * @return 当前状态.  
     */
	EState getState() const { return OwnershipState; }

protected:
	EState OwnershipState { EState::None };
};

/** @brief Strategy interface for allocating and releasing device memory. */
class DeviceMemoryAllocator
{
public:
	/** 
     * @brief Destroys an allocation strategy after all allocations returned by it are released. 
     */
	virtual ~DeviceMemoryAllocator() = default;

	/** 
     * @brief 按资源要求和必需属性分配设备内存
     * @param Requirements 大小、对齐和类型位 
     * @param Property 必需属性 
     * @return 分配对象, 失败时为空或抛出异常.  
     */
	virtual std::shared_ptr<DeviceMemory> allocateMemory(MemoryRequirements Requirements, EMemoryProperty Property) = 0;

	/** 
     * @brief 归还不再被 GPU 使用的分配 
     * @param Memory 待释放对象；空值为无操作 
     */
	virtual void freeMemory(std::shared_ptr<DeviceMemory> Memory) = 0;
};

/**
 * @brief 跨后端 Buffer 资源. 
 *
 * map()/unmap() 只允许用于 HostVisible 内存；频繁更新路径可保持持久映射，
 * 后续上传系统也可以在该接口之上实现 staging/ring allocator. 
 */
class RBuffer
{
public:
    struct Descriptor_t
    {
        DeviceSizeType Size { 0 };
        EBufferUsage Usage {};
        EMemoryUsage MemoryUsage { EMemoryUsage::Auto };
        EMemoryProperty MemoryProperty { EMemoryProperty_t::DeviceLocal };
        EMemoryProperty PreferredMemoryProperty {};
        float MemoryPriority { 0.5f };
        bool DedicatedAllocation { false };
        bool PersistentlyMapped { false };
        std::string DebugName;
    };

public:
	virtual ~RBuffer() = default;
	RBuffer(const RBuffer&) = delete;
	RBuffer& operator=(const RBuffer&) = delete;

	/** 
     * @brief 返回创建此 Buffer 的设备 
     * @return 所属设备 
     */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	
    /** 
     * @brief 返回不可变创建描述
     * @return Buffer 描述 
     */
	[[nodiscard]] virtual const Descriptor_t& getDescriptor() const noexcept = 0;
	
    /** 
     * @brief 查询底层对象是否可用 
     * @return 可用于命令记录时为 true 
     */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;

	/** 
     * @brief 返回非 owning 后端句柄 
     * @return 不可用时为 nullptr
     */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
	
    /** 
     * @brief Returns the stable GPU virtual address of this buffer when supported and requested
     * @return Nonzero address, or zero when unsupported, invalid, or missing DeviceAddress usage 
     */
	[[nodiscard]] virtual DeviceAddress getDeviceAddress() const noexcept { return 0; }
	
    /** 
     * @brief 映射 HostVisible 区间 
     * @param Offset Buffer 内字节偏移 
     * @param Size 字节数；0 表示剩余区间 
     * @return CPU 地址 
     */
	[[nodiscard]] virtual void* map(DeviceSizeType Offset = 0, DeviceSizeType Size = 0) = 0;
	
    /** 
     * @brief 结束当前映射访问 
     */
	virtual void unmap() = 0;
	
    /** 
     * @brief 将非一致内存的 CPU 写入刷新给 GPU 
     * @param Offset 字节偏移 
     * @param Size 字节数；0 表示剩余区间 
     */
	virtual void flush(DeviceSizeType Offset, DeviceSizeType Size) = 0;
	
    /** 
     * @brief 使非一致内存的 GPU 写入对 CPU 可见 
     * @param Offset 字节偏移 
     * @param Size 字节数；0 表示剩余区间 
     */
	virtual void invalidate(DeviceSizeType Offset, DeviceSizeType Size) = 0;

protected:
	RBuffer() = default;
};

/** @brief Immutable sampling-state wrapper owned by one logical device. */
class RSampler
{   
public:
    struct Descriptor_t
    {
        EFilterMode MinFilter { EFilterMode::Linear };
        EFilterMode MagFilter { EFilterMode::Linear };
        EFilterMode MipmapFilter { EFilterMode::Linear };
        ESamplerAddressMode AddressU { ESamplerAddressMode::Repeat };
        ESamplerAddressMode AddressV { ESamplerAddressMode::Repeat };
        ESamplerAddressMode AddressW { ESamplerAddressMode::Repeat };
        float MipLodBias { 0.0f };
        float MinLod { 0.0f };
        float MaxLod { 1000.0f };
        float MaxAnisotropy { 1.0f };
        bool CompareEnable { false };
        ECompareOp CompareOperation { ECompareOp::Always };
        std::string DebugName;
    };
public:
	virtual ~RSampler() = default;
	RSampler(const RSampler&) = delete;
	RSampler& operator=(const RSampler&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Sampler 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回不可变采样描述.  @return 创建描述.  */
	[[nodiscard]] virtual const Descriptor_t& getDescriptor() const noexcept = 0;
	/** @brief 查询底层采样器是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RSampler() = default;
};

/** @brief Typed subresource view into an image. */
class RImageView
{
public:
	/** @brief Immutable image-view creation descriptor. */
    struct Descriptor_t
    {
        std::shared_ptr<RImage> Image;

        // Undefined 表示继承 Image 的格式. 
        EFormat Format { EFormat::Undefined };

        // Auto 表示根据 Image dimension 和 layer 范围推导. 
        EImageViewDimension Dimension { EImageViewDimension::Auto };

        // Auto 表示根据最终格式推导 Color/Depth/DepthStencil. 
        EImageAspect Aspect { EImageAspect::Auto };

        uint32_t BaseMipLevel { 0 };
        uint32_t MipLevelCount { 1 };
        uint32_t BaseArrayLayer { 0 };
        uint32_t ArrayLayerCount { 1 };
    };

    virtual ~RImageView() = default;

    RImageView(const RImageView&) = delete;
    RImageView& operator=(const RImageView&) = delete;
    RImageView(RImageView&&) = delete;
    RImageView& operator=(RImageView&&) = delete;

	/** @brief 返回不可变 View 描述.  @return 创建描述.  */
	[[nodiscard]] const Descriptor_t& getDescriptor() const noexcept
    {
        return Descriptor;
    }

	/** @brief 返回被观察的 Image 并保持其生命周期.  @return Image 引用.  */
	[[nodiscard]] const std::shared_ptr<RImage>& getImage() const noexcept
    {
        return Descriptor.Image;
    }

	/** @brief 返回所属设备.  @return 创建此 View 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 查询 View 是否有效.  @return 有效时为 true.  */
    [[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
    [[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
    explicit RImageView(Descriptor_t Desc)
        : Descriptor(std::move(Desc))
    {
    }

private:
    const Descriptor_t Descriptor;
};

/** @brief Multidimensional image resource with explicit usage and memory semantics. */
class RImage
{
public:
	/** @brief Immutable image creation descriptor. */
	struct Descriptor_t
	{
		EFormat Format { EFormat::Undefined };
		EImageDimension Dimension { EImageDimension::Texture2D };
		uint32_t Width { 1 };
		uint32_t Height { 1 };
		uint32_t Depth { 1 };
		uint32_t MipLevels { 1 };
		uint32_t ArrayLayers { 1 };
		ESharingMode SharingMode { ESharingMode::Exclusive };
		EMemoryProperty MemoryProperty { EMemoryProperty_t::DeviceLocal };
		EImageUsage Usage {};
		ESampleCount SampleCount { ESampleCount::Count1 };
	};

public:
	virtual ~RImage() = default;
	RImage(const RImage&) = delete;
	RImage& operator=(const RImage&) = delete;
	RImage(RImage&&) = delete;
	RImage& operator=(RImage&&) = delete;

	/** @brief 返回不可变 Image 描述.  @return 创建描述.  */
	[[nodiscard]] const Descriptor_t& getDescriptor() const noexcept { return Descriptor; }
	/** @brief 返回所属设备.  @return 创建此 Image 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 查询底层 Image 是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 查询非交换链 Image 是否已绑定内存.  @return 可访问内存时为 true.  */
	[[nodiscard]] virtual bool isMemoryBound() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	explicit RImage(const Descriptor_t& Desc) : Descriptor(Desc) {}

private:
	const Descriptor_t Descriptor;
};

/**
 * @brief 高层"纹理"资源: 把底层图像存储、默认视图与默认采样器打包成一个可直接采样的整体.
 *
 * ## 与 RImage 的语义区别
 *
 * - `RImage` 是"原始图像"原语: 它只描述一段 GPU 显存中的多维像素数据及其格式、维度、
 *   用途与内存策略. RImage 本身 **不携带采样信息**, 不能直接绑定到着色器采样; 调用方必须
 *   另外创建 `RImageView`(选定子资源范围与解释)并配合 `RSampler`(选定过滤/寻址策略)才能采样.
 *
 * - `RTexture` 是"纹理"语义: 对应传统图形 API(如 D3D11 的 Texture2D + ShaderResourceView +
 *   SamplerState)中"创建即可采样"的纹理对象. 它把上述三件事组合为一个整体 —— 底层 `RImage`
 *   存储、覆盖完整 mip/layer 范围的默认 `RImageView`, 以及默认 `RSampler`.
 *
 * `RTexture` 不引入新的 GPU 原语, 只是 RImage/RImageView/RSampler 的组合与便捷封装. 需要独立
 * 控制 attachment/storage 视图, 或同一图像需要多种采样配置时, 仍应直接使用 RImage + RImageView + RSampler.
 * 因此 RTexture 刻意不暴露单一的原生句柄, 应通过 getImage()/getImageView()/getSampler() 访问组合部件.
 */
class RTexture
{
public:
	/** @brief Immutable texture creation descriptor. */
	struct Descriptor_t
	{
		// 底层图像存储描述; Usage 为空时后端默认使用 Sampled|TransferDst.
		RImage::Descriptor_t Image {};
		// 默认采样器描述(过滤/寻址/比较策略); 缺省为线性过滤 + Repeat 寻址.
		RSampler::Descriptor_t Sampler {};
		std::string DebugName;
	};

	virtual ~RTexture() = default;
	RTexture(const RTexture&) = delete;
	RTexture& operator=(const RTexture&) = delete;
	RTexture(RTexture&&) = delete;
	RTexture& operator=(RTexture&&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Texture 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回不可变创建描述.  @return 创建描述.  */
	[[nodiscard]] virtual const Descriptor_t& getDescriptor() const noexcept = 0;
	/** @brief 返回底层图像存储.  @return RImage 引用.  */
	[[nodiscard]] virtual const std::shared_ptr<RImage>& getImage() const noexcept = 0;
	/** @brief 返回覆盖完整 mip/layer 范围的默认视图.  @return RImageView 引用.  */
	[[nodiscard]] virtual const std::shared_ptr<RImageView>& getImageView() const noexcept = 0;
	/** @brief 返回默认采样器.  @return RSampler 引用.  */
	[[nodiscard]] virtual const std::shared_ptr<RSampler>& getSampler() const noexcept = 0;
	/** @brief 查询 Texture 及其组合部件是否全部有效.  @return 全部有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;

protected:
	RTexture() = default;
};

/** @brief Color attachment and optional multisample resolve state. */
struct ColorAttachment
{
	std::shared_ptr<RImageView> View;
	ELoadOp LoadOp { ELoadOp::Load };
	EStoreOp StoreOp { EStoreOp::Store };
	ClearColorValue ClearValue {};
	std::shared_ptr<RImageView> ResolveView;
	EResolveMode ResolveMode { EResolveMode::None };
};

/** @brief Independent load/store/resolve policy for one depth-stencil aspect. */
struct DepthStencilAspectOps
{
	ELoadOp LoadOp { ELoadOp::Load };
	EStoreOp StoreOp { EStoreOp::Store };
	EResolveMode ResolveMode { EResolveMode::None };
};

/** @brief Depth/stencil attachment, resolve targets, and clear payload. */
struct DepthStencilAttachment
{
	std::shared_ptr<RImageView> View;
	std::shared_ptr<RImageView> ResolveView;
	DepthStencilAspectOps Depth {};
	DepthStencilAspectOps Stencil {
		.LoadOp = ELoadOp::DontCare,
		.StoreOp = EStoreOp::DontCare,
		.ResolveMode = EResolveMode::None
	};
	ClearDepthStencilValue ClearValue {};
};

/** @brief Complete dynamic-rendering instance state consumed by beginRendering(). */
struct RenderingInfo
{
	RenderArea Area {};
	std::span<const ColorAttachment> ColorAttachments {};
	const DepthStencilAttachment* DepthStencil { nullptr };
	uint32_t LayerCount { 1 };
	uint32_t ViewMask { 0 };
	/** @brief Records this scope with Vulkan secondary-command-buffer contents semantics; inline draws are then rejected. */
	bool SecondaryCommandBuffers { false };
};

/** @brief Contiguous image mip/layer/aspect range. */
struct ImageSubresourceRange
{
	EImageAspect Aspect { EImageAspect::Auto };
	uint32_t BaseMipLevel { 0 };
	uint32_t MipLevelCount { 1 };
	uint32_t BaseArrayLayer { 0 };
	uint32_t ArrayLayerCount { 1 };
};

/** @brief Image state transition with optional queue ownership transfer. */
struct ImageBarrier
{
	std::shared_ptr<RImage> Image;
	EResourceState Before { EResourceState::Undefined };
	EResourceState After { EResourceState::Undefined };
	ImageSubresourceRange Range {};
	std::optional<ECommandQueueType> SourceQueue;
	std::optional<ECommandQueueType> DestinationQueue;
};

/** @brief 对所有内存访问建立顺序，不包含资源布局转换.  */
struct GlobalBarrier
{
	EResourceState Before { EResourceState::Common };
	EResourceState After { EResourceState::Common };
};

/** @brief Buffer 子区间的可见性与可选队列所有权转换.  */
struct BufferBarrier
{
	std::shared_ptr<RBuffer> Buffer;
	EResourceState Before { EResourceState::Common };
	EResourceState After { EResourceState::Common };
	DeviceSizeType Offset { 0 };
	DeviceSizeType Size { 0 };
	std::optional<ECommandQueueType> SourceQueue;
	std::optional<ECommandQueueType> DestinationQueue;
};

/** @brief Byte ranges participating in one buffer-to-buffer copy. */
struct BufferCopyRegion
{
	DeviceSizeType SourceOffset { 0 };
	DeviceSizeType DestinationOffset { 0 };
	DeviceSizeType Size { 0 };
};

/** @brief Signed image-space texel offset. */
struct ImageOffset3D
{
	int32_t X { 0 };
	int32_t Y { 0 };
	int32_t Z { 0 };
};

/** @brief Unsigned three-dimensional texel extent. */
struct ImageExtent3D
{
	uint32_t Width { 1 };
	uint32_t Height { 1 };
	uint32_t Depth { 1 };
};

/** @brief Source and destination subresources for an image copy. */
struct ImageCopyRegion
{
	ImageSubresourceRange Source {};
	ImageOffset3D SourceOffset {};
	ImageSubresourceRange Destination {};
	ImageOffset3D DestinationOffset {};
	ImageExtent3D Extent {};
};

/** @brief Source and destination boxes for a filtered image blit. */
struct ImageBlitRegion
{
	ImageSubresourceRange Source {};
	std::array<ImageOffset3D, 2> SourceOffsets {};
	ImageSubresourceRange Destination {};
	std::array<ImageOffset3D, 2> DestinationOffsets {};
};

/** @brief Buffer layout and image subresource participating in a transfer. */
struct BufferImageCopyRegion
{
	DeviceSizeType BufferOffset { 0 };
	uint32_t BufferRowLength { 0 };
	uint32_t BufferImageHeight { 0 };
	ImageSubresourceRange Image {};
	ImageOffset3D ImageOffset {};
	ImageExtent3D ImageExtent {};
};

/** @brief Vertex buffer plus byte offset bound to one input slot. */
struct VertexBufferBinding
{
	std::shared_ptr<RBuffer> Buffer;
	DeviceSizeType Offset { 0 };
};

/** @brief Query-pool type, capacity, and pipeline-statistics selection. */
struct QueryPoolDescriptor
{
	EQueryType Type { EQueryType::Timestamp };
	uint32_t Count { 1 };
	EPipelineStatistics PipelineStatistics {};
	std::string DebugName;
};

/** @brief Format operation masks split by image tiling and buffer use. */
struct FormatCapabilities
{
	EFormatFeatures OptimalTiling {};
	EFormatFeatures LinearTiling {};
	EFormatFeatures Buffer {};
};

/** @brief Pipeline/dynamic-rendering attachment compatibility key. */
struct RenderingSignature
{
	std::array<EFormat, MaxColorAttachments> ColorFormats {};
	uint32_t ColorAttachmentCount { 0 };
	EFormat DepthFormat { EFormat::Undefined };
	EFormat StencilFormat { EFormat::Undefined };
	ESampleCount SampleCount { ESampleCount::Count1 };
	uint32_t ViewMask { 0 };

	constexpr bool operator==(const RenderingSignature&) const = default;
};

/**
 * @brief 判断 Dynamic Rendering 附件签名是否兼容. 
 *
 * Load/Store/Clear、实际 ImageView、RenderArea 和 Resolve 目标不影响 Pipeline
 * 兼容性. 所有未使用颜色槽必须被规范化为 EFormat::Undefined. 
 */
[[nodiscard]] constexpr bool isRenderingCompatible(
	const RenderingSignature& PipelineSignature,
	const RenderingSignature& RenderingSignature) noexcept
{
	return PipelineSignature == RenderingSignature;
}

/** @brief 已编译 Shader 字节码及其稳定身份.  */
struct ShaderDescriptor
{
	EShaderStage_t Stage { EShaderStage_t::Vertex };
	std::vector<std::byte> ByteCode;
	std::string EntryPoint { "main" };
	std::string DebugName;
	uint64_t ContentHash { 0 };
};

/**
 * @note RShader 表示一个已经编译的 Shader Module.
 * @note 其不包含 Rasterizer State, Render Target Format, Vertex Input, Depth/Stencil
 * @note 同一个 Vertex RShader 也可以参与多个 Pipeline
 */
/** @brief Immutable compiled shader module and stable cache identity. */
class RShader
{
public:
	virtual ~RShader() = default;
	RShader(const RShader&) = delete;
	RShader& operator=(const RShader&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Shader 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回唯一 Shader 阶段.  @return 阶段枚举.  */
	[[nodiscard]] virtual EShaderStage_t getStage() const noexcept = 0;
	/** @brief 返回稳定内容标识.  @return 用于语义缓存的哈希.  */
	[[nodiscard]] virtual uint64_t getContentHash() const noexcept = 0;
	/** @brief 返回入口点名称.  @return 创建时保存的入口点.  */
	[[nodiscard]] virtual const std::string& getEntryPoint() const noexcept = 0;
	/** @brief 查询模块是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RShader() = default;
};

/** @brief Descriptor/Bind Group Layout 中的一项资源声明.  */
struct BindGroupLayoutEntry
{
	uint32_t Binding { 0 };
	EDescriptorType Type { EDescriptorType::UniformBuffer };
	uint32_t ArrayCount { 1 };
	EShaderStage Visibility {};
	EDescriptorBindingFlags Flags {};

	constexpr bool operator==(const BindGroupLayoutEntry&) const = default;
};

/** @brief Immutable collection of normalized resource binding declarations. */
struct BindGroupLayoutDescriptor
{
	std::vector<BindGroupLayoutEntry> Entries;
	std::string DebugName;
};

/** @brief Device object defining bind-group ABI and compatibility identity. */
class RBindGroupLayout
{
public:
	virtual ~RBindGroupLayout() = default;
	RBindGroupLayout(const RBindGroupLayout&) = delete;
	RBindGroupLayout& operator=(const RBindGroupLayout&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Layout 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回兼容键的加速哈希.  @return 64 位哈希；不能单独证明兼容.  */
	[[nodiscard]] virtual uint64_t getCompatibilityHash() const noexcept = 0;
	/** @brief 返回规范化完整兼容键.  @return 在对象生命周期内有效的字节视图.  */
	[[nodiscard]] virtual std::span<const std::byte> getCompatibilityKey() const noexcept = 0;
	/** @brief 返回按 Binding 规范化的条目.  @return 只读条目视图.  */
	[[nodiscard]] virtual std::span<const BindGroupLayoutEntry> getEntries() const noexcept = 0;
	/** @brief 查询 Layout 是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RBindGroupLayout() = default;
};

/** @brief Byte range of a buffer bound as a structured resource. */
struct BufferBinding
{
	std::shared_ptr<RBuffer> Buffer;
	DeviceSizeType Offset { 0 };
	DeviceSizeType Size { 0 };
};

/** @brief Image view and expected descriptor access layout. */
struct TextureBinding
{
	std::shared_ptr<RImageView> View;
	EDescriptorImageLayout Layout { EDescriptorImageLayout::ShaderReadOnly };
};

/** @brief 带格式解释的 Buffer 区间，用于 Uniform/Storage Texel Buffer.  */
struct TexelBufferBinding
{
	std::shared_ptr<RBuffer> Buffer;
	EFormat Format { EFormat::Undefined };
	DeviceSizeType Offset { 0 };
	DeviceSizeType Size { 0 };
};

/** @brief Sampler resource stored in a bind-group entry. */
struct SamplerBinding
{
	std::shared_ptr<RSampler> Sampler;
};

/** @brief Combined image-view and sampler descriptor payload. */
struct CombinedImageSamplerBinding
{
	std::shared_ptr<RImageView> View;
	std::shared_ptr<RSampler> Sampler;
	EDescriptorImageLayout Layout { EDescriptorImageLayout::ShaderReadOnly };
};

/** @brief Top-level acceleration structure descriptor payload. */
struct AccelerationStructureBinding
{
	std::shared_ptr<RAccelerationStructure> AccelerationStructure;
};

/** @brief Type-safe payload accepted by a bind-group entry. */
using BindGroupResource = std::variant<
	BufferBinding,
	TexelBufferBinding,
	TextureBinding,
	SamplerBinding,
	CombinedImageSamplerBinding,
	AccelerationStructureBinding>;

/** @brief One array element written at a bind-group binding. */
struct BindGroupEntry
{
	uint32_t Binding { 0 };
	uint32_t ArrayElement { 0 };
	BindGroupResource Resource;
};

/** @brief Immutable resource population used to create a bind group. */
struct BindGroupDescriptor
{
	std::shared_ptr<RBindGroupLayout> Layout;
	std::vector<BindGroupEntry> Entries;
	uint32_t VariableArrayCount { 0 };
	std::string DebugName;
};

/** @brief 一个已写入具体资源、创建后不可变的资源绑定组.  */
class RBindGroup
{
public:
	virtual ~RBindGroup() = default;
	RBindGroup(const RBindGroup&) = delete;
	RBindGroup& operator=(const RBindGroup&) = delete;

	/** @brief 返回所属设备.  @return 创建此 BindGroup 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回资源 ABI Layout.  @return 保持生命周期的 Layout.  */
	[[nodiscard]] virtual const std::shared_ptr<RBindGroupLayout>& getLayout() const noexcept = 0;
	/** @brief 查询描述符组是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RBindGroup() = default;
};

/** @brief Identifies bottom-level geometry or top-level instance acceleration structures. */
enum class EAccelerationStructureType : uint8_t { BottomLevel, TopLevel };

/** @brief Selects build or update mode for acceleration-structure recording. */
enum class EAccelerationStructureBuildMode : uint8_t { Build, Update };

/** @brief Selects triangle, AABB, or instance input interpretation for an acceleration-structure build. */
enum class EAccelerationStructureGeometryType : uint8_t { Triangles, AABBs, Instances };

/** @brief Bit values controlling ray interaction with one geometry. */
enum class EAccelerationStructureGeometryFlag_t : uint8_t
{
	None = 0,
	Opaque = 1 << 0,
	NoDuplicateAnyHitInvocation = 1 << 1
};

/** @brief Composable per-geometry ray interaction flags. */
using EAccelerationStructureGeometryFlags = core::Flags<EAccelerationStructureGeometryFlag_t>;

/** @brief Bit values controlling acceleration-structure build performance and mutability. */
enum class EAccelerationStructureBuildFlag_t : uint8_t
{
	None = 0,
	AllowUpdate = 1 << 0,
	AllowCompaction = 1 << 1,
	PreferFastTrace = 1 << 2,
	PreferFastBuild = 1 << 3,
	LowMemory = 1 << 4
};

/** @brief Composable acceleration-structure build policy flags. */
using EAccelerationStructureBuildFlags = core::Flags<EAccelerationStructureBuildFlag_t>;
DEFINE_ENUM_OPERATOR(EAccelerationStructureBuildFlag_t);

/** @brief Triangle vertex/index/transform buffers used to build bottom-level geometry. */
struct AccelerationStructureTriangles
{
	std::shared_ptr<RBuffer> VertexBuffer;
	DeviceSizeType VertexOffset { 0 };
	uint32_t VertexStride { 0 };
	uint32_t MaxVertex { 0 };
	EVertexFormat VertexFormat { EVertexFormat::Float3 };
	std::shared_ptr<RBuffer> IndexBuffer;
	DeviceSizeType IndexOffset { 0 };
	EIndexFormat IndexFormat { EIndexFormat::None };
	std::shared_ptr<RBuffer> TransformBuffer;
	DeviceSizeType TransformOffset { 0 };
};

/** @brief Axis-aligned bounding-box records used to build procedural geometry. */
struct AccelerationStructureAABBs
{
	std::shared_ptr<RBuffer> Buffer;
	DeviceSizeType Offset { 0 };
	uint32_t Stride { 24 };
};

/** @brief Packed top-level instance records referencing bottom-level structures. */
struct AccelerationStructureInstances
{
	std::shared_ptr<RBuffer> Buffer;
	DeviceSizeType Offset { 0 };
	bool ArrayOfPointers { false };
};

/** @brief Type-safe geometry payload used by an acceleration-structure build. */
using AccelerationStructureGeometryData = std::variant<
	AccelerationStructureTriangles,
	AccelerationStructureAABBs,
	AccelerationStructureInstances>;

/** @brief One geometry and primitive range within an acceleration-structure build. */
struct AccelerationStructureGeometry
{
	EAccelerationStructureGeometryType Type { EAccelerationStructureGeometryType::Triangles };
	EAccelerationStructureGeometryFlags Flags {};
	AccelerationStructureGeometryData Data { AccelerationStructureTriangles {} };
	uint32_t PrimitiveCount { 0 };
	uint32_t PrimitiveOffset { 0 };
	uint32_t FirstVertex { 0 };
	uint32_t TransformOffset { 0 };
};

/** @brief Storage, build-scratch, and update-scratch sizes required for an acceleration structure. */
struct AccelerationStructureBuildSizes
{
	DeviceSizeType AccelerationStructureSize { 0 };
	DeviceSizeType BuildScratchSize { 0 };
	DeviceSizeType UpdateScratchSize { 0 };
};

/** @brief Immutable acceleration-structure storage and type descriptor. */
struct AccelerationStructureDescriptor
{
	EAccelerationStructureType Type { EAccelerationStructureType::BottomLevel };
	std::shared_ptr<RBuffer> Storage;
	DeviceSizeType Offset { 0 };
	DeviceSizeType Size { 0 };
	std::string DebugName;
};

/** @brief Backend-neutral acceleration-structure build request and scratch allocation. */
struct AccelerationStructureBuildDescriptor
{
	std::shared_ptr<RAccelerationStructure> Destination;
	std::shared_ptr<RAccelerationStructure> Source;
	EAccelerationStructureBuildMode Mode { EAccelerationStructureBuildMode::Build };
	EAccelerationStructureBuildFlags Flags {};
	std::span<const AccelerationStructureGeometry> Geometries;
	std::shared_ptr<RBuffer> ScratchBuffer;
	DeviceSizeType ScratchOffset { 0 };
};

/** @brief 光线追踪加速结构的后端无关句柄接口.  */
class RAccelerationStructure
{
public:
	virtual ~RAccelerationStructure() = default;
	RAccelerationStructure(const RAccelerationStructure&) = delete;
	RAccelerationStructure& operator=(const RAccelerationStructure&) = delete;

	/** @brief 返回所属设备.  @return 创建此加速结构的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 查询底层加速结构是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
	/** @brief Returns the address usable in instance/build records. @return Nonzero address when supported and valid; otherwise zero. */
	[[nodiscard]] virtual DeviceAddress getDeviceAddress() const noexcept { return 0; }

protected:
	RAccelerationStructure() = default;
};

/** @brief One stage-visible byte range in the pipeline push-constant ABI. */
struct PushConstantRange
{
	EShaderStage Stages {};
	uint32_t Offset { 0 };
	uint32_t Size { 0 };

	constexpr bool operator==(const PushConstantRange&) const = default;
};

/** @brief Ordered bind-group layouts and push-constant ranges forming a pipeline ABI. */
struct PipelineLayoutDescriptor
{
	std::vector<std::shared_ptr<RBindGroupLayout>> BindGroupLayouts;
	std::vector<PushConstantRange> PushConstantRanges;
	std::string DebugName;
};

/**
 * @brief 描述 Shader 可访问资源的不可变 Pipeline ABI. 
 *
 * 例如：
 *  Set 0:
 *    Binding 0 = Camera Uniform Buffer
 *    Binding 1 = Scene Storage Buffer
 *
 *  Set 1:
 *    Binding 0 = Material Texture
 *    Binding 1 = Material Sampler
 *
 *  Push Constants:
 *    Offset 0, Size 64, Vertex|Pixel
 *
 * 其本质上是资源的ABI, 创建 Pipeline 后不能改变 ABI; 相同规范化 Layout 应复用; 未来必须与 Shader Reflection 对照
 */
class RPipelineLayout
{
public:
	virtual ~RPipelineLayout() = default;
	RPipelineLayout(const RPipelineLayout&) = delete;
	RPipelineLayout& operator=(const RPipelineLayout&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Layout 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回完整 ABI 键的加速哈希.  @return 64 位哈希；命中后仍须比较完整键.  */
	[[nodiscard]] virtual uint64_t getCompatibilityHash() const noexcept = 0;
	/** @brief 返回规范化完整 ABI 键.  @return 在对象生命周期内有效的字节视图.  */
	[[nodiscard]] virtual std::span<const std::byte> getCompatibilityKey() const noexcept = 0;
	/** @brief 返回 BindGroup Layout 数量.  @return 连续 Group 槽位数.  */
	[[nodiscard]] virtual uint32_t getBindGroupLayoutCount() const noexcept = 0;
	/** @brief 返回指定 Group 的 Layout.  @param GroupIndex 小于 getBindGroupLayoutCount() 的索引.  @return Layout 引用.  */
	[[nodiscard]] virtual const std::shared_ptr<RBindGroupLayout>& getBindGroupLayout(
		uint32_t GroupIndex) const = 0;
	/** @brief 判断一次 Push Constant 更新是否被 ABI 覆盖.  @param Stages 目标阶段.  @param Offset 字节偏移.  @param Size 字节数.  @return 完整覆盖时为 true.  */
	[[nodiscard]] virtual bool supportsPushConstants(
		EShaderStage Stages,
		uint32_t Offset,
		uint32_t Size) const noexcept = 0;
	/** @brief 查询 Layout 是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RPipelineLayout() = default;
};

/** @brief Raw specialization-constant value keyed by shader constant ID. */
struct SpecializationConstant
{
	uint32_t Id { 0 };
	std::vector<std::byte> Data;
};

/** @brief Shader module and per-stage specialization constants. */
struct PipelineShaderStage
{
	std::shared_ptr<RShader> Shader;
	std::vector<SpecializationConstant> SpecializationConstants;
};

/** @brief Stride and advancement rate for one vertex buffer binding. */
struct VertexBufferLayout
{
	uint32_t Binding { 0 };
	uint32_t Stride { 0 };
	EVertexInputRate InputRate { EVertexInputRate::PerVertex };
};

/** @brief Shader location mapping into a vertex buffer binding. */
struct VertexAttribute
{
	uint32_t Location { 0 };
	uint32_t Binding { 0 };
	EVertexFormat Format { EVertexFormat::Float3 };
	uint32_t Offset { 0 };
};

/** @brief Complete static or dynamically supplied vertex input layout. */
struct VertexInputState
{
	std::vector<VertexBufferLayout> Buffers;
	std::vector<VertexAttribute> Attributes;
};

/** @brief Primitive assembly and restart configuration. */
struct InputAssemblyState
{
	EPrimitiveTopology Topology { EPrimitiveTopology::TriangleList };
	bool PrimitiveRestartEnable { false };
	uint32_t PatchControlPoints { 0 };
};

/** @brief Polygon rasterization, culling, bias, and line-width configuration. */
struct RasterizerState
{
	EFillMode FillMode { EFillMode::Solid };
	ECullMode CullMode { ECullMode::Back };
	EFrontFace FrontFace { EFrontFace::CounterClockwise };
	bool DepthClampEnable { false };
	bool RasterizerDiscardEnable { false };
	bool DepthBiasEnable { false };
	float DepthBiasConstantFactor { 0.0f };
	float DepthBiasClamp { 0.0f };
	float DepthBiasSlopeFactor { 0.0f };
	float LineWidth { 1.0f };
};

/** @brief Multisample coverage, masking, and per-sample shading configuration. */
struct MultisampleState
{
	bool SampleShadingEnable { false };
	float MinSampleShading { 0.0f };
	uint64_t SampleMask { ~uint64_t { 0 } };
	bool AlphaToCoverageEnable { false };
	bool AlphaToOneEnable { false };
};

/** @brief Compare and update policy for one orientation of stencil-tested faces. */
struct StencilFaceState
{
	EStencilOp FailOp { EStencilOp::Keep };
	EStencilOp PassOp { EStencilOp::Keep };
	EStencilOp DepthFailOp { EStencilOp::Keep };
	ECompareOp CompareOp { ECompareOp::Always };
	uint32_t CompareMask { 0xFFFFFFFFu };
	uint32_t WriteMask { 0xFFFFFFFFu };
	uint32_t Reference { 0 };
};

/** @brief Depth and stencil testing state for a graphics pipeline. */
struct DepthStencilState
{
	bool DepthTestEnable { false };
	bool DepthWriteEnable { false };
	ECompareOp DepthCompareOp { ECompareOp::Less };
	bool DepthBoundsTestEnable { false };
	float MinDepthBounds { 0.0f };
	float MaxDepthBounds { 1.0f };
	bool StencilTestEnable { false };
	StencilFaceState Front {};
	StencilFaceState Back {};
};

/** @brief Blend factors and operation for one color or alpha component. */
struct BlendComponent
{
	EBlendFactor SrcFactor { EBlendFactor::One };
	EBlendFactor DstFactor { EBlendFactor::Zero };
	EBlendOp Operation { EBlendOp::Add };

	constexpr bool operator==(const BlendComponent&) const = default;
};

/** @brief Blend and channel-write state for one color attachment. */
struct ColorTargetBlendState
{
	bool BlendEnable { false };
	BlendComponent Color {};
	BlendComponent Alpha {};
	EColorWriteMask WriteMask { EColorWriteMask_t::All };

	constexpr bool operator==(const ColorTargetBlendState&) const = default;
};

/** @brief Per-color-attachment blend state array. */
struct BlendState
{
	std::array<ColorTargetBlendState, MaxColorAttachments> Attachments {};
};

/** @brief Pipeline compilation policy and optional reusable cache. */
struct PipelineCompileOptions
{
	EPipelineCompileFlags Flags {};
	std::shared_ptr<RPipelineCache> Cache;
};

/**
 * @brief Graphics Pipeline 的完整 owning 描述符. 
 *
 * 描述符可安全复制或移动到后台线程. 空 Shader 字段表示对应 Stage 未使用. 
 * Vertex 与 Mesh 路径互斥；Task Shader 只能与 Mesh Shader 一起使用. 
 */
struct GraphicsPipelineDescriptor
{
	std::shared_ptr<RPipelineLayout> Layout;
	PipelineShaderStage Vertex;
	PipelineShaderStage Pixel;
	PipelineShaderStage Geometry;
	PipelineShaderStage Hull;
	PipelineShaderStage Domain;
	PipelineShaderStage Task;
	PipelineShaderStage Mesh;
	VertexInputState VertexInput {};
	InputAssemblyState InputAssembly {};
	RasterizerState Rasterizer {};
	MultisampleState Multisample {};
	DepthStencilState DepthStencil {};
	BlendState Blend {};
	RenderingSignature Rendering {};
	EDynamicStates DynamicStates {
		EDynamicStates(EDynamicState_t::Viewport) | EDynamicState_t::Scissor
	};
	PipelineCompileOptions Compile {};
	std::string DebugName;
};

/** @brief Complete compute pipeline creation descriptor. */
struct ComputePipelineDescriptor
{
	std::shared_ptr<RPipelineLayout> Layout;
	PipelineShaderStage Compute;
	PipelineCompileOptions Compile {};
	std::string DebugName;
};

/** @brief Role of one ray-tracing shader group in a ray-tracing pipeline. */
enum class ERayTracingShaderGroupType : uint8_t
{
	General,
	TrianglesHitGroup,
	ProceduralHitGroup
};

/** @brief Shader indices forming one addressable ray-tracing shader group. */
struct RayTracingShaderGroup
{
	static constexpr uint32_t UnusedShader = std::numeric_limits<uint32_t>::max();
	ERayTracingShaderGroupType Type { ERayTracingShaderGroupType::General };
	uint32_t GeneralShader { UnusedShader };
	uint32_t ClosestHitShader { UnusedShader };
	uint32_t AnyHitShader { UnusedShader };
	uint32_t IntersectionShader { UnusedShader };
};

/** @brief Complete ray-tracing pipeline stages, groups, recursion limit, and compile policy. */
struct RayTracingPipelineDescriptor
{
	std::shared_ptr<RPipelineLayout> Layout;
	std::vector<PipelineShaderStage> Stages;
	std::vector<RayTracingShaderGroup> Groups;
	uint32_t MaxRecursionDepth { 1 };
	PipelineCompileOptions Compile {};
	std::string DebugName;
};

/** @brief One device-address region of a shader binding table. */
struct ShaderBindingTableRegion
{
	DeviceAddress Address { 0 };
	DeviceSizeType Stride { 0 };
	DeviceSizeType Size { 0 };
};

/** @brief Ray-generation, miss, hit, and callable SBT regions consumed by traceRays(). */
struct TraceRaysDescriptor
{
	ShaderBindingTableRegion RayGeneration;
	ShaderBindingTableRegion Miss;
	ShaderBindingTableRegion Hit;
	ShaderBindingTableRegion Callable;
	uint32_t Width { 1 };
	uint32_t Height { 1 };
	uint32_t Depth { 1 };
};

/**
 * @brief 后端 Pipeline 二进制缓存, 用于加速编译.
 *
 * serialize() 返回的字节只保证可交给同一 RHI 版本、后端、设备和兼容驱动. 
 * 磁盘层仍必须附加并校验 RHI/Schema/设备/驱动版本头. 
 */
class RPipelineCache
{
public:
	virtual ~RPipelineCache() = default;
	/** @brief 合并同设备兼容缓存.  @param Sources 源缓存；调用返回后可释放.  */
	virtual void merge(std::span<const std::shared_ptr<RPipelineCache>> Sources) = 0;
	/** @brief 返回所属设备.  @return 创建此缓存的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 序列化驱动缓存字节.  @return 仅适用于兼容设备/驱动的二进制.  */
	[[nodiscard]] virtual std::vector<std::byte> serialize() const = 0;
	/** @brief 查询缓存对象是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief Initial serialized data and debug identity for a pipeline cache. */
struct PipelineCacheDescriptor
{
	std::vector<std::byte> InitialData;
	std::string DebugName;
};

/** @brief Immutable executable graphics, compute, or ray-tracing pipeline. */
class RPipeline
{
public:
	virtual ~RPipeline() = default;
	RPipeline(const RPipeline&) = delete;
	RPipeline& operator=(const RPipeline&) = delete;

	/** @brief 返回所属设备.  @return 创建此 Pipeline 的设备.  */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief 返回执行绑定点类型.  @return Graphics、Compute 或 RayTracing.  */
	[[nodiscard]] virtual EPipelineType getType() const noexcept = 0;
	/** @brief 返回资源 ABI.  @return 保持生命周期的 PipelineLayout.  */
	[[nodiscard]] virtual const std::shared_ptr<RPipelineLayout>& getLayout() const noexcept = 0;
	/** @brief 返回 Graphics 附件兼容签名.  @return 非 Graphics 类型返回 nullptr.  */
	[[nodiscard]] virtual const RenderingSignature* getRenderingSignature() const noexcept = 0;
	/** @brief 返回 Draw 前必须初始化的动态状态集合.  @return 动态状态位.  */
	[[nodiscard]] virtual EDynamicStates getDynamicStates() const noexcept = 0;
	/** @brief 查询 Graphics Pipeline 是否使用 Mesh 路径.  @return 使用 Mesh Shader 时为 true.  */
	[[nodiscard]] virtual bool usesMeshShaders() const noexcept = 0;
	/** @brief 返回语义键的加速哈希.  @return 稳定 64 位哈希；不能单独证明相等.  */
	[[nodiscard]] virtual uint64_t getCacheKey() const noexcept = 0;
	/** @brief 返回诊断名称.  @return 创建时保存的名称.  */
	[[nodiscard]] virtual const std::string& getDebugName() const noexcept = 0;
	/** @brief 查询底层可执行对象是否有效.  @return 有效时为 true.  */
	[[nodiscard]] virtual bool isValid() const noexcept = 0;
	/** @brief 返回非 owning 后端句柄.  @return 不可用时为 nullptr.  */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

protected:
	RPipeline() = default;
};

/** @brief CPU 可等待的单次提交完成标记.  */
class RFence
{
public:
	virtual ~RFence() = default;
	/** @brief Returns the owning device. @return Device that created the fence. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief Observes completion without waiting. @return True when signaled. */
	[[nodiscard]] virtual bool isSignaled() const = 0;
	/** @brief Waits for the signal or timeout. @param TimeoutNanoseconds Maximum host wait; max means infinite. @return True when signaled, false on timeout or device loss. */
	[[nodiscard]] virtual bool wait(uint64_t TimeoutNanoseconds = std::numeric_limits<uint64_t>::max()) = 0;
	/** @brief Returns a signaled fence to the unsignaled state; caller must ensure no pending submission owns it. */
	virtual void reset() = 0;
	/** @brief Returns the non-owning backend handle. @return Handle, or nullptr if unavailable. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief GPU 同步原语；Timeline=false 时 Value 参数被忽略.  */
class RSemaphore
{
public:
	virtual ~RSemaphore() = default;
	/** @brief Returns the owning device. @return Device that created the semaphore. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief Distinguishes timeline from binary semantics. @return True for timeline semaphores. */
	[[nodiscard]] virtual bool isTimeline() const noexcept = 0;
	/** @brief Reads the last completed timeline value. @return Monotonic value; binary semaphores return zero. */
	[[nodiscard]] virtual uint64_t getCompletedValue() const = 0;
	/** @brief Host-signals a timeline semaphore. @param Value Strictly greater value; invalid for binary semaphores. */
	virtual void signal(uint64_t Value) = 0;
	/** @brief Host-waits for a timeline value. @param Value Required completed value. @param TimeoutNanoseconds Maximum host wait. @return True on completion, false on timeout/device loss. */
	[[nodiscard]] virtual bool wait(
		uint64_t Value,
		uint64_t TimeoutNanoseconds = std::numeric_limits<uint64_t>::max()) = 0;
	/** @brief Returns the non-owning backend handle. @return Handle, or nullptr if unavailable. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief Semaphore and timeline value participating in a queue submission. */
struct SemaphoreSubmitInfo
{
	std::shared_ptr<RSemaphore> Semaphore;
	uint64_t Value { 0 };
};

/** @brief Command batches, synchronization waits/signals, and completion fence. */
struct QueueSubmitDescriptor
{
	std::span<const std::shared_ptr<RCommandList>> CommandLists;
	std::span<const SemaphoreSubmitInfo> WaitSemaphores;
	std::span<const SemaphoreSubmitInfo> SignalSemaphores;
	std::shared_ptr<RFence> Fence;
};

/** @brief Swapchain image and binary semaphore waits for presentation. */
struct PresentDescriptor
{
	std::shared_ptr<RSwapchain> Swapchain;
	uint32_t ImageIndex { 0 };
	/** @brief Generation returned by the successful acquire; stale generations must not be presented. */
	uint64_t Generation { 0 };
	std::span<const std::shared_ptr<RSemaphore>> WaitSemaphores;
};

/** @brief Device execution queue supporting submission, presentation, and progress polling. */
class RQueue
{
public:
	virtual ~RQueue() = default;
	/** @brief Returns the owning device. @return Device that created this queue. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief Returns queue capability class. @return Graphics, compute, or copy class. */
	[[nodiscard]] virtual ECommandQueueType getType() const noexcept = 0;
	/** @brief Enqueues command lists and synchronization atomically in submission order. @param Desc Submission payload whose spans need only remain valid through the call. */
	virtual void submit(const QueueSubmitDescriptor& Desc) = 0;
	/** @brief Presents an acquired image. @param Desc Swapchain image and waits. @return Recoverable presentation status; DeviceLost forbids further queue use. */
	[[nodiscard]] virtual EPresentStatus present(const PresentDescriptor& Desc) = 0;
	/** @brief Advances backend completion bookkeeping without blocking for idle. */
	virtual void poll() = 0;
	/** @brief Blocks until all work previously submitted to this queue completes or device loss is observed. */
	virtual void waitIdle() = 0;
	/** @brief Returns the non-owning backend queue handle. @return Handle, or nullptr if unavailable. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief Fixed-capacity query storage with explicit availability and timestamp conversion semantics. */
class RQueryPool
{
public:
	virtual ~RQueryPool() = default;
	/** @brief Returns the owning device. @return Device that created this pool. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief Returns immutable creation settings. @return Pool descriptor. */
	[[nodiscard]] virtual const QueryPoolDescriptor& getDescriptor() const noexcept = 0;
	/** @brief Reads 64-bit query words using legacy wait behavior. @param FirstQuery First slot. @param Results Destination words. @param Wait Whether to block for availability. @return True only when all requested results are available. */
	[[nodiscard]] virtual bool getResults(
		uint32_t FirstQuery,
		std::span<uint64_t> Results,
		bool Wait = false) const = 0;
	/** @brief Reads query words with availability/partial policy. Result64 is implicit for this uint64_t span. @param FirstQuery First slot. @param QueryCount Number of slots. @param Results Destination words, including availability words when requested. @param Flags Read policy. @return True only when every requested query was available; false leaves unavailable words untouched unless Partial is set. */
	[[nodiscard]] virtual bool getResults(
		uint32_t FirstQuery,
		uint32_t QueryCount,
		std::span<uint64_t> Results,
		EQueryResultFlags Flags) const
	{
		(void)QueryCount;
		return getResults(FirstQuery, Results, Flags.has(EQueryResultFlag_t::Wait));
	}
	/** @brief Converts a raw timestamp delta to nanoseconds using device timestamp period. @param TickDelta Raw modular tick delta already masked to TimestampValidBits. @return Nanoseconds, or 0 when timestamp queries are unsupported. */
	[[nodiscard]] virtual double timestampTicksToNanoseconds(uint64_t TickDelta) const noexcept
	{
		(void)TickDelta;
		return 0.0;
	}
	/** @brief Returns the non-owning backend pool handle. @return Handle, or nullptr if unavailable. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief Records typed GPU work and explicit synchronization for one queue class. */
class RCommandList
{
public:
	virtual ~RCommandList() = default;
	/** @brief Returns the owning device. @return Device that created this command list. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	RCommandList(const RCommandList&) = delete;
	RCommandList& operator=(const RCommandList&) = delete;

	/** @brief Returns required queue capability. @return Queue class selected at creation. */
	[[nodiscard]] virtual ECommandQueueType getQueueType() const noexcept = 0;
	/** @brief Returns recording level. @return Primary or secondary. */
	[[nodiscard]] virtual ECommandListLevel getLevel() const noexcept = 0;
	/** @brief Returns lifecycle state. @return Current command-list state. */
	[[nodiscard]] virtual ECommandListState getState() const noexcept = 0;
	/** @brief Returns the non-owning backend command handle. @return Handle, or nullptr when invalid. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;

	/** @brief Begins recording; secondary rendering inheritance comes from the creation descriptor. */
	virtual void begin() = 0;
	/** @brief Ends recording and validates completeness, producing Executable or Invalid state. */
	virtual void end() = 0;
	/** @brief Returns a non-pending list to Initial and releases transient recording references. */
	virtual void reset() = 0;

	/** @brief Records global, buffer, and image memory dependencies. @param GlobalBarriers Global access ordering. @param BufferBarriers Buffer ranges. @param ImageBarriers Image transitions. */
	virtual void barriers(
		std::span<const GlobalBarrier> GlobalBarriers,
		std::span<const BufferBarrier> BufferBarriers,
		std::span<const ImageBarrier> ImageBarriers) = 0;
	/** @brief Convenience wrapper recording only image barriers. @param Barriers Image transitions copied during the call. */
	virtual void imageBarriers(std::span<const ImageBarrier> Barriers)
	{
		barriers({}, {}, Barriers);
	}
	/** @brief Begins a dynamic-rendering scope. @param Info Attachments and render dimensions valid through the call. */
	virtual void beginRendering(const RenderingInfo& Info) = 0;
	/** @brief Ends the active dynamic-rendering scope. */
	virtual void endRendering() = 0;

	/** @brief Sets all active dynamic viewports. @param Viewports Nonempty viewport array within device limits. */
	virtual void setViewports(std::span<const Viewport> Viewports) = 0;
	/** @brief Sets all active dynamic scissors. @param Scissors Nonempty rectangles within device limits. */
	virtual void setScissors(std::span<const RenderArea> Scissors) = 0;
	/** @brief Sets the dynamic blend constant. @param Constants RGBA values. */
	virtual void setBlendConstants(const std::array<float, 4>& Constants) = 0;
	/** @brief Sets front and back dynamic stencil references. @param FrontReference Front value. @param BackReference Back value. */
	virtual void setStencilReference(uint32_t FrontReference, uint32_t BackReference) = 0;
	/** @brief Sets dynamic depth bias. @param ConstantFactor Constant term. @param Clamp Absolute clamp. @param SlopeFactor slope term. */
	virtual void setDepthBias(float ConstantFactor, float Clamp, float SlopeFactor) = 0;
	/** @brief Sets dynamic line width. @param Width Positive width supported by device limits/features. */
	virtual void setLineWidth(float Width) = 0;
	/** @brief Dynamically sets culling mode. @param Mode New mode. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setCullMode(ECullMode Mode) { (void)Mode; return false; }
	/** @brief Dynamically sets front-face winding. @param Face New winding. @return True when recorded; false without ExtendedDynamicState or on invalid state. */
	[[nodiscard]] virtual bool setFrontFace(EFrontFace Face) { (void)Face; return false; }
	/** @brief Dynamically sets primitive topology. @param Topology New topology compatible with the pipeline topology class. @return True when recorded; false when unsupported or incompatible. */
	[[nodiscard]] virtual bool setPrimitiveTopology(EPrimitiveTopology Topology) { (void)Topology; return false; }
	/** @brief Dynamically enables or disables depth testing. @param Enable Requested state. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setDepthTestEnable(bool Enable) { (void)Enable; return false; }
	/** @brief Dynamically enables or disables depth writes. @param Enable Requested state. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setDepthWriteEnable(bool Enable) { (void)Enable; return false; }
	/** @brief Dynamically selects the depth comparison. @param Operation Comparison operation. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setDepthCompareOp(ECompareOp Operation) { (void)Operation; return false; }
	/** @brief Dynamically enables or disables stencil testing. @param Enable Requested state. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setStencilTestEnable(bool Enable) { (void)Enable; return false; }
	/** @brief Dynamically sets front/back stencil operations. @param Front Front-face state. @param Back Back-face state. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setStencilOperations(const StencilFaceState& Front, const StencilFaceState& Back) { (void)Front; (void)Back; return false; }
	/** @brief Dynamically sets stencil compare masks. @param FrontMask Front-face mask. @param BackMask Back-face mask. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setStencilCompareMask(uint32_t FrontMask, uint32_t BackMask) { (void)FrontMask; (void)BackMask; return false; }
	/** @brief Dynamically sets stencil write masks. @param FrontMask Front-face mask. @param BackMask Back-face mask. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool setStencilWriteMask(uint32_t FrontMask, uint32_t BackMask) { (void)FrontMask; (void)BackMask; return false; }
	/** @brief Dynamically supplies vertex bindings and attributes. @param State Complete vertex layout copied during the call. @return True when recorded; false without DynamicVertexInput, without dynamic VertexInput, or on invalid data. */
	[[nodiscard]] virtual bool setVertexInput(const VertexInputState& State) { (void)State; return false; }
	/** @brief Updates a declared push-constant range. @param Layout Bound-compatible layout. @param Stages Receiving stages. @param Offset Byte offset. @param Data Bytes copied during the call. */
	virtual void pushConstants(
		const std::shared_ptr<RPipelineLayout>& Layout,
		EShaderStage Stages,
		uint32_t Offset,
		std::span<const std::byte> Data) = 0;
	/** @brief Binds an executable pipeline. @param Pipeline Valid pipeline matching subsequent commands. */
	virtual void bindPipeline(const std::shared_ptr<RPipeline>& Pipeline) = 0;
	/** @brief Binds resource groups and dynamic offsets. @param PipelineType Bind point. @param Layout Compatible layout. @param FirstGroup First group slot. @param Groups Groups to bind. @param DynamicOffsets Flattened offsets in layout order. */
	virtual void bindBindGroups(
		EPipelineType PipelineType,
		const std::shared_ptr<RPipelineLayout>& Layout,
		uint32_t FirstGroup,
		std::span<const std::shared_ptr<RBindGroup>> Groups,
		std::span<const uint32_t> DynamicOffsets = {}) = 0;
	/** @brief Binds contiguous vertex buffer slots. @param FirstBinding First slot. @param Bindings Buffer ranges copied during the call. */
	virtual void bindVertexBuffers(
		uint32_t FirstBinding,
		std::span<const VertexBufferBinding> Bindings) = 0;
	/** @brief Binds indexed-draw input. @param Buffer Index-usage buffer. @param Offset Aligned byte offset. @param Format Element width. */
	virtual void bindIndexBuffer(
		const std::shared_ptr<RBuffer>& Buffer,
		DeviceSizeType Offset,
		EIndexFormat Format) = 0;

	/** @brief Copies non-overlapping buffer ranges. @param Source Transfer source. @param Destination Transfer destination. @param Regions Copy regions. */
	virtual void copyBuffer(
		const std::shared_ptr<RBuffer>& Source,
		const std::shared_ptr<RBuffer>& Destination,
		std::span<const BufferCopyRegion> Regions) = 0;
	/** @brief Copies linear buffer data into image subresources. @param Source Transfer source. @param Destination Transfer destination. @param Regions Layout/subresource mappings. */
	virtual void copyBufferToImage(
		const std::shared_ptr<RBuffer>& Source,
		const std::shared_ptr<RImage>& Destination,
		std::span<const BufferImageCopyRegion> Regions) = 0;
	/** @brief Copies image subresources into linear buffer data. @param Source Transfer source. @param Destination Transfer destination. @param Regions Layout/subresource mappings. */
	virtual void copyImageToBuffer(
		const std::shared_ptr<RImage>& Source,
		const std::shared_ptr<RBuffer>& Destination,
		std::span<const BufferImageCopyRegion> Regions) = 0;
	/** @brief Copies equal-format-compatible image regions. @param Source Source image. @param Destination Destination image. @param Regions Regions. */
	virtual void copyImage(
		const std::shared_ptr<RImage>& Source,
		const std::shared_ptr<RImage>& Destination,
		std::span<const ImageCopyRegion> Regions) = 0;
	/** @brief Scales or filters image regions. @param Source Source image. @param Destination Destination image. @param Regions Source/destination boxes. @param Filter Sampling filter. */
	virtual void blitImage(
		const std::shared_ptr<RImage>& Source,
		const std::shared_ptr<RImage>& Destination,
		std::span<const ImageBlitRegion> Regions,
		EFilterMode Filter = EFilterMode::Linear) = 0;
	/** @brief Fills a buffer byte range with a repeated 32-bit value. @param Buffer Destination. @param Offset Aligned offset. @param Size Byte count, where zero means remaining range if backend supports it. @param Value Fill pattern. */
	virtual void fillBuffer(
		const std::shared_ptr<RBuffer>& Buffer,
		DeviceSizeType Offset,
		DeviceSizeType Size,
		uint32_t Value) = 0;

	/** @brief Records a non-indexed draw. @param VertexCount Vertices per instance. @param InstanceCount Instance count. @param FirstVertex First vertex. @param FirstInstance First instance. */
	virtual void draw(
		uint32_t VertexCount,
		uint32_t InstanceCount = 1,
		uint32_t FirstVertex = 0,
		uint32_t FirstInstance = 0) = 0;
	/** @brief Records an indexed draw. @param IndexCount Indices per instance. @param InstanceCount Instance count. @param FirstIndex First index. @param VertexOffset Signed base vertex. @param FirstInstance First instance. */
	virtual void drawIndexed(
		uint32_t IndexCount,
		uint32_t InstanceCount = 1,
		uint32_t FirstIndex = 0,
		int32_t VertexOffset = 0,
		uint32_t FirstInstance = 0) = 0;
	/** @brief Dispatches a mesh/task shader grid. @param GroupCountX X groups. @param GroupCountY Y groups. @param GroupCountZ Z groups. Requires MeshShader. */
	virtual void drawMeshTasks(
		uint32_t GroupCountX,
		uint32_t GroupCountY,
		uint32_t GroupCountZ) = 0;
	/** @brief Dispatches a compute grid. @param GroupCountX X groups. @param GroupCountY Y groups. @param GroupCountZ Z groups. */
	virtual void dispatch(
		uint32_t GroupCountX,
		uint32_t GroupCountY,
		uint32_t GroupCountZ) = 0;
	/** @brief Records indirect non-indexed draws. @param Buffer Indirect-usage argument buffer. @param Offset First argument. @param DrawCount Record count. @param Stride Record stride. */
	virtual void drawIndirect(
		const std::shared_ptr<RBuffer>& Buffer,
		DeviceSizeType Offset,
		uint32_t DrawCount,
		uint32_t Stride) = 0;
	/** @brief Records indirect indexed draws. @param Buffer Indirect-usage argument buffer. @param Offset First argument. @param DrawCount Record count. @param Stride Record stride. */
	virtual void drawIndexedIndirect(
		const std::shared_ptr<RBuffer>& Buffer,
		DeviceSizeType Offset,
		uint32_t DrawCount,
		uint32_t Stride) = 0;
	/** @brief Records an indirect compute dispatch. @param Buffer Indirect-usage argument buffer. @param Offset Dispatch record offset. */
	virtual void dispatchIndirect(
		const std::shared_ptr<RBuffer>& Buffer,
		DeviceSizeType Offset) = 0;

	/** @brief Makes query slots reusable. @param Pool Query pool. @param First First slot. @param Count Slot count. */
	virtual void resetQueries(const std::shared_ptr<RQueryPool>& Pool, uint32_t First, uint32_t Count) = 0;
	/** @brief Begins an occlusion or pipeline-statistics query. @param Pool Compatible pool. @param Query Slot index. */
	virtual void beginQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) = 0;
	/** @brief Ends an active query. @param Pool Pool passed to beginQuery(). @param Query Same slot index. */
	virtual void endQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) = 0;
	/** @brief Writes a device timestamp into one slot. @param Pool Timestamp pool. @param Query Slot index. Raw values convert with DeviceLimits::TimestampPeriodNanoseconds. */
	virtual void writeTimestamp(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) = 0;
	/** @brief Copies query results into a buffer using explicit result policy. @param Pool Source pool. @param First First query. @param Count Number of queries. @param Destination Transfer destination buffer. @param Offset Destination byte offset. @param Stride Bytes between query records. @param Flags Width/availability/wait/partial policy. @return True when recorded; false when unsupported or invalid. */
	[[nodiscard]] virtual bool copyQueryResults(const std::shared_ptr<RQueryPool>& Pool, uint32_t First, uint32_t Count,
		const std::shared_ptr<RBuffer>& Destination, DeviceSizeType Offset, DeviceSizeType Stride,
		EQueryResultFlags Flags)
	{
		(void)Pool; (void)First; (void)Count; (void)Destination; (void)Offset; (void)Stride; (void)Flags; return false;
	}
	/** @brief Records one or more acceleration-structure builds. @param Builds Build requests whose spans are consumed during the call. @return True when recorded; false without AccelerationStructure or on invalid ranges. */
	[[nodiscard]] virtual bool buildAccelerationStructures(std::span<const AccelerationStructureBuildDescriptor> Builds) { (void)Builds; return false; }
	/** @brief Dispatches rays using the currently bound ray-tracing pipeline. @param Desc SBT regions and dispatch dimensions satisfying DeviceLimits alignment. @return True when recorded; false without RayTracingPipeline or on invalid state. */
	[[nodiscard]] virtual bool traceRays(const TraceRaysDescriptor& Desc) { (void)Desc; return false; }
	/** @brief Begins a nested diagnostic region. @param Name Label copied during the call. @param Color Optional RGBA debugger color. */
	virtual void beginDebugLabel(std::string_view Name, const std::array<float, 4>& Color = {}) = 0;
	/** @brief Ends the innermost diagnostic region. */
	virtual void endDebugLabel() = 0;
	/** @brief Inserts a diagnostic marker. @param Name Label copied during the call. @param Color Optional RGBA debugger color. */
	virtual void insertDebugLabel(std::string_view Name, const std::array<float, 4>& Color = {}) = 0;
	/** @brief Executes secondary command lists from a primary list. @param CommandLists Executable secondary lists with inheritance compatible with the active rendering scope. */
	virtual void executeSecondary(std::span<const std::shared_ptr<RCommandList>> CommandLists) = 0;

protected:
	RCommandList() = default;
};

/** @brief Static HDR display metadata; values use display-referred normalized chromaticity and nits. */
struct HDRMetadata
{
	std::array<float, 2> DisplayPrimaryRed {};
	std::array<float, 2> DisplayPrimaryGreen {};
	std::array<float, 2> DisplayPrimaryBlue {};
	std::array<float, 2> WhitePoint {};
	float MaxLuminanceNits { 0.0f };
	float MinLuminanceNits { 0.0f };
	float MaxContentLightLevelNits { 0.0f };
	float MaxFrameAverageLightLevelNits { 0.0f };
};

/** @brief One surface format/color-space pair that may be selected together. */
struct SurfaceFormat
{
	EFormat Format { EFormat::Undefined };
	EColorSpace ColorSpace { EColorSpace::SRGB_Nonlinear };
};

/** @brief Presentation surface limits and selectable format/mode combinations. */
struct SwapchainCapabilities
{
	uint32_t MinimumImageCount { 0 };
	uint32_t MaximumImageCount { 0 };
	uint32_t MinimumWidth { 0 };
	uint32_t MinimumHeight { 0 };
	uint32_t MaximumWidth { 0 };
	uint32_t MaximumHeight { 0 };
	std::vector<SurfaceFormat> Formats;
	std::vector<EPresentMode> PresentModes;
	bool SupportsHDRMetadata { false };
};

/** @brief Immutable swapchain preferences; actual selected values are queried from RSwapchain. */
struct SwapchainDescriptor
{
	uint32_t Width { 1 };
	uint32_t Height { 1 };
	EFormat PreferredFormat { EFormat::BGRA8_UNorm };
	EColorSpace ColorSpace { EColorSpace::SRGB_Nonlinear };
	EPresentMode PreferredPresentMode { EPresentMode::Mailbox };
	EImageUsage ImageUsage { EImageUsage_t::Target };
	uint32_t MinimumImageCount { 2 };
	bool Clipped { true };
	/** @brief When true, creation fails instead of falling back from the requested format/color space. */
	bool RequireExactFormatAndColorSpace { false };
	std::optional<HDRMetadata> HDR;
	std::string DebugName;
};

/** @brief Image index plus recoverable acquisition status. */
struct AcquireResult
{
	EAcquireStatus Status { EAcquireStatus::NotReady };
	uint32_t ImageIndex { 0 };
	/** @brief Swapchain generation owning ImageIndex; zero is invalid. */
	uint64_t Generation { 0 };
};

/** @brief 可重建的呈现图像集合；提交和呈现由 RQueue 负责.  */
class RSwapchain
{
public:
	virtual ~RSwapchain() = default;
	/** @brief Returns the owning device. @return Device that created the swapchain. */
	[[nodiscard]] virtual RDevice& getDevice() const noexcept = 0;
	/** @brief Returns requested creation preferences. @return Immutable descriptor; selected format may differ. */
	[[nodiscard]] virtual const SwapchainDescriptor& getDescriptor() const noexcept = 0;
	/** @brief Returns the selected surface format. @return Actual image format. */
	[[nodiscard]] virtual EFormat getFormat() const noexcept = 0;
	/** @brief Returns the selected presentation color space. @return Actual color space; default reports the requested preference. */
	[[nodiscard]] virtual EColorSpace getColorSpace() const noexcept { return getDescriptor().ColorSpace; }
	/** @brief Reports persistent swapchain health. @return Recovery-relevant state; default Ready preserves legacy backends. */
	[[nodiscard]] virtual ESwapchainStatus getStatus() const noexcept { return ESwapchainStatus::Ready; }
	/** @brief Returns the current image generation identity. @return Nonzero value after successful creation; changes whenever images are rebuilt. */
	[[nodiscard]] virtual uint64_t getGeneration() const noexcept { return 0; }
	/** @brief Returns the actual image count. @return Number of addressable swapchain images. */
	[[nodiscard]] virtual uint32_t getImageCount() const noexcept = 0;
	/** @brief Returns one presentation image. @param Index Index less than getImageCount(). @return Stable image wrapper until recreation. */
	[[nodiscard]] virtual const std::shared_ptr<RImage>& getImage(uint32_t Index) const = 0;
	/** @brief Returns the default view for one presentation image. @param Index Index less than getImageCount(). @return Stable view until recreation. */
	[[nodiscard]] virtual const std::shared_ptr<RImageView>& getImageView(uint32_t Index) const = 0;
	/** @brief Acquires an image and signals synchronization on success/suboptimal. @param SignalSemaphore Binary semaphore to signal, or null if a fence is supplied. @param SignalFence Optional fence. @param TimeoutNanoseconds Maximum wait. @return Status and valid image index only for Success/Suboptimal. */
	[[nodiscard]] virtual AcquireResult acquireNextImage(
		const std::shared_ptr<RSemaphore>& SignalSemaphore,
		const std::shared_ptr<RFence>& SignalFence = {},
		uint64_t TimeoutNanoseconds = std::numeric_limits<uint64_t>::max()) = 0;
	/** @brief Rebuilds presentation resources after resize or OutOfDate. @param Width New nonzero width. @param Height New nonzero height. SurfaceLost requires application surface recreation before this call; DeviceLost requires device recreation. */
	virtual void recreate(uint32_t Width, uint32_t Height) = 0;
	/** @brief Rebinds this swapchain to the surface generation recreated by IRHI::recoverSurface(). @return True on success. This does not rebuild any non-presentation GPU resource. */
	[[nodiscard]] virtual bool recoverSurface(uint32_t Width, uint32_t Height) { (void)Width; (void)Height; return false; }
	/** @brief Updates HDR metadata without recreating images when supported. @param Metadata New display metadata. @return True when accepted; false when HDRMetadata is unsupported or the surface mode is not HDR. */
	[[nodiscard]] virtual bool setHDRMetadata(const HDRMetadata& Metadata) { (void)Metadata; return false; }
	/** @brief Returns the non-owning backend swapchain handle. @return Handle, or nullptr when surface resources are unavailable. */
	[[nodiscard]] virtual void* getNativeHandle() const noexcept = 0;
};

/** @brief Logical GPU device, capability authority, and factory for all child RHI objects. */
class RDevice
{
public:
	/** @brief Constructs the backend-neutral logical-device base. */
	RDevice() = default;
	/** @brief Destroys the device interface after all child objects have released ownership. */
	virtual ~RDevice() = default;

	/** 
     * @brief Creates a buffer. 
     * @param Desc Valid immutable descriptor. 
     * @return Resource, or nullptr when unsupported, invalid, out of memory, or device-lost. 
     */
	virtual std::shared_ptr<RBuffer> createBuffer(const RBuffer::Descriptor_t& Desc) = 0;
	
    /** 
     * @brief Creates an image. 
     * @param Desc Immutable dimensions, format, usage, and memory policy. 
     * @return Resource, or nullptr on invalid/unsupported/out-of-memory/device-lost. 
     */
	virtual std::shared_ptr<RImage> createImage(const RImage::Descriptor_t& Desc) = 0;
	
    /** 
     * @brief Creates an image subresource view. 
     * @param Desc Image, interpretation, and range. 
     * @return View, or nullptr when incompatible. 
     */
    virtual std::shared_ptr<RImageView> createImageView(const RImageView::Descriptor_t& Desc) = 0;
	
    /** 
     * @brief Creates immutable sampling state. 
     * @param Desc Filtering/addressing policy. 
     * @return Sampler, or nullptr when unsupported. 
     */
	virtual std::shared_ptr<RSampler> createSampler(const RSampler::Descriptor_t& Desc = {}) = 0;
	
    /** 
     * @brief Creates a compiled shader module. 
     * @param Desc Bytecode, stage, entry point, and identity. 
     * @return Shader, or nullptr on validation/backend failure. 
     */
	virtual std::shared_ptr<RShader> createShader(const ShaderDescriptor& Desc) = 0;
	
    /** 
     * @brief Creates a bind-group ABI declaration. 
     * @param Desc Entries to normalize and validate. 
     * @return Layout, or nullptr on invalid/unsupported declarations. 
     */
	virtual std::shared_ptr<RBindGroupLayout> createBindGroupLayout(
		const BindGroupLayoutDescriptor& Desc) = 0;
    /** 
     * @brief Creates an immutable populated bind group. 
     * @param Desc Layout and complete resource writes. 
     * @return Group, or nullptr on incompatibility. 
     */
	virtual std::shared_ptr<RBindGroup> createBindGroup(
		const BindGroupDescriptor& Desc) = 0;
    /** 
     * @brief Creates a pipeline resource ABI. 
     * @param Desc Ordered group layouts and push-constant ranges. 
     * @return Layout, or nullptr on limit/overlap failure. 
     */
	virtual std::shared_ptr<RPipelineLayout> createPipelineLayout(
		const PipelineLayoutDescriptor& Desc) = 0;
    /** 
     * @brief Creates a reusable backend pipeline cache. 
     * @param Desc Optional compatible initial bytes. 
     * @return Cache, or nullptr when initial data is rejected. 
     */
	virtual std::shared_ptr<RPipelineCache> createPipelineCache(
		const PipelineCacheDescriptor& Desc = {}) = 0;
    /** 
     * @brief Creates a graphics pipeline. 
     * @param Desc Complete static/dynamic state and rendering signature. 
     * @return Pipeline, or nullptr when compilation/validation fails. 
     */
	virtual std::shared_ptr<RPipeline> createGraphicsPipeline(
		const GraphicsPipelineDescriptor& Desc) = 0;
    /** 
     * @brief Creates a compute pipeline. 
     * @param Desc Compute stage, layout, and compile policy. 
     * @return Pipeline, or nullptr when compilation/validation fails. 
     */
	virtual std::shared_ptr<RPipeline> createComputePipeline(
		const ComputePipelineDescriptor& Desc) = 0;
    /** 
     * @brief Creates an optional ray-tracing pipeline. 
     * @param Desc Stages, groups, layout, and recursion depth. 
     * @return Pipeline, or nullptr unless RayTracingPipeline is supported and all limits are met. 
     */
	virtual std::shared_ptr<RPipeline> createRayTracingPipeline(const RayTracingPipelineDescriptor& Desc) { (void)Desc; return {}; }
    /** 
     * @brief Creates acceleration-structure storage interpretation. 
     * @param Desc Type and valid aligned storage range. 
     * @return Wrapper, or nullptr unless AccelerationStructure is supported. 
     */
	virtual std::shared_ptr<RAccelerationStructure> createAccelerationStructure(const AccelerationStructureDescriptor& Desc) { (void)Desc; return {}; }
    /** 
     * @brief Queries allocation and scratch sizes for acceleration-structure geometry. 
     * @param Type Bottom- or top-level target. 
     * @param Flags Build policy. 
     * @param Geometries Geometry descriptions and maximum primitive counts. 
     * @return Nonzero requirements when supported/valid; all-zero otherwise. 
     */
	[[nodiscard]] virtual AccelerationStructureBuildSizes getAccelerationStructureBuildSizes(
		EAccelerationStructureType Type,
		EAccelerationStructureBuildFlags Flags,
		std::span<const AccelerationStructureGeometry> Geometries) const
	{
		(void)Type; (void)Flags; (void)Geometries; return {};
	}
    /** 
     * @brief Retrieves opaque shader-group handles for SBT construction. 
     * @param Pipeline Valid ray-tracing pipeline. 
     * @param FirstGroup First group. 
     * @param GroupCount Number of groups. 
     * @return Packed handles using DeviceLimits::ShaderGroupHandleSize, or empty when unsupported/invalid. 
     */
	[[nodiscard]] virtual std::vector<std::byte> getRayTracingShaderGroupHandles(
		const std::shared_ptr<RPipeline>& Pipeline, uint32_t FirstGroup, uint32_t GroupCount) const
	{
		(void)Pipeline; (void)FirstGroup; (void)GroupCount; return {};
	}
    /** 
     * @brief Creates a command list. 
     * @param Desc Queue class, level, reuse policy, and optional rendering inheritance. 
     * @return List, or nullptr when secondary/inheritance features are unsupported. 
     */
	virtual std::shared_ptr<RCommandList> createCommandList(
		const CommandListDescriptor& Desc = {}) = 0;
    /** 
     * @brief Creates presentation resources for the initialized surface. 
     * @param Desc Format/mode/HDR preferences and dimensions. 
     * @return Swapchain, or nullptr for unavailable/lost surface or unsupported requirements. 
     */
	virtual std::shared_ptr<RSwapchain> createSwapchain(const SwapchainDescriptor& Desc) = 0;
    /** 
     * @brief Retrieves a device-owned queue. 
     * @param Type Required queue capability. 
     * @return Queue, or nullptr when unavailable. 
     */
	virtual std::shared_ptr<RQueue> getQueue(ECommandQueueType Type) = 0;
    /** 
     * @brief Creates a CPU-waitable fence. 
     * @param Signaled Initial state. 
     * @return Fence, or nullptr on failure. 
     */
	virtual std::shared_ptr<RFence> createFence(bool Signaled = false) = 0;
	/** 
     * @brief Creates a binary GPU semaphore. 
     * @return Semaphore, or nullptr on failure. 
     */
	virtual std::shared_ptr<RSemaphore> createSemaphore() = 0;
	/** 
     * @brief Creates a monotonic timeline semaphore. 
     * @param InitialValue Initial completed value. 
     * @return Semaphore, or nullptr unless TimelineSemaphore is supported. 
     */
	virtual std::shared_ptr<RSemaphore> createTimelineSemaphore(uint64_t InitialValue = 0) = 0;
	/** 
     * @brief Creates fixed query storage. 
     * @param Desc Type/count/statistics mask. 
     * @return Pool, or nullptr unless the selected query capability is supported. 
     */
	virtual std::shared_ptr<RQueryPool> createQueryPool(const QueryPoolDescriptor& Desc) = 0;
	/** 
     * @brief Queries format operations. 
     * @param Format Format to inspect. 
     * @return Empty masks for undefined/unsupported formats. 
     */
	[[nodiscard]] virtual FormatCapabilities getFormatCapabilities(EFormat Format) const = 0;
	/** 
     * @brief Queries current presentation surface capabilities. 
     * @return Supported limits/formats/modes, or std::nullopt when surface status reporting is unavailable or the surface is lost. 
     */
	[[nodiscard]] virtual std::optional<SwapchainCapabilities> getSwapchainCapabilities() const { return std::nullopt; }
	/** 
     * @brief Creates a high-level sampled texture combining an image, a default full-range view and a default sampler. 
     * @param Desc Image storage and default sampling policy; an empty image usage defaults to Sampled|TransferDst. 
     * @return Texture, or nullptr when unsupported/invalid/out-of-memory/device-lost. 
     */
	virtual std::shared_ptr<RTexture> createTexture(const RTexture::Descriptor_t& Desc) = 0;
	/** 
     * @brief Allocates legacy device memory. 
     * @param Requirements Size/alignment/type constraints. 
     * @param Property Required memory properties. 
     * @return Allocation, or nullptr on failure. 
     */
	virtual std::shared_ptr<DeviceMemory> allocateMemory(
		MemoryRequirements Requirements,
		EMemoryProperty Property) = 0;
	/** 
     * @brief Allocates memory with required/preferred properties and modern metadata. 
     * @param Desc Allocation constraints and policy. 
     * @return Allocation, or nullptr if requirements cannot be satisfied; default bridges to the legacy allocator. 
     */
	virtual std::shared_ptr<DeviceMemory> allocateMemory(const MemoryAllocationDescriptor& Desc)
	{
		return allocateMemory(Desc.Requirements, Desc.RequiredProperties);
	}
	/** 
     * @brief Releases an allocation after the caller has externally proven GPU idleness. 
     * @param Memory Allocation to release; null is a no-op. 
     */
	virtual void freeMemory(std::shared_ptr<DeviceMemory> Memory) = 0;
	/** 
     * @brief Defers the final shared ownership release until a timeline value completes. 
     * @param Resource Type-erased RHI object ownership. 
     * @param CompletionSemaphore Timeline semaphore from this device. 
     * @param CompletionValue Value proving all resource use complete. 
     * @return True when queued; false when unsupported/invalid, in which case caller retains ownership. 
     */
	[[nodiscard]] virtual bool deferRelease(
		std::shared_ptr<void> Resource,
		const std::shared_ptr<RSemaphore>& CompletionSemaphore,
		uint64_t CompletionValue)
	{
		(void)Resource; (void)CompletionSemaphore; (void)CompletionValue; return false;
	}
	/** 
     * @brief Polls and destroys deferred objects whose completion points have passed; never waits. 
     */
	virtual void collectDeferredReleases() {}

	/** 
     * @brief Blocks until all device queues are idle or device loss is observed. 
     */
	virtual void waitIdle() = 0;
	
    /** 
     * @brief Returns the non-owning backend device handle. 
     * @return Handle, or nullptr after loss. 
     */
	virtual void* getNativeHandle() const = 0;
	
    /** 
     * @brief Returns immutable numeric limits. 
     * @return Limits valid for this device lifetime. 
     */
	[[nodiscard]] virtual const DeviceLimits& getLimits() const noexcept = 0;
	/** 
     * @brief Returns immutable optional capabilities. 
     * @return Features valid for this device lifetime. 
     */
	[[nodiscard]] virtual const DeviceFeatures& getFeatures() const noexcept = 0;
	/** 
     * @brief Reports device health. 
     * @return Ready or terminal loss/removal/reset state; default Ready preserves legacy backends. 
     */
	[[nodiscard]] virtual EDeviceStatus getStatus() const noexcept { return EDeviceStatus::Ready; }

protected:
	class IRHI* OnwerRHI = nullptr; 
};

/** @brief Backend instance responsible for surface initialization and logical-device creation. */
class IRHI
{
public:
	/** @brief Constructs an uninitialized backend instance. */
	IRHI() = default;
	/** @brief Destroys backend instance state after dependent devices are released. */
	virtual ~IRHI() = default;
	
	/** @brief Initializes backend instance and presentation surface state. @param Window Native window abstraction that must outlive surface-dependent objects. */
	virtual void initialize(const ui::GenericWindowPointer& Window) = 0;
	/** @brief Reports successful initialization. @return True when device creation may be attempted. */
	virtual bool isInitialized() const noexcept = 0;
	/** @brief Identifies the active backend. @return Backend API selected by this instance. */
	virtual ESupportedBackendAPI getBackendAPI() const = 0;
	/** @brief Creates a logical device. @return Device, or nullptr when initialization/capability negotiation fails. */
	virtual std::shared_ptr<RDevice> createDevice() = 0;
	/** @brief Recreates backend surface state after SurfaceLost. @param Window Current native window. @return True when swapchains may explicitly bind the new generation; false on failure/device loss. This never rebuilds device resources. */
	[[nodiscard]] virtual bool recoverSurface(const ui::GenericWindowPointer& Window) { (void)Window; return false; }
};
}
