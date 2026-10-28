#include <renderer/RenderPassScheduler.hpp>

#include <renderer/UIShaders.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace runtime::renderer
{

// ============================================================================
//  SceneRenderer(编排器)
// ============================================================================

void RenderPassScheduler::addPass(std::unique_ptr<IRenderPass> Pass)
{
    if (Pass)
        Passes.push_back(std::move(Pass));
}

void RenderPassScheduler::clear()
{
    Passes.clear();
}

void RenderPassScheduler::build(rdg::RDGGraph& Graph, const RenderContext& BaseContext)
{
    // 复制上下文(引用成员指向同一对象), 让子渲染器可通过 FrameResources 填充/读取共享句柄.
    RenderContext Context = BaseContext;
    for (auto& Pass : Passes)
        Pass->addPasses(Graph, Context);
}

// ============================================================================
//  录制工具: 把已解析的 DrawPacket / UI 批次翻译成 RHI 绘制命令
// ============================================================================

namespace
{

/** @brief 构造全屏尺寸的深度纹理描述(供子渲染器复用). */
[[nodiscard]] rdg::TextureDesc makeDepthDesc(uint32_t width, uint32_t height)
{
    rdg::TextureDesc Desc;
    Desc.Format = rhi::EFormat::D32_Float;
    Desc.Dimension = rhi::EImageDimension::Texture2D;
    Desc.Width = width;
    Desc.Height = height;
    Desc.Depth = 1;
    Desc.MipLevels = 1;
    Desc.ArrayLayers = 1;
    Desc.SharingMode = rhi::ESharingMode::Exclusive;
    Desc.MemoryProperty = rhi::EMemoryProperty::DeviceLocal;
    Desc.Usage = rhi::EImageUsage_t::DepthStencil;
    Desc.SampleCount = rhi::ESampleCount::Count1;
    return Desc;
}

/** @brief 构造 HDR 场景颜色纹理描述(RGBA16F, 可作为渲染目标并被后处理采样). */
[[nodiscard]] rdg::TextureDesc makeColorDesc(uint32_t width, uint32_t height)
{
    rdg::TextureDesc Desc;
    Desc.Format = rhi::EFormat::RGBA16_Float;
    Desc.Dimension = rhi::EImageDimension::Texture2D;
    Desc.Width = width;
    Desc.Height = height;
    Desc.Depth = 1;
    Desc.MipLevels = 1;
    Desc.ArrayLayers = 1;
    Desc.SharingMode = rhi::ESharingMode::Exclusive;
    Desc.MemoryProperty = rhi::EMemoryProperty::DeviceLocal;
    Desc.Usage = rhi::EImageUsage_t::Target | rhi::EImageUsage_t::Sampled;
    Desc.SampleCount = rhi::ESampleCount::Count1;
    return Desc;
}

/** @brief 阴影图集深度描述: 既是深度附件, 又可被片元着色器采样. */
[[nodiscard]] rdg::TextureDesc makeShadowDepthDesc(uint32_t width, uint32_t height)
{
    rdg::TextureDesc Desc = makeDepthDesc(width, height);
    Desc.Usage = rhi::EImageUsage_t::DepthStencil | rhi::EImageUsage_t::Sampled;
    return Desc;
}

/** @brief 设置覆盖整个渲染目标的全尺寸视口与裁剪矩形. */
void setFullViewport(rhi::RCommandList& command_list, uint32_t width, uint32_t height)
{
    const std::array viewports { rhi::Viewport {
        .Width = static_cast<float>(width),
        .Height = static_cast<float>(height),
    } };
    const std::array scissors { rhi::RenderArea {
        .Width = width,
        .Height = height,
    } };
    command_list.setViewports(viewports);
    command_list.setScissors(scissors);
}

/**
 * @brief 把视口与裁剪矩形限制到给定像素矩形内(编辑器视口面板).
 *
 * 3D 场景只允许在这个矩形里光栅化, 于是"渲染进编辑器里的 Viewport"不需要离屏
 * 目标: 直接写交换链, 由 UI 在四周画出面板外框即可.
 */
void setRectViewport(rhi::RCommandList& command_list, const rhi::RenderArea& Rect)
{
    const std::array viewports { rhi::Viewport {
        .X = static_cast<float>(Rect.X),
        .Y = static_cast<float>(Rect.Y),
        .Width = static_cast<float>(Rect.Width),
        .Height = static_cast<float>(Rect.Height),
    } };
    const std::array scissors { Rect };
    command_list.setViewports(viewports);
    command_list.setScissors(scissors);
}

/**
 * @brief 取本帧 3D 场景的像素矩形; 未设置(宽高为 0)时返回整屏矩形.
 */
[[nodiscard]] rhi::RenderArea resolveSceneRect(const RenderFrameSubmission* Submission,
    uint32_t width, uint32_t height)
{
    if (Submission)
    {
        const rhi::RenderArea Rect = Submission->SceneViewport;
        if (Rect.Width != 0 && Rect.Height != 0)
        {
            // 编辑器把视口矩形写在窗口逻辑坐标空间, 而这里需要像素坐标:
            // 高 DPI 下按 逻辑/像素 比例换算, 与 UIRenderPass 保持同一映射.
            const double logical_width = Submission->UILogicalWidth
                ? Submission->UILogicalWidth : width;
            const double logical_height = Submission->UILogicalHeight
                ? Submission->UILogicalHeight : height;
            const double scale_x = static_cast<double>(width) / logical_width;
            const double scale_y = static_cast<double>(height) / logical_height;
            // 夹到目标范围内, 避免越界 scissor / viewport.
            const int32_t Left = static_cast<int32_t>(std::clamp(
                std::floor(static_cast<double>(Rect.X) * scale_x),
                0.0, static_cast<double>(width)));
            const int32_t Top = static_cast<int32_t>(std::clamp(
                std::floor(static_cast<double>(Rect.Y) * scale_y),
                0.0, static_cast<double>(height)));
            const int32_t Right = static_cast<int32_t>(std::clamp(
                std::ceil(static_cast<double>(Rect.X + Rect.Width) * scale_x),
                static_cast<double>(Left), static_cast<double>(width)));
            const int32_t Bottom = static_cast<int32_t>(std::clamp(
                std::ceil(static_cast<double>(Rect.Y + Rect.Height) * scale_y),
                static_cast<double>(Top), static_cast<double>(height)));
            if (Right > Left && Bottom > Top)
                return { Left, Top, static_cast<uint32_t>(Right - Left),
                    static_cast<uint32_t>(Bottom - Top) };
        }
    }
    return { 0, 0, width, height };
}

/** @brief 开始一个颜色(可选深度)渲染作用域, 并把视口限制到 SceneRect. */
void beginColorScope(
    rhi::RCommandList& command_list, uint32_t width, uint32_t height,
    const std::shared_ptr<rhi::RImageView>& color_view, rhi::ELoadOp load_op,
    const std::shared_ptr<rhi::RImageView>& depth_view = {},
    const rhi::RenderArea* scene_rect = nullptr)
{
    rhi::RenderingInfo info;
    // 渲染作用域始终覆盖整个目标: 清屏/加载语义作用于全图, 实际光栅化由 viewport 限制.
    info.Area = { 0, 0, width, height };

    rhi::ColorAttachment color;
    color.View = color_view;
    color.LoadOp = load_op;
    color.StoreOp = rhi::EStoreOp::Store;
    color.ClearValue = { .Type = rhi::EClearColorType::Float,
                         .Float32 = { 0.025f, 0.035f, 0.055f, 1.0f } };
    info.ColorAttachments = std::span(&color, 1);

    rhi::DepthStencilAttachment depth;
    if (depth_view)
    {
        depth.View = depth_view;
        depth.Depth.LoadOp = rhi::ELoadOp::Load;
        depth.Depth.StoreOp = rhi::EStoreOp::DontCare;
        depth.Stencil.LoadOp = rhi::ELoadOp::DontCare;
        depth.Stencil.StoreOp = rhi::EStoreOp::DontCare;
        depth.ClearValue = { .Depth = 1.0f, .Stencil = 0 };
        info.DepthStencil = &depth;
    }

    command_list.beginRendering(info);
    if (scene_rect)
        setRectViewport(command_list, *scene_rect);
    else
        setFullViewport(command_list, width, height);
}

/** @brief 开始一个深度-only 渲染作用域, 并把视口限制到 SceneRect. */
void beginDepthScope(
    rhi::RCommandList& command_list, uint32_t width, uint32_t height,
    const std::shared_ptr<rhi::RImageView>& depth_view,
    const rhi::RenderArea* scene_rect = nullptr)
{
    rhi::RenderingInfo info;
    info.Area = { 0, 0, width, height };

    rhi::DepthStencilAttachment depth;
    depth.View = depth_view;
    depth.Depth.LoadOp = rhi::ELoadOp::Clear;
    depth.Depth.StoreOp = rhi::EStoreOp::Store;
    depth.Stencil.LoadOp = rhi::ELoadOp::DontCare;
    depth.Stencil.StoreOp = rhi::EStoreOp::DontCare;
    depth.ClearValue = { .Depth = 1.0f, .Stencil = 0 };
    info.DepthStencil = &depth;

    command_list.beginRendering(info);
    if (scene_rect)
        setRectViewport(command_list, *scene_rect);
    else
        setFullViewport(command_list, width, height);
}

// ============================================================================
//  场景 PBR 网格材质(运行时从 SEED_SCENE_SHADER_DIR 加载 DXC 编译的 SPIR-V)
// ============================================================================

/** @brief 场景帧常量: 与 shaders/scene_mesh.frag.hlsl 的 FrameData cbuffer 对齐(128 字节). */
struct alignas(16) SceneFrameConstants
{
    float CameraPosition[4];
    float LightDirection[4];
    float LightColor[4];
    float ShadowMatrix[16];
    float ShadowParams[4];
};
static_assert(sizeof(SceneFrameConstants) == 128, "SceneFrameConstants must be 128 bytes.");

/** @brief 网格绘制的管线变体: 由所在 Pass 的渲染作用域决定. */
enum class ESceneMeshVariant
{
    /** 深度预通道 / 阴影: 只写深度, 无颜色附件, 无片元着色器. */
    DepthOnly,
    /** 不透明: 颜色 + 深度, 写深度. */
    Opaque,
    /** 透明: 颜色 + 深度, 不写深度, alpha 混合. */
    Transparent,
};

/** @brief 场景网格材质的跨帧缓存(管线 + 帧 UBO + 阴影采样器). */
struct SceneMeshCache
{
    std::shared_ptr<rhi::RPipelineLayout> Layout;
    std::shared_ptr<rhi::RBindGroupLayout> FrameLayout;
    std::shared_ptr<rhi::RPipeline> DepthOnly;
    std::shared_ptr<rhi::RPipeline> Opaque;
    std::shared_ptr<rhi::RPipeline> Transparent;
    std::shared_ptr<rhi::RBuffer> FrameUBO;
    std::byte* FrameUBOMapped { nullptr };
    uint32_t FrameUBOSlotSize { 0 };
    std::shared_ptr<rhi::RSampler> ShadowSampler;
    bool Ready { false };
};

SceneMeshCache& sceneMeshCache()
{
    static SceneMeshCache Cache;
    return Cache;
}

/** @brief 场景网格顶点: Position(float3) + Normal(float3) + Color(float4), stride 40. */
constexpr uint32_t SceneMeshVertexStride = 40;
/** @brief 场景网格材质的 push constant 字节数(MVP + World). */
constexpr uint32_t SceneMeshPushConstantSize = 128;
/** @brief 帧在飞数量, 与 RendererServer 的帧上下文一致. */
constexpr uint32_t SceneMeshFramesInFlight = 3;

/** @brief 从 SEED_SCENE_SHADER_DIR 加载一个 DXC 编译出的 SPIR-V 文件. */
[[nodiscard]] std::vector<std::byte> loadSceneShaderSpv(const char* Name)
{
    const std::string Path = std::string(SEED_SCENE_SHADER_DIR) + "/" + Name;
    std::ifstream Stream(Path, std::ios::binary | std::ios::ate);
    if (!Stream)
        throw std::runtime_error("Failed to open scene shader SPIR-V: " + Path);
    const std::streamsize Size = Stream.tellg();
    if (Size <= 0)
        throw std::runtime_error("Empty scene shader SPIR-V: " + Path);
    Stream.seekg(0, std::ios::beg);
    std::vector<std::byte> Bytes(static_cast<size_t>(Size));
    if (!Stream.read(reinterpret_cast<char*>(Bytes.data()), Size))
        throw std::runtime_error("Failed to read scene shader SPIR-V: " + Path);
    return Bytes;
}

[[nodiscard]] std::shared_ptr<rhi::RShader> createSceneShader(
    rhi::RDevice& Device, rhi::EShaderStage_t Stage, const char* FileName, const char* DebugName)
{
    rhi::ShaderDescriptor Desc;
    Desc.Stage = Stage;
    Desc.ByteCode = loadSceneShaderSpv(FileName);
    Desc.EntryPoint = "main";
    Desc.DebugName = DebugName;
    return Device.createShader(Desc);
}

/** @brief 惰性创建场景 PBR 网格材质的三条管线与帧资源(仅一次). */
void ensureSceneMeshCache(rhi::RDevice& device, SceneMeshCache& cache)
{
    if (cache.Ready)
        return;

    const auto vertex_shader = createSceneShader(device, rhi::EShaderStage_t::Vertex,
        "scene_mesh.vert.spv", "SceneMeshVertex");
    const auto pixel_shader = createSceneShader(device, rhi::EShaderStage_t::Pixel,
        "scene_mesh.frag.spv", "SceneMeshPixel");

    // Set 0(帧): b0 = FrameData UBO, t1 = 阴影贴图, s2 = 阴影比较采样器.
    rhi::BindGroupLayoutDescriptor frame_layout_desc;
    frame_layout_desc.Entries = {
        { .Binding = 0, .Type = rhi::EDescriptorType::UniformBuffer, .ArrayCount = 1,
          .Visibility = rhi::EShaderStage_t::Pixel },
        { .Binding = 1, .Type = rhi::EDescriptorType::SampledTexture, .ArrayCount = 1,
          .Visibility = rhi::EShaderStage_t::Pixel },
        { .Binding = 2, .Type = rhi::EDescriptorType::ComparisonSampler, .ArrayCount = 1,
          .Visibility = rhi::EShaderStage_t::Pixel },
    };
    frame_layout_desc.DebugName = "SceneFrameLayout";
    cache.FrameLayout = device.createBindGroupLayout(frame_layout_desc);

    rhi::PipelineLayoutDescriptor layout_desc;
    layout_desc.BindGroupLayouts = { cache.FrameLayout };
    layout_desc.PushConstantRanges = {
        { .Stages = rhi::EShaderStage_t::Vertex, .Offset = 0, .Size = SceneMeshPushConstantSize }
    };
    layout_desc.DebugName = "SceneMeshLayout";
    cache.Layout = device.createPipelineLayout(layout_desc);

    // 帧 UBO: 3 帧在飞, 每帧一个按设备对齐的槽位(持久映射, HostCoherent).
    const uint64_t Alignment =
        std::max<uint64_t>(device.getLimits().MinUniformBufferOffsetAlignment, 16);
    cache.FrameUBOSlotSize = static_cast<uint32_t>(
        ((sizeof(SceneFrameConstants) + Alignment - 1) / Alignment) * Alignment);
    cache.FrameUBO = device.createBuffer(rhi::RBuffer::Descriptor_t {
        .Size = static_cast<rhi::DeviceSizeType>(cache.FrameUBOSlotSize) * SceneMeshFramesInFlight,
        .Usage = rhi::EBufferUsage::Uniform,
        .MemoryUsage = rhi::EMemoryUsage::CPUToGPU,
        .MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent,
        .PersistentlyMapped = true,
        .DebugName = "SceneFrameUBO",
    });
    cache.FrameUBOMapped = static_cast<std::byte*>(cache.FrameUBO->map());

    // 阴影比较采样器.
    rhi::RSampler::Descriptor_t sampler_desc;
    sampler_desc.MinFilter = rhi::EFilterMode::Linear;
    sampler_desc.MagFilter = rhi::EFilterMode::Linear;
    sampler_desc.AddressU = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.AddressV = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.AddressW = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.CompareEnable = true;
    sampler_desc.CompareOperation = rhi::ECompareOp::LessEqual;
    sampler_desc.DebugName = "SceneShadowSampler";
    cache.ShadowSampler = device.createSampler(sampler_desc);

    const auto make_pipeline = [&](ESceneMeshVariant Variant, const char* Name)
    {
        rhi::GraphicsPipelineDescriptor desc;
        desc.Layout = cache.Layout;
        desc.Vertex.Shader = vertex_shader;
        // 深度-only 无片元着色器, 只写深度(阴影 / 预通道).
        if (Variant != ESceneMeshVariant::DepthOnly)
            desc.Pixel.Shader = pixel_shader;
        desc.VertexInput.Buffers = {
            { .Binding = 0, .Stride = SceneMeshVertexStride, .InputRate = rhi::EVertexInputRate::PerVertex }
        };
        desc.VertexInput.Attributes = {
            { .Location = 0, .Binding = 0, .Format = rhi::EVertexFormat::Float3, .Offset = 0 },
            { .Location = 1, .Binding = 0, .Format = rhi::EVertexFormat::Float3, .Offset = 12 },
            { .Location = 2, .Binding = 0, .Format = rhi::EVertexFormat::Float4, .Offset = 24 },
        };
        // 预览几何绕序可能不一致, 不做背面剔除; 由深度测试保证遮挡.
        desc.Rasterizer.CullMode = rhi::ECullMode::None;
        desc.DepthStencil.DepthTestEnable = true;
        desc.DepthStencil.DepthCompareOp = rhi::ECompareOp::LessEqual;
        // 深度预通道写深度, 不透明写深度, 透明只测不写.
        desc.DepthStencil.DepthWriteEnable = Variant != ESceneMeshVariant::Transparent;
        desc.Rendering.DepthFormat = rhi::EFormat::D32_Float;
        if (Variant != ESceneMeshVariant::DepthOnly)
        {
            desc.Rendering.ColorFormats[0] = rhi::EFormat::RGBA16_Float; // HDR 场景色
            desc.Rendering.ColorAttachmentCount = 1;
        }
        if (Variant == ESceneMeshVariant::Transparent)
        {
            desc.Blend.Attachments[0].BlendEnable = true;
            desc.Blend.Attachments[0].Color.SrcFactor = rhi::EBlendFactor::SrcAlpha;
            desc.Blend.Attachments[0].Color.DstFactor = rhi::EBlendFactor::OneMinusSrcAlpha;
            desc.Blend.Attachments[0].Color.Operation = rhi::EBlendOp::Add;
            desc.Blend.Attachments[0].Alpha.SrcFactor = rhi::EBlendFactor::One;
            desc.Blend.Attachments[0].Alpha.DstFactor = rhi::EBlendFactor::OneMinusSrcAlpha;
            desc.Blend.Attachments[0].Alpha.Operation = rhi::EBlendOp::Add;
        }
        desc.DebugName = Name;
        return device.createGraphicsPipeline(desc);
    };

    cache.DepthOnly = make_pipeline(ESceneMeshVariant::DepthOnly, "SceneMeshDepthOnly");
    cache.Opaque = make_pipeline(ESceneMeshVariant::Opaque, "SceneMeshOpaque");
    cache.Transparent = make_pipeline(ESceneMeshVariant::Transparent, "SceneMeshTransparent");
    cache.Ready = true;
}

/** @brief 上传本帧帧常量并创建帧绑定组(引用当前阴影图视图). */
[[nodiscard]] std::shared_ptr<rhi::RBindGroup> createSceneFrameBindGroup(
    rhi::RDevice& device, SceneMeshCache& cache, uint32_t frame_index,
    const std::shared_ptr<rhi::RImageView>& shadow_view, const SceneFrameConstants& constants)
{
    if (!cache.FrameUBOMapped || !shadow_view || !cache.ShadowSampler)
        return {};
    std::memcpy(cache.FrameUBOMapped + static_cast<size_t>(frame_index) * cache.FrameUBOSlotSize,
        &constants, sizeof(constants));
    const rhi::DeviceSizeType offset =
        static_cast<rhi::DeviceSizeType>(frame_index) * cache.FrameUBOSlotSize;
    return device.createBindGroup({
        .Layout = cache.FrameLayout,
        .Entries = {
            { .Binding = 0, .ArrayElement = 0,
              .Resource = rhi::BufferBinding { cache.FrameUBO, offset, sizeof(SceneFrameConstants) } },
            { .Binding = 1, .ArrayElement = 0,
              .Resource = rhi::TextureBinding { shadow_view, rhi::EDescriptorImageLayout::DepthStencilReadOnly } },
            { .Binding = 2, .ArrayElement = 0,
              .Resource = rhi::SamplerBinding { cache.ShadowSampler } },
        },
        .DebugName = "SceneFrameBindGroup",
    });
}

/** @brief 方向光正交阴影 VP(以 Center 为中心的正交包围盒, Vulkan [0,1] 深度). */
[[nodiscard]] infra::Matrix4x4 makeDirectionalShadowVP(
    const infra::Vec3f& LightDir, const infra::Vec3f& Center, float HalfExtent)
{
    const infra::Vec3f Forward = LightDir.normalized();
    const infra::Vec3f WorldUp = std::abs(Forward.y()) < 0.999f
        ? infra::Vec3f { 0.0f, 1.0f, 0.0f }
        : infra::Vec3f { 1.0f, 0.0f, 0.0f };
    const infra::Vec3f Right = infra::Vec3f::cross(WorldUp, Forward).normalized();
    const infra::Vec3f Up = infra::Vec3f::cross(Forward, Right);

    const infra::Vec3f Eye = Center - Forward * HalfExtent;
    infra::Matrix4x4 View = infra::Matrix4x4::identity();
    View[0] = { Right.x(), Right.y(), Right.z(), -Right.dot(Eye) };
    View[1] = { Up.x(), Up.y(), Up.z(), -Up.dot(Eye) };
    View[2] = { Forward.x(), Forward.y(), Forward.z(), -Forward.dot(Eye) };
    View[3] = { 0.0f, 0.0f, 0.0f, 1.0f };

    const float n = 0.0f;
    const float f = HalfExtent * 2.0f;
    const float r = HalfExtent, l = -HalfExtent, t = HalfExtent, b = -HalfExtent;
    infra::Matrix4x4 Proj = infra::Matrix4x4::identity();
    Proj[0] = { 2.0f / (r - l), 0.0f, 0.0f, -(r + l) / (r - l) };
    Proj[1] = { 0.0f, 2.0f / (t - b), 0.0f, -(t + b) / (t - b) };
    Proj[2] = { 0.0f, 0.0f, 1.0f / (n - f), n / (n - f) };
    Proj[3] = { 0.0f, 0.0f, 0.0f, 1.0f };

    return Proj * View;
}

/** @brief 取方向光(优先交换包里的第一个方向光, 否则返回默认方向光). */
[[nodiscard]] const LightData& getDirectionalLight(const RenderFrameSubmission* Submission)
{
    static const LightData DefaultLight = [] {
        LightData Light;
        Light.Type = ELightType::Directional;
        Light.DirectionWS = infra::Vec3f { 0.35f, 0.80f, 0.45f };
        Light.Color = infra::LinearColor4D { 1.0f, 1.0f, 1.0f, 1.0f };
        Light.Intensity = 1.5f;
        Light.CastShadow = true;
        return Light;
    }();
    if (Submission)
    {
        for (const LightData& Light : Submission->Lights)
        {
            if (Light.Type == ELightType::Directional)
                return Light;
        }
    }
    return DefaultLight;
}

/** @brief 填充场景帧常量(与 shaders/scene_mesh.frag.hlsl 的 FrameData cbuffer 对齐). */
[[nodiscard]] SceneFrameConstants makeFrameConstants(const RenderView& View,
    const LightData& Light, const infra::Matrix4x4& ShadowVP, float ShadowMapSize)
{
    SceneFrameConstants C {};
    C.CameraPosition[0] = View.CameraPosition.x();
    C.CameraPosition[1] = View.CameraPosition.y();
    C.CameraPosition[2] = View.CameraPosition.z();
    C.CameraPosition[3] = 1.0f;

    const infra::Vec3f Dir = Light.DirectionWS.normalized();
    C.LightDirection[0] = Dir.x();
    C.LightDirection[1] = Dir.y();
    C.LightDirection[2] = Dir.z();
    C.LightDirection[3] = 0.0f;

    C.LightColor[0] = Light.Color.r();
    C.LightColor[1] = Light.Color.g();
    C.LightColor[2] = Light.Color.b();
    C.LightColor[3] = Light.Intensity;

    std::memcpy(C.ShadowMatrix, ShadowVP.M.data(), sizeof(float) * 16);

    C.ShadowParams[0] = 0.0015f; // 深度 bias
    C.ShadowParams[1] = ShadowMapSize;
    C.ShadowParams[2] = Light.CastShadow ? 1.0f : 0.0f;
    C.ShadowParams[3] = 0.05f;   // 环境项强度
    return C;
}

/**
 * @brief 录制一个 DrawPacket: 优先用已解析的材质; 否则回落到默认 PBR 网格材质.
 *
 * @param variant          所在 Pass 的管线变体(决定颜色附件/深度写/混合).
 * @param view_projection  本帧相机或光的 View-Projection(默认材质推导 MVP 用).
 * @param frame_bind_group 帧绑定组(不透明/透明 Pass 采样阴影用; 深度-only 传空).
 */
void recordDrawPacket(rhi::RCommandList& command_list, const DrawPacket& packet,
    ESceneMeshVariant variant, const infra::Matrix4x4& view_projection,
    const std::shared_ptr<rhi::RBindGroup>& frame_bind_group)
{
    if (packet.Pipeline)
    {
        command_list.bindPipeline(packet.Pipeline);

        if (!packet.PushConstants.empty() && packet.PipelineLayout)
        {
            command_list.pushConstants(packet.PipelineLayout,
                rhi::EShaderStage_t::Vertex | rhi::EShaderStage_t::Pixel,
                0, packet.PushConstants);
        }

        if (!packet.BindGroups.empty())
        {
            command_list.bindBindGroups(rhi::EPipelineType::Graphics, packet.PipelineLayout,
                0, packet.BindGroups, packet.DynamicOffsets);
        }
    }
    else
    {
        SceneMeshCache& cache = sceneMeshCache();
        rhi::RDevice& device = command_list.getDevice();
        ensureSceneMeshCache(device, cache);
        const std::shared_ptr<rhi::RPipeline>& pipeline =
            variant == ESceneMeshVariant::DepthOnly ? cache.DepthOnly
            : variant == ESceneMeshVariant::Transparent ? cache.Transparent
            : cache.Opaque;
        if (!pipeline || !cache.Layout)
            return;

        command_list.bindPipeline(pipeline);
        if (frame_bind_group)
        {
            const std::array groups { frame_bind_group };
            command_list.bindBindGroups(rhi::EPipelineType::Graphics, cache.Layout, 0, groups);
        }

        // 着色器按"四个行向量"读取矩阵(见 scene_mesh.vert.hlsl 的说明), 直接按行拷贝.
        const infra::Matrix4x4 mvp = view_projection * packet.LocalToWorld;
        std::array<std::byte, SceneMeshPushConstantSize> constants {};
        std::memcpy(constants.data(), mvp.M.data(), sizeof(float) * 16);
        std::memcpy(constants.data() + sizeof(float) * 16,
            packet.LocalToWorld.M.data(), sizeof(float) * 16);
        command_list.pushConstants(cache.Layout, rhi::EShaderStage_t::Vertex, 0, constants);
    }

    if (!packet.VertexBindings.empty())
        command_list.bindVertexBuffers(0, packet.VertexBindings);

    // 逐实例属性缓冲(binding 1, PerInstance): 预留的 GPU 实例化数据通路.
    if (packet.InstanceBuffer)
    {
        const rhi::VertexBufferBinding InstanceBinding { packet.InstanceBuffer, packet.InstanceOffset };
        command_list.bindVertexBuffers(1, std::span<const rhi::VertexBufferBinding>(&InstanceBinding, 1));
    }

    if (packet.IndexBuffer)
        command_list.bindIndexBuffer(packet.IndexBuffer, packet.IndexOffset, packet.IndexFormat);

    if (packet.IndexCount > 0)
    {
        command_list.drawIndexed(packet.IndexCount, packet.InstanceCount, packet.FirstIndex,
            packet.VertexOffset, packet.FirstInstance);
    }
}

/** @brief 录制一个已按 SortKey 排序的绘制列表(默认 PBR 材质变体 + VP 矩阵). */
void recordDrawList(rhi::RCommandList& command_list, const DrawList& draw_list,
    ESceneMeshVariant variant, const infra::Matrix4x4& view_projection,
    const std::shared_ptr<rhi::RBindGroup>& frame_bind_group = {})
{
    for (const DrawPacket& packet : draw_list)
        recordDrawPacket(command_list, packet, variant, view_projection, frame_bind_group);
}

// ============================================================================
//  后处理(全屏 ToneMapping)
// ============================================================================

struct PostCache
{
    std::shared_ptr<rhi::RPipelineLayout> Layout;
    std::shared_ptr<rhi::RBindGroupLayout> TextureLayout;
    std::shared_ptr<rhi::RPipeline> Pipeline;
    std::shared_ptr<rhi::RSampler> Sampler;
    bool Ready { false };
};

PostCache& postCache()
{
    static PostCache Cache;
    return Cache;
}

void ensurePostCache(rhi::RDevice& device, PostCache& cache)
{
    if (cache.Ready)
        return;

    const auto vertex_shader = createSceneShader(device, rhi::EShaderStage_t::Vertex,
        "fullscreen.vert.spv", "FullscreenVertex");
    const auto pixel_shader = createSceneShader(device, rhi::EShaderStage_t::Pixel,
        "post.frag.spv", "PostPixel");

    rhi::BindGroupLayoutDescriptor layout_desc;
    layout_desc.Entries = {
        { .Binding = 0, .Type = rhi::EDescriptorType::SampledTexture, .ArrayCount = 1,
          .Visibility = rhi::EShaderStage_t::Pixel },
        { .Binding = 1, .Type = rhi::EDescriptorType::Sampler, .ArrayCount = 1,
          .Visibility = rhi::EShaderStage_t::Pixel },
    };
    layout_desc.DebugName = "PostLayout";
    cache.TextureLayout = device.createBindGroupLayout(layout_desc);

    rhi::PipelineLayoutDescriptor pipe_layout_desc;
    pipe_layout_desc.BindGroupLayouts = { cache.TextureLayout };
    pipe_layout_desc.PushConstantRanges = {
        { .Stages = rhi::EShaderStage_t::Pixel, .Offset = 0, .Size = 16 }
    };
    pipe_layout_desc.DebugName = "PostPipelineLayout";
    cache.Layout = device.createPipelineLayout(pipe_layout_desc);

    rhi::RSampler::Descriptor_t sampler_desc;
    sampler_desc.MinFilter = rhi::EFilterMode::Linear;
    sampler_desc.MagFilter = rhi::EFilterMode::Linear;
    sampler_desc.AddressU = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.AddressV = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.AddressW = rhi::ESamplerAddressMode::ClampToEdge;
    sampler_desc.DebugName = "PostSampler";
    cache.Sampler = device.createSampler(sampler_desc);

    rhi::GraphicsPipelineDescriptor pipe_desc;
    pipe_desc.Layout = cache.Layout;
    pipe_desc.Vertex.Shader = vertex_shader;
    pipe_desc.Pixel.Shader = pixel_shader;
    // 全屏三角形: 无顶点输入, 3 个顶点由 SV_VertexID 生成.
    pipe_desc.Rasterizer.CullMode = rhi::ECullMode::None;
    pipe_desc.DepthStencil.DepthTestEnable = false;
    pipe_desc.DepthStencil.DepthWriteEnable = false;
    pipe_desc.Rendering.ColorFormats[0] = rhi::EFormat::BGRA8_UNorm;
    pipe_desc.Rendering.ColorAttachmentCount = 1;
    pipe_desc.DebugName = "PostPipeline";
    cache.Pipeline = device.createGraphicsPipeline(pipe_desc);
    cache.Ready = true;
}

/** @brief 全屏采样 HDR 场景色并做 ToneMapping + gamma, 输出到当前(交换链)附件. */
void recordPostPass(rhi::RCommandList& command_list, const std::shared_ptr<rhi::RImageView>& scene_view)
{
    PostCache& cache = postCache();
    rhi::RDevice& device = command_list.getDevice();
    ensurePostCache(device, cache);
    if (!cache.Pipeline || !cache.Layout || !scene_view)
        return;

    const auto bind_group = device.createBindGroup({
        .Layout = cache.TextureLayout,
        .Entries = {
            { .Binding = 0, .ArrayElement = 0, .Resource = rhi::TextureBinding { scene_view } },
            { .Binding = 1, .ArrayElement = 0, .Resource = rhi::SamplerBinding { cache.Sampler } },
        },
        .DebugName = "PostBindGroup",
    });
    if (!bind_group)
        return;

    command_list.bindPipeline(cache.Pipeline);
    const std::array groups { bind_group };
    command_list.bindBindGroups(rhi::EPipelineType::Graphics, cache.Layout, 0, groups);
    std::array<std::byte, 16> exposure {};
    const float exposure_value = 1.0f;
    std::memcpy(exposure.data(), &exposure_value, sizeof(exposure_value));
    command_list.pushConstants(cache.Layout, rhi::EShaderStage_t::Pixel, 0, exposure);
    command_list.draw(3, 1, 0, 0);
}

/**
 * @brief 2D UI 绘制的跨帧缓存(管线 + 绑定布局 + 顶点/索引/暂存缓冲 + 纹理表).
 *
 * 仅由渲染线程的 UIRenderPass 调用, 因此用静态缓存是安全的.
 * @note 字体图集与图片纹理统一走 RGBA8 + CombinedImageSampler 绑定组;
 *       不携带纹理的纯色批次绑定 1x1 白色纹理, 于是全部批次共用同一条管线.
 */
struct UITextureEntry
{
    std::shared_ptr<rhi::RTexture> Texture;
    std::shared_ptr<rhi::RBindGroup> BindGroup;
    /** @brief 是否已做过首次 Undefined→CopyDst 转换(后续更新从 ShaderRead 出发). */
    bool Uploaded { false };
};

struct UIDrawCache
{
    std::shared_ptr<rhi::RPipelineLayout> PipelineLayout;
    std::shared_ptr<rhi::RPipeline> Pipeline;
    /** @brief 单条目绑定组布局: binding 0 = CombinedImageSampler(像素阶段). */
    std::shared_ptr<rhi::RBindGroupLayout> TextureLayout;
    std::shared_ptr<rhi::RBuffer> VertexBuffer;
    std::shared_ptr<rhi::RBuffer> IndexBuffer;
    /** @brief 纹理上传用的 host-visible 暂存缓冲(逐帧复用, 按需扩容). */
    std::shared_ptr<rhi::RBuffer> StagingBuffer;
    uint64_t StagingCapacity { 0 };
    uint64_t VertexCapacity { 0 };
    uint64_t IndexCapacity { 0 };
    /** @brief 纹理句柄 → GPU 纹理 + 绑定组. */
    std::unordered_map<uint64_t, UITextureEntry> Textures;
    /** @brief 无纹理批次的白色回退(1x1, 全 1). */
    UITextureEntry White;
    bool Ready { false };
};

/** @brief 文件作用域缓存(需在设备销毁前显式释放, 避免静态析构晚于 RHI shutdown). */
UIDrawCache& uiDrawCache()
{
    static UIDrawCache Cache;
    return Cache;
}

/** @brief 创建一张 RGBA8 的 UI 纹理并登记其采样绑定组. */
UITextureEntry createUITextureEntry(
    rhi::RDevice& device, UIDrawCache& cache, uint32_t width, uint32_t height,
    const char* debug_name)
{
    UITextureEntry entry;
    rhi::RTexture::Descriptor_t desc;
    desc.Image.Format = rhi::EFormat::RGBA8_UNorm;
    desc.Image.Width = std::max(1u, width);
    desc.Image.Height = std::max(1u, height);
    desc.Image.Usage = rhi::EImageUsage_t::Sampled | rhi::EImageUsage_t::TransferDst;
    // 图集是预栅格化的 1:1 采样, 线性过滤 + Clamp 足以避免邻接字形渗色.
    desc.Sampler.MinFilter = rhi::EFilterMode::Linear;
    desc.Sampler.MagFilter = rhi::EFilterMode::Linear;
    desc.Sampler.MipmapFilter = rhi::EFilterMode::Linear;
    desc.Sampler.AddressU = rhi::ESamplerAddressMode::ClampToEdge;
    desc.Sampler.AddressV = rhi::ESamplerAddressMode::ClampToEdge;
    desc.Sampler.AddressW = rhi::ESamplerAddressMode::ClampToEdge;
    desc.DebugName = debug_name;
    entry.Texture = device.createTexture(desc);
    if (!entry.Texture || !entry.Texture->isValid())
        return entry;

    entry.BindGroup = device.createBindGroup({
        .Layout = cache.TextureLayout,
        .Entries = {{
            .Binding = 0,
            .ArrayElement = 0,
            .Resource = rhi::CombinedImageSamplerBinding {
                entry.Texture->getImageView(), entry.Texture->getSampler() }
        }},
        .DebugName = debug_name
    });
    return entry;
}

/** @brief 惰性初始化 UI 管线与缓冲(仅一次). */
void ensureUIDrawCache(rhi::RDevice& device, UIDrawCache& cache)
{
    if (cache.Ready)
        return;

    // 绑定组布局: 单个 combined image sampler, 只被像素阶段读取.
    rhi::BindGroupLayoutDescriptor texture_layout_desc;
    texture_layout_desc.Entries = {{
        .Binding = 0,
        .Type = rhi::EDescriptorType::CombinedImageSampler,
        .ArrayCount = 1,
        .Visibility = rhi::EShaderStage_t::Pixel
    }};
    texture_layout_desc.DebugName = "UI2DTextureLayout";
    cache.TextureLayout = device.createBindGroupLayout(texture_layout_desc);

    rhi::ShaderDescriptor vs;
    vs.Stage = rhi::EShaderStage_t::Vertex;
    vs.ByteCode.assign(UI2DVertexSpv.begin(), UI2DVertexSpv.end());
    vs.EntryPoint = "main";
    vs.DebugName = "UI2DVertex";
    auto vertex_shader = device.createShader(vs);

    rhi::ShaderDescriptor ps;
    ps.Stage = rhi::EShaderStage_t::Pixel;
    ps.ByteCode.assign(UI2DPixelSpv.begin(), UI2DPixelSpv.end());
    ps.EntryPoint = "main";
    ps.DebugName = "UI2DPixel";
    auto pixel_shader = device.createShader(ps);

    rhi::PipelineLayoutDescriptor layout_desc;
    // 顶点已由 CPU 侧变换到 NDC, 无需 push constant; 只有一组纹理绑定.
    layout_desc.BindGroupLayouts = { cache.TextureLayout };
    layout_desc.DebugName = "UI2DLayout";
    cache.PipelineLayout = device.createPipelineLayout(layout_desc);

    rhi::GraphicsPipelineDescriptor pipe_desc;
    pipe_desc.Layout = cache.PipelineLayout;
    pipe_desc.Vertex.Shader = vertex_shader;
    pipe_desc.Pixel.Shader = pixel_shader;
    // 顶点布局与 ui::UIVertex 对齐: Position(float2) + UV(float2) + Tint(float4).
    pipe_desc.VertexInput.Buffers = {
        { .Binding = 0, .Stride = 32, .InputRate = rhi::EVertexInputRate::PerVertex }
    };
    pipe_desc.VertexInput.Attributes = {
        { .Location = 0, .Binding = 0, .Format = rhi::EVertexFormat::Float2, .Offset = 0 },
        { .Location = 1, .Binding = 0, .Format = rhi::EVertexFormat::Float2, .Offset = 8 },
        { .Location = 2, .Binding = 0, .Format = rhi::EVertexFormat::Float4, .Offset = 16 },
    };
    pipe_desc.Rasterizer.CullMode = rhi::ECullMode::None;
    pipe_desc.Blend.Attachments[0].BlendEnable = true;
    pipe_desc.Blend.Attachments[0].Color.SrcFactor = rhi::EBlendFactor::SrcAlpha;
    pipe_desc.Blend.Attachments[0].Color.DstFactor = rhi::EBlendFactor::OneMinusSrcAlpha;
    pipe_desc.Blend.Attachments[0].Color.Operation = rhi::EBlendOp::Add;
    pipe_desc.Blend.Attachments[0].Alpha.SrcFactor = rhi::EBlendFactor::One;
    pipe_desc.Blend.Attachments[0].Alpha.DstFactor = rhi::EBlendFactor::OneMinusSrcAlpha;
    pipe_desc.Blend.Attachments[0].Alpha.Operation = rhi::EBlendOp::Add;
    pipe_desc.DepthStencil.DepthTestEnable = false;
    pipe_desc.DepthStencil.DepthWriteEnable = false;
    pipe_desc.Rendering.ColorFormats[0] = rhi::EFormat::BGRA8_UNorm;
    pipe_desc.Rendering.ColorAttachmentCount = 1;
    pipe_desc.DebugName = "UI2DPipeline";
    cache.Pipeline = device.createGraphicsPipeline(pipe_desc);

    // 白色 1x1 回退: 让"无纹理"批次也能走同一条采样管线.
    cache.White = createUITextureEntry(device, cache, 1, 1, "UIWhiteTexture");

    cache.Ready = true;
}

/** @brief 保证缓冲容量, 不足时重建(host-visible + coherent). */
void ensureBufferCapacity(
    rhi::RDevice& device,
    std::shared_ptr<rhi::RBuffer>& buffer,
    uint64_t& capacity,
    uint64_t required,
    rhi::EBufferUsage usage,
    const char* debug_name)
{
    if (capacity >= required && buffer)
        return;
    capacity = std::max<uint64_t>(required, capacity * 2);
    rhi::RBuffer::Descriptor_t desc;
    desc.Size = capacity;
    desc.Usage = usage;
    desc.MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent;
    desc.DebugName = debug_name;
    buffer = device.createBuffer(desc);
}

/**
 * @brief 在进入 render scope 之前把本帧的 UI 纹理上传到 GPU.
 *
 * 传输命令(copyBufferToImage)不能在渲染作用域内录制, 因此 UIRenderPass 必须在
 * beginColorScope 之前调用本函数. 首次见到的句柄会创建 RGBA8 纹理; 之后只按脏区
 * 做一次局部拷贝(字形逐格入库, 单帧脏区通常只有几十 KB).
 *
 * @param command_list 当前帧命令列表(必须处于未进入渲染作用域的状态).
 * @param submission   本帧交换包, 提供纹理上传队列.
 */
void uploadUISampledTextures(
    rhi::RCommandList& command_list,
    const RenderFrameSubmission& submission)
{
    UIDrawCache& cache = uiDrawCache();
    rhi::RDevice& device = command_list.getDevice();
    ensureUIDrawCache(device, cache);
    if (!cache.TextureLayout || !cache.White.Texture || !cache.White.BindGroup)
        return;

    // 一次上传任务: 目标纹理 + 紧密排列的行数据 + 目标子矩形.
    struct UploadJob
    {
        UITextureEntry* Entry { nullptr };
        const std::byte* Rows { nullptr };
        int32_t X {};
        int32_t Y {};
        uint32_t Width {};
        uint32_t Height {};
    };

    std::vector<UploadJob> jobs;

    // ---- 1. 白色回退纹理(仅首次) ----
    static constexpr std::array<std::byte, 4> WhitePixel {
        std::byte { 0xFF }, std::byte { 0xFF }, std::byte { 0xFF }, std::byte { 0xFF } };
    if (!cache.White.Uploaded)
    {
        jobs.push_back(UploadJob {
            &cache.White, WhitePixel.data(), 0, 0, 1, 1 });
    }

    // ---- 2. 本帧脏区: 首次出现的句柄先建 GPU 纹理 ----
    for (const ui::UISampledTextureUpload& upload : submission.TextureUploads)
    {
        if (upload.UploadWidth == 0 || upload.UploadHeight == 0 || upload.Pixels.empty())
            continue;
        auto it = cache.Textures.find(upload.Handle);
        if (it == cache.Textures.end())
        {
            UITextureEntry entry = createUITextureEntry(device, cache,
                upload.Width, upload.Height, "UITexture");
            if (!entry.Texture || !entry.BindGroup)
                continue;
            it = cache.Textures.emplace(upload.Handle, std::move(entry)).first;
        }
        jobs.push_back(UploadJob { &it->second, upload.Pixels.data(),
            static_cast<int32_t>(upload.X), static_cast<int32_t>(upload.Y),
            upload.UploadWidth, upload.UploadHeight });
    }
    if (jobs.empty())
        return;

    // ---- 3. 打包进暂存缓冲(每块按 4 字节对齐, 行紧密排列) ----
    uint64_t total_bytes = 0;
    std::vector<uint64_t> offsets(jobs.size(), 0);
    for (std::size_t i = 0; i < jobs.size(); ++i)
    {
        offsets[i] = total_bytes;
        total_bytes += static_cast<uint64_t>(jobs[i].Width) * jobs[i].Height * 4;
        total_bytes = (total_bytes + 3) & ~3ull;
    }
    ensureBufferCapacity(device, cache.StagingBuffer, cache.StagingCapacity,
        total_bytes, rhi::EBufferUsage::TransferSrc, "UIUploadStaging");
    if (!cache.StagingBuffer)
        return;

    auto* staging = static_cast<std::byte*>(cache.StagingBuffer->map(0, total_bytes));
    if (!staging)
        return;
    for (std::size_t i = 0; i < jobs.size(); ++i)
    {
        const std::size_t bytes =
            static_cast<std::size_t>(jobs[i].Width) * jobs[i].Height * 4;
        std::memcpy(staging + offsets[i], jobs[i].Rows, bytes);
    }
    cache.StagingBuffer->unmap();

    // ---- 4. 每张纹理: 转 TransferDst → 拷贝 → 转回 PixelShaderResource ----
    for (std::size_t i = 0; i < jobs.size(); ++i)
    {
        UITextureEntry& entry = *jobs[i].Entry;
        if (!entry.Texture || !entry.Texture->isValid())
            continue;
        const std::array<rhi::ImageBarrier, 1> to_copy { rhi::ImageBarrier {
            .Image = entry.Texture->getImage(),
            .Before = entry.Uploaded ? rhi::EResourceState::PixelShaderResource
                                     : rhi::EResourceState::Undefined,
            .After = rhi::EResourceState::CopyDst } };
        command_list.imageBarriers(to_copy);

        const std::array<rhi::BufferImageCopyRegion, 1> regions { rhi::BufferImageCopyRegion {
            .BufferOffset = offsets[i],
            .BufferRowLength = 0,
            .BufferImageHeight = 0,
            .Image = {},
            .ImageOffset = { jobs[i].X, jobs[i].Y, 0 },
            .ImageExtent = { jobs[i].Width, jobs[i].Height, 1 } } };
        command_list.copyBufferToImage(cache.StagingBuffer, entry.Texture->getImage(), regions);

        const std::array<rhi::ImageBarrier, 1> to_sample { rhi::ImageBarrier {
            .Image = entry.Texture->getImage(),
            .Before = rhi::EResourceState::CopyDst,
            .After = rhi::EResourceState::PixelShaderResource } };
        command_list.imageBarriers(to_sample);
        entry.Uploaded = true;
    }
}

/**
 * @brief 录制原始 UI 绘制列表(顶点/索引/批次).
 *
 * 顶点坐标在 CPU 侧变换到 NDC; 逐批次绑定纹理(CombinedImageSampler)并按
 * batch.Clip 设置 scissor. 未携带纹理的纯色批次绑定 1x1 白色回退纹理.
 */
void recordUIDrawLists(
    rhi::RCommandList& command_list,
    const RenderFrameSubmission& submission,
    uint32_t width, uint32_t height)
{
    const std::vector<ui::UIDrawList>& draw_lists = submission.UIDrawLists;
    if (draw_lists.empty())
        return;

    UIDrawCache& cache = uiDrawCache();
    rhi::RDevice& device = command_list.getDevice();
    ensureUIDrawCache(device, cache);
    if (!cache.Pipeline || !cache.PipelineLayout)
        return;

    // 合并所有列表的顶点/索引总量.
    uint64_t total_vertices = 0;
    uint64_t total_indices = 0;
    for (const ui::UIDrawList& list : draw_lists)
    {
        total_vertices += list.Vertices.size();
        total_indices += list.Indices.size();
    }
    if (total_vertices == 0 || total_indices == 0)
        return;

    ensureBufferCapacity(device, cache.VertexBuffer, cache.VertexCapacity,
        total_vertices * sizeof(ui::UIVertex), rhi::EBufferUsage::Vertex, "UIVertexBuffer");
    ensureBufferCapacity(device, cache.IndexBuffer, cache.IndexCapacity,
        total_indices * sizeof(uint32_t), rhi::EBufferUsage::Index, "UIIndexBuffer");

    // 变换: 逻辑坐标 → NDC.
    // UI 顶点与裁剪矩形都位于窗口"逻辑坐标"空间(高 DPI 下小于像素尺寸), 因此映射
    // 分母必须是逻辑尺寸, 而不是交换链像素尺寸; 像素尺寸只用于把逻辑裁剪矩形换算成
    // scissor(见下方 ScaleX/ScaleY).
    const float logical_width = static_cast<float>(submission.UILogicalWidth
        ? submission.UILogicalWidth : std::max(1u, width));
    const float logical_height = static_cast<float>(submission.UILogicalHeight
        ? submission.UILogicalHeight : std::max(1u, height));
    // 注意 Vulkan 的 NDC 是 Y 向下(+1 在屏幕底部), 而 UI 的逻辑坐标也是 Y 向下,
    // 因此 Y 必须是正斜率: y=0 → -1(顶), y=logical_height → +1(底).
    const float sx = 2.0f / logical_width;
    const float sy = 2.0f / logical_height;
    // 逻辑坐标 → 像素坐标的缩放, 用于把 Batch.Clip 换算成 scissor.
    const double scale_x = static_cast<double>(width) / static_cast<double>(logical_width);
    const double scale_y = static_cast<double>(height) / static_cast<double>(logical_height);

    // 上传(逐列表拼接; 顶点坐标在 CPU 侧变换到 NDC).
    auto* vdst = static_cast<ui::UIVertex*>(
        cache.VertexBuffer->map(0, total_vertices * sizeof(ui::UIVertex)));
    auto* idst = static_cast<uint32_t*>(
        cache.IndexBuffer->map(0, total_indices * sizeof(uint32_t)));
    uint64_t voff = 0;
    uint64_t ioff = 0;
    for (const ui::UIDrawList& list : draw_lists)
    {
        for (size_t i = 0; i < list.Vertices.size(); ++i)
        {
            ui::UIVertex v = list.Vertices[i];
            v.Position.X = v.Position.X * sx - 1.0f;
            v.Position.Y = v.Position.Y * sy - 1.0f;
            vdst[voff + i] = v;
        }
        std::memcpy(idst + ioff, list.Indices.data(), list.Indices.size() * sizeof(uint32_t));
        voff += list.Vertices.size();
        ioff += list.Indices.size();
    }
    cache.VertexBuffer->unmap();
    cache.IndexBuffer->unmap();

    command_list.bindPipeline(cache.Pipeline);
    const std::array bindings { rhi::VertexBufferBinding { cache.VertexBuffer, 0 } };
    command_list.bindVertexBuffers(0, bindings);
    command_list.bindIndexBuffer(cache.IndexBuffer, 0, rhi::EIndexFormat::UInt32);

    // 默认全屏 scissor; 逐批次按 Clip 覆盖, 批次结束后恢复.
    const rhi::RenderArea full_scissor {
        0, 0, std::max(1u, width), std::max(1u, height) };

    // 当前已绑定的纹理句柄(0 表示白色回退); HasBound 区分"尚未绑定".
    uint64_t bound_handle = 0;
    bool has_bound = false;

    voff = 0;
    ioff = 0;
    for (const ui::UIDrawList& list : draw_lists)
    {
        for (const ui::UIDrawBatch& batch : list.Batches)
        {
            if (batch.IndexCount == 0)
                continue;

            // ---- 纹理绑定: 句柄变化时才切换(句柄 0 = 白色回退) ----
            const uint64_t handle = batch.Texture.Value;
            if (!has_bound || handle != bound_handle)
            {
                const UITextureEntry* entry = &cache.White;
                if (handle != 0)
                {
                    const auto it = cache.Textures.find(handle);
                    if (it != cache.Textures.end() && it->second.BindGroup)
                        entry = &it->second;
                    else
                        entry = &cache.White;
                }
                if (entry->BindGroup)
                {
                    const std::array groups { entry->BindGroup };
                    command_list.bindBindGroups(rhi::EPipelineType::Graphics,
                        cache.PipelineLayout, 0, groups);
                    bound_handle = handle;
                    has_bound = true;
                }
            }

            // ---- 裁剪: Clip 是逻辑坐标, 与顶点同一坐标空间(Y 向下) ----
            // 按 逻辑/像素 比例换算成 scissor 的像素矩形(与 SDL GPU 路径一致:
            // 左/上向下取整, 右/下向上取整, 再夹到目标范围内).
            if (batch.Clip)
            {
                const ui::UIRect& clip = *batch.Clip;
                const int32_t left = static_cast<int32_t>(std::clamp(
                    std::floor(static_cast<double>(clip.X) * scale_x),
                    0.0, static_cast<double>(full_scissor.Width)));
                const int32_t top = static_cast<int32_t>(std::clamp(
                    std::floor(static_cast<double>(clip.Y) * scale_y),
                    0.0, static_cast<double>(full_scissor.Height)));
                const int32_t right = static_cast<int32_t>(std::clamp(
                    std::ceil(static_cast<double>(clip.X + clip.Width) * scale_x),
                    static_cast<double>(left), static_cast<double>(full_scissor.Width)));
                const int32_t bottom = static_cast<int32_t>(std::clamp(
                    std::ceil(static_cast<double>(clip.Y + clip.Height) * scale_y),
                    static_cast<double>(top), static_cast<double>(full_scissor.Height)));
                // 完全被裁掉: 整批跳过(RHI 不接受零面积 scissor).
                if (right <= left || bottom <= top)
                    continue;
                const rhi::RenderArea clipped {
                    left, top,
                    static_cast<uint32_t>(right - left),
                    static_cast<uint32_t>(bottom - top) };
                const std::array scissors { clipped };
                command_list.setScissors(scissors);
            }
            else
            {
                const std::array scissors { full_scissor };
                command_list.setScissors(scissors);
            }

            command_list.drawIndexed(batch.IndexCount, 1,
                static_cast<uint32_t>(ioff) + batch.FirstIndex,
                static_cast<int32_t>(voff), 0);
        }
        voff += list.Vertices.size();
        ioff += list.Indices.size();
    }

    // 恢复全屏 scissor, 避免影响后续 Pass 的状态继承.
    const std::array scissors { full_scissor };
    command_list.setScissors(scissors);
}

} // namespace

/** @brief 释放 UI 绘制缓存(管线/布局/缓冲/纹理)与默认网格材质, 需在 RHI 设备销毁前调用. */
void releaseUIDrawCache() noexcept
{
    UIDrawCache& Cache = uiDrawCache();
    Cache.Textures.clear();
    Cache.White = {};
    Cache.Pipeline.reset();
    Cache.PipelineLayout.reset();
    Cache.TextureLayout.reset();
    Cache.VertexBuffer.reset();
    Cache.IndexBuffer.reset();
    Cache.StagingBuffer.reset();
    Cache.StagingCapacity = 0;
    Cache.Ready = false;

    SceneMeshCache& Scene = sceneMeshCache();
    if (Scene.FrameUBO && Scene.FrameUBOMapped)
    {
        Scene.FrameUBO->unmap();
        Scene.FrameUBOMapped = nullptr;
    }
    Scene.DepthOnly.reset();
    Scene.Opaque.reset();
    Scene.Transparent.reset();
    Scene.Layout.reset();
    Scene.FrameLayout.reset();
    Scene.FrameUBO.reset();
    Scene.ShadowSampler.reset();
    Scene.Ready = false;

    PostCache& Post = postCache();
    Post.Pipeline.reset();
    Post.Layout.reset();
    Post.TextureLayout.reset();
    Post.Sampler.reset();
    Post.Ready = false;
}

// ============================================================================
//  具体子渲染器
// ============================================================================

void ShadowRenderPass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        // 阴影图集(可采样): 方向光视角的深度图.
        const rdg::ResourceId shadow_atlas = b.createTexture("ShadowAtlas",
            makeShadowDepthDesc(Context.Width, Context.Height));
        Context.FrameResources.ShadowAtlas = shadow_atlas;

        // 方向光(优先取交换包里的方向光, 否则用默认光).
        const LightData& Light = getDirectionalLight(Context.Submission);
        const infra::Vec3f ShadowCenter = Context.View.CameraPosition;
        Context.FrameResources.ShadowViewProjection =
            makeDirectionalShadowVP(Light.DirectionWS, ShadowCenter, 40.0f);

        b.setRenderTargets({}, shadow_atlas);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context, shadow_atlas](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            const rhi::RenderArea SceneRect = resolveSceneRect(Context.Submission,
                Context.Width, Context.Height);
            beginDepthScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(shadow_atlas), &SceneRect);
            // 用光的 ViewProjection 渲染阴影深度(不是相机 VP).
            if (Context.Submission)
                recordDrawList(command_list, Context.Submission->ShadowDraws,
                    ESceneMeshVariant::DepthOnly, Context.FrameResources.ShadowViewProjection);
            command_list.endRendering();
        });
    });
}

