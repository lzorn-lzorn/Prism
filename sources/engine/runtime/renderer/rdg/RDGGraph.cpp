/**
 * ============================================================================
 *  renderer/rdg/RDGGraph.cpp
 * ============================================================================
 *
 *  RDG 的图算法与执行实现.
 *
 *  编译管线(全部为纯 CPU 逻辑, 不接触设备):
 *
 *      buildDependencyGraph()  收集 RAW / WAR / WAW 边
 *      topologicalSort()       Kahn + 最小堆, 得到稳定执行序
 *      analyzeLifetimes()      统计 [FirstUse, LastUse]
 *      cullPasses()            删除对最终输出无贡献的 Pass
 *      buildAliasPlan()        按堆做区间分配, 产出显存槽位
 *      generateBarriers()      按显存槽位推导状态转换
 *      instantiateResources()  把计划落到真实 RHI 资源(可选)
 *
 * ============================================================================
 */

#include <rdg/RDG.hpp>

#include <queue>
#include <stdexcept>

namespace renderer::rdg
{

namespace
{

// ============================================================================
// 显存槽位
// ============================================================================

/** @brief 把大小向上取整到 2 的幂, 保证槽位内的空闲区间表一定是合并过的. */
[[nodiscard]] constexpr uint64_t roundUpToPowerOfTwo(uint64_t Value) noexcept
{
	uint64_t Result = 1;
	while (Result < Value && Result < (uint64_t { 1 } << 62))
		Result <<= 1;
	return Result;
}

/** @brief 向上对齐到 Alignment(Alignment 为 0 时视为 1). */
[[nodiscard]] constexpr uint64_t alignUpValue(uint64_t Value, uint64_t Alignment) noexcept
{
	const uint64_t Step = std::max<uint64_t>(1, Alignment);
	return (Value + Step - 1) / Step * Step;
}

/**
 * @brief 一段按时间区间复用的显存.
 *
 * 槽位内允许放进多个资源, 但任意两个资源的 [FirstUse, LastUse] 生命周期不得重叠.
 * 槽位内部用空闲区间表切分: 资源过期后归还区间, 同一段显存才能再次被复用.
 * 堆总大小 = 该堆内所有槽位容量之和 —— 比自建通用堆分配器更容易解释与复核.
 */
struct AliasSlot
{
	struct Occupancy
	{
		ResourceId Resource {};
		uint32_t   FirstUse { 0 };
		uint32_t   LastUse { 0 };
		uint64_t   Offset { 0 };
		uint64_t   Size { 0 };
	};

	uint32_t Slot { InvalidId };
	uint32_t HeapIndex { InvalidId };
	/** @brief 本槽位在堆内的起始偏移. */
	uint64_t Base { 0 };
	uint64_t Capacity { 0 };
	/** @brief 槽位真正需要预留的显存: 峰值同时存活量. */
	uint64_t HighWater { 0 };
	std::vector<Occupancy> Occupancies;

	[[nodiscard]] static uint64_t alignUp(uint64_t Value, uint64_t Alignment) noexcept
	{
		return (Value + Alignment - 1) / Alignment * Alignment;
	}

	/** @brief 生命周期是否与槽位内任一占用者重叠. */
	[[nodiscard]] bool isFreeFor(uint32_t FirstUse, uint32_t LastUse) const noexcept
	{
		for (const Occupancy& Used : Occupancies)
		{
			if (!(LastUse < Used.FirstUse || Used.LastUse < FirstUse))
				return false;
		}
		return true;
	}

	/**
	 * @brief 在槽位内为一段新占用挑选偏移.
	 *
	 * 槽位内的占用者按生命周期已经互不重叠, 因此只需按时间先后顺序摆放:
	 * 起点取"所有在本段开始前就已结束的占用"的最大结束偏移.
	 *
	 * @note 为什么不把空闲区间直接复用: 复用同一段显存的两个资源对驱动来说
	 * 是同一个对象, 布局状态必须连续, 因此每个占用都拿到一段独立字节区间,
	 * 槽位容量收敛到"峰值同时存活量"即可.
	 */
	[[nodiscard]] uint64_t allocate(
		uint64_t Size,
		uint64_t Alignment,
		uint32_t FirstUse,
		uint32_t LastUse)
	{
		Alignment = std::max<uint64_t>(1, Alignment);
		if (Size == 0)
			return 0;

		uint64_t Cursor = 0;
		for (const Occupancy& Used : Occupancies)
		{
			if (Used.LastUse >= FirstUse)
				continue;
			Cursor = std::max(Cursor, Used.Offset + Used.Size);
		}

		const uint64_t Offset = alignUp(Cursor, Alignment);
		Capacity = std::max(Capacity, Offset + Size);
		HighWater = Capacity;
		return Offset;
	}

