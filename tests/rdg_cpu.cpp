/**
 * ============================================================================
 *  tests/rdg_cpu.cpp
 * ============================================================================
 *
 *  RDG 图算法与执行录制的 CPU 侧验证.
 *
 *  覆盖:
 *      1. 句柄与资源注册
 *      2. RAW / WAR / WAW 依赖构建
 *      3. 拓扑排序稳定性与声明顺序无关性
 *      4. Pass 裁剪(对最终输出无贡献的 Pass)
 *      5. 生命周期分析
 *      6. 显存别名计划(共享槽位 + 生命周期不重叠)
 *      7. 屏障推导(含别名复用点的 Undefined 转换)
 *      8. 执行期屏障录制顺序与 Pass 调用顺序
 *      9. 池不可用 / placed 被拒时的独立分配退化
 *
 *  全部不依赖 GPU: 执行期用伪 RCommandList 把屏障与 Pass 调用录制下来做断言.
 *
 * ============================================================================
 */

#include <rdg/RDG.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace
{

using namespace renderer::rdg;

int g_failures = 0;
int g_checks = 0;

#define RDG_CHECK(Expr)                                                          \
	do                                                                           \
	{                                                                            \
		++g_checks;                                                              \
		if (!(Expr))                                                             \
		{                                                                        \
			std::printf("    [FAIL] %s:%d  %s\n", __FILE__, __LINE__, #Expr);    \
			++g_failures;                                                        \
		}                                                                        \
	} while (false)

#define RDG_CHECK_EQ(Lhs, Rhs)                                                          \
	do                                                                                  \
	{                                                                                   \
		++g_checks;                                                                     \
		const auto RdgLeftValue = (Lhs);                                                \
		const auto RdgRightValue = (Rhs);                                               \
		if (!(RdgLeftValue == RdgRightValue))                                           \
		{                                                                               \
			std::printf("    [FAIL] %s:%d  %s == %s\n", __FILE__, __LINE__, #Lhs, #Rhs); \
			++g_failures;                                                               \
		}                                                                               \
	} while (false)

// ============================================================================
// 伪 RHI 资源: 只需要"非空", 执行期只取裸指针
// ============================================================================

class StubBuffer final : public rhi::RBuffer
{
public:
	[[nodiscard]] rhi::RDevice& getDevice() const noexcept override { return *DeviceStub(); }
	[[nodiscard]] const Descriptor_t& getDescriptor() const noexcept override
	{
		static const Descriptor_t Desc {};
		return Desc;
	}
	[[nodiscard]] bool isValid() const noexcept override { return true; }
	[[nodiscard]] void* getNativeHandle() const noexcept override { return nullptr; }
	[[nodiscard]] void* map(rhi::DeviceSizeType, rhi::DeviceSizeType) override { return nullptr; }
	void unmap() override {}
	void flush(rhi::DeviceSizeType, rhi::DeviceSizeType) override {}
	void invalidate(rhi::DeviceSizeType, rhi::DeviceSizeType) override {}

	/** @brief 仅用于让 getDevice() 有合法返回; 测试不会真的访问设备. */
	static rhi::RDevice*& DeviceStub()
	{
		static rhi::RDevice* Device = nullptr;
		return Device;
	}
};

class StubImage final : public rhi::RImage
{
public:
	StubImage() : rhi::RImage(Descriptor_t {}) {}
	[[nodiscard]] rhi::RDevice& getDevice() const noexcept override
	{
		return *StubBuffer::DeviceStub();
	}
	[[nodiscard]] bool isValid() const noexcept override { return true; }
	[[nodiscard]] bool isMemoryBound() const noexcept override { return true; }
	[[nodiscard]] void* getNativeHandle() const noexcept override { return nullptr; }
};

/** @brief 伪显存堆: 只按顺序切分区间, 用来验证 RDG 的别名分配请求. */
class StubHeap final : public rhi::RTransientHeap
{
public:
	explicit StubHeap(rhi::DeviceSizeType InSize) : Size(InSize) {}

	[[nodiscard]] rhi::RDevice& getDevice() const noexcept override
	{
		return *StubBuffer::DeviceStub();
	}
	[[nodiscard]] const rhi::MemoryHeapDescriptor& getDescriptor() const noexcept override
	{
		return Descriptor;
	}
	[[nodiscard]] bool isValid() const noexcept override { return true; }
	[[nodiscard]] rhi::DeviceSizeType getSize() const noexcept override { return Size; }
	[[nodiscard]] void* getNativeHandle() const noexcept override { return nullptr; }

	[[nodiscard]] std::optional<rhi::DeviceSizeType> suballocate(
		rhi::DeviceSizeType InSize,
		rhi::DeviceSizeType InAlignment) override
	{
		const rhi::DeviceSizeType aligned = (Cursor + InAlignment - 1) / InAlignment * InAlignment;
		if (aligned + InSize > Size)
			return std::nullopt;
		Cursor = aligned + InSize;
		return aligned;
	}
	void releaseSuballocation(rhi::DeviceSizeType, rhi::DeviceSizeType) override {}

private:
	rhi::MemoryHeapDescriptor Descriptor;
	rhi::DeviceSizeType       Size { 0 };
	rhi::DeviceSizeType       Cursor { 0 };
};

// ============================================================================
// 伪命令列表: 只录制 RDG 执行期真正用到的调用
// ============================================================================

class RecordingCommandList final : public rhi::RCommandList
{
public:
	struct BarrierRecord
	{
		rhi::EResourceState Before { rhi::EResourceState::Undefined };
		rhi::EResourceState After { rhi::EResourceState::Undefined };
	};

	/** @brief 录到的全部转换(按提交顺序). */
	std::vector<BarrierRecord> Barriers;
	/** @brief 每次 barriers() 调用的条目数. */
	std::vector<size_t> BarrierBatches;
	std::vector<std::string> Labels;
	std::vector<std::string> EndedLabels;

	void clear()
	{
		Barriers.clear();
		BarrierBatches.clear();
		Labels.clear();
		EndedLabels.clear();
	}

	[[nodiscard]] rhi::RDevice& getDevice() const noexcept override
	{
		return *StubBuffer::DeviceStub();
	}
	[[nodiscard]] rhi::ECommandQueueType getQueueType() const noexcept override
	{
		return rhi::ECommandQueueType::Graphics;
	}
	[[nodiscard]] rhi::ECommandListLevel getLevel() const noexcept override
	{
		return rhi::ECommandListLevel::Primary;
	}
	[[nodiscard]] rhi::ECommandListState getState() const noexcept override
	{
		return rhi::ECommandListState::Recording;
	}
	[[nodiscard]] void* getNativeHandle() const noexcept override { return nullptr; }

	void begin() override {}
	void end() override {}
	void reset() override {}

	void barriers(
		std::span<const rhi::GlobalBarrier>,
		std::span<const rhi::BufferBarrier> BufferBarriers,
		std::span<const rhi::ImageBarrier> ImageBarriers) override
	{
		BarrierBatches.push_back(BufferBarriers.size() + ImageBarriers.size());
		for (const rhi::BufferBarrier& Barrier : BufferBarriers)
			Barriers.push_back({ Barrier.Before, Barrier.After });
		for (const rhi::ImageBarrier& Barrier : ImageBarriers)
			Barriers.push_back({ Barrier.Before, Barrier.After });
	}

	void beginRendering(const rhi::RenderingInfo&) override {}
	void endRendering() override {}
	void setViewports(std::span<const rhi::Viewport>) override {}
	void setScissors(std::span<const rhi::RenderArea>) override {}
	void setBlendConstants(const std::array<float, 4>&) override {}
	void setStencilReference(uint32_t, uint32_t) override {}
	void setDepthBias(float, float, float) override {}
	void setLineWidth(float) override {}
	void pushConstants(
		const std::shared_ptr<rhi::RPipelineLayout>&,
		rhi::EShaderStage,
		uint32_t,
		std::span<const std::byte>) override
	{
	}
	void bindPipeline(const std::shared_ptr<rhi::RPipeline>&) override {}
	void bindBindGroups(
		rhi::EPipelineType,
		const std::shared_ptr<rhi::RPipelineLayout>&,
		uint32_t,
		std::span<const std::shared_ptr<rhi::RBindGroup>>,
		std::span<const uint32_t>) override
	{
	}
	void bindVertexBuffers(uint32_t, std::span<const rhi::VertexBufferBinding>) override {}
	void bindIndexBuffer(
		const std::shared_ptr<rhi::RBuffer>&,
		rhi::DeviceSizeType,
		rhi::EIndexFormat) override
	{
	}
	void copyBuffer(
		const std::shared_ptr<rhi::RBuffer>&,
		const std::shared_ptr<rhi::RBuffer>&,
		std::span<const rhi::BufferCopyRegion>) override
	{
	}
	void copyBufferToImage(
		const std::shared_ptr<rhi::RBuffer>&,
		const std::shared_ptr<rhi::RImage>&,
		std::span<const rhi::BufferImageCopyRegion>) override
	{
	}
	void copyImageToBuffer(
		const std::shared_ptr<rhi::RImage>&,
		const std::shared_ptr<rhi::RBuffer>&,
		std::span<const rhi::BufferImageCopyRegion>) override
	{
	}
	void copyImage(
		const std::shared_ptr<rhi::RImage>&,
		const std::shared_ptr<rhi::RImage>&,
		std::span<const rhi::ImageCopyRegion>) override
	{
	}
	void blitImage(
		const std::shared_ptr<rhi::RImage>&,
		const std::shared_ptr<rhi::RImage>&,
		std::span<const rhi::ImageBlitRegion>,
		rhi::EFilterMode) override
	{
	}
	void fillBuffer(
		const std::shared_ptr<rhi::RBuffer>&,
		rhi::DeviceSizeType,
		rhi::DeviceSizeType,
		uint32_t) override
	{
	}
	void draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
	void drawIndexed(uint32_t, uint32_t, uint32_t, int32_t, uint32_t) override {}
	void drawMeshTasks(uint32_t, uint32_t, uint32_t) override {}
	void dispatch(uint32_t, uint32_t, uint32_t) override {}
	void drawIndirect(
		const std::shared_ptr<rhi::RBuffer>&,
		rhi::DeviceSizeType,
		uint32_t,
		uint32_t) override
	{
	}
	void drawIndexedIndirect(
		const std::shared_ptr<rhi::RBuffer>&,
		rhi::DeviceSizeType,
		uint32_t,
		uint32_t) override
	{
	}
	void dispatchIndirect(const std::shared_ptr<rhi::RBuffer>&, rhi::DeviceSizeType) override {}
	void resetQueries(const std::shared_ptr<rhi::RQueryPool>&, uint32_t, uint32_t) override {}
	void beginQuery(const std::shared_ptr<rhi::RQueryPool>&, uint32_t) override {}
	void endQuery(const std::shared_ptr<rhi::RQueryPool>&, uint32_t) override {}
	void writeTimestamp(const std::shared_ptr<rhi::RQueryPool>&, uint32_t) override {}

	void beginDebugLabel(std::string_view Name, const std::array<float, 4>&) override
	{
		Labels.emplace_back(Name);
	}
	void endDebugLabel() override { EndedLabels.emplace_back(); }
	void insertDebugLabel(std::string_view, const std::array<float, 4>&) override {}
	void executeSecondary(std::span<const std::shared_ptr<rhi::RCommandList>>) override {}
};

// ============================================================================
// 伪资源池: 记录 RDG 对池的每一次请求
// ============================================================================

class RecordingPool final : public IRDGResourcePool
{
public:
	enum class ECall
	{
		BeginFrame,
		Allocate,
		PlacedBuffer,
		PlacedImage,
		Buffer,
		Image,
	};

	struct Allocation
	{
		uint32_t HeapIndex { InvalidId };
		uint64_t Size { 0 };
		uint64_t Alignment { 0 };
		uint32_t ResourceIndex { InvalidId };
	};

	/** @brief 置 false 模拟"后端不支持 placed resource". */
	bool Aliasing { true };
	/** @brief 置 true 模拟"堆可用但 placed 创建被驱动拒绝". */
	bool RejectPlaced { false };

	std::vector<ECall>      Calls;
	std::vector<Allocation> Allocations;
	std::vector<uint64_t>   HeapReserved;
	uint32_t                FallbackBufferCount { 0 };
	uint32_t                FallbackImageCount { 0 };

	[[nodiscard]] uint32_t count(ECall Call) const
	{
		return static_cast<uint32_t>(std::count(Calls.begin(), Calls.end(), Call));
	}

	/** @brief 池内共用的伪显存堆. */
	std::shared_ptr<StubHeap> Heap;

	[[nodiscard]] bool supportsAliasing() const noexcept override { return Aliasing; }

	void beginFrame(std::span<const RDGHeapLayout> Layouts) override
	{
		Calls.push_back(ECall::BeginFrame);
		HeapReserved.clear();
		for(const RDGHeapLayout& Layout : Layouts)
			HeapReserved.push_back(Layout.ReservedSize);
	}

	[[nodiscard]] std::optional<RDGHeapAllocation> allocateMemory(
		uint32_t HeapIndex,
		uint64_t Size,
		uint64_t Alignment,
		uint32_t ResourceIndex) override
	{
		if (!Aliasing)
			return std::nullopt;
		Calls.push_back(ECall::Allocate);
		Allocations.push_back({ HeapIndex, Size, Alignment, ResourceIndex });
		// 复用同一个伪堆, 让 RDG 认为这次 placed 创建落到了真实显存上.
		if (!Heap)
			Heap = std::make_shared<StubHeap>(64ull * 1024ull * 1024ull);
		return RDGHeapAllocation { Heap, 0, Size };
	}

	[[nodiscard]] std::shared_ptr<rhi::RBuffer> createPlacedBuffer(
		const RDGHeapAllocation&,
		const BufferDesc&) override
	{
		Calls.push_back(ECall::PlacedBuffer);
		return RejectPlaced ? std::shared_ptr<rhi::RBuffer> {} : std::make_shared<StubBuffer>();
	}

	[[nodiscard]] std::shared_ptr<rhi::RImage> createPlacedImage(
		const RDGHeapAllocation&,
		const TextureDesc&) override
	{
		Calls.push_back(ECall::PlacedImage);
		return RejectPlaced ? std::shared_ptr<rhi::RImage> {} : std::make_shared<StubImage>();
	}

	[[nodiscard]] std::shared_ptr<rhi::RBuffer> createBuffer(const BufferDesc&) override
	{
		Calls.push_back(ECall::Buffer);
		++FallbackBufferCount;
		return std::make_shared<StubBuffer>();
	}

	[[nodiscard]] std::shared_ptr<rhi::RImage> createImage(const TextureDesc&) override
	{
		Calls.push_back(ECall::Image);
		++FallbackImageCount;
		return std::make_shared<StubImage>();
	}
};

// ============================================================================
// 测试数据
// ============================================================================

TextureDesc ColorTarget(std::string Name, uint32_t Size = 64)
{
	TextureDesc Desc;
	Desc.Format = rhi::EFormat::RGBA16_Float;
	Desc.Dimension = rhi::EImageDimension::Texture2D;
	Desc.Width = Size;
	Desc.Height = Size;
	Desc.Usage = rhi::EImageUsage(rhi::EImageUsage_t::Target) | rhi::EImageUsage_t::Sampled;
	Desc.Name = std::move(Name);
	return Desc;
}

TextureDesc DepthTarget(std::string Name, uint32_t Size = 64)
{
	TextureDesc Desc;
	Desc.Format = rhi::EFormat::D32_Float;
	Desc.Dimension = rhi::EImageDimension::Texture2D;
	Desc.Width = Size;
	Desc.Height = Size;
	Desc.Usage = rhi::EImageUsage(rhi::EImageUsage_t::DepthStencil) | rhi::EImageUsage_t::Sampled;
	Desc.Name = std::move(Name);
	return Desc;
}

BufferDesc StorageBufferDesc(std::string Name, uint64_t Size)
{
	BufferDesc Desc;
	Desc.Size = Size;
	Desc.Usage = rhi::EBufferUsage(rhi::EBufferUsage_t::Storage) | rhi::EBufferUsage_t::TransferDst;
	Desc.Name = std::move(Name);
	return Desc;
}

// ============================================================================
// 1. 依赖构建与拓扑排序
// ============================================================================

void testDependencyAndOrder()
{
	std::vector<std::string> Order;

	RDGGraph Graph;
	ResourceId Source;
	ResourceId Middling;
	ResourceId Final;

	const PassId Producer = Graph.addPass("Produce", [&](RDGBuilder& B)
	{
		Source = B.createTexture("Source", ColorTarget("Source"));
		B.write(Source, EResourceState::RenderTarget);
		B.setExecute([&Order](RDGPassContext&) { Order.emplace_back("Produce"); });
	});
	const PassId Middle = Graph.addPass("Middle", [&](RDGBuilder& B)
	{
		Middling = B.createTexture("Middling", ColorTarget("Middling"));
		B.read(Source, EResourceState::PixelShaderResource); // RAW: Middle <- Produce
		B.write(Middling, EResourceState::RenderTarget);
		B.setExecute([&Order](RDGPassContext&) { Order.emplace_back("Middle"); });
	});
	const PassId Consumer = Graph.addPass("Consume", [&](RDGBuilder& B)
	{
		Final = B.createTexture("Final", ColorTarget("Final"));
		B.keepAlive(Final);
		B.read(Middling, EResourceState::PixelShaderResource); // RAW: Consume <- Middle
		B.write(Final, EResourceState::RenderTarget);
		B.setExecute([&Order](RDGPassContext&) { Order.emplace_back("Consume"); });
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);

	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 3 });
	RDG_CHECK(Plan.getExecutionOrder()[0] == Producer);
	RDG_CHECK(Plan.getExecutionOrder()[1] == Middle);
	RDG_CHECK(Plan.getExecutionOrder()[2] == Consumer);

	RDG_CHECK_EQ(Graph.getPasses()[Middle.Value].Dependencies.size(), size_t { 1 });
	RDG_CHECK(Graph.getPasses()[Middle.Value].Dependencies[0] == Producer);
	RDG_CHECK_EQ(Graph.getPasses()[Consumer.Value].Dependencies.size(), size_t { 1 });
	RDG_CHECK(Graph.getPasses()[Consumer.Value].Dependencies[0] == Middle);

	RecordingCommandList CommandList;
	Graph.execute(CommandList, Plan);
	RDG_CHECK_EQ(Order.size(), size_t { 3 });
	RDG_CHECK_EQ(Order[0], std::string("Produce"));
	RDG_CHECK_EQ(Order[1], std::string("Middle"));
	RDG_CHECK_EQ(Order[2], std::string("Consume"));
	// Pass 名会成为命令列表的调试标签.
	RDG_CHECK_EQ(CommandList.Labels.size(), size_t { 3 });
	RDG_CHECK_EQ(CommandList.Labels[0], std::string("Produce"));
	RDG_CHECK_EQ(CommandList.EndedLabels.size(), size_t { 3 });
}

void testExecutionOrderFollowsDependenciesNotDeclaration()
{
	std::vector<std::string> Order;

	RDGGraph Graph;
	ResourceId Target;

	// 后面的 Pass 写在前面, 但它生产 Target, 必须先执行.
	Graph.addPass("Consumer", [&](RDGBuilder& B)
	{
		const ResourceId Imported = B.importTexture("Imported", std::make_shared<StubImage>());
		B.read(Imported, EResourceState::PixelShaderResource);
		B.setExecute([&Order](RDGPassContext&) { Order.emplace_back("Consumer"); });
	});
	Graph.addPass("Producer", [&](RDGBuilder& B)
	{
		Target = B.createTexture("Target", ColorTarget("Target"));
		B.keepAlive(Target);
		B.write(Target, EResourceState::RenderTarget);
		B.setExecute([&Order](RDGPassContext&) { Order.emplace_back("Producer"); });
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 2 });
	RDG_CHECK(Plan.getExecutionOrder()[0] == PassId { 0 });
	RDG_CHECK(Plan.getExecutionOrder()[1] == PassId { 1 });
	// 绑定表必须按全部资源建立, 即使没有资源池也要能取到导入资源.
	RDG_CHECK_EQ(Plan.getBindings().size(), Graph.getResources().size());
	RDG_CHECK_EQ(Plan.getBindings().size(), size_t { 2 });
	RDG_CHECK(Plan.getBindings()[0].isValid());

	RecordingCommandList CommandList;
	Graph.execute(CommandList, Plan);
	RDG_CHECK_EQ(Order.size(), size_t { 2 });
	RDG_CHECK_EQ(Order[0], std::string("Consumer"));
	RDG_CHECK_EQ(Order[1], std::string("Producer"));
}

void testDiamondDependencies()
{
	RDGGraph Graph;
	ResourceId Source;
	ResourceId BranchA;
	ResourceId BranchB;
	ResourceId Merged;

	Graph.addPass("Source", [&](RDGBuilder& B)
	{
		Source = B.createTexture("Source", ColorTarget("Source"));
		B.write(Source, EResourceState::RenderTarget);
	});
	Graph.addPass("BranchA", [&](RDGBuilder& B)
	{
		BranchA = B.createTexture("BranchA", ColorTarget("BranchA"));
		B.read(Source, EResourceState::PixelShaderResource);
		B.write(BranchA, EResourceState::RenderTarget);
	});
	Graph.addPass("BranchB", [&](RDGBuilder& B)
	{
		BranchB = B.createTexture("BranchB", ColorTarget("BranchB"));
		B.read(Source, EResourceState::PixelShaderResource);
		B.write(BranchB, EResourceState::RenderTarget);
	});
	Graph.addPass("Merge", [&](RDGBuilder& B)
	{
		Merged = B.createTexture("Merged", ColorTarget("Merged"));
		B.keepAlive(Merged);
		B.read(BranchA, EResourceState::PixelShaderResource);
		B.read(BranchB, EResourceState::PixelShaderResource);
		B.write(Merged, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);

	// 声明顺序恰好满足依赖; 关键是 Merge 必须排在两个分支之后.
	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 4 });
	RDG_CHECK(Plan.getExecutionOrder()[3] == PassId { 3 });
	RDG_CHECK_EQ(Graph.getPasses()[3].Dependencies.size(), size_t { 2 });

	// GBuffer: Source(RenderTarget -> PixelShaderResource) + BranchA(Undefined -> RenderTarget)
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 1 }).size(), size_t { 2 });
	// BranchB 读 Source: 已经是 PixelShaderResource, 无需转换; 只写自己的分支目标.
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 2 }).size(), size_t { 1 }); // Undefined -> RenderTarget(BranchB)

	const RDGAliasValidation Validation = Plan.validateAliasing(Graph.getResources());
	RDG_CHECK(Validation.isValid);
}

