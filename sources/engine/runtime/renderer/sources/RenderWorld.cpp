#include <renderer/RenderWorld.hpp>

#include <algorithm>

namespace runtime::renderer
{

uint32_t RenderWorld::addProxy(RenderProxy InProxy)
{
	Proxies.push_back(std::move(InProxy));
	ChangeFlags.set(EChangeFlags_t::Geometry);
	ChangeFlags.set(EChangeFlags_t::Visibility);
	return static_cast<uint32_t>(Proxies.size() - 1);
}

void RenderWorld::removeProxy(uint32_t Index)
{
	if (Index >= Proxies.size())
		return;
	// 交换到末尾后弹出, 保持数组紧凑; 调用方必须放弃所有旧索引.
	std::swap(Proxies[Index], Proxies.back());
	Proxies.pop_back();
	ChangeFlags.set(EChangeFlags_t::Geometry);
	ChangeFlags.set(EChangeFlags_t::Visibility);
}

void RenderWorld::updateTransform(uint32_t Index, const core::Matrix4x4& LocalToWorld)
{
	if (Index >= Proxies.size())
		return;
	Proxies[Index].LocalToWorld = LocalToWorld;
	ChangeFlags.set(EChangeFlags_t::Transform);
}

void RenderWorld::updateMaterial(uint32_t Index, uint32_t MaterialId)
{
	if (Index >= Proxies.size())
		return;
	Proxies[Index].MaterialId = MaterialId;
	ChangeFlags.set(EChangeFlags_t::Material);
}

void RenderWorld::markChanged(EChangeFlags Flags) noexcept
{
	ChangeFlags.Value |= Flags.Value;
}

EChangeFlags RenderWorld::consumeChangeFlags() noexcept
{
	EChangeFlags Result = ChangeFlags;
	ChangeFlags = EChangeFlags_t::None;
	return Result;
}

void RenderWorld::collectVisible(std::vector<VisibleProxy>& Out) const
{
	Out.clear();
	Out.reserve(Proxies.size());

	for (uint32_t Index = 0; Index < Proxies.size(); ++Index)
	{
		const RenderProxy& P = Proxies[Index];
		Out.push_back({
			.ProxyIndex = Index,
			.Key = makeSortKey(P.Transparent, P.MaterialId, P.StableOrder),
		});
	}

	// 按 SortKey 稳定排序: 不透明在前(按材质聚合), 透明在后(按提交序).
	std::stable_sort(Out.begin(), Out.end(), [](const VisibleProxy& Lhs,
												const VisibleProxy& Rhs) {
		return Lhs.Key < Rhs.Key;
	});
}

const RenderProxy* RenderWorld::getProxy(uint32_t Index) const noexcept
{
	return Index < Proxies.size() ? &Proxies[Index] : nullptr;
}

RenderProxy* RenderWorld::getProxy(uint32_t Index) noexcept
{
	return Index < Proxies.size() ? &Proxies[Index] : nullptr;
}

DrawPacket::SortKey RenderWorld::makeSortKey(bool Transparent, uint32_t MaterialId,
	uint32_t StableOrder) noexcept
{
	const uint64_t TransparentBit = Transparent ? (uint64_t { 1 } << 63) : 0;
	const uint64_t MaterialBits = (uint64_t { MaterialId } & 0x7FFFFFFF) << 32;
	const uint64_t OrderBits = uint64_t { StableOrder } & 0xFFFFFFFF;
	return TransparentBit | MaterialBits | OrderBits;
}

} // namespace runtime::renderer