	/** @brief 记录一段占用; 资源句柄与偏移由调用方回填. */
	void addOccupancy(ResourceId InResource, uint32_t FirstUse, uint32_t LastUse, uint64_t Offset, uint64_t Size)
	{
		Occupancies.push_back({ InResource, FirstUse, LastUse, Offset, Size });
	}
};

} // namespace

// ============================================================================
// 资源大小 / 对齐
// ============================================================================

// (RDGResource::computePlannedSize / computePlannedAlignment 在头文件中内联实现)

// ============================================================================
// 构建阶段
// ============================================================================

RDGPass& RDGGraph::beginPass(std::string_view Name)
{
	Passes.emplace_back();
	RDGPass& Pass = Passes.back();
	Pass.Id.Value = static_cast<uint32_t>(Passes.size() - 1);
	Pass.Name.assign(Name);
	return Pass;
}

void RDGGraph::endPass(RDGPass& Pass)
{
	NameToPass.insert_or_assign(Pass.Name, Pass.Id);
}

ResourceId RDGGraph::registerResource(RDGResource Resource)
{
	const uint32_t Index = static_cast<uint32_t>(Resources.size());
	Resource.Id.Value = Index;
	Resources.push_back(std::move(Resource));

	RDGResource& Stored = Resources.back();
	NameToResource.insert_or_assign(Stored.Name, Stored.Id);
	return Stored.Id;
}

ResourceId RDGGraph::findResourceByName(std::string_view Name) const noexcept
{
	const auto It = NameToResource.find(std::string(Name));
	return It == NameToResource.end() ? ResourceId {} : It->second;
}

PassId RDGGraph::findPassByName(std::string_view Name) const noexcept
{
	const auto It = NameToPass.find(std::string(Name));
	return It == NameToPass.end() ? PassId {} : It->second;
}

void RDGGraph::reset()
{
	Resources.clear();
	Passes.clear();
	SortedPasses.clear();
	NameToResource.clear();
	NameToPass.clear();
	AliasingEnabled = true;
}

// ---- RDGBuilder ----

ResourceId RDGBuilder::createTexture(std::string_view Name, const TextureDesc& Description)
{
	if (!Description.isValid())
		throw std::invalid_argument("RDG: transient texture description is invalid.");

	RDGResource Resource;
	Resource.Name = std::string(Name);
	Resource.Kind = EResourceKind::Texture;
	Resource.Lifetime = EResourceLifetime::Transient;
	Resource.TextureDescription = Description;
	Resource.TextureDescription->Name = Resource.Name;

	if (rhi::isDepthFormat(Description.Format))
		Resource.PoolIndex = EPoolKind::DepthStencil;
	else if (Description.Usage.has(rhi::EImageUsage_t::Target))
		Resource.PoolIndex = EPoolKind::RenderTarget;
	else if (Description.Usage.has(rhi::EImageUsage_t::Storage))
		Resource.PoolIndex = EPoolKind::UAV;
	else
		Resource.PoolIndex = EPoolKind::RenderTarget;

	return Graph->registerResource(std::move(Resource));
}

ResourceId RDGBuilder::createBuffer(std::string_view Name, const BufferDesc& Description)
{
	if (!Description.isValid())
		throw std::invalid_argument("RDG: transient buffer description is invalid.");

	RDGResource Resource;
	Resource.Name = std::string(Name);
	Resource.Kind = EResourceKind::Buffer;
	Resource.Lifetime = EResourceLifetime::Transient;
	Resource.BufferDescription = Description;
	Resource.BufferDescription->Name = Resource.Name;
	Resource.PoolIndex = EPoolKind::Buffer;
	return Graph->registerResource(std::move(Resource));
}

ResourceId RDGBuilder::importTexture(std::string_view Name, std::shared_ptr<rhi::RImage> Image)
{
	if (!Image)
		throw std::invalid_argument("RDG: imported texture must be non-null.");

	RDGResource Resource;
	Resource.Name = std::string(Name);
	Resource.Kind = EResourceKind::Texture;
	Resource.Lifetime = EResourceLifetime::Imported;
	Resource.ImportedImage = std::move(Image);
	Resource.KeepAlive = true;
	return Graph->registerResource(std::move(Resource));
}

ResourceId RDGBuilder::importBuffer(std::string_view Name, std::shared_ptr<rhi::RBuffer> Buffer)
{
	if (!Buffer)
		throw std::invalid_argument("RDG: imported buffer must be non-null.");

	RDGResource Resource;
	Resource.Name = std::string(Name);
	Resource.Kind = EResourceKind::Buffer;
	Resource.Lifetime = EResourceLifetime::Imported;
	Resource.ImportedBuffer = std::move(Buffer);
	Resource.KeepAlive = true;
	return Graph->registerResource(std::move(Resource));
}

RDGBuilder& RDGBuilder::keepAlive(ResourceId Id)
{
	RDGResource* Resource = Graph->findResourceMut(Id);
	if (!Resource)
		throw std::invalid_argument("RDG: keepAlive() received an invalid resource handle.");
	if (Resource->isImported())
		throw std::logic_error("RDG: imported resources are always kept alive.");
	Resource->KeepAlive = true;
	return *this;
}

EResourceAccessMask RDGBuilder::access(ResourceId Id, EResourceAccess Access, EResourceState State)
{
	RDGResource* Resource = Graph->findResourceMut(Id);
	if (!Resource)
		throw std::invalid_argument("RDG: access() received an invalid resource handle.");
	if (!isAccessValidInState(Access, State))
	{
		throw std::invalid_argument(
			"RDG: access mode is incompatible with the requested resource state on resource '" +
			Resource->Name + "'.");
	}

	for (RDGResourceAccess& Existing : Pass->Accesses)
	{
		if (!(Existing.Resource == Id))
			continue;
		Existing.Mask = static_cast<EResourceAccessMask>(Existing.Mask | toMask(Access));
		// 写状态优先: 读改写与"先读后写"都必须让 Pass 以写状态收尾.
		if (isWriteState(State) || !isWriteState(Existing.State))
			Existing.State = State;
		return Existing.Mask;
	}

	Pass->Accesses.push_back({ Id, toMask(Access), State });
	return toMask(Access);
}

RDGBuilder& RDGBuilder::read(ResourceId Id, EResourceState State)
{
	access(Id, EResourceAccess::Read, State);
	return *this;
}

RDGBuilder& RDGBuilder::write(ResourceId Id, EResourceState State)
{
	access(Id, EResourceAccess::Write, State);
	return *this;
}

RDGBuilder& RDGBuilder::readWrite(ResourceId Id, EResourceState State)
{
	access(Id, EResourceAccess::ReadWrite, State);
	return *this;
}

RDGBuilder& RDGBuilder::setRenderTargets(std::span<const ResourceId> Color, ResourceId Depth)
{
	for (ResourceId Target : Color)
		write(Target, EResourceState::RenderTarget);
	if (Depth.isValid())
		write(Depth, EResourceState::DepthWrite);
	return *this;
}

// ============================================================================
// 编译
// ============================================================================

RDGCompiledPlan RDGGraph::compile(IRDGResourcePool* Pool, bool EnableAliasing)
{
	AliasingEnabled = EnableAliasing;

	buildDependencyGraph();
	topologicalSort();
	analyzeLifetimes();
	cullPasses();
	buildAliasPlan(AliasingEnabled);
	generateBarriers();

	RDGCompiledPlan Plan;
	Plan.ExecutionOrder = SortedPasses;
	Plan.CulledPassCount = static_cast<uint32_t>(Passes.size() - SortedPasses.size());
	// 先按编译参数置位, 实例化阶段再根据池的实际能力修正.
	Plan.AliasingEnabled = AliasingEnabled;

	// 屏障与槽位信息复制到计划中, 之后图对象可以继续复用.
	Plan.PassBarriers.reserve(SortedPasses.size());
	for (PassId Id : SortedPasses)
	{
		RDGPassBarriers Entry;
		Entry.Pass = Id;
		const auto It = PassBarrierScratch.find(Id.Value);
		if (It != PassBarrierScratch.end())
			Entry.Barriers = It->second;
		Plan.PassBarriers.push_back(std::move(Entry));
	}

	Plan.MemorySlots = MemorySlotScratch;
	Plan.HeapLayouts = HeapLayoutScratch;
	for (RDGHeapLayout& Layout : Plan.HeapLayouts)
		Layout.SlotCount = 0;
	for (const RDGMemorySlot& Slot : Plan.MemorySlots)
	{
		if (Slot.HeapIndex < Plan.HeapLayouts.size())
			Plan.HeapLayouts[Slot.HeapIndex].SlotCount += 1;
	}
	for (const RDGHeapLayout& Layout : Plan.HeapLayouts)
		Plan.TotalHeapBytes += Layout.ReservedSize;

	// 先让池登记每个瞬态资源的显存需求(它会据此回填每个堆的内存类型位),
	// 再申请堆, 最后实例化资源.
	if (Pool)
	{
		for (const RDGResource& Resource : Resources)
		{
			if (!Resource.isUsedTransient() || Resource.HeapIndex >= Plan.HeapLayouts.size())
				continue;
			Pool->prepareResource(
				Resource.Id,
				Resource.Kind,
				Plan.HeapLayouts[Resource.HeapIndex],
				Resource.BufferDescription ? &*Resource.BufferDescription : nullptr,
				Resource.TextureDescription ? &*Resource.TextureDescription : nullptr);
		}

		Pool->beginFrame(Plan.HeapLayouts);
		if (!Pool->supportsAliasing())
			Plan.AliasingEnabled = false;
	}
	instantiateResources(Pool, Plan);

	return Plan;
}

void RDGGraph::buildDependencyGraph()
{
	for (RDGPass& Pass : Passes)
		Pass.Dependencies.clear();

	// 每个资源最近一次写入 / 读取它的 Pass.
	std::vector<PassId> LastWriter(Resources.size());
	std::vector<PassId> LastReader(Resources.size());

	for (RDGPass& Pass : Passes)
	{
		auto addDependency = [&Pass](PassId Producer)
		{
			if (Producer.isValid() && !(Producer == Pass.Id))
				Pass.Dependencies.push_back(Producer);
		};

		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			if (Access.Resource.Value >= Resources.size())
				continue;
			// 读 -> 之前的写 = RAW
			if (hasAccess(Access.Mask, EResourceAccess::Read))
				addDependency(LastWriter[Access.Resource.Value]);
			// 写 -> 之前的写 = WAW, 之前的读 = WAR
			if (hasAccess(Access.Mask, EResourceAccess::Write))
			{
				addDependency(LastWriter[Access.Resource.Value]);
				addDependency(LastReader[Access.Resource.Value]);
			}
		}

		std::sort(Pass.Dependencies.begin(), Pass.Dependencies.end(),
			[](PassId Lhs, PassId Rhs) { return Lhs.Value < Rhs.Value; });
		Pass.Dependencies.erase(
			std::unique(Pass.Dependencies.begin(), Pass.Dependencies.end()),
			Pass.Dependencies.end());

		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			if (Access.Resource.Value >= Resources.size())
				continue;
			if (hasAccess(Access.Mask, EResourceAccess::Write))
				LastWriter[Access.Resource.Value] = Pass.Id;
			if (hasAccess(Access.Mask, EResourceAccess::Read))
				LastReader[Access.Resource.Value] = Pass.Id;
		}
	}
}

