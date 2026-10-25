#include <renderer/RendererServer.hpp>

#include <renderer/RenderThread.hpp>
#include <renderer/ShaderBinding.hpp>
#include <RHIServer.hpp>

#include <atomic>
#include <fstream>
#include <future>
#include <stdexcept>
#include <vector>
#include <array>

namespace runtime::renderer
{
struct RendererServer::Implementation
{
    struct FrameContext
    {
        std::shared_ptr<rhi::RCommandList> Commands;
        std::shared_ptr<rhi::RSemaphore> ImageAvailable;
        std::shared_ptr<rhi::RQueryPool> TimestampQueries;
        std::shared_ptr<rhi::RFence> FrameFence;
        uint64_t CompletionValue { 0 };
        bool TimestampPending { false };
    };

    static constexpr uint32_t FramesInFlight = 3;
    runtime::platform::GenericWindowPointer Window;
    std::shared_ptr<rhi::RDevice> Device;
    std::shared_ptr<rhi::RQueue> GraphicsQueue;
    std::shared_ptr<rhi::RSwapchain> Swapchain;
    std::shared_ptr<rhi::RSemaphore> FrameTimeline;
    bool UseTimelineSemaphore { false };
    std::array<FrameContext, FramesInFlight> Frames;
    std::vector<std::shared_ptr<rhi::RSemaphore>> RenderFinishedSemaphores;
    std::vector<bool> ImageInitialized;
    uint32_t FrameIndex { 0 };
    uint64_t NextTimelineValue { 1 };
    uint32_t Width { 0 };
    uint32_t Height { 0 };
    std::atomic<bool> SurfaceRecoveryRequired { false };
    std::atomic<double> LastGPUFrameTimeNanoseconds { -1.0 };

    // 渲染世界与 UI 渲染器(Renderer 持有的高层状态).
    RenderProxyWorld ProxyWorld;
    RenderFrameSubmission LastSubmission;

    // 待消费的 UI 绘制列表(GameThread 提交, buildRenderSubmission 消费后清空).
    std::vector<ui::UIDrawList> PendingUIDrawLists;
    // 与绘制列表配套的纹理脏区(同上生命周期).
    std::vector<ui::UISampledTextureUpload> PendingTextureUploads;

    // 场景渲染器(PassScheduler); 当前帧相机矩阵由 RenderThreadWorker 持有(命令投递).
    RenderPassScheduler SceneRenderer;

    // 当前渲染视口(Editor 3DView 面板或 standalone 主视口); GameThread 写入, 只读访问.
    Viewport ActiveViewport {};

    // 3D 场景在最终画面中的像素矩形(编辑器 Viewport 面板); 宽高为 0 表示整屏.
    rhi::RenderArea SceneViewport {};

    // RDG 瞬态资源池(placed resource 别名); 跨帧复用, 生命周期与设备一致.
    std::unique_ptr<rdg::RDGResourcePool> RDGPool;

    /**
     * @brief 渲染线程: 消费渲染命令 + 串行执行帧录制/提交任务.
     *
     * renderFrame / renderFrameGraph 在 GameThread 上完成 World→Submission 的
     * 预处理后, 把"RDG 构建 + 录制 + 提交"打包成任务投递到这里, 由渲染线程执行,
     * 从而把 RHI 的 CPU 侧开销从 GameThread 挪走(帧 pacing 仍由下方 fence 承担).
     */
    RenderThread RenderThreadWorker;

    /**
     * @brief 一帧录制回调: 接收命令列表、交换链图像/视图、尺寸与图像是否已初始化.
     * 由 renderFrame(FrameRecorder) 与 renderFrameGraph(FrameGraphSetup) 复用同一
     * Acquire → Record → Submit → Present → Retire 帧边界.
     */
    using RecordFn = std::function<void(rhi::RCommandList&, const std::shared_ptr<rhi::RImage>&,
        const std::shared_ptr<rhi::RImageView>&, uint32_t, uint32_t, bool)>;
    RendererServer::EFrameStatus renderFrameImpl(const RecordFn& Record);

    void collectTimestamp(FrameContext& Frame)
    {
        if (!Frame.TimestampPending || !Frame.TimestampQueries) return;
        std::array<uint64_t, 2> values {};
        if (!Frame.TimestampQueries->getResults(0, values, false)) return;
        const uint32_t valid_bits = Device->getLimits().TimestampValidBits;
        if (valid_bits == 0) return;
        const uint64_t mask = valid_bits >= 64
            ? std::numeric_limits<uint64_t>::max()
            : (uint64_t { 1 } << valid_bits) - 1;
        const uint64_t delta = (values[1] - values[0]) & mask;
        LastGPUFrameTimeNanoseconds.store(
            Frame.TimestampQueries->timestampTicksToNanoseconds(delta),
            std::memory_order_relaxed);
        Frame.TimestampPending = false;
    }