void DepthPrePass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        // 声明共享主深度并写回共享句柄, 供 Opaque 等后续 Pass 复用.
        const rdg::ResourceId depth = b.createTexture("SceneDepth",
            makeDepthDesc(Context.Width, Context.Height));
        Context.FrameResources.SceneDepth = depth;
        b.setRenderTargets({}, depth);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context, depth](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            const rhi::RenderArea SceneRect = resolveSceneRect(Context.Submission,
                Context.Width, Context.Height);
            beginDepthScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(depth), &SceneRect);
            // 深度预通道复用不透明绘制列表, 由深度-only 管线建立 Early-Z.
            if (Context.Submission)
                recordDrawList(command_list, Context.Submission->OpaqueDraws,
                    ESceneMeshVariant::DepthOnly, Context.View.ViewProjection);
            command_list.endRendering();
        });
    });
}

void OpaqueRenderPass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        // HDR 场景色(RGBA16F): Opaque/Transparent 写, PostProcess 读.
        const rdg::ResourceId scene_color = b.createTexture("SceneColor",
            makeColorDesc(Context.Width, Context.Height));
        Context.FrameResources.SceneColor = scene_color;

        const std::array color { scene_color };
        b.setRenderTargets(color, Context.FrameResources.SceneDepth);
        // 阴影图是深度图, 采样时应处于只读深度布局.
        b.read(Context.FrameResources.ShadowAtlas, rdg::EResourceState::DepthRead);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context, scene_color](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            const rhi::RenderArea SceneRect = resolveSceneRect(Context.Submission,
                Context.Width, Context.Height);
            beginColorScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(scene_color), rhi::ELoadOp::Clear,
                ctx.getViewShared(Context.FrameResources.SceneDepth), &SceneRect);

            // 帧常量(相机/光/阴影 VP)+ 当前阴影图视图组成帧绑定组.
            const LightData& Light = getDirectionalLight(Context.Submission);
            const SceneFrameConstants frame_constants = makeFrameConstants(Context.View, Light,
                Context.FrameResources.ShadowViewProjection, static_cast<float>(Context.Width));
            SceneMeshCache& mesh_cache = sceneMeshCache();
            ensureSceneMeshCache(command_list.getDevice(), mesh_cache);
            const auto frame_bind_group = createSceneFrameBindGroup(command_list.getDevice(),
                mesh_cache, Context.FrameIndex, ctx.getViewShared(Context.FrameResources.ShadowAtlas),
                frame_constants);

            if (Context.Submission)
                recordDrawList(command_list, Context.Submission->OpaqueDraws,
                    ESceneMeshVariant::Opaque, Context.View.ViewProjection, frame_bind_group);
            command_list.endRendering();
        });
    });
}