void RDGGraph::topologicalSort()
{
	const size_t Count = Passes.size();
	SortedPasses.clear();
	SortedPasses.reserve(Count);

	std::vector<std::vector<uint32_t>> Dependents(Count);
	std::vector<uint32_t>              Indegree(Count, 0);

	for (const RDGPass& Pass : Passes)
	{
		Indegree[Pass.Id.Value] = static_cast<uint32_t>(Pass.Dependencies.size());
		for (PassId Dependency : Pass.Dependencies)
			Dependents[Dependency.Value].push_back(Pass.Id.Value);
	}

	// 最小堆: 入度为 0 时优先跑 PassId 更小的 Pass, 保证声明顺序即是执行顺序.
	std::priority_queue<uint32_t, std::vector<uint32_t>, std::greater<>> Ready;
	for (uint32_t Index = 0; Index < Count; ++Index)
		if (Indegree[Index] == 0)
			Ready.push(Index);

	while (!Ready.empty())
	{
		const uint32_t Current = Ready.top();
		Ready.pop();
		SortedPasses.push_back(PassId { Current });

		for (uint32_t Dependent : Dependents[Current])
			if (--Indegree[Dependent] == 0)
				Ready.push(Dependent);
	}

	if (SortedPasses.size() != Count)
		throw std::runtime_error("RDG: pass dependency graph contains a cycle.");
}