    void create(const runtime::platform::GenericWindowPointer& InWindow, const std::shared_ptr<rhi::RDevice>& InDevice)
    {
        Window = InWindow;
        Device = InDevice;
        Width = static_cast<uint32_t>(std::max(0, Window->getSize().Width));
        Height = static_cast<uint32_t>(std::max(0, Window->getSize().Height));
        GraphicsQueue = Device->getQueue(rhi::ECommandQueueType::Graphics);
        RDGPool = std::make_unique<rdg::RDGResourcePool>(*Device);
        // 1.0 基线: 时间线信号量不可用时用 per-frame fence 做帧 pacing 与延迟释放.
        UseTimelineSemaphore = Device->getFeatures().TimelineSemaphore;
        if (UseTimelineSemaphore)
            FrameTimeline = Device->createTimelineSemaphore();
        for (auto& frame : Frames)
        {
            frame.Commands = Device->createCommandList();
            frame.ImageAvailable = Device->createSemaphore();
            if (!UseTimelineSemaphore)
                frame.FrameFence = Device->createFence();
            if (Device->getFeatures().TimestampQueries)
                frame.TimestampQueries = Device->createQueryPool({
                    .Type = rhi::EQueryType::Timestamp,
                    .Count = 2,
                    .DebugName = "RendererFrameTimestamps"
                });
        }
        rhi::SwapchainDescriptor desc;
        desc.Width = Width;
        desc.Height = Height;
        desc.ImageUsage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
            rhi::EImageUsage_t::TransferDst;
        desc.MinimumImageCount = FramesInFlight;
        desc.DebugName = "MainSwapchain";
        Swapchain = Device->createSwapchain(desc);
        Width = Swapchain->getDescriptor().Width;
        Height = Swapchain->getDescriptor().Height;
        ImageInitialized.assign(Swapchain->getImageCount(), false);
        RenderFinishedSemaphores.resize(Swapchain->getImageCount());
        for (auto& semaphore : RenderFinishedSemaphores)
            semaphore = Device->createSemaphore();

        // 注册默认 Pass 链(声明顺序即执行顺序, 拓扑由 RDG 后续接管):
        //   Shadow → DepthPre → Opaque → Transparent → PostProcess → UI.
        SceneRenderer.addPass(std::make_unique<ShadowRenderPass>());
        SceneRenderer.addPass(std::make_unique<DepthPrePass>());
        SceneRenderer.addPass(std::make_unique<OpaqueRenderPass>());
        SceneRenderer.addPass(std::make_unique<TransparentRenderPass>());
        SceneRenderer.addPass(std::make_unique<PostProcessRenderPass>());
        SceneRenderer.addPass(std::make_unique<UIRenderPass>());

        // 最后启动渲染线程: 保证其上所有资源已就绪.
        RenderThreadWorker.start();
    }

    void recreate(uint32_t NewWidth, uint32_t NewHeight)
    {
        Width = NewWidth;
        Height = NewHeight;
        Swapchain->recreate(Width, Height);
        Width = Swapchain->getDescriptor().Width;
        Height = Swapchain->getDescriptor().Height;
        GraphicsQueue->poll();
        for (auto& frame : Frames)
        {
            frame.ImageAvailable = Device->createSemaphore();
        }
        ImageInitialized.assign(Swapchain->getImageCount(), false);
        RenderFinishedSemaphores.resize(Swapchain->getImageCount());
        for (auto& semaphore : RenderFinishedSemaphores)
            semaphore = Device->createSemaphore();
    }