void TransparentRenderPass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        const std::array color { Context.FrameResources.SceneColor };
        b.setRenderTargets(color, Context.FrameResources.SceneDepth);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            const rhi::RenderArea SceneRect = resolveSceneRect(Context.Submission,
                Context.Width, Context.Height);
            beginColorScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(Context.FrameResources.SceneColor), rhi::ELoadOp::Load,
                ctx.getViewShared(Context.FrameResources.SceneDepth), &SceneRect);

            const LightData& Light = getDirectionalLight(Context.Submission);
            const SceneFrameConstants frame_constants = makeFrameConstants(Context.View, Light,
                Context.FrameResources.ShadowViewProjection, static_cast<float>(Context.Width));
            SceneMeshCache& mesh_cache = sceneMeshCache();
            ensureSceneMeshCache(command_list.getDevice(), mesh_cache);
            const auto frame_bind_group = createSceneFrameBindGroup(command_list.getDevice(),
                mesh_cache, Context.FrameIndex, ctx.getViewShared(Context.FrameResources.ShadowAtlas),
                frame_constants);

            // 透明列表已按深度从后向前排序; 只读深度不写深度.
            if (Context.Submission)
                recordDrawList(command_list, Context.Submission->TransparentDraws,
                    ESceneMeshVariant::Transparent, Context.View.ViewProjection, frame_bind_group);
            command_list.endRendering();
        });
    });
}