void RDGGraph::analyzeLifetimes()
{
	for (RDGResource& Resource : Resources)
	{
		Resource.FirstUse = InvalidId;
		Resource.LastUse = 0;
		Resource.UseCount = 0;
		Resource.ReferencingPasses.clear();
	}

	for (uint32_t TopoIndex = 0; TopoIndex < SortedPasses.size(); ++TopoIndex)
	{
		RDGPass& Pass = Passes[SortedPasses[TopoIndex].Value];
		Pass.TopoIndex = TopoIndex;

		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			if (Access.Resource.Value >= Resources.size())
				continue;
			RDGResource& Resource = Resources[Access.Resource.Value];
			if (!Resource.isTransient())
				continue;

			Resource.FirstUse = std::min(Resource.FirstUse, TopoIndex);
			Resource.LastUse = std::max(Resource.LastUse, TopoIndex);
			Resource.UseCount += 1;
			Resource.ReferencingPasses.push_back(Pass.Id);
		}
	}
}

void RDGGraph::cullPasses()
{
	if (SortedPasses.empty())
		return;

	/**
	 * Pass 裁剪.
	 *
	 * 一个 Pass 必须执行, 当且仅当满足下面任一条:
	 *   1. 它没有访问声明 —— 纯副作用 Pass(调试标记 / 时间戳), 无法用依赖表达;
	 *   2. 它写了被导出(KeepAlive)的资源, 或写了被导入的外部资源;
	 *   3. 它是只读 Pass —— 只读意味着它是消费者(后处理 / 表现), 删掉就没有输出了.
	 *
	 * 其余"只写不读、且不写导出资源"的 Pass 都是中间产物生产者的候选,
	 * 若它写的资源没有任何存活的消费者, 整条链都会被裁掉.
	 * 判定用"从消费者出发沿依赖反向传播"的闭包: 声明顺序 == 拓扑顺序的子集,
	 * 依赖边恒指向更早的 Pass, 因此一次反向扫描即可收敛.
	 */
	std::vector<bool> Alive(Passes.size(), false);
	for (PassId Id : SortedPasses)
	{
		const RDGPass& Pass = Passes[Id.Value];
		if (Pass.Accesses.empty())
		{
			Alive[Id.Value] = true;
			continue;
		}

		bool HasWrite = false;
		bool WritesExported = false;
		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			if (!hasAccess(Access.Mask, EResourceAccess::Write))
				continue;
			HasWrite = true;
			const RDGResource* Resource = findResource(Access.Resource);
			// 写导入资源等于改了外部状态(例如交换链图像), 必须保留.
			if (Resource && (Resource->KeepAlive || Resource->isImported()))
			{
				WritesExported = true;
				break;
			}
		}
		// 只读 Pass 与写导出资源的 Pass 都是"消费者", 直接保留.
		Alive[Id.Value] = WritesExported || !HasWrite;
	}

	// 反向传播: SortedPasses 是拓扑序, 因此从后往前一次扫描就能把闭包传开.
	for (size_t Index = SortedPasses.size(); Index-- > 0;)
	{
		const PassId Id = SortedPasses[Index];
		if (!Alive[Id.Value])
			continue;
		for (PassId Dependency : Passes[Id.Value].Dependencies)
			Alive[Dependency.Value] = true;
	}

	std::erase_if(SortedPasses, [&Alive](PassId Id) { return !Alive[Id.Value]; });

	// 3. 被裁掉的 Pass 不再算作资源的使用者.
	const auto isCulled = [&Alive](const RDGPass& Pass) { return !Alive[Pass.Id.Value]; };
	for (RDGResource& Resource : Resources)
	{
		std::erase_if(Resource.ReferencingPasses, [&](PassId Id)
		{
			return Id.Value < Passes.size() && isCulled(Passes[Id.Value]);
		});
		Resource.UseCount = static_cast<uint32_t>(Resource.ReferencingPasses.size());
	}

	// 4. 重新计算生命周期(拓扑下标已经改变).
	for (uint32_t TopoIndex = 0; TopoIndex < SortedPasses.size(); ++TopoIndex)
		Passes[SortedPasses[TopoIndex].Value].TopoIndex = TopoIndex;

	for (RDGResource& Resource : Resources)
	{
		Resource.FirstUse = InvalidId;
		Resource.LastUse = 0;
	}
	for (uint32_t TopoIndex = 0; TopoIndex < SortedPasses.size(); ++TopoIndex)
	{
		const RDGPass& Pass = Passes[SortedPasses[TopoIndex].Value];
		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			RDGResource* Resource = findResourceMut(Access.Resource);
			if (!Resource || !Resource->isTransient())
				continue;
			Resource->FirstUse = std::min(Resource->FirstUse, TopoIndex);
			Resource->LastUse = std::max(Resource->LastUse, TopoIndex);
		}
	}
}