    bool recoverSurface()
    {
        if (!SurfaceRecoveryRequired.load(std::memory_order_relaxed) ||
            Device->getStatus() != rhi::EDeviceStatus::Ready)
            return false;
        auto* rhi_instance = rhi::RenderServer::self().getRHI();
        if (!rhi_instance || !rhi_instance->recoverSurface(Window) ||
            !Swapchain->recoverSurface(Width, Height))
            return false;
        for (auto& frame : Frames) frame.ImageAvailable = Device->createSemaphore();
        ImageInitialized.assign(Swapchain->getImageCount(), false);
        RenderFinishedSemaphores.assign(Swapchain->getImageCount(), {});
        for (auto& semaphore : RenderFinishedSemaphores) semaphore = Device->createSemaphore();
        SurfaceRecoveryRequired.store(false, std::memory_order_relaxed);
        return true;
    }
};

RendererServer::EFrameStatus RendererServer::Implementation::renderFrameImpl(const RecordFn& Record)
{
    if (!Device || !Swapchain)
        throw std::logic_error("Renderer must be initialized before rendering a frame.");
    if (Device->getStatus() != rhi::EDeviceStatus::Ready)
        return RendererServer::EFrameStatus::DeviceLost;
    if (SurfaceRecoveryRequired.load(std::memory_order_relaxed) ||
        Swapchain->getStatus() == rhi::ESwapchainStatus::SurfaceLost)
    {
        SurfaceRecoveryRequired.store(true, std::memory_order_relaxed);
        return RendererServer::EFrameStatus::SurfaceLost;
    }
    Device->collectDeferredReleases();
    if (Width == 0 || Height == 0)
        return RendererServer::EFrameStatus::Skipped;

    auto& frame = Frames[FrameIndex];
    if (frame.CompletionValue != 0)
    {
        if (UseTimelineSemaphore)
        {
            if (!FrameTimeline->wait(frame.CompletionValue))
                return Device->getStatus() == rhi::EDeviceStatus::Ready
                    ? RendererServer::EFrameStatus::Skipped
                    : RendererServer::EFrameStatus::DeviceLost;
        }
        else if (!frame.FrameFence->wait())
        {
            return Device->getStatus() == rhi::EDeviceStatus::Ready
                ? RendererServer::EFrameStatus::Skipped
                : RendererServer::EFrameStatus::DeviceLost;
        }
        GraphicsQueue->poll();
        Device->collectDeferredReleases();
        if (!UseTimelineSemaphore)
            frame.FrameFence->reset();
        collectTimestamp(frame);
        frame.Commands->reset();
    }

    const rhi::AcquireResult acquired = Swapchain->acquireNextImage(frame.ImageAvailable);
    if (acquired.Status == rhi::EAcquireStatus::OutOfDate)
    {
        recreate(Width, Height);
        return RendererServer::EFrameStatus::SwapchainRecreated;
    }
    if (acquired.Status == rhi::EAcquireStatus::DeviceLost)
        return RendererServer::EFrameStatus::DeviceLost;
    if (acquired.Status == rhi::EAcquireStatus::SurfaceLost)
    {
        SurfaceRecoveryRequired.store(true, std::memory_order_relaxed);
        return RendererServer::EFrameStatus::SurfaceLost;
    }
    if (acquired.Status == rhi::EAcquireStatus::NotReady)
        return RendererServer::EFrameStatus::Skipped;

    const auto& image = Swapchain->getImage(acquired.ImageIndex);
    const auto& view = Swapchain->getImageView(acquired.ImageIndex);
    frame.Commands->begin();
    if (frame.TimestampQueries)
    {
        frame.Commands->resetQueries(frame.TimestampQueries, 0, 2);
        frame.Commands->writeTimestamp(frame.TimestampQueries, 0);
    }

    // 录制(单渲染作用域回调 或 RDG 多 Pass), 由调用方提供.
    Record(*frame.Commands, image, view, Width, Height, ImageInitialized[acquired.ImageIndex]);

    if (frame.TimestampQueries)
        frame.Commands->writeTimestamp(frame.TimestampQueries, 1);
    frame.Commands->end();

    const uint64_t completion_value = NextTimelineValue++;
    const auto& render_finished = RenderFinishedSemaphores[acquired.ImageIndex];
    const std::array waits { rhi::SemaphoreSubmitInfo { frame.ImageAvailable, 0 } };
    const std::array commands { frame.Commands };
    try
    {
        if (UseTimelineSemaphore)
        {
            const std::array signals {
                rhi::SemaphoreSubmitInfo { render_finished, 0 },
                rhi::SemaphoreSubmitInfo { FrameTimeline, completion_value }
            };
            GraphicsQueue->submit({
                .CommandLists = commands,
                .WaitSemaphores = waits,
                .SignalSemaphores = signals
            });
        }
        else
        {
            const std::array signals { rhi::SemaphoreSubmitInfo { render_finished, 0 } };
            GraphicsQueue->submit({
                .CommandLists = commands,
                .WaitSemaphores = waits,
                .SignalSemaphores = signals,
                .Fence = frame.FrameFence
            });
        }
    }
    catch (...)
    {
        if (Device->getStatus() != rhi::EDeviceStatus::Ready)
            return RendererServer::EFrameStatus::DeviceLost;
        throw;
    }
    frame.CompletionValue = completion_value;
    frame.TimestampPending = frame.TimestampQueries != nullptr;
    ImageInitialized[acquired.ImageIndex] = true;

    const std::array present_waits { render_finished };
    const auto present_status = GraphicsQueue->present({
        .Swapchain = Swapchain,
        .ImageIndex = acquired.ImageIndex,
        .Generation = acquired.Generation,
        .WaitSemaphores = present_waits
    });
    FrameIndex = (FrameIndex + 1) % Implementation::FramesInFlight;
    if (present_status == rhi::EPresentStatus::DeviceLost)
        return RendererServer::EFrameStatus::DeviceLost;
    if (present_status == rhi::EPresentStatus::OutOfDate ||
        present_status == rhi::EPresentStatus::Suboptimal)
    {
        recreate(Width, Height);
        return RendererServer::EFrameStatus::SwapchainRecreated;
    }
    if (present_status == rhi::EPresentStatus::SurfaceLost)
    {
        SurfaceRecoveryRequired.store(true, std::memory_order_relaxed);
        return RendererServer::EFrameStatus::SurfaceLost;
    }
    if (acquired.Status == rhi::EAcquireStatus::Suboptimal)
    {
        recreate(Width, Height);
        return RendererServer::EFrameStatus::SwapchainRecreated;
    }
    return RendererServer::EFrameStatus::Rendered;
}

namespace
{

std::vector<std::byte> loadBinaryFile(const char* Path)
{
    std::ifstream stream(Path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("Failed to open the SPIR-V reflection fixture.");
    const std::streamsize size = stream.tellg();
    if (size <= 0)
        throw std::runtime_error("SPIR-V reflection fixture is empty.");
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> result(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char*>(result.data()), size))
        throw std::runtime_error("Failed to read the SPIR-V reflection fixture.");
    return result;
}

} // namespace

RendererServer::RendererServer() = default;

RendererServer& RendererServer::self() noexcept
{
    static RendererServer instance;
    return instance;
}

RendererServer::~RendererServer()
{
    shutdown();
}

void RendererServer::initialize(
    rhi::ESupportedBackendAPI BackendAPI,
    const runtime::platform::GenericWindowPointer& Window)
{
    rhi::RenderServer::self().initialize(BackendAPI, Window);
    Impl = std::make_unique<Implementation>();
    Impl->create(Window, rhi::RenderServer::self().getDevice());
}

void RendererServer::shutdown() noexcept
{
    // 先停渲染线程: 它可能正在录制/提交, 必须在设备 waitIdle 前合入.
    // RenderThreadWorker 析构会再次 requestStop/join(幂等), 此处显式调用保证顺序.
    if (Impl)
    {
        try
        {
            Impl->RenderThreadWorker.requestStop();
            Impl->RenderThreadWorker.join();
        }
        catch (...)
        {
            // 关闭路径不得抛出.
        }
    }

    auto& server = rhi::RenderServer::self();
    if (const auto& device = server.getDevice())
    {
        try
        {
            device->waitIdle();
            device->collectDeferredReleases();
        }
        catch (...)
        {
            // Destructors and failure cleanup must not throw; RHI shutdown remains safe.
        }
    }
    // 释放 UI 绘制缓存(静态持有 RHI 管线/缓冲), 必须在设备销毁前执行.
    releaseUIDrawCache();
    Impl.reset();
    server.shutdown();
}

bool RendererServer::isInitialized() const noexcept
{
    return rhi::RenderServer::self().isInitialized();
}

const std::shared_ptr<rhi::RDevice>& RendererServer::getDevice() const noexcept
{
    return rhi::RenderServer::self().getDevice();
}

std::optional<double> RendererServer::getLastGPUFrameTimeNanoseconds() const noexcept
{
    if (!Impl)
        return std::nullopt;
    const double Value = Impl->LastGPUFrameTimeNanoseconds.load(std::memory_order_relaxed);
    return Value < 0.0 ? std::nullopt : std::optional<double> { Value };
}

bool RendererServer::deferRelease(std::shared_ptr<void> Resource)
{
    if (!Impl || !Impl->Device || !Resource)
        return false;
    // 帧索引 / 时间线值归渲染线程所有, 因此延迟释放也必须在渲染线程上执行, 避免竞争.
    std::promise<bool> Promise;
    std::future<bool> Future = Promise.get_future();
    Impl->RenderThreadWorker.postTask([this, Resource = std::move(Resource),
        Promise = std::move(Promise)]() mutable {
        bool Ok = false;
        if (Impl->UseTimelineSemaphore && Impl->FrameTimeline)
        {
            // 时间线值快照了本次调用前所有提交; 设备队列持有它们直到完成, 收集时不等待.
            Ok = Impl->Device->deferRelease(
                std::move(Resource), Impl->FrameTimeline, Impl->NextTimelineValue - 1);
        }
        else
        {
            // 1.0 基线: 以当前帧 fence 作为完成点.
            const auto& Frame = Impl->Frames[Impl->FrameIndex];
            Ok = Frame.FrameFence &&
                Impl->Device->deferRelease(std::move(Resource), Frame.FrameFence);
        }
        Promise.set_value(Ok);
    });
    return Future.get();
}

void RendererServer::runBindGroupSmokeTest()
{
    const auto& device = getDevice();
    if (!device)
        throw std::logic_error("Renderer must be initialized before running its BindGroup smoke test.");

    const auto byte_code = loadBinaryFile(SEED_SPIRV_REFLECT_SAMPLE_PATH);
    const ShaderInterface shader = SpirvShaderReflector::reflect(
        byte_code, rhi::EShaderStage_t::Pixel, "main");
    const PipelineInterface pipeline_interface = PipelineInterfaceBuilder::merge(
        std::span<const ShaderInterface>(&shader, 1));

    ResourceBindingTable resources;
    std::vector<std::shared_ptr<rhi::RBuffer>> buffers;
    std::shared_ptr<rhi::RImage> sampled_image;
    std::shared_ptr<rhi::RImageView> sampled_view;
    std::shared_ptr<rhi::RSampler> sampler;
    for (const auto& reflected : pipeline_interface.Resources)
    {
        switch (reflected.Type)
        {
        case rhi::EDescriptorType::UniformBuffer:
        case rhi::EDescriptorType::ReadOnlyStorageBuffer:
        case rhi::EDescriptorType::ReadWriteStorageBuffer:
        {
            rhi::EBufferUsageFlags usage = reflected.Type == rhi::EDescriptorType::UniformBuffer
                ? rhi::EBufferUsageFlags(rhi::EBufferUsage::Uniform)
                : rhi::EBufferUsageFlags(rhi::EBufferUsage::Storage);
            auto buffer = device->createBuffer(rhi::RBuffer::Descriptor_t {
                .Size = reflected.BlockSize == 0 ? 256u : reflected.BlockSize,
                .Usage = usage,
                .MemoryProperty = rhi::EMemoryProperty::DeviceLocal,
                .DebugName = reflected.Name
            });
            resources.set(reflected.Id, reflected.Name, rhi::BufferBinding { buffer, 0, 0 });
            buffers.emplace_back(std::move(buffer));
            break;
        }
        case rhi::EDescriptorType::SampledTexture:
        {
            if (!sampled_image)
            {
                sampled_image = device->createImage(rhi::RImage::Descriptor_t {
                    .Format = rhi::EFormat::RGBA8_UNorm,
                    .Dimension = rhi::EImageDimension::Texture2D,
                    .Width = 1,
                    .Height = 1,
                    .Depth = 1,
                    .MipLevels = 1,
                    .ArrayLayers = 1,
                    .SharingMode = rhi::ESharingMode::Exclusive,
                    .MemoryProperty = rhi::EMemoryProperty::DeviceLocal,
                    .Usage = rhi::EImageUsage_t::Sampled,
                    .SampleCount = rhi::ESampleCount::Count1
                });
                sampled_view = device->createImageView(rhi::RImageView::Descriptor_t {
                    .Image = sampled_image
                });
            }
            resources.set(reflected.Id, reflected.Name, rhi::TextureBinding { sampled_view });
            break;
        }
        case rhi::EDescriptorType::Sampler:
        case rhi::EDescriptorType::ComparisonSampler:
        {
            if (!sampler)
                sampler = device->createSampler();
            resources.set(reflected.Id, reflected.Name, rhi::SamplerBinding { sampler });
            break;
        }
        default:
            throw std::runtime_error("Smoke-test fixture contains an unexpected resource type.");
        }
    }

    ShaderBindingResolver resolver(*device);
    auto resolved = resolver.resolve(pipeline_interface, resources);
    if (resolved.BindGroups.empty() || !resolved.PipelineLayout)
        throw std::runtime_error("Automatic BindGroup resolution returned an incomplete result.");

    auto command_list = device->createCommandList();
    command_list->begin();
    command_list->bindBindGroups(
        rhi::EPipelineType::Graphics,
        resolved.PipelineLayout,
        0,
        resolved.BindGroups);
    command_list->end();

    auto upload_buffer = device->createBuffer(rhi::RBuffer::Descriptor_t {
        .Size = 256,
        .Usage = rhi::EBufferUsage::TransferSrc,
        .MemoryUsage = rhi::EMemoryUsage::CPUToGPU,
        .PersistentlyMapped = true,
        .DebugName = "AllocatorUploadSmoke"
    });
    auto* upload_data = static_cast<std::byte*>(upload_buffer->map());
    upload_data[0] = std::byte { 0x5a };
    upload_buffer->flush(0, 1);
    upload_buffer->unmap();

    if (device->getFeatures().BufferDeviceAddress)
    {
        auto address_buffer = device->createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = 256,
            .Usage = rhi::EBufferUsageFlags(rhi::EBufferUsage::Storage) |
                rhi::EBufferUsage::DeviceAddress,
            .MemoryUsage = rhi::EMemoryUsage::GPUOnly,
            .DebugName = "BufferDeviceAddressSmoke"
        });
        if (address_buffer->getDeviceAddress() == 0)
            throw std::runtime_error("Enabled Vulkan buffer device address returned zero.");
    }

    const auto& features = device->getFeatures();
    const auto& limits = device->getLimits();
    if ((features.AccelerationStructure || features.RayTracingPipeline || features.RayQuery) &&
        !features.BufferDeviceAddress)
        throw std::runtime_error("Vulkan ray tracing was exposed without buffer device address.");
    if ((features.RayTracingPipeline || features.RayQuery) && !features.AccelerationStructure)
        throw std::runtime_error("Vulkan shader ray tracing was exposed without acceleration structures.");
    if (features.RayTracingPipeline &&
        (limits.ShaderGroupHandleSize == 0 || limits.ShaderGroupHandleAlignment == 0 ||
         limits.ShaderBindingTableAlignment == 0 || limits.MaxShaderGroupStride == 0 ||
         limits.MaxRayRecursionDepth == 0 || limits.MaxRayDispatchInvocationCount == 0))
        throw std::runtime_error("Vulkan ray-tracing pipeline limits were not populated.");
    if (features.AccelerationStructure)
    {
        auto storage = device->createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = 256,
            .Usage = rhi::EBufferUsageFlags(rhi::EBufferUsage::AccelerationStructureStorage) |
                rhi::EBufferUsage::DeviceAddress,
            .MemoryUsage = rhi::EMemoryUsage::GPUOnly,
            .DebugName = "AccelerationStructureStorageSmoke"
        });
        auto acceleration_structure = device->createAccelerationStructure({
            .Type = rhi::EAccelerationStructureType::TopLevel,
            .Storage = storage,
            .Offset = 0,
            .Size = 256,
            .DebugName = "AccelerationStructureSmoke"
        });
        if (!acceleration_structure || !acceleration_structure->isValid() ||
            acceleration_structure->getDeviceAddress() == 0)
            throw std::runtime_error("Enabled Vulkan acceleration-structure creation failed.");
        if (features.RayTracingPipeline || features.RayQuery)
        {
            const auto visibility = features.RayTracingPipeline
                ? rhi::EShaderStage_t::RayGeneration : rhi::EShaderStage_t::Compute;
            auto layout = device->createBindGroupLayout({ .Entries = {{
                .Binding = 0, .Type = rhi::EDescriptorType::AccelerationStructure,
                .ArrayCount = 1, .Visibility = visibility }},
                .DebugName = "AccelerationStructureLayoutSmoke" });
            auto group = device->createBindGroup({ .Layout = layout, .Entries = {{
                .Binding = 0, .ArrayElement = 0,
                .Resource = rhi::AccelerationStructureBinding { acceleration_structure } }},
                .DebugName = "AccelerationStructureBindGroupSmoke" });
            if (!group || !group->isValid())
                throw std::runtime_error("Vulkan acceleration-structure descriptor write failed.");
        }
    }

    bool retired = false;
    auto retirement_probe = std::shared_ptr<void>(new int(0), [&retired](void* value)
    {
        delete static_cast<int*>(value);
        retired = true;
    });
    if (Impl->UseTimelineSemaphore)
    {
        if (!device->deferRelease(retirement_probe, Impl->FrameTimeline, 0))
            throw std::runtime_error("Vulkan deferred release unexpectedly rejected a valid timeline point.");
    }
    else
    {
        auto signaled_fence = device->createFence(true);
        if (!device->deferRelease(retirement_probe, signaled_fence))
            throw std::runtime_error("Vulkan deferred release unexpectedly rejected a valid fence.");
    }
    retirement_probe.reset();
    device->collectDeferredReleases();
    if (!retired)
        throw std::runtime_error("Completed Vulkan deferred release was not collected.");
}

