#include <renderer/RendererServer.hpp>

#include <renderer/ShaderBinding.hpp>
#include <RHIServer.hpp>

#include <fstream>
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
		uint64_t CompletionValue { 0 };
		bool TimestampPending { false };
	};

	static constexpr uint32_t FramesInFlight = 3;
	ui::GenericWindowPointer Window;
	std::shared_ptr<rhi::RDevice> Device;
	std::shared_ptr<rhi::RQueue> GraphicsQueue;
	std::shared_ptr<rhi::RSwapchain> Swapchain;
	std::shared_ptr<rhi::RSemaphore> FrameTimeline;
	std::array<FrameContext, FramesInFlight> Frames;
	std::vector<std::shared_ptr<rhi::RSemaphore>> RenderFinishedSemaphores;
	std::vector<bool> ImageInitialized;
	uint32_t FrameIndex { 0 };
	uint64_t NextTimelineValue { 1 };
	uint32_t Width { 0 };
	uint32_t Height { 0 };
	bool SurfaceRecoveryRequired { false };
	std::optional<double> LastGPUFrameTimeNanoseconds;

	// 渲染世界与 UI 渲染器(Renderer 持有的高层状态).
	RenderWorld RenderWorld;
	UIRenderer UIRenderer;

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
		LastGPUFrameTimeNanoseconds = Frame.TimestampQueries->timestampTicksToNanoseconds(delta);
		Frame.TimestampPending = false;
	}

	void create(const ui::GenericWindowPointer& InWindow, const std::shared_ptr<rhi::RDevice>& InDevice)
	{
		Window = InWindow;
		Device = InDevice;
		Width = static_cast<uint32_t>(std::max(0, Window->getWidth()));
		Height = static_cast<uint32_t>(std::max(0, Window->getHeight()));
		GraphicsQueue = Device->getQueue(rhi::ECommandQueueType::Graphics);
		FrameTimeline = Device->createTimelineSemaphore();
		for (auto& frame : Frames)
		{
			frame.Commands = Device->createCommandList();
			frame.ImageAvailable = Device->createSemaphore();
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
		if (!SurfaceRecoveryRequired || Device->getStatus() != rhi::EDeviceStatus::Ready)
			return false;
		auto* rhi_instance = rhi::RenderServer::self().getRHI();
		if (!rhi_instance || !rhi_instance->recoverSurface(Window) ||
			!Swapchain->recoverSurface(Width, Height))
			return false;
		for (auto& frame : Frames) frame.ImageAvailable = Device->createSemaphore();
		ImageInitialized.assign(Swapchain->getImageCount(), false);
		RenderFinishedSemaphores.assign(Swapchain->getImageCount(), {});
		for (auto& semaphore : RenderFinishedSemaphores) semaphore = Device->createSemaphore();
		SurfaceRecoveryRequired = false;
		return true;
	}
};

