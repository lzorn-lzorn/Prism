/**
 * ============================================================================
 *  renderer/rdg/RDGPlan.cpp
 * ============================================================================
 *
 *  编译计划的校验与落盘:
 *
 *      RDGCompiledPlan::validateAliasing()  别名计划自洽性(生命周期不得重叠)
 *      RDGResourcePool                      把计划翻译成真实 RHI 资源
 *
 * ============================================================================
 */

#include <rdg/RDG.hpp>

#include <algorithm>

namespace renderer::rdg
{

// ============================================================================
// 别名自洽性检查
// ============================================================================

RDGAliasValidation RDGCompiledPlan::validateAliasing(
	std::span<const RDGResource> Resources) const
{
	RDGAliasValidation Result;

	for (const RDGMemorySlot& Slot : MemorySlots)
	{
		// 逐对检查: 同一段显存上的任意两个资源, 生命周期必须严格不重叠.
		for (size_t Left = 0; Left < Slot.Owners.size(); ++Left)
		{
			const ResourceId LeftId = Slot.Owners[Left];
			if (LeftId.Value >= Resources.size())
				continue;

			for (size_t Right = Left + 1; Right < Slot.Owners.size(); ++Right)
			{
				const ResourceId RightId = Slot.Owners[Right];
				if (RightId.Value >= Resources.size())
					continue;

				const RDGResource& A = Resources[LeftId.Value];
				const RDGResource& B = Resources[RightId.Value];
				if (A.FirstUse == InvalidId || B.FirstUse == InvalidId)
					continue;

				const bool Disjoint = A.LastUse < B.FirstUse || B.LastUse < A.FirstUse;
				if (!Disjoint)
				{
					Result.isValid = false;
					Result.FirstConflictSlot = Slot.Slot;
					Result.First = LeftId;
					Result.Second = RightId;
					return Result;
				}
			}
		}
	}

	return Result;
}

// ============================================================================
// 显存区间不重叠检查
// ============================================================================

bool RDGCompiledPlan::validateNoOverlap(std::span<const RDGResource> Resources) const
{
	struct Range
	{
		uint64_t Begin { 0 };
		uint64_t End { 0 };
	};

	// 按堆分组: 不同堆是独立显存, 不需要比较.
	std::vector<std::vector<Range>> RangesByHeap(HeapLayouts.size());
	for (const RDGResource& Resource : Resources)
	{
		if (!Resource.isUsedTransient() || Resource.HeapIndex >= RangesByHeap.size())
			continue;
		RangesByHeap[Resource.HeapIndex].push_back({
			Resource.AliasOffset, Resource.AliasOffset + Resource.AliasSize });
	}

	for (std::vector<Range>& Ranges : RangesByHeap)
	{
		std::sort(Ranges.begin(), Ranges.end(),
			[](const Range& Lhs, const Range& Rhs) { return Lhs.Begin < Rhs.Begin; });
		for (size_t Index = 1; Index < Ranges.size(); ++Index)
		{
			if (Ranges[Index].Begin < Ranges[Index - 1].End)
				return false;
		}
	}
	return true;
}

// ============================================================================
// 默认资源池
// ============================================================================

namespace
{
/** @brief 把堆大小向上取整到配置的粒度. */
[[nodiscard]] uint64_t roundUpToGranularity(uint64_t Value, uint64_t Granularity) noexcept
{
	if (Granularity <= 1)
		return Value;
	return (Value + Granularity - 1) / Granularity * Granularity;
}

/** @brief 把一个 RDG 堆布局翻译成 RHI 堆描述. */
[[nodiscard]] rhi::MemoryHeapDescriptor makeHeapDescriptor(const RDGHeapLayout& Layout)
{
	rhi::MemoryHeapDescriptor Desc;
	Desc.Size = Layout.ReservedSize;
	// 堆内子分配至少要满足资源计划用的对齐.
	Desc.Alignment = DefaultResourceAlignment;
	Desc.MemoryTypeBits = Layout.MemoryTypeBits;
	Desc.RequiredProperties = rhi::EMemoryProperty(rhi::EMemoryProperty_t::DeviceLocal);
	Desc.DebugName = "RDG_" + std::string(getPoolKindName(Layout.Pool));
	return Desc;
}
} // namespace

void RDGResourcePool::prepareResource(
	ResourceId Resource,
	EResourceKind Kind,
	RDGHeapLayout& Heap,
	const BufferDesc* Buffer,
	const TextureDesc* Texture)
{
	(void)Resource;
	if (!AliasingEnabled)
		return;

	std::optional<rhi::MemoryRequirements> Requirements;
	if (Kind == EResourceKind::Buffer && Buffer)
	{
		rhi::BufferRequirementsRequest Request;
		Request.Size = Buffer->Size;
		Request.Usage = Buffer->Usage;
		Request.MemoryUsage = Buffer->MemoryUsage;
		Request.MemoryProperty = Buffer->MemoryProperty;
		Request.PreferredMemoryProperty = Buffer->PreferredMemoryProperty;
		Request.DedicatedAllocation = Buffer->DedicatedAllocation;
		Requirements = Device->getBufferMemoryRequirements(Request);
	}
	else if (Kind == EResourceKind::Texture && Texture)
	{
		rhi::ImageRequirementsRequest Request;
		Request.Format = Texture->Format;
		Request.Dimension = Texture->Dimension;
		Request.Width = Texture->Width;
		Request.Height = Texture->Height;
		Request.Depth = Texture->Depth;
		Request.MipLevels = Texture->MipLevels;
		Request.ArrayLayers = Texture->ArrayLayers;
		Request.SharingMode = Texture->SharingMode;
		Request.Usage = Texture->Usage;
		Request.SampleCount = Texture->SampleCount;
		Request.MemoryProperty = Texture->MemoryProperty;
		Requirements = Device->getImageMemoryRequirements(Request);
	}

	if (!Requirements)
		return;
	// 堆必须能同时容纳全部资源 -> 取类型位交集.
	Heap.MemoryTypeBits = Heap.MemoryTypeBits == 0
		? Requirements->MemoryTypeBits
		: (Heap.MemoryTypeBits & Requirements->MemoryTypeBits);
}

void RDGResourcePool::beginFrame(std::span<const RDGHeapLayout> Layouts)
{
	// 上一帧切出去的区间全部归还给堆, 本帧重新按计划切分.
	for (const ReleasedRange& ReleasedRange : Released)
	{
		if (ReleasedRange.Heap && ReleasedRange.Heap->isValid())
			ReleasedRange.Heap->releaseSuballocation(ReleasedRange.Offset, ReleasedRange.Size);
	}
	Released.clear();
	CreatedHeaps.assign(Layouts.size(), false);

	if (!AliasingEnabled || Layouts.empty())
	{
		AliasingSupported = false;
		return;
	}

	if (Heaps.size() < Layouts.size())
		Heaps.resize(Layouts.size());

	bool AllCreated = true;
	for (size_t Index = 0; Index < Layouts.size(); ++Index)
	{
		const RDGHeapLayout& Layout = Layouts[Index];
		uint64_t Size = std::max(Layout.ReservedSize, PoolOptions.MinimumHeapSize);
		Size = roundUpToGranularity(Size, PoolOptions.HeapSizeGranularity);
		if (Size > PoolOptions.MaximumHeapSize)
		{
			// 单个堆过大时放弃别名: 逐资源独立创建更安全.
			AllCreated = false;
			continue;
		}

		if (Heaps[Index] && Heaps[Index]->isValid() && Heaps[Index]->getSize() >= Size)
		{
			CreatedHeaps[Index] = true;
			continue;
		}

		rhi::MemoryHeapDescriptor Desc = makeHeapDescriptor(Layout);
		Desc.Size = Size;
		Heaps[Index] = Device->createTransientHeap(Desc);
		if (!Heaps[Index])
		{
			// 后端不支持 placed resource: 关闭别名, 但功能保持可用.
			AllCreated = false;
			continue;
		}
		CreatedHeaps[Index] = true;
	}

	AliasingSupported = AllCreated;
}

std::optional<RDGHeapAllocation> RDGResourcePool::allocateMemory(
	uint32_t HeapIndex,
	uint64_t Size,
	uint64_t Alignment,
	uint32_t ResourceIndex)
{
	(void)ResourceIndex;
	if (!AliasingSupported || HeapIndex >= Heaps.size() || HeapIndex >= CreatedHeaps.size() ||
		!CreatedHeaps[HeapIndex] || !Heaps[HeapIndex] || !Heaps[HeapIndex]->isValid())
		return std::nullopt;

	// 计划里的偏移只是"规划用"的; 真正切显存必须按驱动给出的对齐在这里做.
	const auto Offset = Heaps[HeapIndex]->suballocate(Size, std::max<uint64_t>(1, Alignment));
	if (!Offset)
		return std::nullopt;

	Released.push_back(ReleasedRange { Heaps[HeapIndex], *Offset, Size });
	return RDGHeapAllocation { Heaps[HeapIndex], *Offset, Size };
}

std::shared_ptr<rhi::RBuffer> RDGResourcePool::createPlacedBuffer(
	const RDGHeapAllocation& Allocation,
	const BufferDesc& Description)
{
	if (!Allocation.Heap)
		return {};

	rhi::RBuffer::Descriptor_t Desc;
	Desc.Size = Description.Size;
	Desc.Usage = Description.Usage;
	Desc.MemoryUsage = Description.MemoryUsage;
	Desc.MemoryProperty = Description.MemoryProperty;
	Desc.PreferredMemoryProperty = Description.PreferredMemoryProperty;
	Desc.MemoryPriority = Description.MemoryPriority;
	Desc.DedicatedAllocation = Description.DedicatedAllocation;
	Desc.PersistentlyMapped = Description.PersistentlyMapped;
	Desc.DebugName = Description.Name;

	return Device->createPlacedBuffer(Desc, Allocation.Heap, Allocation.Offset);
}

std::shared_ptr<rhi::RImage> RDGResourcePool::createPlacedImage(
	const RDGHeapAllocation& Allocation,
	const TextureDesc& Description)
{
	if (!Allocation.Heap)
		return {};

	rhi::RImage::Descriptor_t Desc;
	Desc.Format = Description.Format;
	Desc.Dimension = Description.Dimension;
	Desc.Width = Description.Width;
	Desc.Height = Description.Height;
	Desc.Depth = Description.Depth;
	Desc.MipLevels = Description.MipLevels;
	Desc.ArrayLayers = Description.ArrayLayers;
	Desc.SharingMode = Description.SharingMode;
	Desc.MemoryProperty = Description.MemoryProperty;
	Desc.Usage = Description.Usage;
	Desc.SampleCount = Description.SampleCount;

	return Device->createPlacedImage(Desc, Allocation.Heap, Allocation.Offset);
}

std::shared_ptr<rhi::RBuffer> RDGResourcePool::createBuffer(const BufferDesc& Description)
{
	rhi::RBuffer::Descriptor_t Desc;
	Desc.Size = Description.Size;
	Desc.Usage = Description.Usage;
	Desc.MemoryUsage = Description.MemoryUsage;
	Desc.MemoryProperty = Description.MemoryProperty;
	Desc.PreferredMemoryProperty = Description.PreferredMemoryProperty;
	Desc.MemoryPriority = Description.MemoryPriority;
	Desc.DedicatedAllocation = Description.DedicatedAllocation;
	Desc.PersistentlyMapped = Description.PersistentlyMapped;
	Desc.DebugName = Description.Name;
	return Device->createBuffer(Desc);
}

std::shared_ptr<rhi::RImage> RDGResourcePool::createImage(const TextureDesc& Description)
{
	rhi::RImage::Descriptor_t Desc;
	Desc.Format = Description.Format;
	Desc.Dimension = Description.Dimension;
	Desc.Width = Description.Width;
	Desc.Height = Description.Height;
	Desc.Depth = Description.Depth;
	Desc.MipLevels = Description.MipLevels;
	Desc.ArrayLayers = Description.ArrayLayers;
	Desc.SharingMode = Description.SharingMode;
	Desc.MemoryProperty = Description.MemoryProperty;
	Desc.Usage = Description.Usage;
	Desc.SampleCount = Description.SampleCount;
	return Device->createImage(Desc);
}

void RDGResourcePool::releaseHeaps()
{
	Heaps.clear();
	Released.clear();
}

} // namespace renderer::rdg