void PostProcessRenderPass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        const std::array color { Context.Backbuffer };
        b.setRenderTargets(color);
        b.read(Context.FrameResources.SceneColor, rdg::EResourceState::PixelShaderResource);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            beginColorScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(Context.Backbuffer), rhi::ELoadOp::Clear);
            // ToneMapping(Bloom/TAA 留待后续 Pass): 采样 HDR 场景色, 输出到交换链.
            recordPostPass(command_list, ctx.getViewShared(Context.FrameResources.SceneColor));
            command_list.endRendering();
        });
    });
}

void UIRenderPass::addPasses(rdg::RDGGraph& Graph, const RenderContext& Context)
{
    Graph.addPass(getName(), [&Context](rdg::RDGBuilder& b) {
        const std::array color { Context.Backbuffer };
        b.setRenderTargets(color);
        b.setType(rdg::ERDGPassType::Raster);
        b.setExecute([Context](rdg::RDGPassContext& ctx) {
            auto& command_list = ctx.getCommandList();
            // 纹理上传是 transfer 命令, 必须落在 render scope 之外.
            if (Context.Submission)
                uploadUISampledTextures(command_list, *Context.Submission);
            beginColorScope(command_list, Context.Width, Context.Height,
                ctx.getViewShared(Context.Backbuffer), rhi::ELoadOp::Load);
            if (Context.Submission)
                recordUIDrawLists(command_list, *Context.Submission,
                    Context.Width, Context.Height);
            command_list.endRendering();
        });
    });
}

} // namespace runtime::renderer
