#include <renderer/RenderProxyWorld.hpp>

#include <algorithm>

#include <type_traits>
#include <utility>

namespace runtime::renderer
{

namespace
{

[[nodiscard]] const RenderProxyHeader& getHeader(const RenderProxyVariant& Proxy)
{
	return std::visit([](const auto& Value) -> const RenderProxyHeader& {
		return Value.Header;
	}, Proxy);
}

[[nodiscard]] RenderProxyHeader& getHeader(RenderProxyVariant& Proxy)
{
	return std::visit([](auto& Value) -> RenderProxyHeader& {
		return Value.Header;
	}, Proxy);
}

[[nodiscard]] bool isGeometryProxy(ERenderProxyKind Kind) noexcept
{
	return Kind == ERenderProxyKind::OpaqueMesh ||
		Kind == ERenderProxyKind::TransparentMesh ||
		Kind == ERenderProxyKind::RigidBodyMesh ||
		Kind == ERenderProxyKind::TerrainPatch;
}

[[nodiscard]] bool isTransparentProxy(ERenderProxyKind Kind) noexcept
{
	return Kind == ERenderProxyKind::TransparentMesh;
}

} // namespace

uint32_t RenderProxyWorld::addProxy(RenderProxyVariant InProxy)
{
	const ERenderProxyKind Kind = getHeader(InProxy).Kind;
	Proxies.push_back(std::move(InProxy));

	if (isGeometryProxy(Kind))
	{
		ChangeFlags.set(EChangeFlags_t::Geometry);
		ChangeFlags.set(EChangeFlags_t::Visibility);
	}
	if (Kind == ERenderProxyKind::Light)
		ChangeFlags.set(EChangeFlags_t::Light);

	return static_cast<uint32_t>(Proxies.size() - 1);
}

uint32_t RenderProxyWorld::addProxy(RenderProxy InProxy)
{
	InProxy.Header.Kind = InProxy.Header.Kind == ERenderProxyKind::TransparentMesh
		? ERenderProxyKind::TransparentMesh
		: ERenderProxyKind::OpaqueMesh;
	return addProxy(RenderProxyVariant { std::move(InProxy) });
}

void RenderProxyWorld::removeProxy(uint32_t Index)
{
	if (Index >= Proxies.size())
		return;
	// 交换到末尾后弹出, 保持数组紧凑; 调用方必须放弃所有旧索引.
	std::swap(Proxies[Index], Proxies.back());
	Proxies.pop_back();
	ChangeFlags.set(EChangeFlags_t::Geometry);
	ChangeFlags.set(EChangeFlags_t::Visibility);
}

void RenderProxyWorld::updateTransform(uint32_t Index, const core::Matrix4x4& LocalToWorld)
{
	if (Index >= Proxies.size())
		return;
	RenderProxyVariant& Proxy = Proxies[Index];
	RenderProxyHeader& Header = getHeader(Proxy);
	const core::Matrix4x4 Previous = Header.LocalToWorld;
	Header.LocalToWorld = LocalToWorld;

	std::visit([&](auto& Value) {
		using ProxyType = std::decay_t<decltype(Value)>;
		if constexpr (std::is_same_v<ProxyType, MeshRenderProxy>)
		{
			if (Value.Header.Kind == ERenderProxyKind::RigidBodyMesh)
				Value.PreviousLocalToWorld = Previous;
		}
		if constexpr (std::is_same_v<ProxyType, TerrainRenderProxy>)
		{
			Value.Patch.LocalToWorld = LocalToWorld;
		}
	}, Proxy);

	ChangeFlags.set(EChangeFlags_t::Transform);
	ChangeFlags.set(EChangeFlags_t::Visibility);
}

void RenderProxyWorld::updateMaterial(uint32_t Index, uint32_t MaterialId)
{
	if (Index >= Proxies.size())
		return;
	RenderProxyVariant& Proxy = Proxies[Index];
	RenderProxyHeader& Header = getHeader(Proxy);
	Header.MaterialId = MaterialId;
	std::visit([&](auto& Value) {
		using ProxyType = std::decay_t<decltype(Value)>;
		if constexpr (std::is_same_v<ProxyType, TerrainRenderProxy>)
			Value.Patch.MaterialId = MaterialId;
		if constexpr (std::is_same_v<ProxyType, FluidRenderProxy>)
			Value.Fluid.MaterialId = MaterialId;
	}, Proxy);
	ChangeFlags.set(EChangeFlags_t::Material);
}

void RenderProxyWorld::markChanged(EChangeFlags Flags) noexcept
{
	ChangeFlags.Value |= Flags.Value;
}

EChangeFlags RenderProxyWorld::consumeChangeFlags() noexcept
{
	EChangeFlags Result = ChangeFlags;
	ChangeFlags = EChangeFlags_t::None;
	return Result;
}

void RenderProxyWorld::collectVisible(std::vector<VisibleProxy>& Out) const
{
	Out.clear();
	Out.reserve(Proxies.size());

	for (uint32_t Index = 0; Index < Proxies.size(); ++Index)
	{
		const RenderProxyHeader& Header = getHeader(Proxies[Index]);
		if (!isGeometryProxy(Header.Kind))
			continue;
		const bool Transparent = isTransparentProxy(Header.Kind);
		Out.push_back({
			.ProxyIndex = Index,
			.Key = makeSortKey(Transparent, Header.MaterialId, Header.StableOrder),
		});
	}

	// 按 SortKey 稳定排序: 不透明在前(按材质聚合), 透明在后(按提交序).
	std::stable_sort(Out.begin(), Out.end(), [](const VisibleProxy& Lhs,
												const VisibleProxy& Rhs) {
		return Lhs.Key < Rhs.Key;
	});
}

void RenderProxyWorld::buildSubmission(RenderFrameSubmission& Out, EChangeFlags WorldChanges,
	EChangeFlags UIChanges) const
{
	Out.clear();
	Out.WorldChanges = WorldChanges;
	Out.UIChanges = UIChanges;

	std::vector<VisibleProxy> Visible;
	collectVisible(Visible);
	Out.OpaqueDraws.reserve(Visible.size());
	Out.TransparentDraws.reserve(Visible.size());

	for (const VisibleProxy& V : Visible)
	{
		if (V.Key & (uint64_t { 1 } << 63))
			Out.TransparentDraws.emplace_back();
		else
			Out.OpaqueDraws.emplace_back();
	}

	for (const RenderProxyVariant& Proxy : Proxies)
	{
		std::visit([&](const auto& Value) {
			using ProxyType = std::decay_t<decltype(Value)>;
			if constexpr (std::is_same_v<ProxyType, LightRenderProxy>)
			{
				Out.Lights.push_back(Value.Light);
			}
			else if constexpr (std::is_same_v<ProxyType, FluidRenderProxy>)
			{
				Out.Fluids.push_back(Value.Fluid);
			}
			else if constexpr (std::is_same_v<ProxyType, GasRenderProxy>)
			{
				Out.Gases.push_back(Value.Gas);
			}
			else if constexpr (std::is_same_v<ProxyType, TerrainRenderProxy>)
			{
				Out.Terrains.push_back(Value.Patch);
			}
			else if constexpr (std::is_same_v<ProxyType, MeshRenderProxy>)
			{
				if (Value.Header.Kind == ERenderProxyKind::RigidBodyMesh && Value.Mesh)
				{
					RigidBodyRenderData Rigid;
					Rigid.VertexBuffer = Value.Mesh->VertexBuffer;
					Rigid.IndexBuffer = Value.Mesh->IndexBuffer;
					Rigid.VertexInput = Value.Mesh->VertexInput;
					Rigid.IndexFormat = Value.Mesh->IndexFormat;
					Rigid.IndexCount = Value.Mesh->IndexCount;
					Rigid.MaterialId = Value.Header.MaterialId;
					Rigid.CurrentLocalToWorld = Value.Header.LocalToWorld;
					Rigid.PreviousLocalToWorld = Value.PreviousLocalToWorld;
					Rigid.LinearVelocityWS = Value.LinearVelocityWS;
					Rigid.AngularVelocityWS = Value.AngularVelocityWS;
					Rigid.Sleeping = Value.Sleeping;
					Rigid.Transparent = Value.Header.Kind == ERenderProxyKind::TransparentMesh;
					Rigid.StableOrder = Value.Header.StableOrder;
					Out.RigidBodies.push_back(std::move(Rigid));
				}
			}
		}, Proxy);
	}
}

const RenderProxyVariant* RenderProxyWorld::getProxy(uint32_t Index) const noexcept
{
	return Index < Proxies.size() ? &Proxies[Index] : nullptr;
}

RenderProxyVariant* RenderProxyWorld::getProxy(uint32_t Index) noexcept
{
	return Index < Proxies.size() ? &Proxies[Index] : nullptr;
}

DrawPacket::SortKey RenderProxyWorld::makeSortKey(bool Transparent, uint32_t MaterialId,
	uint32_t StableOrder) noexcept
{
	const uint64_t TransparentBit = Transparent ? (uint64_t { 1 } << 63) : 0;
	const uint64_t MaterialBits = (uint64_t { MaterialId } & 0x7FFFFFFF) << 32;
	const uint64_t OrderBits = uint64_t { StableOrder } & 0xFFFFFFFF;
	return TransparentBit | MaterialBits | OrderBits;
}

void RenderProxyWorld::clear() noexcept
{
	Proxies.clear();
	ChangeFlags = EChangeFlags_t::All;
}

} // namespace runtime::renderer