RendererServer::EFrameStatus RendererServer::renderFrame(const FrameRecorder& Recorder)
{
    if (!Impl)
        throw std::logic_error("Renderer must be initialized before rendering a frame.");
    // 录制/提交在渲染线程上执行; GameThread 用 promise/future 阻塞等待本帧结果,
    // 由此实现"游戏领先渲染"的帧握手(渲染线程内部再以 fence 做 GPU 侧的帧 pacing).
    std::promise<EFrameStatus> Promise;
    std::future<EFrameStatus> Future = Promise.get_future();
    Impl->RenderThreadWorker.postTask([this, Recorder, Promise = std::move(Promise)]() mutable {
        const EFrameStatus Status = Impl->renderFrameImpl([&](rhi::RCommandList& Commands,
            const std::shared_ptr<rhi::RImage>& Image,
            const std::shared_ptr<rhi::RImageView>& View,
            uint32_t Width, uint32_t Height, bool ImageInitialized) {
            // 单渲染作用域历史路径: 手动 barrier + 清屏 + 用户回调.
            const rhi::ImageBarrier begin_barrier {
                .Image = Image,
                .Before = ImageInitialized
                    ? rhi::EResourceState::Present
                    : rhi::EResourceState::Undefined,
                .After = rhi::EResourceState::RenderTarget
            };
            Commands.imageBarriers(std::span(&begin_barrier, 1));

            const rhi::ColorAttachment color {
                .View = View,
                .LoadOp = rhi::ELoadOp::Clear,
                .StoreOp = rhi::EStoreOp::Store,
                .ClearValue = {
                    .Type = rhi::EClearColorType::Float,
                    .Float32 = { 0.025f, 0.035f, 0.055f, 1.0f }
                }
            };
            const rhi::RenderingInfo rendering {
                .Area = { 0, 0, Width, Height },
                .ColorAttachments = std::span(&color, 1)
            };
            Commands.beginRendering(rendering);
            if (Recorder)
                Recorder(Commands, View, Width, Height);
            Commands.endRendering();

            const rhi::ImageBarrier present_barrier {
                .Image = Image,
                .Before = rhi::EResourceState::RenderTarget,
                .After = rhi::EResourceState::Present
            };
            Commands.imageBarriers(std::span(&present_barrier, 1));
        });
        Promise.set_value(Status);
    });
    return Future.get();
}