void RDGGraph::buildAliasPlan(bool EnableAliasing)
{
	MemorySlotScratch.clear();
	HeapLayoutScratch.clear();
	PassBarrierScratch.clear();
	ResourceSlot.assign(Resources.size(), InvalidId);

	std::vector<AliasSlot> AliasSlotScratch;

	for (RDGResource& Resource : Resources)
	{
		Resource.Aliased = false;
		Resource.HeapIndex = InvalidId;
		Resource.MemorySlot = InvalidId;
		Resource.AliasOffset = 0;
		Resource.AliasSize = 0;
		Resource.PlannedSize = Resource.computePlannedSize();
		Resource.PlannedAlignment = Resource.computePlannedAlignment();
	}

	// 规划顺序: 堆 -> FirstUse -> 资源下标. 与声明顺序解耦, 保证结果稳定可复现.
	std::vector<ResourceId> Order;
	Order.reserve(Resources.size());
	for (const RDGResource& Resource : Resources)
		if (Resource.isUsedTransient())
			Order.push_back(Resource.Id);

	std::sort(Order.begin(), Order.end(), [this](ResourceId Lhs, ResourceId Rhs)
	{
		const RDGResource& A = Resources[Lhs.Value];
		const RDGResource& B = Resources[Rhs.Value];
		if (A.PoolIndex != B.PoolIndex)
			return A.PoolIndex < B.PoolIndex;
		if (A.FirstUse != B.FirstUse)
			return A.FirstUse < B.FirstUse;
		return A.Id.Value < B.Id.Value;
	});

	// 每个堆建立布局条目; 只有真正使用到的堆才会被创建.
	std::array<uint32_t, PoolKindCount> PoolHeapIndex {};
	PoolHeapIndex.fill(InvalidId);
	std::array<std::vector<uint32_t>, PoolKindCount> PoolSlots;

	if (EnableAliasing)
	{
		for (ResourceId Id : Order)
		{
			const RDGResource& Resource = Resources[Id.Value];
			const size_t Pool = static_cast<size_t>(Resource.PoolIndex);
			if (PoolHeapIndex[Pool] != InvalidId)
				continue;
			PoolHeapIndex[Pool] = static_cast<uint32_t>(HeapLayoutScratch.size());
			HeapLayoutScratch.push_back(RDGHeapLayout {
				.HeapIndex = PoolHeapIndex[Pool],
				.Pool = static_cast<EPoolKind>(Pool),
				.PeakSize = 0,
				.ReservedSize = 0,
				.SlotCount = 0
			});
		}
	}

	for (ResourceId Id : Order)
	{
		const size_t Pool = static_cast<size_t>(Resources[Id.Value].PoolIndex);
		const uint64_t Size = Resources[Id.Value].PlannedSize;
		const uint64_t Alignment = std::max<uint64_t>(1, Resources[Id.Value].PlannedAlignment);
		const uint32_t FirstUse = Resources[Id.Value].FirstUse;
		const uint32_t LastUse = Resources[Id.Value].LastUse;

		if (EnableAliasing)
		{
			uint32_t SlotIndex = InvalidId;
			uint64_t Offset = 0;
			// 该槽位此前是否已被别的资源占用过(决定本次是否算"显存别名共用").
			bool Reused = false;

			// 1. 依次尝试已有槽位: 只要生命周期与槽位内所有占用都不重叠就能放进去.
			for (uint32_t Candidate : PoolSlots[Pool])
			{
				AliasSlot& Slot = AliasSlotScratch[Candidate];
				if (!Slot.isFreeFor(FirstUse, LastUse))
					continue;

				Reused = !Slot.Occupancies.empty();
				Offset = Slot.allocate(Size, Alignment, FirstUse, LastUse);
				Slot.addOccupancy(Id, FirstUse, LastUse, Offset, Size);
				SlotIndex = Candidate;
				break;
			}

			// 2. 没有可用槽位: 新建一个. 它的堆内起始偏移在所有资源摆放完成后再统一分配
			//    (槽位容量会随着后续资源放进来而增长).
			if (SlotIndex == InvalidId)
			{
				AliasSlot Fresh;
				Fresh.Slot = static_cast<uint32_t>(AliasSlotScratch.size());
				Fresh.HeapIndex = PoolHeapIndex[Pool];
				Offset = Fresh.allocate(Size, Alignment, FirstUse, LastUse);
				Fresh.addOccupancy(Id, FirstUse, LastUse, Offset, Size);
				SlotIndex = Fresh.Slot;
				AliasSlotScratch.push_back(std::move(Fresh));
				PoolSlots[Pool].push_back(SlotIndex);
			}

			// @note 必须在 vector 插入之后重新取引用: push_back 会让旧引用失效.
			AliasSlot& Slot = AliasSlotScratch[SlotIndex];
			RDGResource& Resource = Resources[Id.Value];
			Resource.HeapIndex = Slot.HeapIndex;
			Resource.MemorySlot = SlotIndex;
			// 槽位内的相对偏移; 加上槽位基址才是堆内绝对偏移(基址稍后统一分配).
			Resource.AliasOffset = Offset;
			Resource.AliasSize = Size;
			// 与别的资源共用同一段显存才算"别名"; 独占槽位不算.
			Resource.Aliased = Reused;
			ResourceSlot[Id.Value] = SlotIndex;
		}
		else
		{
			// 关闭别名: 每个资源独占一段显存, 仅用于与开启别名时做对照.
			AliasSlot Fresh;
			Fresh.Slot = static_cast<uint32_t>(AliasSlotScratch.size());
			Fresh.HeapIndex = InvalidId;
			Fresh.Capacity = Size;
			Fresh.HighWater = Size;
			Fresh.Occupancies.push_back({ Id, FirstUse, LastUse, 0, Size });
			RDGResource& Resource = Resources[Id.Value];
			Resource.AliasOffset = 0;
			Resource.AliasSize = Size;
			ResourceSlot[Id.Value] = Fresh.Slot;
			AliasSlotScratch.push_back(std::move(Fresh));
		}
	}

	// 归纳堆布局: 预留大小取"该堆同时存活的最大字节数".
	//
	// 这是比"槽位容量之和"更紧的下界: 同一堆内的多个槽位也可能并不同时活跃,
	// 例如一个槽位只在前半帧使用, 另一个只在后半帧使用, 那么堆只需要
	// 两者中较大的那一个, 而不是两者相加.
	if (EnableAliasing)
	{
		// 先把每个槽位在堆内的区间定下来: 按创建顺序线性摆放, 互不相交.
		for (size_t Pool = 0; Pool < PoolKindCount; ++Pool)
		{
			uint64_t Cursor = 0;
			for (uint32_t SlotIndex : PoolSlots[Pool])
			{
				AliasSlot& Slot = AliasSlotScratch[SlotIndex];
				Slot.Base = alignUpValue(Cursor, DefaultResourceAlignment);
				// 槽位可能被多个资源复用, 但它们的时间区间不重叠, 所以实际跨度
				// 由槽位内部的最大占用末端决定.
				uint64_t Span = 0;
				for (const AliasSlot::Occupancy& Occupied : Slot.Occupancies)
					Span = std::max(Span, Occupied.Offset + Occupied.Size);
				Slot.Capacity = Span;
				Cursor = Slot.Base + Span;

				// 回填资源上的堆内绝对偏移.
				for (const AliasSlot::Occupancy& Occupied : Slot.Occupancies)
					if (RDGResource* Resource = findResourceMut(Occupied.Resource))
						Resource->AliasOffset = Slot.Base + Occupied.Offset;
			}
			// 预留大小 = 该池所有槽位线性摆放后的总跨度.
			// 由于槽位之间没有时间重叠, 这是"堆需要多大"的一个保守但正确的上界;
			// 下面的扫描线会给出同时存活量的下界并用于诊断.
			if (PoolHeapIndex[Pool] != InvalidId)
				HeapLayoutScratch[PoolHeapIndex[Pool]].ReservedSize = Cursor;
		}

		for (const AliasSlot& Slot : AliasSlotScratch)
		{
			RDGHeapLayout& Layout = HeapLayoutScratch[Slot.HeapIndex];
			Layout.SlotCount += 1;
			Layout.PeakSize = std::max(Layout.PeakSize, Slot.HighWater);
		}

		// 用扫描线累加同一时刻仍然存活的资源占用.
		std::vector<std::pair<uint32_t, uint64_t>> Begins;
		std::vector<std::pair<uint32_t, uint64_t>> Ends;
		for (size_t Pool = 0; Pool < PoolKindCount; ++Pool)
		{
			if (PoolHeapIndex[Pool] == InvalidId)
				continue;

			Begins.clear();
			Ends.clear();
			for (const AliasSlot& Slot : AliasSlotScratch)
			{
				if (static_cast<size_t>(HeapLayoutScratch[Slot.HeapIndex].Pool) != Pool)
					continue;
				for (const AliasSlot::Occupancy& Occupied : Slot.Occupancies)
				{
					Begins.emplace_back(Occupied.FirstUse, Occupied.Size);
					Ends.emplace_back(Occupied.LastUse, Occupied.Size);
				}
			}
			std::sort(Begins.begin(), Begins.end(),
				[](const auto& Lhs, const auto& Rhs) { return Lhs.first < Rhs.first; });
			std::sort(Ends.begin(), Ends.end(),
				[](const auto& Lhs, const auto& Rhs) { return Lhs.first < Rhs.first; });

			uint64_t Live = 0;
			uint64_t Peak = 0;
			size_t BeginCursor = 0;
			size_t EndCursor = 0;
			while (BeginCursor < Begins.size())
			{
				// 先处理所有在本时刻之前结束的区间.
				while (EndCursor < Ends.size() && Ends[EndCursor].first < Begins[BeginCursor].first)
				{
					Live -= std::min(Live, Ends[EndCursor].second);
					++EndCursor;
				}
				Live += Begins[BeginCursor].second;
				Peak = std::max(Peak, Live);
				++BeginCursor;
			}
			// 峰值(同时存活量的下界)只用于诊断: 它说明还有多少压缩空间.
			HeapLayoutScratch[PoolHeapIndex[Pool]].PeakSize =
				std::max(HeapLayoutScratch[PoolHeapIndex[Pool]].PeakSize, Peak);
		}

		// 峰值占用(诊断用)保持为单槽位内的最大同时存活量.
		for (const AliasSlot& Slot : AliasSlotScratch)
		{
			RDGHeapLayout& Layout = HeapLayoutScratch[Slot.HeapIndex];
			uint64_t Peak = 0;
			for (const AliasSlot::Occupancy& Probe : Slot.Occupancies)
			{
				uint64_t Live = 0;
				for (const AliasSlot::Occupancy& Other : Slot.Occupancies)
				{
					const bool Overlaps = !(Other.LastUse < Probe.FirstUse ||
						Probe.LastUse < Other.FirstUse);
					if (Overlaps)
						Live += Other.Size;
				}
				Peak = std::max(Peak, Live);
			}
			Layout.PeakSize = std::max(Layout.PeakSize, Peak);
		}
	}

	MemorySlotScratch.reserve(AliasSlotScratch.size());
	for (const AliasSlot& Slot : AliasSlotScratch)
	{
		RDGMemorySlot Entry;
		Entry.Slot = Slot.Slot;
		Entry.HeapIndex = Slot.HeapIndex;
		// 槽位在堆内的起始偏移(堆内绝对偏移).
		Entry.Offset = Slot.Base;
		Entry.Size = Slot.HighWater;
		Entry.Owners.reserve(Slot.Occupancies.size());
		for (const AliasSlot::Occupancy& Occupied : Slot.Occupancies)
			Entry.Owners.push_back(Occupied.Resource);
		if (Slot.HeapIndex != InvalidId)
			Entry.Pool = HeapLayoutScratch[Slot.HeapIndex].Pool;
		std::sort(Entry.Owners.begin(), Entry.Owners.end(), [this](ResourceId Lhs, ResourceId Rhs)
		{
			return Resources[Lhs.Value].FirstUse < Resources[Rhs.Value].FirstUse;
		});
		MemorySlotScratch.push_back(std::move(Entry));
	}
}