// ============================================================================
// 2. Pass 裁剪
// ============================================================================

void testPassCulling()
{
	RDGGraph Graph;
	ResourceId Dead;
	ResourceId DeadSource;
	ResourceId Live;
	ResourceId LiveSource;

	Graph.addPass("DeadProducer", [&](RDGBuilder& B)
	{
		Dead = B.createTexture("Dead", ColorTarget("Dead"));
		B.write(Dead, EResourceState::RenderTarget);
	});
	Graph.addPass("DeadConsumer", [&](RDGBuilder& B)
	{
		DeadSource = B.createTexture("DeadSource", ColorTarget("DeadSource"));
		B.read(Dead, EResourceState::PixelShaderResource);
		B.write(DeadSource, EResourceState::RenderTarget);
	});
	Graph.addPass("LiveProducer", [&](RDGBuilder& B)
	{
		Live = B.createTexture("Live", ColorTarget("Live"));
		B.write(Live, EResourceState::RenderTarget);
	});
	Graph.addPass("LiveConsumer", [&](RDGBuilder& B)
	{
		LiveSource = B.createTexture("LiveSource", ColorTarget("LiveSource"));
		B.keepAlive(LiveSource);
		B.read(Live, EResourceState::PixelShaderResource);
		B.write(LiveSource, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);

	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 2 });
	RDG_CHECK_EQ(Plan.getCulledPassCount(), 2u);
	RDG_CHECK(Plan.getExecutionOrder()[0] == PassId { 2 });
	RDG_CHECK(Plan.getExecutionOrder()[1] == PassId { 3 });

	const RDGResource* DeadResource = Graph.findResource(Dead);
	RDG_CHECK(DeadResource != nullptr);
	RDG_CHECK_EQ(DeadResource->FirstUse, InvalidId);

	// 被裁掉的资源不参与显存分配.
	RecordingPool Pool;
	const RDGCompiledPlan PooledPlan = Graph.compile(&Pool, true);
	RDG_CHECK(PooledPlan.getCulledPassCount() == 2u);
	for (const RecordingPool::Allocation& Allocation : Pool.Allocations)
		RDG_CHECK(Allocation.ResourceIndex != Dead.Value);
}

