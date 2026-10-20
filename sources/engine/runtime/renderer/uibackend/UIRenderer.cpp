#include <infra/common/Error.hpp>
#include "UIRenderer.hpp"
#include "SdlInternal.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <utility>

#ifndef EDITOR_UI_SHADER_DIR
#error EDITOR_UI_SHADER_DIR must be defined by editor_configure_ui_shaders
#endif

namespace ui
{
namespace
{
template <class Type, auto Release> struct GPUResource
{
    SDL_GPUDevice *Device{};
    Type *Pointer{};
    explicit GPUResource(SDL_GPUDevice *InDevice) : Device(InDevice)
    {
    }
    ~GPUResource()
    {
        if (Pointer)
        {
            Release(Device, Pointer);
        }
    }
    GPUResource(const GPUResource &) = delete;
    GPUResource &operator=(const GPUResource &) = delete;
};

using BufferResource = GPUResource<SDL_GPUBuffer, SDL_ReleaseGPUBuffer>;
using TransferResource = GPUResource<SDL_GPUTransferBuffer, SDL_ReleaseGPUTransferBuffer>;
using TextureResource = GPUResource<SDL_GPUTexture, SDL_ReleaseGPUTexture>;

struct CommandScope
{
    SDL_GPUCommandBuffer *Pointer{};
    bool AcquiredSwapchain{};
    explicit CommandScope(SDL_GPUDevice *Device) : Pointer(SDL_AcquireGPUCommandBuffer(Device))
    {
        if (!Pointer)
        {
            throw detail::makeError("SDL_AcquireGPUCommandBuffer");
        }
    }
    ~CommandScope()
    {
        if (Pointer)
        {
            if (AcquiredSwapchain)
            {
                SDL_SubmitGPUCommandBuffer(Pointer);
            }
            else
            {
                SDL_CancelGPUCommandBuffer(Pointer);
            }
        }
    }
    CommandScope(const CommandScope &) = delete;
    CommandScope &operator=(const CommandScope &) = delete;
    void submit()
    {
        auto *Command = std::exchange(Pointer, nullptr);
        detail::check(SDL_SubmitGPUCommandBuffer(Command), "SDL_SubmitGPUCommandBuffer");
    }
};

Uint32 checkedBytes(std::size_t Count, std::size_t Stride)
{
    if (Count > std::numeric_limits<Uint32>::max() / Stride)
    {
        Error::raise(EErrorCode::InvalidArgument, "UI data exceeds SDL GPU transfer buffer limits");
    }
    return static_cast<Uint32>(Count * Stride);
}

Uint32 validatePixels(Uint32 Width, Uint32 Height, std::span<const std::byte> Pixels)
{
    if (!Width || !Height || Width > 8192 || Height > 8192)
    {
        Error::raise(EErrorCode::InvalidArgument, "UI textures require dimensions in [1, 8192]");
    }
    const auto Size = checkedBytes(static_cast<std::size_t>(Width) * Height, 4);
    if (Pixels.size() != Size || !Pixels.data())
    {
        Error::raise(EErrorCode::InvalidArgument, "UI texture data must be exactly tightly packed RGBA8");
    }
    return Size;
}

bool isFiniteColor(Color Value)
{
    return std::isfinite(Value.R) && std::isfinite(Value.G) && std::isfinite(Value.B) && std::isfinite(Value.A);
}

std::uint64_t allocateTextureHandle()
{
    static std::atomic<std::uint64_t> Next{1};
    auto Value = Next.load(std::memory_order_relaxed);
    for (;;)
    {
        if (Value == std::numeric_limits<std::uint64_t>::max())
        {
            Error::raise(EErrorCode::Overflow, "UI texture handle space exhausted");
        }
        if (Next.compare_exchange_weak(Value, Value + 1, std::memory_order_relaxed))
        {
            return Value;
        }
    }
}

SDL_Rect makeScissor(const std::optional<UIRect> &Clip, UISize LogicalSize, Uint32 Width, Uint32 Height)
{
    if (!Clip)
    {
        return {0, 0, static_cast<int>(Width), static_cast<int>(Height)};
    }
    if (Clip->Width == 0 || Clip->Height == 0)
    {
        return {0, 0, 0, 0};
    }
    const double ScaleX = static_cast<double>(Width) / LogicalSize.Width;
    const double ScaleY = static_cast<double>(Height) / LogicalSize.Height;
    const auto Left = std::clamp(std::floor(Clip->X * ScaleX), 0.0, double(Width));
    const auto Top = std::clamp(std::floor(Clip->Y * ScaleY), 0.0, double(Height));
    const auto Right = std::clamp(std::ceil((double(Clip->X) + Clip->Width) * ScaleX), Left, double(Width));
    const auto Bottom = std::clamp(std::ceil((double(Clip->Y) + Clip->Height) * ScaleY), Top, double(Height));
    return {static_cast<int>(Left), static_cast<int>(Top), static_cast<int>(Right - Left),
            static_cast<int>(Bottom - Top)};
}
}

struct UIRenderer::Impl
{
    struct TextureRecord
    {
        SDL_GPUTexture *Pointer{};
        Uint32 Width{}, Height{};
    };
    std::shared_ptr<detail::WindowState> State;
    SDL_GPUDevice *Device{};
    bool Claimed{}, Active{};
    UIFrameInfo Frame{};
    std::vector<UIDrawList> Lists;
    std::unordered_map<std::uint64_t, TextureRecord> Textures;
    SDL_GPUTexture *White{};
    SDL_GPUSampler *Sampler{};
    SDL_GPUShader *VertexShader{};
    SDL_GPUShader *FragmentShader{};
    SDL_GPUGraphicsPipeline *Pipeline{};
    SDL_GPUTextureFormat PipelineFormat{SDL_GPU_TEXTUREFORMAT_INVALID};
    EUIMode Mode{EUIMode::Retained};
    std::unique_ptr<BufferResource> RetainedVertices;
    std::unique_ptr<BufferResource> RetainedIndices;
    std::size_t RetainedVertexCount{};
    std::size_t RetainedIndexCount{};
    bool ReuseRetainedGeometry{};