void RDGGraph::generateBarriers()
{
	PassBarrierScratch.clear();

	/**
	 * 状态挂在"显存槽位"上而不是"资源"上.
	 *
	 * 原因: 被别名复用的两个资源在驱动看来是同一段显存, 布局状态是连续的.
	 * 若按资源记录, 复用点就会漏掉 Undefined -> Target 的转换.
	 * LastUser 同时用于识别"槽位换人", 从而在复用点强制走一次 Undefined.
	 */
	struct SlotState
	{
		ResourceId     LastUser {};
		EResourceState State { EResourceState::Undefined };
		bool           Valid { false };
	};
	std::vector<SlotState> SlotStates(MemorySlotScratch.size());

	/** @brief 记录一次提交: 该槽位的最新使用者与布局. */
	auto commit = [&](uint32_t SlotIndex, ResourceId Resource, EResourceState Target)
	{
		SlotState& State = SlotStates[SlotIndex];
		State.LastUser = Resource;
		State.State = Target;
		State.Valid = true;
	};

	for (PassId Id : SortedPasses)
	{
		const RDGPass& Pass = Passes[Id.Value];
		if (Pass.Accesses.empty())
			continue;

		std::vector<RDGBarrier> Barriers;
		for (const RDGResourceAccess& Access : Pass.Accesses)
		{
			if (Access.Resource.Value >= ResourceSlot.size())
				continue;
			const uint32_t SlotIndex = ResourceSlot[Access.Resource.Value];
			if (SlotIndex == InvalidId)
				continue;

			const SlotState& State = SlotStates[SlotIndex];
			const bool SharedSlot = MemorySlotScratch[SlotIndex].Owners.size() > 1;
			// 同一槽位里换成另一个资源使用 -> 别名复用点.
			const bool AliasedReuse = SharedSlot && State.Valid &&
				!(State.LastUser == Access.Resource);
			const EResourceState Before = State.Valid && !AliasedReuse
				? State.State
				: EResourceState::Undefined;
			const EResourceState Target = Access.State;
			// 同一 Pass 内"先读后写"(采样后当附件写)即使布局不变也必须建立执行依赖.
			const bool ReadModifyWrite = hasAccess(Access.Mask, EResourceAccess::Read) &&
				hasAccess(Access.Mask, EResourceAccess::Write);

			// 第一次使用 / 需要换布局 / 换了资源 / 读写同 Pass, 四种情况都要插屏障.
			if (!State.Valid || Before != Target || AliasedReuse || ReadModifyWrite)
			{
				Barriers.push_back(RDGBarrier {
					.Resource = Access.Resource,
					.MemorySlot = SlotIndex,
					.Before = Before,
					.After = Target,
					.AliasedMemory = AliasedReuse
				});
			}

			commit(SlotIndex, Access.Resource, Target);
		}

		if (!Barriers.empty())
			PassBarrierScratch.insert_or_assign(Id.Value, std::move(Barriers));
	}
}