RendererServer::EFrameStatus RendererServer::renderFrameGraph(const FrameGraphSetup& Setup)
{
    if (!Impl || !Impl->Device || !Impl->RDGPool)
        throw std::logic_error("Renderer must be initialized before rendering a frame.");
    // 刷新 World → Renderer 的统一交换包(GameThread 侧预处理; 渲染线程读其快照).
    (void)buildRenderSubmission();
    std::promise<EFrameStatus> Promise;
    std::future<EFrameStatus> Future = Promise.get_future();
    Impl->RenderThreadWorker.postTask([this, Setup, Promise = std::move(Promise)]() mutable {
        const EFrameStatus Status = Impl->renderFrameImpl([&](rhi::RCommandList& Commands,
            const std::shared_ptr<rhi::RImage>& Image,
            const std::shared_ptr<rhi::RImageView>& View,
            uint32_t Width, uint32_t Height, bool ImageInitialized) {
            // RDG 多 Pass 路径: 交换链图像作为 backbuffer 被图托管.
            rdg::RDGGraph graph;
            rdg::ResourceId backbuffer {};
            graph.addSetupPass("ImportBackbuffer", [&](rdg::RDGBuilder& b) {
                backbuffer = b.importTexture("Backbuffer", Image, View);
            });

            if (Setup)
            {
                // 应用自定义 Pass 编排(可覆盖默认 Pass 链).
                Setup(graph, backbuffer);
            }
            else if (Impl->SceneRenderer.getPassCount() > 0)
            {
                // 无回调: 驱动 SceneRenderer 声明 Shadow → DepthPre → Opaque →
                // Transparent → Post → UI 的完整 Pass 链, 3D 与 2D 合并进同一张帧图.
                // 视图取渲染线程当前值(setRenderView 已作为命令投递并应用).
                RenderFrameResources frame_resources;
                const RenderContext context {
                    .ProxyWorld = Impl->ProxyWorld,
                    .View = Impl->RenderThreadWorker.getCurrentView(),
                    .Submission = &Impl->LastSubmission,
                    .Backbuffer = backbuffer,
                    .FrameResources = frame_resources,
                    .FrameIndex = Impl->FrameIndex,
                    .Width = Width,
                    .Height = Height,
                };
                Impl->SceneRenderer.build(graph, context);
            }
            else
            {
                // 无子 Pass: 声明一次 backbuffer 写入(仅供出口 barrier 正确推导).
                graph.addPass("Clear", [&](rdg::RDGBuilder& b) {
                    const std::array color { backbuffer };
                    b.setRenderTargets(color);
                    b.setType(rdg::ERDGPassType::Raster);
                });
            }

            // 瞬态堆已按帧在飞数三缓冲, 跨帧复用安全, 重新启用别名.
            const rdg::RDGCompiledPlan plan = graph.compile(Impl->RDGPool.get(), /*EnableAliasing=*/true);

            // 交换链入口: 图对 imported 资源不插入布局屏障, 这里显式转换到 RenderTarget.
            const rhi::ImageBarrier begin_barrier {
                .Image = Image,
                .Before = ImageInitialized
                    ? rhi::EResourceState::Present
                    : rhi::EResourceState::Undefined,
                .After = rhi::EResourceState::RenderTarget
            };
            Commands.imageBarriers(std::span(&begin_barrier, 1));

            graph.execute(Commands, plan);

            // 交换链出口: 最后一个 Pass 已把 backbuffer 写到 RenderTarget, 这里统一转 Present.
            const rhi::ImageBarrier present_barrier {
                .Image = Image,
                .Before = rhi::EResourceState::RenderTarget,
                .After = rhi::EResourceState::Present
            };
            Commands.imageBarriers(std::span(&present_barrier, 1));
        });
        Promise.set_value(Status);
    });
    return Future.get();
}