    ~Impl()
    {
        if (Device)
        {
            SDL_WaitForGPUIdle(Device);
            for (const auto &[Handle, Texture] : Textures)
            {
                SDL_ReleaseGPUTexture(Device, Texture.Pointer);
            }
            if (Pipeline)
            {
                SDL_ReleaseGPUGraphicsPipeline(Device, Pipeline);
            }
            if (VertexShader)
            {
                SDL_ReleaseGPUShader(Device, VertexShader);
            }
            if (FragmentShader)
            {
                SDL_ReleaseGPUShader(Device, FragmentShader);
            }
            if (Sampler)
            {
                SDL_ReleaseGPUSampler(Device, Sampler);
            }
            if (White)
            {
                SDL_ReleaseGPUTexture(Device, White);
            }
            if (Claimed)
            {
                SDL_ReleaseWindowFromGPUDevice(Device, State->Window);
                State->GPUClaimed = false;
            }
            RetainedVertices.reset();
            RetainedIndices.reset();
            SDL_DestroyGPUDevice(Device);
        }
    }

    void requireIdle() const
    {
        if (Active)
        {
            Error::raise(EErrorCode::Logic, "Texture mutation is forbidden during an active UI frame");
        }
    }

    const TextureRecord &findTexture(TextureHandle Handle) const
    {
        const auto Found = Textures.find(Handle.Value);
        if (!Handle.Value || Found == Textures.end())
        {
            Error::raise(EErrorCode::InvalidArgument, "Stale, foreign, or zero UI texture handle");
        }
        return Found->second;
    }