RendererServer::EFrameStatus RendererServer::Implementation::renderFrameImpl(const RecordFn& Record)
{
	if (!Device || !Swapchain)
		throw std::logic_error("Renderer must be initialized before rendering a frame.");
	if (Device->getStatus() != rhi::EDeviceStatus::Ready)
		return RendererServer::EFrameStatus::DeviceLost;
	if (SurfaceRecoveryRequired ||
		Swapchain->getStatus() == rhi::ESwapchainStatus::SurfaceLost)
	{
		SurfaceRecoveryRequired = true;
		return RendererServer::EFrameStatus::SurfaceLost;
	}
	Device->collectDeferredReleases();
	if (Width == 0 || Height == 0)
		return RendererServer::EFrameStatus::Skipped;

	auto& frame = Frames[FrameIndex];
	if (frame.CompletionValue != 0)
	{
		if (!FrameTimeline->wait(frame.CompletionValue))
			return Device->getStatus() == rhi::EDeviceStatus::Ready
				? RendererServer::EFrameStatus::Skipped
				: RendererServer::EFrameStatus::DeviceLost;
		GraphicsQueue->poll();
		Device->collectDeferredReleases();
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
		SurfaceRecoveryRequired = true;
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
	const std::array signals {
		rhi::SemaphoreSubmitInfo { render_finished, 0 },
		rhi::SemaphoreSubmitInfo { FrameTimeline, completion_value }
	};
	const std::array commands { frame.Commands };
	try
	{
		GraphicsQueue->submit({
			.CommandLists = commands,
			.WaitSemaphores = waits,
			.SignalSemaphores = signals
		});
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
		SurfaceRecoveryRequired = true;
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
	const ui::GenericWindowPointer& Window)
{
	rhi::RenderServer::self().initialize(BackendAPI, Window);
	Impl = std::make_unique<Implementation>();
	Impl->create(Window, rhi::RenderServer::self().getDevice());
}

void RendererServer::shutdown() noexcept
{
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
	return Impl ? Impl->LastGPUFrameTimeNanoseconds : std::nullopt;
}

bool RendererServer::deferRelease(std::shared_ptr<void> Resource)
{
	if (!Impl || !Impl->Device || !Impl->FrameTimeline || !Resource)
		return false;
	// The timeline value snapshots all submissions made before this call. The device queue
	// retains entries until that value completes; collection never waits.
	return Impl->Device->deferRelease(
		std::move(Resource), Impl->FrameTimeline, Impl->NextTimelineValue - 1);
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
			rhi::EBufferUsage usage = reflected.Type == rhi::EDescriptorType::UniformBuffer
				? rhi::EBufferUsage(rhi::EBufferUsage_t::Uniform)
				: rhi::EBufferUsage(rhi::EBufferUsage_t::Storage);
			auto buffer = device->createBuffer(rhi::RBuffer::Descriptor_t {
				.Size = reflected.BlockSize == 0 ? 256u : reflected.BlockSize,
				.Usage = usage,
				.MemoryProperty = rhi::EMemoryProperty_t::DeviceLocal,
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
					.MemoryProperty = rhi::EMemoryProperty_t::DeviceLocal,
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
		.Usage = rhi::EBufferUsage_t::TransferSrc,
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
			.Usage = rhi::EBufferUsage(rhi::EBufferUsage_t::Storage) |
				rhi::EBufferUsage_t::DeviceAddress,
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
			.Usage = rhi::EBufferUsage(rhi::EBufferUsage_t::AccelerationStructureStorage) |
				rhi::EBufferUsage_t::DeviceAddress,
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
	if (!device->deferRelease(retirement_probe, Impl->FrameTimeline, 0))
		throw std::runtime_error("Vulkan deferred release unexpectedly rejected a valid timeline point.");
	retirement_probe.reset();
	device->collectDeferredReleases();
	if (!retired)
		throw std::runtime_error("Completed Vulkan deferred release was not collected.");
}

RendererServer::EFrameStatus RendererServer::renderFrame(const FrameRecorder& Recorder)
{
	if (!Impl)
		throw std::logic_error("Renderer must be initialized before rendering a frame.");
	return Impl->renderFrameImpl([&](rhi::RCommandList& Commands,
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
}

RendererServer::EFrameStatus RendererServer::renderFrameGraph(const FrameGraphSetup& Setup)
{
	if (!Impl || !Impl->Device)
		throw std::logic_error("Renderer must be initialized before rendering a frame.");
	return Impl->renderFrameImpl([&](rhi::RCommandList& Commands,
		const std::shared_ptr<rhi::RImage>& Image,
		const std::shared_ptr<rhi::RImageView>& View,
		uint32_t Width, uint32_t Height, bool ImageInitialized) {
		// RDG 多 Pass 路径: 交换链图像作为 backbuffer 被图托管.
		RDGBuilder builder(*Impl->Device);
		const RDGResourceId backbuffer = builder.importImage(Image, View, "Backbuffer",
			ImageInitialized ? rhi::EResourceState::Present : rhi::EResourceState::Undefined);

		if (Setup)
		{
			// 应用用 SceneRenderer 编排子渲染器(3D 各阶段 + UI)向同一 RDG 声明 Pass.
			Setup(builder, backbuffer);
		}
		else
		{
			// 无回调: 退化为一次清屏 Pass.
			builder.addGraphicsPass("Clear",
				[&](RDGPassBuilder& PassBuilder) {
					PassBuilder.renderTarget(backbuffer, rhi::ELoadOp::Clear,
						rhi::EStoreOp::Store,
						{ .Type = rhi::EClearColorType::Float,
						  .Float32 = { 0.025f, 0.035f, 0.055f, 1.0f } });
				},
				[](RDGExecuteContext&) {});
		}

		builder.compile();
		builder.execute(Commands);

		// 交换链出口: 最后一个 Pass 已把 backbuffer 写到 RenderTarget, 这里统一转 Present.
		const rhi::ImageBarrier present_barrier {
			.Image = Image,
			.Before = rhi::EResourceState::RenderTarget,
			.After = rhi::EResourceState::Present
		};
		Commands.imageBarriers(std::span(&present_barrier, 1));
	});
}

RenderWorld& RendererServer::getRenderWorld() noexcept
{
	static RenderWorld Fallback;
	return Impl ? Impl->RenderWorld : Fallback;
}

UIRenderer& RendererServer::getUIRenderer() noexcept
{
	static UIRenderer Fallback;
	return Impl ? Impl->UIRenderer : Fallback;
}

void RendererServer::resize(uint32_t Width, uint32_t Height)
{
	if (!Impl || !Impl->Swapchain)
		throw std::logic_error("Renderer must be initialized before resizing.");
	Impl->Width = Width;
	Impl->Height = Height;
	if (Impl->Device->getStatus() != rhi::EDeviceStatus::Ready)
		throw std::runtime_error("Render device is lost; resize cannot recover device resources.");
	if (!Impl->SurfaceRecoveryRequired)
		Impl->recreate(Width, Height);
}

bool RendererServer::recoverSurface()
{
	if (!Impl) return false;
	return Impl->recoverSurface();
}

} // namespace runtime::renderer