RenderProxyWorld& RendererServer::getRenderProxyWorld() noexcept
{
    static RenderProxyWorld Fallback;
    return Impl ? Impl->ProxyWorld : Fallback;
}

RenderPassScheduler& RendererServer::getSceneRenderer() noexcept
{
    static RenderPassScheduler Fallback;
    return Impl ? Impl->SceneRenderer : Fallback;
}

void RendererServer::setRenderView(const RenderView& InView) noexcept
{
    // 视图归渲染线程所有: 以命令投递, 由渲染线程在下一帧前应用, 避免跨线程写竞争.
    if (Impl)
        Impl->RenderThreadWorker.enqueue(RenderCommand { RenderCommandSetRenderView { InView } });
}

void RendererServer::setRenderViewport(const Viewport& InViewport)
{
    if (!Impl)
        return;
    // 视口描述缓存于 GameThread 侧(编辑器只读), 相机则投递给渲染线程.
    Impl->ActiveViewport = InViewport;
    setRenderView(InViewport.Camera);
}

const RenderFrameSubmission& RendererServer::buildRenderSubmission()
{
    static RenderFrameSubmission Fallback;
    if (!Impl)
        return Fallback;

    const EChangeFlags world_changes = Impl->ProxyWorld.consumeChangeFlags();
    const EChangeFlags ui_changes = {};
    Impl->ProxyWorld.buildSubmission(Impl->LastSubmission, world_changes, ui_changes);
    // 消费 GameThread 侧提交的 UI 绘制列表与配套的纹理脏区.
    Impl->LastSubmission.UIDrawLists = std::move(Impl->PendingUIDrawLists);
    Impl->PendingUIDrawLists.clear();
    Impl->LastSubmission.TextureUploads = std::move(Impl->PendingTextureUploads);
    Impl->PendingTextureUploads.clear();
    // 3D 场景的绘制矩形(编辑器视口面板); 未设置时为整屏.
    Impl->LastSubmission.SceneViewport = Impl->SceneViewport;
    return Impl->LastSubmission;
}