// ============================================================================
// 3. 生命周期
// ============================================================================

void testLifetimes()
{
	RDGGraph Graph;
	ResourceId Long;
	ResourceId Short;

	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		Long = B.createTexture("Long", ColorTarget("Long"));
		B.keepAlive(Long);
		B.write(Long, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		Short = B.createTexture("Short", ColorTarget("Short"));
		B.read(Long, EResourceState::PixelShaderResource);
		B.write(Short, EResourceState::RenderTarget);
	});
	Graph.addPass("P2", [&](RDGBuilder& B)
	{
		B.read(Long, EResourceState::PixelShaderResource);
		B.read(Short, EResourceState::PixelShaderResource);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 3 });

	const RDGResource* LongResource = Graph.findResource(Long);
	const RDGResource* ShortResource = Graph.findResource(Short);
	RDG_CHECK(LongResource != nullptr);
	RDG_CHECK(ShortResource != nullptr);
	if (!LongResource || !ShortResource)
		return;

	RDG_CHECK_EQ(LongResource->FirstUse, 0u);
	RDG_CHECK_EQ(LongResource->LastUse, 2u);
	RDG_CHECK_EQ(ShortResource->FirstUse, 1u);
	RDG_CHECK_EQ(ShortResource->LastUse, 2u);
	RDG_CHECK_EQ(LongResource->UseCount, 3u);
	RDG_CHECK_EQ(ShortResource->UseCount, 2u);
}

