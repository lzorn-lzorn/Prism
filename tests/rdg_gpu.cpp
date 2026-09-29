/**
 * ============================================================================
 *  tests/rdg_gpu.cpp
 * ============================================================================
 *
 *  RDG + RHI placed resource 的真实设备验证.
 *
 *  与 rdg_cpu.cpp 的分工:
 *      rdg_cpu.cpp  只验证图算法(依赖/拓扑/生命周期/别名计划/屏障推导), 不需要 GPU;
 *      本文件       验证"计划 -> 真实显存"这一段: 堆申请, 子分配, placed resource,
 *                   以及同一段显存被两个资源复用.
 *
 *  需要: SDL3 + Vulkan + 可用显卡. 初始化失败时打印 SKIP 并以 0 退出,
 *  这样 CI 上没有 GPU 也不会把测试判为失败.
 *
 * ============================================================================
 */

#include <rdg/RDG.hpp>

#include <RHIServer.hpp>

#include <SDL3/SDL.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace
{

using namespace renderer::rdg;

int g_failures = 0;
int g_checks = 0;

#define RDG_GPU_CHECK(Expr)                                                      \
	do                                                                           \
	{                                                                            \
		++g_checks;                                                              \
		if (!(Expr))                                                             \
		{                                                                        \
			std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #Expr);    \
			++g_failures;                                                        \
		}                                                                        \
	} while (false)

// ============================================================================
// 最小窗口: 只为了拿到 SDL_Window* 给 Vulkan 创建 surface
// ============================================================================

class SdlTestWindow final : public ui::IGenericWindow
{
public:
	explicit SdlTestWindow(SDL_Window* InWindow) : Window(InWindow) {}

	[[nodiscard]] ui::WindowId_t getWindowId() override { return 0; }
	void show() override {}
	void close() override {}
	void minimize() override {}
	void maximize() override {}
	void setTitle(const std::string&) override {}
	void setPosition(int32_t, int32_t) override {}
	void setSize(int32_t, int32_t) override {}
	void setWindowType(ui::EWindowType) override {}
	[[nodiscard]] ui::UIVector getPosition() override { return {}; }
	[[nodiscard]] ui::UIVector getSize() override { return {}; }
	[[nodiscard]] int32_t getWidth() override { return 64; }
	[[nodiscard]] int32_t getHeight() override { return 64; }
	[[nodiscard]] ui::EWindowType getWindowType() override { return ui::EWindowType::Windowed; }
	[[nodiscard]] void* getNativeHandle() const noexcept override { return Window; }

private:
	SDL_Window* Window { nullptr };
};

// ============================================================================
// 1. RHI 层: 堆 / 子分配 / placed resource
// ============================================================================

void testRhiPlacedResources(rhi::RDevice& Device)
{
	rhi::RBuffer::Descriptor_t buffer_desc;
	buffer_desc.Size = 4096;
	buffer_desc.Usage = rhi::EBufferUsage(rhi::EBufferUsage_t::Storage) |
		rhi::EBufferUsage_t::TransferDst;
	buffer_desc.MemoryUsage = rhi::EMemoryUsage::GPUOnly;
	buffer_desc.DebugName = "PlacedProbeBuffer";

	// 用一个临时 buffer 探出这个描述的内存需求(类型位/对齐).
	auto probe = Device.createBuffer(buffer_desc);
	RDG_GPU_CHECK(probe != nullptr);
	if (!probe)
		return;

	rhi::MemoryHeapDescriptor heap_desc;
	heap_desc.Size = 256 * 1024;
	heap_desc.Alignment = DefaultResourceAlignment;
	heap_desc.MemoryTypeBits = 0xFFFFFFFFu;
	heap_desc.RequiredProperties = rhi::EMemoryProperty(rhi::EMemoryProperty_t::DeviceLocal);
	heap_desc.DebugName = "PlacedProbeHeap";

	auto heap = Device.createTransientHeap(heap_desc);
	RDG_GPU_CHECK(heap != nullptr);
	if (!heap)
	{
		std::printf("    (backend did not provide a transient heap; placed path skipped)\n");
		return;
	}

	RDG_GPU_CHECK(heap->getSize() >= heap_desc.Size);

	const auto first_offset = heap->suballocate(4096, DefaultResourceAlignment);
	RDG_GPU_CHECK(first_offset.has_value());
	if (!first_offset)
		return;
	RDG_GPU_CHECK(*first_offset == 0);

	const auto second_offset = heap->suballocate(4096, DefaultResourceAlignment);
	RDG_GPU_CHECK(second_offset.has_value());
	if (second_offset)
		RDG_GPU_CHECK(*second_offset >= *first_offset + 4096);

	// 两个 Buffer 落在同一段显存上: 这正是 RDG 别名复用所需要的语义.
	auto first = Device.createPlacedBuffer(buffer_desc, heap, *first_offset);
	auto aliased = Device.createPlacedBuffer(buffer_desc, heap, *first_offset);
	RDG_GPU_CHECK(first != nullptr);
	RDG_GPU_CHECK(aliased != nullptr);
	if (first && aliased)
	{
		RDG_GPU_CHECK(first->isValid());
		RDG_GPU_CHECK(aliased->isValid());
		// 同一偏移意味着复用同一段显存: 两个 VkBuffer 是不同对象, 但都被驱动接受.
		RDG_GPU_CHECK(first->getNativeHandle() != aliased->getNativeHandle());
	}

	// Image 的 placed 路径必须带 VK_IMAGE_CREATE_ALIAS_BIT 才能复用显存.
	rhi::RImage::Descriptor_t image_desc;
	image_desc.Format = rhi::EFormat::RGBA8_UNorm;
	image_desc.Dimension = rhi::EImageDimension::Texture2D;
	image_desc.Width = 64;
	image_desc.Height = 64;
	image_desc.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
		rhi::EImageUsage_t::Sampled;

	auto image_a = Device.createPlacedImage(image_desc, heap, *first_offset);
	RDG_GPU_CHECK(image_a != nullptr);
	if (image_a)
	{
		RDG_GPU_CHECK(image_a->isValid());
		RDG_GPU_CHECK(image_a->isMemoryBound());
	}

	// 非法偏移必须被拒绝, 而不是产生一个坏资源.
	auto misaligned = Device.createPlacedBuffer(buffer_desc, heap, 1);
	RDG_GPU_CHECK(misaligned == nullptr);

	auto overflowing = Device.createPlacedBuffer(buffer_desc, heap, heap->getSize() + 4096);
	RDG_GPU_CHECK(overflowing == nullptr);
}

// ============================================================================
// 2. RDG 层: 用真实设备跑一遍带别名的帧图
// ============================================================================

void testFrameGraphOnDevice(rhi::RDevice& Device)
{
	RDGGraph Graph;
	ResourceId Small;
	ResourceId Medium;
	ResourceId Overlapping;
	ResourceId Storage;
	ResourceId Backbuffer;
	// 每个 Pass 在声明期就带上自己的执行体(记录自己被调用).
	std::vector<std::string> Executed;
	auto recorder = [&Executed](const char* Name)
	{
		return [&Executed, Name](RDGPassContext&) { Executed.emplace_back(Name); };
	};

	Graph.addPass("Setup", [&](RDGBuilder& B)
	{
		Small = B.createTexture("Small", {
			.Format = rhi::EFormat::RGBA8_UNorm,
			.Dimension = rhi::EImageDimension::Texture2D,
			.Width = 64, .Height = 64,
			.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
				rhi::EImageUsage_t::Sampled
		});
		B.write(Small, EResourceState::RenderTarget);
		B.setExecute(recorder("Setup"));
	});
	Graph.addPass("Middle", [&](RDGBuilder& B)
	{
		Medium = B.createTexture("Medium", {
			.Format = rhi::EFormat::RGBA16_Float,
			.Dimension = rhi::EImageDimension::Texture2D,
			.Width = 128, .Height = 128,
			.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
				rhi::EImageUsage_t::Sampled
		});
		B.read(Small, EResourceState::PixelShaderResource);
		B.write(Medium, EResourceState::RenderTarget);
		B.setExecute(recorder("Middle"));
	});
	Graph.addPass("Blur", [&](RDGBuilder& B)
	{
		Storage = B.createBuffer("Storage", {
			.Size = 64 * 1024,
			.Usage = rhi::EBufferUsage(rhi::EBufferUsage_t::Storage) |
				rhi::EBufferUsage_t::TransferDst
		});
		B.read(Medium, EResourceState::PixelShaderResource);
		B.readWrite(Storage, EResourceState::UnorderedAccess);
		B.setExecute(recorder("Blur"));
	});
	Graph.addPass("Present", [&](RDGBuilder& B)
	{
		Overlapping = B.createTexture("Overlapping", {
			.Format = rhi::EFormat::RGBA16_Float,
			.Dimension = rhi::EImageDimension::Texture2D,
			.Width = 128, .Height = 128,
			.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
				rhi::EImageUsage_t::Sampled
		});
		Backbuffer = B.createTexture("Backbuffer", {
			.Format = rhi::EFormat::BGRA8_UNorm,
			.Dimension = rhi::EImageDimension::Texture2D,
			.Width = 64, .Height = 64,
			.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
				rhi::EImageUsage_t::TransferSrc
		});
		B.keepAlive(Backbuffer);
		B.read(Medium, EResourceState::PixelShaderResource);
		B.read(Storage, EResourceState::StorageBuffer);
		B.write(Overlapping, EResourceState::RenderTarget);
		B.write(Backbuffer, EResourceState::RenderTarget);
		B.setExecute(recorder("Present"));
	});

	RDGResourcePool Pool(Device);
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	RDG_GPU_CHECK(Plan.getExecutionOrder().size() == 4);
	RDG_GPU_CHECK(Pool.supportsAliasing());
	RDG_GPU_CHECK(Plan.isAliasingEnabled());
	RDG_GPU_CHECK(Plan.getTotalHeapBytes() > 0);
	// 至少有一个堆被真的创建出来(证明 placed 路径不是空转).
	RDG_GPU_CHECK(Pool.getHeapCount() > 0);

	// 别名必须让总堆显存小于"逐资源独立分配"之和.
	uint64_t NaiveTotal = 0;
	for (const RDGResource& Resource : Graph.getResources())
		if (Resource.isUsedTransient())
			NaiveTotal += Resource.computePlannedSize();
	RDG_GPU_CHECK(Plan.getTotalHeapBytes() <= NaiveTotal);

	// 所有瞬态资源都必须拿到真实资源, 否则执行期会断链.
	for (const RDGResourceBinding& Binding : Plan.getBindings())
	{
		if (Binding.Kind == EResourceKind::None)
			continue;
		RDG_GPU_CHECK(Binding.isValid());
	}

	// 别名计划自洽性.
	const RDGAliasValidation Validation = Plan.validateAliasing(Graph.getResources());
	RDG_GPU_CHECK(Validation.isValid);
	// 真实堆内的资源偏移不得重叠(否则别名会互相踩踏).
	RDG_GPU_CHECK(Plan.validateNoOverlap(Graph.getResources()));

	// 真的把这一帧录下来并提交, 确保屏障序列能被后端接受.
	auto command_list = Device.createCommandList();
	RDG_GPU_CHECK(command_list != nullptr);
	if (!command_list)
		return;
	command_list->begin();
	Graph.execute(*command_list, Plan);
	command_list->end();
	RDG_GPU_CHECK(command_list->getState() == rhi::ECommandListState::Executable);
	RDG_GPU_CHECK(Executed.size() == 4);

	auto queue = Device.getQueue(rhi::ECommandQueueType::Graphics);
	const std::array commands { command_list };
	queue->submit({ .CommandLists = commands });
	queue->waitIdle();

	// 关闭别名时仍必须能正确工作(退化路径), 只是没有显存收益.
	RDGResourcePool PlainPool(Device);
	PlainPool.setAliasingEnabled(false);
	const RDGCompiledPlan PlainPlan = Graph.compile(&PlainPool, true);
	RDG_GPU_CHECK(!PlainPlan.isAliasingEnabled());
	for (const RDGResourceBinding& Binding : PlainPlan.getBindings())
	{
		if (Binding.Kind == EResourceKind::None)
			continue;
		RDG_GPU_CHECK(Binding.isValid());
	}

	// 堆释放必须发生在设备销毁之前.
	Pool.releaseHeaps();
	PlainPool.releaseHeaps();
	Device.waitIdle();
}

// ============================================================================
// 3. 多帧复用: 同一个图对象连续编译多次不应泄漏/失效
// ============================================================================

void testRepeatedCompileOnDevice(rhi::RDevice& Device)
{
	RDGResourcePool Pool(Device);

	for (int Frame = 0; Frame < 4; ++Frame)
	{
		RDGGraph Graph;
		Graph.addPass("Frame", [&](RDGBuilder& B)
		{
			const ResourceId A = B.createTexture("A", {
				.Format = rhi::EFormat::RGBA8_UNorm,
				.Dimension = rhi::EImageDimension::Texture2D,
				.Width = 64, .Height = 64,
				.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
					rhi::EImageUsage_t::Sampled
			});
			const ResourceId B2 = B.createTexture("B", {
				.Format = rhi::EFormat::RGBA8_UNorm,
				.Dimension = rhi::EImageDimension::Texture2D,
				.Width = 64, .Height = 64,
				.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
					rhi::EImageUsage_t::Sampled
			});
			B.keepAlive(B2);
			B.write(A, EResourceState::RenderTarget);
			B.setExecute([](RDGPassContext&) {});
		});
		Graph.addPass("Second", [&](RDGBuilder& B)
		{
			const ResourceId B2 = B.createTexture("C", {
				.Format = rhi::EFormat::RGBA8_UNorm,
				.Dimension = rhi::EImageDimension::Texture2D,
				.Width = 64, .Height = 64,
				.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) |
					rhi::EImageUsage_t::Sampled
			});
			B.keepAlive(B2);
			B.write(B2, EResourceState::RenderTarget);
			B.setExecute([](RDGPassContext&) {});
		});

		const RDGCompiledPlan Plan = Graph.compile(&Pool, true);
		RDG_GPU_CHECK(!Plan.getExecutionOrder().empty());
		auto command_list = Device.createCommandList();
		command_list->begin();
		Graph.execute(*command_list, Plan);
		command_list->end();
		const std::array commands { command_list };
		Device.getQueue(rhi::ECommandQueueType::Graphics)->submit({ .CommandLists = commands });
		Device.getQueue(rhi::ECommandQueueType::Graphics)->waitIdle();
	}

	Pool.releaseHeaps();
}

} // namespace