void RendererServer::setSceneViewport(const rhi::RenderArea& Rect) noexcept
{
    if (!Impl)
        return;
    Impl->SceneViewport = Rect;
}

void RendererServer::submitUIDrawLists(
    std::vector<ui::UIDrawList> DrawLists,
    std::vector<ui::UISampledTextureUpload> TextureUploads)
{
    if (!Impl)
        return;
    Impl->PendingUIDrawLists = std::move(DrawLists);
    Impl->PendingTextureUploads = std::move(TextureUploads);
}

const RenderFrameSubmission& RendererServer::getLastRenderSubmission() const noexcept
{
    static RenderFrameSubmission Fallback;
    return Impl ? Impl->LastSubmission : Fallback;
}

void RendererServer::resize(uint32_t Width, uint32_t Height)
{
    if (!Impl || !Impl->Swapchain)
        throw std::logic_error("Renderer must be initialized before resizing.");
    if (Impl->Device->getStatus() != rhi::EDeviceStatus::Ready)
        throw std::runtime_error("Render device is lost; resize cannot recover device resources.");
    // 交换链重建归渲染线程所有: 通过任务串行化, 避免与帧录制并发触碰 Swapchain/Frames.
    Impl->RenderThreadWorker.postTask([this, Width, Height]() {
        if (Impl->SurfaceRecoveryRequired.load(std::memory_order_relaxed))
            return;
        Impl->Width = Width;
        Impl->Height = Height;
        Impl->recreate(Width, Height);
    });
}

bool RendererServer::recoverSurface()
{
    if (!Impl) return false;
    return Impl->recoverSurface();
}

} // namespace runtime::renderer