// ============================================================================
// 4. 别名计划
// ============================================================================

void testAliasPlanReusesDisjointLifetimes()
{
	RDGGraph Graph;
	ResourceId First;
	ResourceId Second;
	ResourceId Overlapping;

	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		First = B.createTexture("First", ColorTarget("First"));
		B.keepAlive(First);
		B.write(First, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		Second = B.createTexture("Second", ColorTarget("Second"));
		B.keepAlive(Second);
		B.write(Second, EResourceState::RenderTarget);
	});
	Graph.addPass("P2", [&](RDGBuilder& B)
	{
		Overlapping = B.createTexture("Overlapping", ColorTarget("Overlapping"));
		B.keepAlive(Overlapping);
		B.read(Second, EResourceState::PixelShaderResource);
		B.write(Overlapping, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);

	const RDGAliasValidation Validation = Plan.validateAliasing(Graph.getResources());
	RDG_CHECK(Validation.isValid);
	// 堆内所有资源的显存区间必须互不相交.
	RDG_CHECK(Plan.validateNoOverlap(Graph.getResources()));

	const RDGResource* FirstResource = Graph.findResource(First);
	const RDGResource* SecondResource = Graph.findResource(Second);
	const RDGResource* OverlappingResource = Graph.findResource(Overlapping);
	RDG_CHECK(FirstResource && SecondResource && OverlappingResource);
	if (!FirstResource || !SecondResource || !OverlappingResource)
		return;

	// First 生命周期 [0,0], Second [1,2]: Second 复用了 First 的显存.
	RDG_CHECK(SecondResource->Aliased);
	RDG_CHECK_EQ(FirstResource->MemorySlot, SecondResource->MemorySlot);
	// Overlapping 与 Second 在同一个 Pass 内被使用 -> 生命周期重叠, 不能共用.
	RDG_CHECK(OverlappingResource->MemorySlot != SecondResource->MemorySlot);

	uint64_t NaiveTotal = 0;
	for (const RDGResource& Resource : Graph.getResources())
		if (Resource.isUsedTransient())
			NaiveTotal += Resource.computePlannedSize();
	// 别名永远不该比逐资源独立分配更贵.
	RDG_CHECK(Plan.getTotalHeapBytes() <= NaiveTotal);
	RDG_CHECK(Plan.getTotalHeapBytes() > 0);

	// 槽位内两个资源顺序摆放: First(0..32KB) 与 Second(32KB..64KB) 在同一槽位,
	// 因此该槽位需要预留 64KB, 而不是两个独立槽位的 64KB + 32KB.
	RDG_CHECK_EQ(Plan.getMemorySlots().size(), size_t { 2 });
	for (const RDGMemorySlot& Slot : Plan.getMemorySlots())
	{
		if (Slot.Slot == FirstResource->MemorySlot)
			RDG_CHECK_EQ(Slot.Size, FirstResource->AliasSize + SecondResource->AliasSize);
	}
}

void testAliasingCanBeDisabled()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.write(A, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		const ResourceId B2 = B.createTexture("B", ColorTarget("B"));
		B.keepAlive(B2);
		B.write(B2, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, false);
	for (const RDGMemorySlot& Slot : Plan.getMemorySlots())
		RDG_CHECK_EQ(Slot.Owners.size(), size_t { 1 });
}

void testSeparatePoolsForDifferentResourceKinds()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId Color = B.createTexture("Color", ColorTarget("Color"));
		const ResourceId Depth = B.createTexture("Depth", DepthTarget("Depth"));
		const ResourceId Storage = B.createBuffer("Storage", StorageBufferDesc("Storage", 4096));
		B.keepAlive(Color);
		B.keepAlive(Depth);
		B.keepAlive(Storage);
		B.write(Color, EResourceState::RenderTarget);
		B.write(Depth, EResourceState::DepthWrite);
		B.write(Storage, EResourceState::UnorderedAccess);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);

	// RT / DS / Buffer 三种堆必须分别创建, 不能混用别名空间.
	RDG_CHECK_EQ(Plan.getHeapLayouts().size(), size_t { 3 });
	std::vector<EPoolKind> Pools;
	for (const RDGHeapLayout& Layout : Plan.getHeapLayouts())
		Pools.push_back(Layout.Pool);
	RDG_CHECK(std::find(Pools.begin(), Pools.end(), EPoolKind::RenderTarget) != Pools.end());
	RDG_CHECK(std::find(Pools.begin(), Pools.end(), EPoolKind::DepthStencil) != Pools.end());
	RDG_CHECK(std::find(Pools.begin(), Pools.end(), EPoolKind::Buffer) != Pools.end());
}