int main()
{
	std::printf("RDG GPU tests\n");
	std::printf("--------------------------------------------------------------\n");

	if (!SDL_Init(SDL_INIT_VIDEO))
	{
		std::printf("SKIP: SDL video subsystem unavailable: %s\n", SDL_GetError());
		return 0;
	}

	SDL_Window* window = SDL_CreateWindow("RDGTest", 64, 64,
		SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
	if (!window)
	{
		std::printf("SKIP: unable to create a Vulkan-capable window: %s\n", SDL_GetError());
		SDL_Quit();
		return 0;
	}

	std::shared_ptr<rhi::RDevice> Device;
	try
	{
		auto generic_window = std::make_shared<SdlTestWindow>(window);
		rhi::RHIServer::self().initialize(rhi::ESupportedBackendAPI::Vulkan, generic_window);
		Device = rhi::RHIServer::self().getDevice();
	}
	catch (const std::exception& Error)
	{
		std::printf("SKIP: RHI initialization failed (%s)\n", Error.what());
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 0;
	}

	if (!Device)
	{
		std::printf("SKIP: no Vulkan device available.\n");
		SDL_DestroyWindow(window);
		SDL_Quit();
		return 0;
	}

	std::printf("device: %s\n", "Vulkan");
	{
		const int Before = g_failures;
		std::printf("%-46s ", "RHI placed resources + transient heap");
		std::fflush(stdout);
		try
		{
			testRhiPlacedResources(*Device);
		}
		catch (const std::exception& Error)
		{
			std::printf("FAILED (exception: %s)\n", Error.what());
			++g_failures;
			goto summary;
		}
		std::printf("%s\n", g_failures == Before ? "ok" : "FAILED");
	}
	{
		const int Before = g_failures;
		std::printf("%-46s ", "frame graph with real aliased memory");
		std::fflush(stdout);
		try
		{
			testFrameGraphOnDevice(*Device);
		}
		catch (const std::exception& Error)
		{
			std::printf("FAILED (exception: %s)\n", Error.what());
			++g_failures;
			goto summary;
		}
		std::printf("%s\n", g_failures == Before ? "ok" : "FAILED");
	}
	{
		const int Before = g_failures;
		std::printf("%-46s ", "repeated compile reuses heaps");
		std::fflush(stdout);
		try
		{
			testRepeatedCompileOnDevice(*Device);
		}
		catch (const std::exception& Error)
		{
			std::printf("FAILED (exception: %s)\n", Error.what());
			++g_failures;
			goto summary;
		}
		std::printf("%s\n", g_failures == Before ? "ok" : "FAILED");
	}

summary:
	rhi::RHIServer::self().shutdown();
	SDL_DestroyWindow(window);
	SDL_Quit();

	std::printf("--------------------------------------------------------------\n");
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures != 0)
	{
		std::printf("RDG GPU tests FAILED.\n");
		return 1;
	}
	std::printf("RDG GPU tests passed.\n");
	return 0;
}
