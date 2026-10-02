#include <renderer/ShaderBinding.hpp>

#include <spirv_reflect.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace runtime::renderer
{
namespace
{

rhi::EDescriptorType toRhiDescriptorType(const SpvReflectDescriptorBinding& Binding)
{
	switch (Binding.descriptor_type)
	{
	case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLER:
		return rhi::EDescriptorType::Sampler;
	case SPV_REFLECT_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
		return rhi::EDescriptorType::CombinedImageSampler;
	case SPV_REFLECT_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
		return rhi::EDescriptorType::SampledTexture;
	case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_IMAGE:
		return rhi::EDescriptorType::StorageTexture;
	case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		return rhi::EDescriptorType::UniformTexelBuffer;
	case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
		return rhi::EDescriptorType::StorageTexelBuffer;
	case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case SPV_REFLECT_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		return rhi::EDescriptorType::UniformBuffer;
	case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case SPV_REFLECT_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
		return (Binding.resource_type & SPV_REFLECT_RESOURCE_FLAG_UAV) != 0 &&
			(Binding.decoration_flags & SPV_REFLECT_DECORATION_NON_WRITABLE) == 0
			? rhi::EDescriptorType::ReadWriteStorageBuffer
			: rhi::EDescriptorType::ReadOnlyStorageBuffer;
	case SPV_REFLECT_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		return rhi::EDescriptorType::InputAttachment;
	case SPV_REFLECT_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
		return rhi::EDescriptorType::AccelerationStructure;
	default:
		throw std::invalid_argument("SPIR-V contains an unsupported descriptor type.");
	}
}

SpvReflectShaderStageFlagBits toReflectStage(rhi::EShaderStage_t Stage)
{
	switch (Stage)
	{
	case rhi::EShaderStage_t::Vertex: return SPV_REFLECT_SHADER_STAGE_VERTEX_BIT;
	case rhi::EShaderStage_t::Pixel: return SPV_REFLECT_SHADER_STAGE_FRAGMENT_BIT;
	case rhi::EShaderStage_t::Compute: return SPV_REFLECT_SHADER_STAGE_COMPUTE_BIT;
	case rhi::EShaderStage_t::Geometry: return SPV_REFLECT_SHADER_STAGE_GEOMETRY_BIT;
	case rhi::EShaderStage_t::Hull: return SPV_REFLECT_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
	case rhi::EShaderStage_t::Domain: return SPV_REFLECT_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
	case rhi::EShaderStage_t::Mesh: return SPV_REFLECT_SHADER_STAGE_MESH_BIT_EXT;
	case rhi::EShaderStage_t::Amplification:
	case rhi::EShaderStage_t::Task: return SPV_REFLECT_SHADER_STAGE_TASK_BIT_EXT;
	default:
		throw std::invalid_argument("Unsupported shader stage for SPIR-V reflection.");
	}
}

bool isRuntimeArray(const SpvReflectDescriptorBinding& Binding)
{
	for (uint32_t index = 0; index < Binding.array.dims_count; ++index)
	{
		if (Binding.array.dims[index] == 0)
			return true;
	}
	return Binding.count == 0;
}

std::string reflectedName(const SpvReflectDescriptorBinding& Binding)
{
	if (Binding.name && Binding.name[0] != '\0')
		return Binding.name;
	return "set" + std::to_string(Binding.set) + ".binding" + std::to_string(Binding.binding);
}

template <typename ValueType>
void appendBytes(std::string& Result, const ValueType& Value)
{
	static_assert(std::is_trivially_copyable_v<ValueType>);
	Result.append(reinterpret_cast<const char*>(&Value), sizeof(Value));
}

std::string makeLayoutKey(const PipelineInterface& Interface)
{
	std::string result;
	appendBytes(result, Interface.BindGroups.size());
	for (const auto& group : Interface.BindGroups)
	{
		appendBytes(result, group.Entries.size());
		for (const auto& entry : group.Entries)
		{
			appendBytes(result, entry.Binding);
			appendBytes(result, entry.Type);
			appendBytes(result, entry.ArrayCount);
			appendBytes(result, entry.Visibility.Value);
			appendBytes(result, entry.Flags.Value);
		}
	}
	appendBytes(result, Interface.PushConstants.size());
	for (const auto& range : Interface.PushConstants)
	{
		appendBytes(result, range.Stages.Value);
		appendBytes(result, range.Offset);
		appendBytes(result, range.Size);
	}
	return result;
}

} // namespace

ShaderInterface SpirvShaderReflector::reflect(
	std::span<const std::byte> ByteCode,
	rhi::EShaderStage_t Stage,
	std::string_view EntryPoint)
{
	if (ByteCode.empty() || ByteCode.size() % sizeof(uint32_t) != 0)
		throw std::invalid_argument("SPIR-V bytecode must be non-empty and four-byte aligned in size.");
	if (EntryPoint.empty())
		throw std::invalid_argument("SPIR-V reflection requires an entry-point name.");

	SpvReflectShaderModule module {};
	const SpvReflectResult create_result = spvReflectCreateShaderModule(
		ByteCode.size(), ByteCode.data(), &module);
	if (create_result != SPV_REFLECT_RESULT_SUCCESS)
		throw std::invalid_argument("SPIRV-Reflect rejected the shader module.");
	struct ModuleGuard
	{
		SpvReflectShaderModule* Module;
		~ModuleGuard() { spvReflectDestroyShaderModule(Module); }
	} module_guard { &module };

	const std::string entry_point_name(EntryPoint);
	const SpvReflectEntryPoint* entry_point = spvReflectGetEntryPoint(&module, entry_point_name.c_str());
	if (!entry_point)
		throw std::invalid_argument("Requested SPIR-V entry point was not found.");
	if (entry_point->shader_stage != toReflectStage(Stage))
		throw std::invalid_argument("Requested RHI stage does not match the SPIR-V entry point stage.");

	ShaderInterface result;
	result.Stage = Stage;
	result.EntryPoint = entry_point_name;
	uint32_t binding_count = 0;
	if (spvReflectEnumerateEntryPointDescriptorBindings(
			&module, entry_point_name.c_str(), &binding_count, nullptr) != SPV_REFLECT_RESULT_SUCCESS)
		throw std::runtime_error("Failed to enumerate SPIR-V descriptor bindings.");
	std::vector<SpvReflectDescriptorBinding*> bindings(binding_count);
	if (binding_count != 0 && spvReflectEnumerateEntryPointDescriptorBindings(
			&module, entry_point_name.c_str(), &binding_count, bindings.data()) != SPV_REFLECT_RESULT_SUCCESS)
		throw std::runtime_error("Failed to read SPIR-V descriptor bindings.");
	result.Resources.reserve(binding_count);
	for (const SpvReflectDescriptorBinding* binding : bindings)
	{
		if (!binding)
			throw std::runtime_error("SPIRV-Reflect returned a null descriptor binding.");
		const std::string name = reflectedName(*binding);
		result.Resources.emplace_back(ShaderResourceBinding {
			.Id = makeResourceId(name),
			.Name = name,
			.Group = binding->set,
			.Binding = binding->binding,
			.ArrayCount = binding->count,
			.BlockSize = binding->block.padded_size,
			.Type = toRhiDescriptorType(*binding),
			.Visibility = rhi::EShaderStage(Stage),
			.RuntimeArray = isRuntimeArray(*binding)
		});
	}

	uint32_t push_constant_count = 0;
	if (spvReflectEnumerateEntryPointPushConstantBlocks(
			&module, entry_point_name.c_str(), &push_constant_count, nullptr) != SPV_REFLECT_RESULT_SUCCESS)
		throw std::runtime_error("Failed to enumerate SPIR-V push constants.");
	std::vector<SpvReflectBlockVariable*> push_constants(push_constant_count);
	if (push_constant_count != 0 && spvReflectEnumerateEntryPointPushConstantBlocks(
			&module, entry_point_name.c_str(), &push_constant_count, push_constants.data()) !=
			SPV_REFLECT_RESULT_SUCCESS)
		throw std::runtime_error("Failed to read SPIR-V push constants.");
	for (const SpvReflectBlockVariable* block : push_constants)
	{
		if (!block || block->size == 0)
			continue;
		result.PushConstants.emplace_back(rhi::PushConstantRange {
			.Stages = rhi::EShaderStage(Stage),
			.Offset = block->offset,
			.Size = block->size
		});
	}

	std::ranges::sort(result.Resources, [](const auto& lhs, const auto& rhs)
	{
		return std::tie(lhs.Group, lhs.Binding) < std::tie(rhs.Group, rhs.Binding);
	});
	return result;
}

PipelineInterface PipelineInterfaceBuilder::merge(
	std::span<const ShaderInterface> Interfaces,
	const PipelineInterfaceBuildOptions& Options)
{
	if (Interfaces.empty())
		throw std::invalid_argument("A pipeline interface requires at least one shader stage.");
	if (Options.RuntimeArrayMaxCount == 0)
		throw std::invalid_argument("Runtime descriptor array maximum count must be non-zero.");

	PipelineInterface result;
	std::map<std::pair<uint32_t, uint32_t>, size_t> locations;
	std::unordered_map<ResourceId, std::string> names;
	for (const auto& interface : Interfaces)
	{
		for (const auto& resource : interface.Resources)
		{
			if (const auto collision = names.find(resource.Id);
				collision != names.end() && collision->second != resource.Name)
				throw std::invalid_argument("Shader resource-name hash collision detected.");
			names.emplace(resource.Id, resource.Name);

			const auto key = std::pair(resource.Group, resource.Binding);
			if (const auto existing = locations.find(key); existing != locations.end())
			{
				auto& merged = result.Resources[existing->second];
				if (merged.Type != resource.Type || merged.RuntimeArray != resource.RuntimeArray ||
					(!merged.RuntimeArray && merged.ArrayCount != resource.ArrayCount) ||
					merged.Name != resource.Name ||
					(merged.BlockSize != 0 && resource.BlockSize != 0 &&
					 merged.BlockSize != resource.BlockSize))
				{
					throw std::invalid_argument("Shader stages declare incompatible resources at one set/binding.");
				}
				merged.Visibility.Value |= resource.Visibility.Value;
				merged.BlockSize = std::max(merged.BlockSize, resource.BlockSize);
				continue;
			}
			locations.emplace(key, result.Resources.size());
			result.Resources.emplace_back(resource);
		}
	}
	std::ranges::sort(result.Resources, [](const auto& lhs, const auto& rhs)
	{
		return std::tie(lhs.Group, lhs.Binding) < std::tie(rhs.Group, rhs.Binding);
	});

	if (!result.Resources.empty())
		result.BindGroups.resize(static_cast<size_t>(result.Resources.back().Group) + 1);
	for (const auto& resource : result.Resources)
	{
		rhi::EDescriptorBindingFlags flags;
		if (resource.RuntimeArray)
		{
			flags.set(rhi::EDescriptorBindingFlag_t::VariableArrayCount);
			if (Options.RuntimeArraysArePartiallyBound)
				flags.set(rhi::EDescriptorBindingFlag_t::PartiallyBound);
			if (Options.RuntimeArraysUseUpdateAfterBind)
				flags.set(rhi::EDescriptorBindingFlag_t::UpdateAfterBind);
		}
		result.BindGroups[resource.Group].Entries.emplace_back(rhi::BindGroupLayoutEntry {
			.Binding = resource.Binding,
			.Type = resource.Type,
			.ArrayCount = resource.RuntimeArray ? Options.RuntimeArrayMaxCount : resource.ArrayCount,
			.Visibility = resource.Visibility,
			.Flags = flags
		});
	}

	for (const auto& interface : Interfaces)
	{
		for (const auto& range : interface.PushConstants)
		{
			auto exact = std::ranges::find_if(result.PushConstants, [&](const auto& existing)
			{
				return existing.Offset == range.Offset && existing.Size == range.Size;
			});
			if (exact != result.PushConstants.end())
			{
				exact->Stages.Value |= range.Stages.Value;
				continue;
			}
			const uint64_t range_end = static_cast<uint64_t>(range.Offset) + range.Size;
			for (const auto& existing : result.PushConstants)
			{
				const uint64_t existing_end = static_cast<uint64_t>(existing.Offset) + existing.Size;
				if (range.Offset < existing_end && existing.Offset < range_end)
					throw std::invalid_argument("Partially overlapping push-constant blocks are unsupported.");
			}
			result.PushConstants.emplace_back(range);
		}
	}
	std::ranges::sort(result.PushConstants, {}, &rhi::PushConstantRange::Offset);
	return result;
}

void ResourceBindingTable::set(
	std::string_view Name,
	rhi::BindGroupResource Resource,
	uint32_t ArrayElement)
{
	set(makeResourceId(Name), Name, std::move(Resource), ArrayElement);
}

void ResourceBindingTable::set(
	ResourceId Id,
	std::string_view Name,
	rhi::BindGroupResource Resource,
	uint32_t ArrayElement)
{
	if (Name.empty())
		throw std::invalid_argument("A semantic resource binding requires a non-empty name.");
	auto [iterator, inserted] = Slots.try_emplace(Id, Slot { std::string(Name), {} });
	if (!inserted && iterator->second.Name != Name)
		throw std::invalid_argument("Resource ID collides with a different shader resource name.");
	iterator->second.Elements.insert_or_assign(ArrayElement, std::move(Resource));
}

const rhi::BindGroupResource* ResourceBindingTable::find(
	ResourceId Id,
	std::string_view ExpectedName,
	uint32_t ArrayElement) const
{
	const auto slot = Slots.find(Id);
	if (slot == Slots.end() || slot->second.Name != ExpectedName)
		return nullptr;
	const auto element = slot->second.Elements.find(ArrayElement);
	return element == slot->second.Elements.end() ? nullptr : &element->second;
}

uint32_t ResourceBindingTable::getElementCount(ResourceId Id, std::string_view ExpectedName) const
{
	const auto slot = Slots.find(Id);
	if (slot == Slots.end() || slot->second.Name != ExpectedName || slot->second.Elements.empty())
		return 0;
	const uint32_t maximum = std::ranges::max_element(
		slot->second.Elements, {}, [](const auto& value) { return value.first; })->first;
	if (maximum == std::numeric_limits<uint32_t>::max())
		throw std::overflow_error("Resource array element count overflows uint32_t.");
	return maximum + 1;
}

std::shared_ptr<rhi::RPipelineLayout> ShaderBindingResolver::getOrCreatePipelineLayout(
	const PipelineInterface& Interface)
{
	const std::string key = makeLayoutKey(Interface);
	std::scoped_lock lock(CacheMutex);
	if (const auto existing = LayoutCache.find(key); existing != LayoutCache.end())
	{
		if (auto layout = existing->second.lock())
			return layout;
	}

	rhi::PipelineLayoutDescriptor descriptor;
	descriptor.PushConstantRanges = Interface.PushConstants;
	descriptor.BindGroupLayouts.reserve(Interface.BindGroups.size());
	for (const auto& group : Interface.BindGroups)
		descriptor.BindGroupLayouts.emplace_back(Device->createBindGroupLayout(group));
	auto layout = Device->createPipelineLayout(descriptor);
	LayoutCache.insert_or_assign(key, layout);
	return layout;
}

ResolvedPipelineBindings ShaderBindingResolver::resolve(
	const PipelineInterface& Interface,
	const ResourceBindingTable& Resources)
{
	ResolvedPipelineBindings result;
	result.PipelineLayout = getOrCreatePipelineLayout(Interface);
	result.BindGroupLayouts.reserve(result.PipelineLayout->getBindGroupLayoutCount());
	result.BindGroups.reserve(result.PipelineLayout->getBindGroupLayoutCount());
	for (uint32_t group_index = 0;
		group_index < result.PipelineLayout->getBindGroupLayoutCount();
		++group_index)
	{
		const auto& layout = result.PipelineLayout->getBindGroupLayout(group_index);
		result.BindGroupLayouts.emplace_back(layout);
		rhi::BindGroupDescriptor descriptor;
		descriptor.Layout = layout;
		for (const auto& reflected : Interface.Resources)
		{
			if (reflected.Group != group_index)
				continue;
			uint32_t count = reflected.ArrayCount;
			if (reflected.RuntimeArray)
			{
				count = Resources.getElementCount(reflected.Id, reflected.Name);
				if (count == 0)
					count = 1;
				descriptor.VariableArrayCount = count;
			}
			for (uint32_t element = 0; element < count; ++element)
			{
				const auto* resource = Resources.find(reflected.Id, reflected.Name, element);
				if (!resource)
				{
					if (reflected.RuntimeArray)
						continue;
					throw std::invalid_argument(
						"ResourceBindingTable is missing required shader resource '" +
						reflected.Name + "'.");
				}
				descriptor.Entries.emplace_back(rhi::BindGroupEntry {
					.Binding = reflected.Binding,
					.ArrayElement = element,
					.Resource = *resource
				});
			}
		}
		result.BindGroups.emplace_back(Device->createBindGroup(descriptor));
	}
	return result;
}

} // namespace runtime::renderer