// ============================================================================
// 5. 屏障推导
// ============================================================================

void testBarrierDerivation()
{
	RDGGraph Graph;
	ResourceId Shadow;
	ResourceId GBuffer;
	ResourceId SceneColor;
	// 执行期录制需要"资源已绑定", 因此这里用一个伪池实例化瞬态资源.
	RecordingPool Pool;

	Graph.addPass("Shadow", [&](RDGBuilder& B)
	{
		Shadow = B.createTexture("Shadow", DepthTarget("Shadow"));
		B.write(Shadow, EResourceState::DepthWrite);
	});
	Graph.addPass("GBuffer", [&](RDGBuilder& B)
	{
		GBuffer = B.createTexture("GBuffer", ColorTarget("GBuffer"));
		B.read(Shadow, EResourceState::PixelShaderResource);
		B.write(GBuffer, EResourceState::RenderTarget);
	});
	Graph.addPass("Lighting", [&](RDGBuilder& B)
	{
		SceneColor = B.createTexture("SceneColor", ColorTarget("SceneColor"));
		B.keepAlive(SceneColor);
		B.read(GBuffer, EResourceState::PixelShaderResource);
		B.read(Shadow, EResourceState::PixelShaderResource);
		B.write(SceneColor, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	const auto ShadowBarriers = Plan.findBarriers(PassId { 0 });
	RDG_CHECK_EQ(ShadowBarriers.size(), size_t { 1 });
	RDG_CHECK(ShadowBarriers[0].Before == EResourceState::Undefined);
	RDG_CHECK(ShadowBarriers[0].After == EResourceState::DepthWrite);

	// GBuffer: Shadow(DepthWrite -> PixelShaderResource) + GBuffer(Undefined -> RenderTarget)
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 1 }).size(), size_t { 2 });
	// Lighting: GBuffer(PixelShaderResource -> RenderTarget) + SceneColor(Undefined -> RenderTarget)
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 2 }).size(), size_t { 2 });

	RecordingCommandList CommandList;
	Graph.execute(CommandList, Plan);

	// 每个 Pass 恰好一次 barriers() 调用.
	RDG_CHECK_EQ(CommandList.BarrierBatches.size(), size_t { 3 });
	// 1(Shadow) + 2(GBuffer) + 2(Lighting) = 5
	RDG_CHECK_EQ(CommandList.Barriers.size(), size_t { 5 });
	if (CommandList.Barriers.size() < 3)
		return;
	RDG_CHECK(CommandList.Barriers[0].After == rhi::EResourceState::DepthWrite);
	RDG_CHECK(CommandList.Barriers[1].Before == rhi::EResourceState::DepthWrite);
	RDG_CHECK(CommandList.Barriers[1].After == rhi::EResourceState::PixelShaderResource);
}