void RDGGraph::instantiateResources(IRDGResourcePool* Pool, RDGCompiledPlan& Plan)
{
	// 绑定表始终按资源数量建立: 没有池时导入资源依然要可用(设备无关测试/离屏回放).
	Plan.Bindings.assign(Resources.size(), RDGResourceBinding {});
	Plan.AllocationAlignments.assign(Resources.size(), DefaultResourceAlignment);

	for (RDGResource& Resource : Resources)
	{
		RDGResourceBinding& Binding = Plan.Bindings[Resource.Id.Value];
		Binding.Id = Resource.Id;
		Binding.Kind = Resource.Kind;
		if (Resource.isImported())
		{
			Binding.Buffer = Resource.ImportedBuffer;
			Binding.Image = Resource.ImportedImage;
		}
	}

	if (Pool == nullptr)
		return;

	// 阶段一: 先把所有需要别名的瞬态资源的显存区间申请下来.
	// 必须在创建任何资源之前完成: 复用同一槽位的两个资源需要引用对方的偏移.
	// "需要别名"是显存槽位的属性: 只要该槽位被一个以上资源共享, 其中每个资源
	// 都必须落在堆上(独立创建会各自占一份显存, 别名收益也就没了).
	struct Placement
	{
		std::optional<RDGHeapAllocation> Allocation;
	};
	std::vector<Placement> Placements(Resources.size());

	for (const RDGResource& Resource : Resources)
	{
		if (!Resource.isUsedTransient())
			continue;
		if (Resource.MemorySlot >= MemorySlotScratch.size())
			continue;
		if (Resource.HeapIndex >= Plan.HeapLayouts.size())
			continue;
		if (MemorySlotScratch[Resource.MemorySlot].Owners.size() <= 1)
			continue;

		Placements[Resource.Id.Value].Allocation = Pool->allocateMemory(
			Plan.HeapLayouts[Resource.HeapIndex].HeapIndex,
			Resource.AliasSize,
			Plan.AllocationAlignments[Resource.Id.Value],
			Resource.Id.Value);
	}

	// 阶段二: 创建资源. 能放进堆的就不要独立显存.
	bool AnyPlaced = false;
	bool AliasedRequest = false;
	for (RDGResource& Resource : Resources)
	{
		if (Resource.isImported() || !Resource.isUsedTransient())
			continue;

		RDGResourceBinding& Binding = Plan.Bindings[Resource.Id.Value];
		const std::optional<RDGHeapAllocation>& Allocation =
			Placements[Resource.Id.Value].Allocation;
		if (Allocation && Allocation->Heap)
		{
			AliasedRequest = true;
			Binding.MemoryOffset = Allocation->Offset;
			if (Resource.Kind == EResourceKind::Buffer && Resource.BufferDescription)
				Binding.Buffer = Pool->createPlacedBuffer(*Allocation, *Resource.BufferDescription);
			else if (Resource.Kind == EResourceKind::Texture && Resource.TextureDescription)
				Binding.Image = Pool->createPlacedImage(*Allocation, *Resource.TextureDescription);
			AnyPlaced = Binding.isValid();
		}

		// 堆不可用(或 placed 创建被后端拒绝)时退化到独立创建, 功能保持一致.
		if (!Binding.isValid())
		{
			Binding.MemoryOffset = 0;
			if (Resource.Kind == EResourceKind::Buffer && Resource.BufferDescription)
				Binding.Buffer = Pool->createBuffer(*Resource.BufferDescription);
			else if (Resource.Kind == EResourceKind::Texture && Resource.TextureDescription)
				Binding.Image = Pool->createImage(*Resource.TextureDescription);
		}
	}

	// 只有真正落到堆上的资源才算别名生效.
	Plan.AliasingEnabled = AliasedRequest && AnyPlaced && Pool->supportsAliasing();
}

