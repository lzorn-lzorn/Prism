#pragma once

#include <RHI.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace runtime::renderer
{

using ResourceId = uint64_t;

/**
 * @brief Produces a deterministic identifier for a shader resource name.
 * @param Name UTF-8 resource name emitted by the shader compiler.
 * @return Stable FNV-1a identifier. Collisions are checked by ResourceBindingTable.
 */
[[nodiscard]] constexpr ResourceId makeResourceId(std::string_view Name) noexcept
{
	ResourceId hash = 14695981039346656037ull;
	for (const char value : Name)
	{
		hash ^= static_cast<uint8_t>(value);
		hash *= 1099511628211ull;
	}
	return hash;
}

/** @brief One shader-visible descriptor discovered through reflection. */
struct ShaderResourceBinding
{
	ResourceId Id { 0 };
	std::string Name;
	uint32_t Group { 0 };
	uint32_t Binding { 0 };
	uint32_t ArrayCount { 1 };
	uint32_t BlockSize { 0 };
	rhi::EDescriptorType Type { rhi::EDescriptorType::UniformBuffer };
	rhi::EShaderStage Visibility {};
	bool RuntimeArray { false };
};

/** @brief Reflection result for one SPIR-V entry point. */
struct ShaderInterface
{
	rhi::EShaderStage_t Stage { rhi::EShaderStage_t::Vertex };
	std::string EntryPoint;
	std::vector<ShaderResourceBinding> Resources;
	std::vector<rhi::PushConstantRange> PushConstants;
};

/** @brief Controls conversion from reflected runtime arrays to finite RHI layouts. */
struct PipelineInterfaceBuildOptions
{
	uint32_t RuntimeArrayMaxCount { 1024 };
	bool RuntimeArraysArePartiallyBound { true };
	bool RuntimeArraysUseUpdateAfterBind { false };
};

/**
 * @brief Merged shader ABI used to create RHI layouts and resolve concrete resources.
 *
 * BindGroups are indexed by shader set/space. Empty groups are retained when shader
 * sets are sparse so that all following set numbers preserve their ABI position.
 */
struct PipelineInterface
{
	std::vector<rhi::BindGroupLayoutDescriptor> BindGroups;
	std::vector<ShaderResourceBinding> Resources;
	std::vector<rhi::PushConstantRange> PushConstants;
};

/** @brief SPIR-V reflection front-end owned by Renderer rather than the low-level RHI. */
class SpirvShaderReflector final
{
public:
	/**
	 * @brief Reflects descriptors and push constants used by one entry point.
	 * @param ByteCode Complete SPIR-V module bytes.
	 * @param Stage RHI stage expected for EntryPoint.
	 * @param EntryPoint Entry-point name to inspect.
	 * @return Normalized renderer-side shader ABI.
	 * @throws std::invalid_argument if SPIR-V or entry-point metadata is invalid.
	 */
	[[nodiscard]] static ShaderInterface reflect(
		std::span<const std::byte> ByteCode,
		rhi::EShaderStage_t Stage,
		std::string_view EntryPoint = "main");
};

/** @brief Validates and merges several shader-stage interfaces into one pipeline ABI. */
class PipelineInterfaceBuilder final
{
public:
	/**
	 * @brief Merges descriptor visibility and push-constant visibility across stages.
	 * @param Interfaces Stage interfaces belonging to one pipeline.
	 * @param Options Policy for unbounded descriptor arrays.
	 * @return Canonical interface sorted by group, binding and push-constant offset.
	 */
	[[nodiscard]] static PipelineInterface merge(
		std::span<const ShaderInterface> Interfaces,
		const PipelineInterfaceBuildOptions& Options = {});
};

/**
 * @brief Semantic table mapping reflected resource names to concrete RHI resources.
 *
 * The table is independent of descriptor set numbers. This keeps material and frame
 * code stable when shader bindings are reassigned during asset compilation.
 */
class ResourceBindingTable final
{
public:
	/** @brief Adds or replaces one resource array element by shader-visible name. */
	void set(std::string_view Name, rhi::BindGroupResource Resource, uint32_t ArrayElement = 0);

	/** @brief Adds or replaces one resource array element by a precomputed stable ID. */
	void set(ResourceId Id, std::string_view Name, rhi::BindGroupResource Resource,
		uint32_t ArrayElement = 0);

	/** @return Resource at the requested array element, or nullptr when not present. */
	[[nodiscard]] const rhi::BindGroupResource* find(
		ResourceId Id,
		std::string_view ExpectedName,
		uint32_t ArrayElement = 0) const;

	/** @return Highest populated element plus one, or zero when the resource is absent. */
	[[nodiscard]] uint32_t getElementCount(ResourceId Id, std::string_view ExpectedName) const;

private:
	struct Slot
	{
		std::string Name;
		std::unordered_map<uint32_t, rhi::BindGroupResource> Elements;
	};
	std::unordered_map<ResourceId, Slot> Slots;
};

/** @brief Result of resolving a PipelineInterface against a resource table. */
struct ResolvedPipelineBindings
{
	std::shared_ptr<rhi::RPipelineLayout> PipelineLayout;
	std::vector<std::shared_ptr<rhi::RBindGroupLayout>> BindGroupLayouts;
	std::vector<std::shared_ptr<rhi::RBindGroup>> BindGroups;
};

/**
 * @brief Creates and reuses RHI layouts, then materializes immutable BindGroups.
 *
 * Layouts are cached by canonical ABI bytes. Concrete BindGroups are deliberately
 * not cached because resource lifetime/version policy belongs to materials and the
 * render graph above this service.
 */
class ShaderBindingResolver final
{
public:
	explicit ShaderBindingResolver(rhi::RDevice& Device) noexcept : Device(&Device) {}

	/**
	 * @brief Resolves all reflected resources and creates bindable RHI objects.
	 * @param Interface Merged pipeline ABI.
	 * @param Resources Semantic resource table.
	 * @return Pipeline layout, group layouts and concrete groups in set order.
	 */
	[[nodiscard]] ResolvedPipelineBindings resolve(
		const PipelineInterface& Interface,
		const ResourceBindingTable& Resources);

private:
	[[nodiscard]] std::shared_ptr<rhi::RPipelineLayout> getOrCreatePipelineLayout(
		const PipelineInterface& Interface);

	rhi::RDevice* Device { nullptr };
	std::mutex CacheMutex;
	std::unordered_map<std::string, std::weak_ptr<rhi::RPipelineLayout>> LayoutCache;
};

} // namespace runtime::renderer