void testAliasedReuseResetsLayout()
{
	RDGGraph Graph;
	ResourceId Overlapping;
	ResourceId First;
	ResourceId Second;
	RecordingPool Pool;

	// Overlapping 生命周期 [0,0]; First [1,1]; Second [2,2].
	// 三者生命周期互不重叠 -> 全部复用同一段显存, 复用点必须走 Undefined.
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		Overlapping = B.createTexture("Overlapping", ColorTarget("Overlapping"));
		B.keepAlive(Overlapping);
		B.write(Overlapping, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		First = B.createTexture("First", ColorTarget("First"));
		B.keepAlive(First);
		B.write(First, EResourceState::RenderTarget);
	});
	Graph.addPass("P2", [&](RDGBuilder& B)
	{
		Second = B.createTexture("Second", ColorTarget("Second"));
		B.keepAlive(Second);
		B.write(Second, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);
	const RDGResource* FirstResource = Graph.findResource(First);
	RDG_CHECK(FirstResource != nullptr);
	if (!FirstResource)
		return;
	// First 复用了 Overlapping 的显存 -> 它就是别名资源.
	RDG_CHECK(FirstResource->Aliased);
	RDG_CHECK_EQ(FirstResource->MemorySlot, Graph.findResource(Overlapping)->MemorySlot);

	// P1 的屏障必须是 Undefined -> RenderTarget, 而不是沿用 P0 留下的 RenderTarget.
	const auto Barriers = Plan.findBarriers(PassId { 1 });
	RDG_CHECK_EQ(Barriers.size(), size_t { 1 });
	if (Barriers.empty())
		return;
	RDG_CHECK(Barriers[0].AliasedMemory);
	RDG_CHECK(Barriers[0].Before == EResourceState::Undefined);
	RDG_CHECK(Barriers[0].After == EResourceState::RenderTarget);

	// 录制结果: 三次 Undefined -> RenderTarget.
	RecordingCommandList CommandList;
	Graph.execute(CommandList, Plan);
	RDG_CHECK_EQ(CommandList.Barriers.size(), size_t { 3 });
	for (const RecordingCommandList::BarrierRecord& Record : CommandList.Barriers)
	{
		RDG_CHECK(Record.Before == rhi::EResourceState::Undefined);
		RDG_CHECK(Record.After == rhi::EResourceState::RenderTarget);
	}
}

void testRedundantBarriersSuppressed()
{
	RDGGraph Graph;
	ResourceId Target;

	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		Target = B.createTexture("Target", ColorTarget("Target"));
		B.keepAlive(Target);
		B.write(Target, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		B.read(Target, EResourceState::PixelShaderResource);
	});
	Graph.addPass("P2", [&](RDGBuilder& B)
	{
		B.read(Target, EResourceState::PixelShaderResource);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 0 }).size(), size_t { 1 });
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 1 }).size(), size_t { 1 });
	RDG_CHECK_EQ(Plan.findBarriers(PassId { 2 }).size(), size_t { 0 });
}

void testReadWriteMergesIntoSingleTransition()
{
	RDGGraph Graph;
	ResourceId Target;
	ResourceId Output;

	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		Target = B.createTexture("Target", ColorTarget("Target"));
		B.keepAlive(Target);
		B.write(Target, EResourceState::RenderTarget);
	});
	// 同一 Pass 内先采样后混合写: 必须合并成一次转换(读状态 -> 写状态),
	// 而不是各自独立地产生两次相互矛盾的转换.
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		Output = B.createTexture("Output", ColorTarget("Output"));
		B.keepAlive(Output);
		B.read(Target, EResourceState::PixelShaderResource);
		B.write(Target, EResourceState::RenderTarget);
		B.write(Output, EResourceState::RenderTarget);
	});

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	const auto Barriers = Plan.findBarriers(PassId { 1 });
	// Output: Undefined -> RenderTarget.
	// Target: P0 留下的状态已是 RenderTarget, 而 P1 声明"读 PSR + 写 RT"合并成
	//         一个访问状态 RT, 因此只产生一次 RT -> RT 的执行依赖屏障(读写同 Pass).
	RDG_CHECK_EQ(Barriers.size(), size_t { 2 });
	bool SawMergedTarget = false;
	for (const RDGBarrier& Barrier : Barriers)
	{
		if (Barrier.Resource == Target)
		{
			SawMergedTarget = true;
			RDG_CHECK(Barrier.Before == EResourceState::RenderTarget);
			RDG_CHECK(Barrier.After == EResourceState::RenderTarget);
		}
	}
	RDG_CHECK(SawMergedTarget);
}

// ============================================================================
// 6. 资源池交互
// ============================================================================