    SDL_GPUShader *loadShader(const char *Name, SDL_GPUShaderStage Stage)
    {
        const auto Formats = SDL_GetGPUShaderFormats(Device);
        SDL_GPUShaderFormat Format{};
        const char *Extension{};
        const char *Entry = "main";
        if (Formats & SDL_GPU_SHADERFORMAT_SPIRV)
        {
            Format = SDL_GPU_SHADERFORMAT_SPIRV;
            Extension = ".spv";
        }
#ifdef EDITOR_UI_HAS_MSL
        else if (Formats & SDL_GPU_SHADERFORMAT_MSL)
        {
            Format = SDL_GPU_SHADERFORMAT_MSL;
            Extension = ".msl";
            Entry = "main0";
        }
#endif
#ifdef EDITOR_UI_HAS_DXIL
        else if (Formats & SDL_GPU_SHADERFORMAT_DXIL)
        {
            Format = SDL_GPU_SHADERFORMAT_DXIL;
            Extension = ".dxil";
        }
#endif
        else
        {
            Error::raise(EErrorCode::Runtime, "SDL GPU device has no compiled UI shader format");
        }
        const char *BasePath = SDL_GetBasePath();
        auto Path = BasePath ? std::string(BasePath) + "shaders/" + Name + Extension
                             : std::string(EDITOR_UI_SHADER_DIR) + "/" + Name + Extension;
        std::size_t Size{};
        std::unique_ptr<void, decltype(&SDL_free)> Code(SDL_LoadFile(Path.c_str(), &Size), SDL_free);
        if (!Code)
        {
            Path = std::string(EDITOR_UI_SHADER_DIR) + "/" + Name + Extension;
            Code.reset(SDL_LoadFile(Path.c_str(), &Size));
        }
        if (!Code)
        {
            Error::raise(EErrorCode::Runtime, "Cannot load UI shader " + Path + ": " + SDL_GetError());
        }
        SDL_GPUShaderCreateInfo Info{};
        Info.code_size = Size;
        Info.code = static_cast<const Uint8 *>(Code.get());
        Info.entrypoint = Entry;
        Info.format = Format;
        Info.stage = Stage;
        Info.num_uniform_buffers = Stage == SDL_GPU_SHADERSTAGE_VERTEX ? 1 : 0;
        Info.num_samplers = Stage == SDL_GPU_SHADERSTAGE_FRAGMENT ? 1 : 0;
        auto *Shader = SDL_CreateGPUShader(Device, &Info);
        if (!Shader)
        {
            throw detail::makeError("SDL_CreateGPUShader");
        }
        return Shader;
    }