// ============================================================================
// 执行
// ============================================================================

void RDGGraph::execute(rhi::RCommandList& CommandList, const RDGCompiledPlan& Plan) const
{
	std::vector<rhi::ImageBarrier>  ImageBarriers;
	std::vector<rhi::BufferBarrier> BufferBarriers;

	for (const RDGPassBarriers& Entry : Plan.PassBarriers)
	{
		if (Entry.Barriers.empty())
			continue;

		ImageBarriers.clear();
		BufferBarriers.clear();

		for (const RDGBarrier& Barrier : Entry.Barriers)
		{
			if (Barrier.Resource.Value >= Plan.Bindings.size())
				continue;
			const RDGResourceBinding& Binding = Plan.Bindings[Barrier.Resource.Value];

			if (Binding.Kind == EResourceKind::Texture && Binding.Image)
			{
				const RDGResource* Resource = findResource(Barrier.Resource);
				rhi::ImageBarrier Record;
				Record.Image = Binding.Image;
				Record.Before = toRHIState(Barrier.Before);
				Record.After = toRHIState(Barrier.After);
				Record.Range.Aspect = Resource && Resource->TextureDescription &&
					rhi::isDepthFormat(Resource->TextureDescription->Format)
					? rhi::EImageAspect::Depth
					: rhi::EImageAspect::Color;
				ImageBarriers.push_back(std::move(Record));
			}
			else if (Binding.Kind == EResourceKind::Buffer && Binding.Buffer)
			{
				rhi::BufferBarrier Record;
				Record.Buffer = Binding.Buffer;
				Record.Before = toRHIState(Barrier.Before);
				Record.After = toRHIState(Barrier.After);
				Record.Offset = 0;
				Record.Size = 0;
				BufferBarriers.push_back(std::move(Record));
			}
		}

		if (!ImageBarriers.empty() || !BufferBarriers.empty())
			CommandList.barriers({}, BufferBarriers, ImageBarriers);
	}

	for (PassId Id : Plan.ExecutionOrder)
	{
		const RDGPass& Pass = Passes[Id.Value];
		if (!Pass.Execute)
			continue;

		RDGPassContext PassContext(CommandList, Plan.Bindings, Pass);
		CommandList.beginDebugLabel(Pass.Name);
		Pass.Execute(PassContext);
		CommandList.endDebugLabel();
	}
}

} // namespace renderer::rdg