void testPoolReceivesAliasedAllocations()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.keepAlive(A);
		B.write(A, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		const ResourceId B2 = B.createTexture("B", ColorTarget("B"));
		B.keepAlive(B2);
		B.write(B2, EResourceState::RenderTarget);
	});

	RecordingPool Pool;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::BeginFrame), 1u);
	// 别名槽位里的每个资源都以 placed resource 落盘(否则各自独立创建就没有别名收益).
	RDG_CHECK_EQ(Pool.Allocations.size(), size_t { 2 });
	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::PlacedImage), 2u);
	RDG_CHECK_EQ(Pool.FallbackImageCount, 0u);
	RDG_CHECK(Plan.isAliasingEnabled());
	RDG_CHECK_EQ(Pool.HeapReserved.size(), size_t { 1 });
}

void testPoolDegradesWhenAliasingUnsupported()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.keepAlive(A);
		B.write(A, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		const ResourceId B2 = B.createTexture("B", ColorTarget("B"));
		B.keepAlive(B2);
		B.write(B2, EResourceState::RenderTarget);
	});

	RecordingPool Pool;
	Pool.Aliasing = false;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	RDG_CHECK(!Plan.isAliasingEnabled());
	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::PlacedImage), 0u);
	// 池不支持别名时, 所有瞬态资源都退化为独立创建.
	RDG_CHECK_EQ(Pool.FallbackImageCount, 2u);
}

void testPoolDegradesWhenPlacedCreationRejected()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.keepAlive(A);
		B.write(A, EResourceState::RenderTarget);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		const ResourceId B2 = B.createTexture("B", ColorTarget("B"));
		B.keepAlive(B2);
		B.write(B2, EResourceState::RenderTarget);
	});

	RecordingPool Pool;
	Pool.RejectPlaced = true;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	// 堆申请成功但 placed 创建被驱动拒绝 -> 资源回退到独立创建, 功能不受影响.
	RDG_CHECK(Pool.count(RecordingPool::ECall::PlacedImage) > 0u);
	RDG_CHECK_EQ(Pool.FallbackImageCount, 2u);
	RDG_CHECK(!Plan.isAliasingEnabled());
}

void testImportedResourcesBypassPool()
{
	RDGGraph Graph;
	ResourceId Backbuffer;

	Graph.addPass("Import", [&](RDGBuilder& B)
	{
		// 导入资源天生导出, 无需 keepAlive.
		Backbuffer = B.importTexture("Backbuffer", std::make_shared<StubImage>());
	});
	Graph.addPass("Present", [&](RDGBuilder& B)
	{
		const ResourceId Found = B.getGraph().findResourceByName("Backbuffer");
		B.read(Found, EResourceState::PixelShaderResource);
	});

	RecordingPool Pool;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	RDG_CHECK_EQ(Pool.Allocations.size(), size_t { 0 });
	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::PlacedImage), 0u);
	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::Image), 0u);

	RDG_CHECK(Backbuffer.isValid());
	const RDGResourceBinding& Binding = Plan.getBindings()[Backbuffer.Value];
	RDG_CHECK(Binding.isValid());
	RDG_CHECK(Binding.Kind == EResourceKind::Texture);
}

void testBufferAliasingUsesBufferPool()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createBuffer("A", StorageBufferDesc("A", 4096));
		B.keepAlive(A);
		B.write(A, EResourceState::UnorderedAccess);
	});
	Graph.addPass("P1", [&](RDGBuilder& B)
	{
		const ResourceId B2 = B.createBuffer("B", StorageBufferDesc("B", 8192));
		B.keepAlive(B2);
		B.write(B2, EResourceState::UnorderedAccess);
	});

	RecordingPool Pool;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);

	RDG_CHECK(Plan.isAliasingEnabled());
	// A(4096) 与 B(8192) 生命周期不重叠 -> 同一槽位, 两个 Buffer 都建在堆上.
	RDG_CHECK_EQ(Pool.Allocations.size(), size_t { 2 });
	RDG_CHECK_EQ(Pool.count(RecordingPool::ECall::PlacedBuffer), 2u);
	// Buffer 池与 RT/DS 池互不混用: 只应有一个 Buffer 堆.
	RDG_CHECK_EQ(Plan.getHeapLayouts().size(), size_t { 1 });
	RDG_CHECK(Plan.getHeapLayouts()[0].Pool == EPoolKind::Buffer);
}

// ============================================================================
// 7. 上下文与错误处理
// ============================================================================

void testContextResolvesDeclaredTypesOnly()
{
	bool SawTexture = false;
	bool SawBuffer = false;
	bool SawTypeMismatch = false;

	RDGGraph Graph;
	Graph.addPass("Use", [&](RDGBuilder& B)
	{
		const ResourceId Texture = B.createTexture("Texture", ColorTarget("Texture"));
		const ResourceId Storage = B.createBuffer("Storage", StorageBufferDesc("Storage", 4096));
		B.keepAlive(Texture);
		B.write(Texture, EResourceState::UnorderedAccess);
		B.read(Storage, EResourceState::StorageBuffer);
		B.setExecute([&, Texture, Storage](RDGPassContext& Context)
		{
			SawTexture = Context.getTexture(Texture) != nullptr;
			SawBuffer = Context.getBuffer(Storage) != nullptr;
			SawTypeMismatch = Context.getBuffer(Texture) == nullptr &&
				Context.getTexture(Storage) == nullptr &&
				Context.getTexture(ResourceId {}) == nullptr;
		});
	});

	RecordingPool Pool;
	const RDGCompiledPlan Plan = Graph.compile(&Pool, true);
	RecordingCommandList CommandList;
	Graph.execute(CommandList, Plan);

	RDG_CHECK(SawTexture);
	RDG_CHECK(SawBuffer);
	RDG_CHECK(SawTypeMismatch);
	RDG_CHECK_EQ(CommandList.Labels.size(), size_t { 1 });
	RDG_CHECK_EQ(CommandList.Labels[0], std::string("Use"));
}