    void ensurePipeline()
    {
        const auto Format = SDL_GetGPUSwapchainTextureFormat(Device, State->Window);
        if (Format == SDL_GPU_TEXTUREFORMAT_INVALID)
        {
            throw detail::makeError("SDL_GetGPUSwapchainTextureFormat");
        }
        if (Pipeline && PipelineFormat == Format)
        {
            return;
        }
        static_assert(std::is_standard_layout_v<UIVertex> && sizeof(UIVertex) == 32);
        const SDL_GPUVertexBufferDescription Buffer{0, sizeof(UIVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0};
        const SDL_GPUVertexAttribute Attributes[] = {
            {0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(UIVertex, Position)},
            {1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(UIVertex, UV)},
            {2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT4, offsetof(UIVertex, Tint)}};
        SDL_GPUColorTargetDescription Target{};
        Target.format = Format;
        Target.blend_state.enable_blend = true;
        Target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        Target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        Target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
        Target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        Target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        Target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
        SDL_GPUGraphicsPipelineCreateInfo Info{};
        Info.vertex_shader = VertexShader;
        Info.fragment_shader = FragmentShader;
        Info.vertex_input_state = {&Buffer, 1, Attributes, 3};
        Info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        Info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        Info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        Info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
        Info.rasterizer_state.enable_depth_clip = true;
        // @note 即使禁用深度和模板测试，D3D12 仍会校验枚举字段。
        // @note 零初始化会选中 SDL 的无效比较和模板枚举值。
        Info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_ALWAYS;
        const SDL_GPUStencilOpState Stencil{
            SDL_GPU_STENCILOP_KEEP, SDL_GPU_STENCILOP_KEEP,
            SDL_GPU_STENCILOP_KEEP, SDL_GPU_COMPAREOP_ALWAYS};
        Info.depth_stencil_state.front_stencil_state = Stencil;
        Info.depth_stencil_state.back_stencil_state = Stencil;
        Info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        Info.target_info.color_target_descriptions = &Target;
        Info.target_info.num_color_targets = 1;
        auto *NewPipeline = SDL_CreateGPUGraphicsPipeline(Device, &Info);
        if (!NewPipeline)
        {
            throw detail::makeError("SDL_CreateGPUGraphicsPipeline");
        }
        if (Pipeline)
        {
            SDL_ReleaseGPUGraphicsPipeline(Device, Pipeline);
        }
        Pipeline = NewPipeline;
        PipelineFormat = Format;
    }

    void uploadTexture(SDL_GPUTexture *Texture, const UITextureUpdate &Update)
    {
        const auto Size = validatePixels(Update.Width, Update.Height, Update.Pixels);
        TransferResource Transfer(Device);
        const SDL_GPUTransferBufferCreateInfo Info{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, Size, 0};
        Transfer.Pointer = SDL_CreateGPUTransferBuffer(Device, &Info);
        if (!Transfer.Pointer)
        {
            throw detail::makeError("SDL_CreateGPUTransferBuffer");
        }
        auto *Mapped = SDL_MapGPUTransferBuffer(Device, Transfer.Pointer, false);
        if (!Mapped)
        {
            throw detail::makeError("SDL_MapGPUTransferBuffer");
        }
        std::memcpy(Mapped, Update.Pixels.data(), Size);
        SDL_UnmapGPUTransferBuffer(Device, Transfer.Pointer);
        CommandScope Command(Device);
        auto *Pass = SDL_BeginGPUCopyPass(Command.Pointer);
        if (!Pass)
        {
            throw detail::makeError("SDL_BeginGPUCopyPass");
        }
        const SDL_GPUTextureTransferInfo Source{Transfer.Pointer, 0, Update.Width, Update.Height};
        const SDL_GPUTextureRegion Destination{Texture,       0, 0, Update.X, Update.Y, 0, Update.Width,
                                               Update.Height, 1};
        SDL_UploadToGPUTexture(Pass, &Source, &Destination, false);
        SDL_EndGPUCopyPass(Pass);
        Command.submit();
    }

    SDL_GPUTexture *makeTexture(const UITextureData &Data)
    {
        validatePixels(Data.Width, Data.Height, Data.Pixels);
        SDL_GPUTextureCreateInfo Info{};
        Info.type = SDL_GPU_TEXTURETYPE_2D;
        Info.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        Info.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        Info.width = Data.Width;
        Info.height = Data.Height;
        Info.layer_count_or_depth = 1;
        Info.num_levels = 1;
        Info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        if (!SDL_GPUTextureSupportsFormat(Device, Info.format, Info.type, Info.usage))
        {
            Error::raise(EErrorCode::Runtime, "SDL GPU device does not support sampled RGBA8 textures");
        }
        TextureResource Texture(Device);
        Texture.Pointer = SDL_CreateGPUTexture(Device, &Info);
        if (!Texture.Pointer)
        {
            throw detail::makeError("SDL_CreateGPUTexture");
        }
        uploadTexture(Texture.Pointer, {0, 0, Data.Width, Data.Height, Data.Pixels});
        return std::exchange(Texture.Pointer, nullptr);
    }

    // @note 渲染并呈现当前活动帧；返回呈现是否成功。
    bool canRender()
    {
        if (!Active)
        {
            Error::raise(EErrorCode::Logic, "endFrame requires canBeginFrame");
        }
        Active = false;
        auto Submitted = std::move(Lists);
        Lists.clear();
        const bool WantReuse = ReuseRetainedGeometry;
        ReuseRetainedGeometry = false;
        ensurePipeline();
        std::size_t VertexCount{}, IndexCount{};
        for (const auto &List : Submitted)
        {
            if (List.Vertices.size() > std::numeric_limits<Uint32>::max() / sizeof(UIVertex) - VertexCount ||
                List.Indices.size() > std::numeric_limits<Uint32>::max() / sizeof(Uint32) - IndexCount)
            {
                Error::raise(EErrorCode::InvalidArgument, "Combined UI submissions exceed GPU buffer limits");
            }
            VertexCount += List.Vertices.size();
            IndexCount += List.Indices.size();
        }
        const auto VertexBytes = checkedBytes(VertexCount, sizeof(UIVertex));
        const auto IndexBytes = checkedBytes(IndexCount, sizeof(Uint32));
        if (IndexBytes > std::numeric_limits<Uint32>::max() - VertexBytes)
        {
            Error::raise(EErrorCode::InvalidArgument, "Combined UI upload exceeds GPU transfer limits");
        }
        const bool Reuse = WantReuse && VertexBytes && IndexBytes && RetainedVertices && RetainedIndices &&
                           RetainedVertexCount == VertexCount && RetainedIndexCount == IndexCount;
        BufferResource Vertices(Device), Indices(Device);
        TransferResource Transfer(Device);
        SDL_GPUBuffer *VertexBuffer{};
        SDL_GPUBuffer *IndexBuffer{};
        if (Reuse)
        {
            VertexBuffer = RetainedVertices->Pointer;
            IndexBuffer = RetainedIndices->Pointer;
        }
        else if (VertexBytes && IndexBytes)
        {
            const SDL_GPUBufferCreateInfo VertexInfo{SDL_GPU_BUFFERUSAGE_VERTEX, VertexBytes, 0};
            const SDL_GPUBufferCreateInfo IndexInfo{SDL_GPU_BUFFERUSAGE_INDEX, IndexBytes, 0};
            Vertices.Pointer = SDL_CreateGPUBuffer(Device, &VertexInfo);
            Indices.Pointer = SDL_CreateGPUBuffer(Device, &IndexInfo);
            if (!Vertices.Pointer || !Indices.Pointer)
            {
                throw detail::makeError("SDL_CreateGPUBuffer");
            }
            const SDL_GPUTransferBufferCreateInfo TransferInfo{SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
                                                               VertexBytes + IndexBytes, 0};
            Transfer.Pointer = SDL_CreateGPUTransferBuffer(Device, &TransferInfo);
            if (!Transfer.Pointer)
            {
                throw detail::makeError("SDL_CreateGPUTransferBuffer");
            }
            auto *Mapped = static_cast<std::byte *>(SDL_MapGPUTransferBuffer(Device, Transfer.Pointer, false));
            if (!Mapped)
            {
                throw detail::makeError("SDL_MapGPUTransferBuffer");
            }
            std::size_t VertexOffset{}, IndexOffset{VertexBytes};
            for (const auto &List : Submitted)
            {
                const auto Size = List.Vertices.size() * sizeof(UIVertex);
                const auto IndicesSize = List.Indices.size() * sizeof(Uint32);
                if (Size)
                {
                    std::memcpy(Mapped + VertexOffset, List.Vertices.data(), Size);
                }
                if (IndicesSize)
                {
                    std::memcpy(Mapped + IndexOffset, List.Indices.data(), IndicesSize);
                }
                VertexOffset += Size;
                IndexOffset += IndicesSize;
            }
            SDL_UnmapGPUTransferBuffer(Device, Transfer.Pointer);
            VertexBuffer = Vertices.Pointer;
            IndexBuffer = Indices.Pointer;
        }
        CommandScope Command(Device);
        if (Transfer.Pointer)
        {
            auto *Pass = SDL_BeginGPUCopyPass(Command.Pointer);
            if (!Pass)
            {
                throw detail::makeError("SDL_BeginGPUCopyPass");
            }
            const SDL_GPUTransferBufferLocation VertexSource{Transfer.Pointer, 0};
            const SDL_GPUTransferBufferLocation IndexSource{Transfer.Pointer, VertexBytes};
            const SDL_GPUBufferRegion VertexDestination{Vertices.Pointer, 0, VertexBytes};
            const SDL_GPUBufferRegion IndexDestination{Indices.Pointer, 0, IndexBytes};
            SDL_UploadToGPUBuffer(Pass, &VertexSource, &VertexDestination, false);
            SDL_UploadToGPUBuffer(Pass, &IndexSource, &IndexDestination, false);
            SDL_EndGPUCopyPass(Pass);
        }
        SDL_GPUTexture *Swapchain{};
        Uint32 Width{}, Height{};
        detail::check(
            SDL_WaitAndAcquireGPUSwapchainTexture(Command.Pointer, State->Window, &Swapchain, &Width, &Height),
            "SDL_WaitAndAcquireGPUSwapchainTexture");
        Command.AcquiredSwapchain = Swapchain != nullptr;
        if (!Swapchain)
        {
            Command.submit();
            return false;
        }
        if (!Width || !Height || Width > static_cast<Uint32>(std::numeric_limits<int>::max()) ||
            Height > static_cast<Uint32>(std::numeric_limits<int>::max()))
        {
            Error::raise(EErrorCode::Runtime, "Invalid acquired UI swapchain dimensions");
        }
        ensurePipeline();
        SDL_GPUColorTargetInfo Target{};
        Target.texture = Swapchain;
        Target.clear_color = {Frame.ClearColor.R, Frame.ClearColor.G, Frame.ClearColor.B, Frame.ClearColor.A};
        Target.load_op = SDL_GPU_LOADOP_CLEAR;
        Target.store_op = SDL_GPU_STOREOP_STORE;
        auto *Pass = SDL_BeginGPURenderPass(Command.Pointer, &Target, 1, nullptr);
        if (!Pass)
        {
            throw detail::makeError("SDL_BeginGPURenderPass");
        }
        SDL_BindGPUGraphicsPipeline(Pass, Pipeline);
        const SDL_GPUViewport Viewport{0, 0, float(Width), float(Height), 0, 1};
        SDL_SetGPUViewport(Pass, &Viewport);
        const std::array<float, 4> Transform{2.0f / Frame.LogicalSize.Width, -2.0f / Frame.LogicalSize.Height,
                                             -1.0f, 1.0f};
        SDL_PushGPUVertexUniformData(Command.Pointer, 0, Transform.data(), sizeof(Transform));
        Uint32 VertexOffset{}, IndexOffset{};
        for (const auto &List : Submitted)
        {
            if (!List.Batches.empty() && !List.Vertices.empty() && !List.Indices.empty() && VertexBuffer &&
                IndexBuffer)
            {
                const SDL_GPUBufferBinding VertexBinding{VertexBuffer, VertexOffset};
                const SDL_GPUBufferBinding IndexBinding{IndexBuffer, IndexOffset};
                SDL_BindGPUVertexBuffers(Pass, 0, &VertexBinding, 1);
                SDL_BindGPUIndexBuffer(Pass, &IndexBinding, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                for (const auto &Batch : List.Batches)
                {
                    if (!Batch.IndexCount)
                    {
                        continue;
                    }
                    const auto Scissor = makeScissor(Batch.Clip, Frame.LogicalSize, Width, Height);
                    if (!Scissor.w || !Scissor.h)
                    {
                        continue;
                    }
                    SDL_SetGPUScissor(Pass, &Scissor);
                    auto *Texture =
                        Batch.Texture.Value ? Textures.find(Batch.Texture.Value)->second.Pointer : White;
                    const SDL_GPUTextureSamplerBinding Binding{Texture, Sampler};
                    SDL_BindGPUFragmentSamplers(Pass, 0, &Binding, 1);
                    SDL_DrawGPUIndexedPrimitives(Pass, Batch.IndexCount, 1, Batch.FirstIndex, 0, 0);
                }
            }
            VertexOffset += static_cast<Uint32>(List.Vertices.size() * sizeof(UIVertex));
            IndexOffset += static_cast<Uint32>(List.Indices.size() * sizeof(Uint32));
        }
        SDL_EndGPURenderPass(Pass);
        Command.submit();

        // 非复用路径：把本帧缓冲提升为跨帧保留缓冲；旧缓冲延后一帧释放。
        if (!Reuse && VertexBytes && IndexBytes)
        {
            RetainedVertices = std::make_unique<BufferResource>(Device);
            RetainedVertices->Pointer = Vertices.Pointer;
            Vertices.Pointer = nullptr;
            RetainedIndices = std::make_unique<BufferResource>(Device);
            RetainedIndices->Pointer = Indices.Pointer;
            Indices.Pointer = nullptr;
            RetainedVertexCount = VertexCount;
            RetainedIndexCount = IndexCount;
        }
        else if (!Reuse)
        {
            RetainedVertices.reset();
            RetainedIndices.reset();
            RetainedVertexCount = 0;
            RetainedIndexCount = 0;
        }
        return true;
    }
};

UIRenderer::UIRenderer(IGenericWindow &Window, EGpuBackend Backend, bool Debug) : ImplPtr(std::make_unique<Impl>())
{
    const auto *NativeWindow = dynamic_cast<const SdlWindow *>(&Window);
    if (!NativeWindow)
    {
        Error::raise(EErrorCode::InvalidArgument, "SDL GPU renderer requires an SDL window");
    }
    ImplPtr->State = NativeWindow->getState();
    if (ImplPtr->State->GPUClaimed)
    {
        Error::raise(EErrorCode::Runtime, "Window already has a GPU renderer");
    }
    const char *Driver{};
    SDL_GPUShaderFormat Formats = SDL_GPU_SHADERFORMAT_SPIRV;
#ifdef EDITOR_UI_HAS_MSL
    Formats |= SDL_GPU_SHADERFORMAT_MSL;
#endif
#ifdef EDITOR_UI_HAS_DXIL
    Formats |= SDL_GPU_SHADERFORMAT_DXIL;
#endif
    switch (Backend)
    {
    case EGpuBackend::Vulkan:
        Driver = "vulkan";
        Formats = SDL_GPU_SHADERFORMAT_SPIRV;
        break;
    case EGpuBackend::Direct3D12:
#ifdef EDITOR_UI_HAS_DXIL
        Driver = "direct3d12";
        Formats = SDL_GPU_SHADERFORMAT_DXIL;
        break;
#else
        Error::raise(EErrorCode::Runtime, "Direct3D12 UI rendering requires EDITOR_UI_BUILD_DXIL=ON and dxc");
#endif
    case EGpuBackend::Metal:
#ifdef EDITOR_UI_HAS_MSL
        Driver = "metal";
        Formats = SDL_GPU_SHADERFORMAT_MSL;
        break;
#else
        Error::raise(EErrorCode::Runtime, "Metal UI rendering requires EDITOR_UI_BUILD_MSL=ON and spirv-cross");
#endif
    case EGpuBackend::Auto:
        break;
    default:
        Error::raise(EErrorCode::InvalidArgument, "Unknown GPU backend");
    }
    ImplPtr->Device = SDL_CreateGPUDevice(Formats, Debug, Driver);
    if (!ImplPtr->Device)
    {
        throw detail::makeError("SDL_CreateGPUDevice");
    }
    detail::check(SDL_ClaimWindowForGPUDevice(ImplPtr->Device, ImplPtr->State->Window),
                  "SDL_ClaimWindowForGPUDevice");
    ImplPtr->Claimed = true;
    ImplPtr->State->GPUClaimed = true;
    detail::check(SDL_SetGPUSwapchainParameters(ImplPtr->Device, ImplPtr->State->Window,
                                                SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_VSYNC),
                  "SDL_SetGPUSwapchainParameters");
    ImplPtr->VertexShader = ImplPtr->loadShader("ui.vert", SDL_GPU_SHADERSTAGE_VERTEX);
    ImplPtr->FragmentShader = ImplPtr->loadShader("ui.frag", SDL_GPU_SHADERSTAGE_FRAGMENT);
    SDL_GPUSamplerCreateInfo SamplerInfo{};
    SamplerInfo.min_filter = SDL_GPU_FILTER_LINEAR;
    SamplerInfo.mag_filter = SDL_GPU_FILTER_LINEAR;
    SamplerInfo.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    SamplerInfo.address_mode_u = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SamplerInfo.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    SamplerInfo.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    ImplPtr->Sampler = SDL_CreateGPUSampler(ImplPtr->Device, &SamplerInfo);
    if (!ImplPtr->Sampler)
    {
        throw detail::makeError("SDL_CreateGPUSampler");
    }
    const std::array<std::byte, 4> White{std::byte{255}, std::byte{255}, std::byte{255}, std::byte{255}};
    ImplPtr->White = ImplPtr->makeTexture({1, 1, White});
    ImplPtr->ensurePipeline();
}

UIRenderer::~UIRenderer() = default;

std::string UIRenderer::getBackendName() const
{
    return SDL_GetGPUDeviceDriver(ImplPtr->Device);
}

bool UIRenderer::canBeginFrame(const UIFrameInfo &Frame)
{
    if (ImplPtr->Active)
    {
        Error::raise(EErrorCode::Logic, "Nested UI frames are forbidden");
    }
    if (Frame.LogicalSize.Width <= 0 || Frame.LogicalSize.Height <= 0 || Frame.PixelSize.Width < 0 ||
        Frame.PixelSize.Height < 0 || !isFiniteColor(Frame.ClearColor))
    {
        Error::raise(EErrorCode::InvalidArgument, "Invalid UI frame dimensions or clear color");
    }
    int Width{}, Height{};
    detail::check(SDL_GetWindowSizeInPixels(ImplPtr->State->Window, &Width, &Height), "SDL_GetWindowSizeInPixels");
    if ((SDL_GetWindowFlags(ImplPtr->State->Window) & SDL_WINDOW_MINIMIZED) || Width <= 0 || Height <= 0)
    {
        return false;
    }
    ImplPtr->Frame = Frame;
    ImplPtr->Lists.clear();
    ImplPtr->Active = true;
    return true;
}

void UIRenderer::submitDrawList(const UIDrawList &DrawList, bool ReuseBuffers)
{
    if (!ImplPtr->Active)
    {
        Error::raise(EErrorCode::Logic, "submitDrawList requires canBeginFrame");
    }
    DrawList.validate();
    checkedBytes(DrawList.Vertices.size(), sizeof(UIVertex));
    checkedBytes(DrawList.Indices.size(), sizeof(Uint32));
    for (const auto &Vertex : DrawList.Vertices)
    {
        if (!std::isfinite(Vertex.Position.X) || !std::isfinite(Vertex.Position.Y) || !std::isfinite(Vertex.UV.X) ||
            !std::isfinite(Vertex.UV.Y) || !isFiniteColor(Vertex.Tint))
        {
            Error::raise(EErrorCode::InvalidArgument, "UI vertices must contain finite values");
        }
    }
    for (const auto Index : DrawList.Indices)
    {
        if (Index >= DrawList.Vertices.size())
        {
            Error::raise(EErrorCode::InvalidArgument, "UI index out of bounds");
        }
    }
    for (const auto &Batch : DrawList.Batches)
    {
        if (Batch.FirstIndex > DrawList.Indices.size() ||
            Batch.IndexCount > DrawList.Indices.size() - Batch.FirstIndex || Batch.IndexCount % 3)
        {
            Error::raise(EErrorCode::InvalidArgument, "Invalid UI triangle batch index range");
        }
        if (Batch.Texture.Value)
        {
            ImplPtr->findTexture(Batch.Texture);
        }
        if (Batch.Clip &&
            (!std::isfinite(Batch.Clip->X) || !std::isfinite(Batch.Clip->Y) || !std::isfinite(Batch.Clip->Width) ||
             !std::isfinite(Batch.Clip->Height) || Batch.Clip->Width < 0 || Batch.Clip->Height < 0))
        {
            Error::raise(EErrorCode::InvalidArgument, "Invalid UI batch clip rectangle");
        }
    }
    ImplPtr->Lists.push_back(DrawList);
    ImplPtr->ReuseRetainedGeometry = ReuseBuffers;
}

void UIRenderer::endFrame()
{
    ImplPtr->canRender();
}

void UIRenderer::setUIMode(EUIMode Mode) noexcept
{
    ImplPtr->Mode = Mode;
}

EUIMode UIRenderer::getUIMode() const noexcept
{
    return ImplPtr->Mode;
}

bool UIRenderer::canClear(Color ClearColor)
{
    if (ImplPtr->Active)
    {
        Error::raise(EErrorCode::Logic, "canClear is forbidden during an active UI frame");
    }
    int Width{}, Height{}, PixelWidth{}, PixelHeight{};
    detail::check(SDL_GetWindowSize(ImplPtr->State->Window, &Width, &Height), "SDL_GetWindowSize");
    detail::check(SDL_GetWindowSizeInPixels(ImplPtr->State->Window, &PixelWidth, &PixelHeight),
                  "SDL_GetWindowSizeInPixels");
    if ((SDL_GetWindowFlags(ImplPtr->State->Window) & SDL_WINDOW_MINIMIZED) || Width <= 0 || Height <= 0 ||
        PixelWidth <= 0 || PixelHeight <= 0)
    {
        return false;
    }
    if (!canBeginFrame({{Width, Height}, {PixelWidth, PixelHeight}, ClearColor}))
    {
        return false;
    }
    return ImplPtr->canRender();
}

TextureHandle UIRenderer::createTexture(const UITextureData &Data)
{
    ImplPtr->requireIdle();
    const TextureHandle Handle{allocateTextureHandle()};
    TextureResource Texture(ImplPtr->Device);
    Texture.Pointer = ImplPtr->makeTexture(Data);
    ImplPtr->Textures.emplace(Handle.Value, Impl::TextureRecord{Texture.Pointer, Data.Width, Data.Height});
    Texture.Pointer = nullptr;
    return Handle;
}

void UIRenderer::updateTexture(TextureHandle Texture, const UITextureUpdate &Update)
{
    ImplPtr->requireIdle();
    const auto &Record = ImplPtr->findTexture(Texture);
    validatePixels(Update.Width, Update.Height, Update.Pixels);
    if (Update.X > Record.Width || Update.Y > Record.Height || Update.Width > Record.Width - Update.X ||
        Update.Height > Record.Height - Update.Y)
    {
        Error::raise(EErrorCode::InvalidArgument, "UI texture update is outside the texture bounds");
    }
    ImplPtr->uploadTexture(Record.Pointer, Update);
}

void UIRenderer::destroyTexture(TextureHandle Texture)
{
    ImplPtr->requireIdle();
    const auto &Record = ImplPtr->findTexture(Texture);
    SDL_ReleaseGPUTexture(ImplPtr->Device, Record.Pointer);
    ImplPtr->Textures.erase(Texture.Value);
}
}