void testInvalidDeclarationsAreRejected()
{
	RDGGraph Graph;
	bool InvalidStateRejected = false;
	bool InvalidHandleRejected = false;
	bool InvalidDescRejected = false;
	bool ImportedNotKeepAlive = false;

	try
	{
		Graph.addPass("BadDesc", [&](RDGBuilder& B)
		{
			TextureDesc Desc = ColorTarget("Bad");
			Desc.Width = 0;
			(void)B.createTexture("Bad", Desc);
		});
	}
	catch (const std::invalid_argument&)
	{
		InvalidDescRejected = true;
	}

	Graph.addPass("Bad", [&](RDGBuilder& B)
	{
		const ResourceId Target = B.createTexture("Target", ColorTarget("Target"));
		try
		{
			// 写布局下不可能"只读".
			B.read(Target, EResourceState::RenderTarget);
		}
		catch (const std::invalid_argument&)
		{
			InvalidStateRejected = true;
		}
		try
		{
			B.write(ResourceId { 0xFFFFFFu }, EResourceState::RenderTarget);
		}
		catch (const std::invalid_argument&)
		{
			InvalidHandleRejected = true;
		}

		const ResourceId Imported = B.importTexture("Imported", std::make_shared<StubImage>());
		try
		{
			// 导入资源天生导出, 再标记 keepAlive 属于逻辑错误.
			B.keepAlive(Imported);
		}
		catch (const std::logic_error&)
		{
			ImportedNotKeepAlive = true;
		}
	});

	RDG_CHECK(InvalidDescRejected);
	RDG_CHECK(InvalidStateRejected);
	RDG_CHECK(InvalidHandleRejected);
	RDG_CHECK(ImportedNotKeepAlive);
}

void testPlannedSizeEstimation()
{
	RDGResource Texture;
	Texture.Kind = EResourceKind::Texture;
	TextureDesc Desc = ColorTarget("Estimate");
	Desc.Width = 16;
	Desc.Height = 16;
	Desc.MipLevels = 5;
	Texture.TextureDescription = Desc;

	// (256 + 64 + 16 + 4 + 1) texel * 8 byte = 2728
	RDG_CHECK_EQ(Texture.computePlannedSize(), uint64_t { 341 * 8 });

	RDGResource Buffer;
	Buffer.Kind = EResourceKind::Buffer;
	Buffer.BufferDescription = StorageBufferDesc("B", 1234);
	RDG_CHECK_EQ(Buffer.computePlannedSize(), uint64_t { 1234 });
}

void testReset()
{
	RDGGraph Graph;
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.keepAlive(A);
		B.write(A, EResourceState::RenderTarget);
	});
	RDG_CHECK_EQ(Graph.getResources().size(), size_t { 1 });
	RDG_CHECK_EQ(Graph.getPasses().size(), size_t { 1 });

	Graph.reset();
	RDG_CHECK_EQ(Graph.getResources().size(), size_t { 0 });
	RDG_CHECK_EQ(Graph.getPasses().size(), size_t { 0 });
	RDG_CHECK(!Graph.findResourceByName("A").isValid());

	// 复用同一个对象构建下一帧.
	Graph.addPass("P0", [&](RDGBuilder& B)
	{
		const ResourceId A = B.createTexture("A", ColorTarget("A"));
		B.keepAlive(A);
		B.write(A, EResourceState::RenderTarget);
	});
	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 1 });
}

void testDeferredPassBuilder()
{
	RDGGraph Graph;
	ResourceId Target;

	auto Pass = beginPass(Graph, "Deferred");
	Pass->setType(ERDGPassType::Raster);
	Target = Pass->createTexture("Target", ColorTarget("Target"));
	Pass->keepAlive(Target);
	Pass->setRenderTargets(std::span<const ResourceId>(&Target, 1));
	Pass->setExecute([](RDGPassContext&) {});
	RDG_CHECK_EQ(Pass.getPassId(), PassId { 0 });

	const RDGCompiledPlan Plan = Graph.compile(nullptr, true);
	RDG_CHECK_EQ(Plan.getExecutionOrder().size(), size_t { 1 });
	const RDGPass& PassRecord = Graph.getPasses()[0];
	RDG_CHECK(PassRecord.Type == ERDGPassType::Raster);
	RDG_CHECK(PassRecord.writes(Target));
}

} // namespace

// ============================================================================
// 主入口
// ============================================================================

namespace
{
using TestFn = void (*)();

struct TestCase
{
	const char* Name;
	TestFn Function;
};
} // namespace

int main(int argc, char** argv)
{
	const TestCase Cases[] = {
		{ "dependency graph + topological order", testDependencyAndOrder },
		{ "execution order follows dependencies", testExecutionOrderFollowsDependenciesNotDeclaration },
		{ "diamond dependencies", testDiamondDependencies },
		{ "pass culling", testPassCulling },
		{ "lifetime intervals", testLifetimes },
		{ "alias plan reuses disjoint lifetimes", testAliasPlanReusesDisjointLifetimes },
		{ "aliasing can be disabled", testAliasingCanBeDisabled },
		{ "separate heaps per resource kind", testSeparatePoolsForDifferentResourceKinds },
		{ "barrier derivation", testBarrierDerivation },
		{ "aliased reuse resets layout", testAliasedReuseResetsLayout },
		{ "redundant barriers suppressed", testRedundantBarriersSuppressed },
		{ "read+write merges into one transition", testReadWriteMergesIntoSingleTransition },
		{ "pool receives aliased allocations", testPoolReceivesAliasedAllocations },
		{ "pool degrades without aliasing support", testPoolDegradesWhenAliasingUnsupported },
		{ "pool degrades when placed rejected", testPoolDegradesWhenPlacedCreationRejected },
		{ "imported resources bypass the pool", testImportedResourcesBypassPool },
		{ "buffer aliasing uses the buffer pool", testBufferAliasingUsesBufferPool },
		{ "context resolves declared types only", testContextResolvesDeclaredTypesOnly },
		{ "invalid declarations are rejected", testInvalidDeclarationsAreRejected },
		{ "planned size estimation", testPlannedSizeEstimation },
		{ "graph reset and reuse", testReset },
		{ "deferred pass builder facade", testDeferredPassBuilder },
	};

	std::printf("RDG CPU tests\n");
	std::printf("--------------------------------------------------------------\n");

	// 支持只跑一个用例(定位崩溃用): SeedRDGTests <substring>
	const std::string Filter = argc > 1 ? argv[1] : std::string {};

	for (const TestCase& Case : Cases)
	{
		if (!Filter.empty() && std::string(Case.Name).find(Filter) == std::string::npos)
			continue;

		const int Before = g_failures;
		std::printf("%-46s ", Case.Name);
		std::fflush(stdout);
		try
		{
			Case.Function();
		}
		catch (const std::exception& Error)
		{
			std::printf("FAILED (exception: %s)\n", Error.what());
			std::fflush(stdout);
			++g_failures;
			continue;
		}
		std::printf("%s\n", g_failures == Before ? "ok" : "FAILED");
		std::fflush(stdout);
	}

	std::printf("--------------------------------------------------------------\n");
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	if (g_failures != 0)
	{
		std::printf("RDG CPU tests FAILED.\n");
		return 1;
	}
	std::printf("RDG CPU tests passed.\n");
	return 0;
}
