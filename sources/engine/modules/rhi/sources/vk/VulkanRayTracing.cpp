#include "VulkanRayTracing.hpp"

#include "VulkanBuffer.hpp"
#include "VulkanDevice.hpp"
#include "VulkanPipeline.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

namespace rhi
{
namespace
{
template <typename Function>
Function load(VulkanDevice& Device, const char* Name)
{
	auto function = reinterpret_cast<Function>(Device.getVkDevice().getProcAddr(Name));
	if (!function) throw std::logic_error(std::string("Missing Vulkan ray-tracing command: ") + Name);
	return function;
}

VkAccelerationStructureTypeKHR toVk(EAccelerationStructureType Type)
{
	return Type == EAccelerationStructureType::TopLevel
		? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
		: VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
}

VkBuildAccelerationStructureFlagsKHR toVk(EAccelerationStructureBuildFlags Flags)
{
	VkBuildAccelerationStructureFlagsKHR result = 0;
	if (Flags.has(EAccelerationStructureBuildFlag_t::AllowUpdate)) result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
	if (Flags.has(EAccelerationStructureBuildFlag_t::AllowCompaction)) result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
	if (Flags.has(EAccelerationStructureBuildFlag_t::PreferFastTrace)) result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	if (Flags.has(EAccelerationStructureBuildFlag_t::PreferFastBuild)) result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
	if (Flags.has(EAccelerationStructureBuildFlag_t::LowMemory)) result |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
	return result;
}

VkGeometryFlagsKHR toVk(EAccelerationStructureGeometryFlags Flags)
{
	VkGeometryFlagsKHR result = 0;
	if (Flags.has(EAccelerationStructureGeometryFlag_t::Opaque)) result |= VK_GEOMETRY_OPAQUE_BIT_KHR;
	if (Flags.has(EAccelerationStructureGeometryFlag_t::NoDuplicateAnyHitInvocation)) result |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
	return result;
}

VkFormat rayTracingVertexFormat(EVertexFormat Format)
{
	switch (Format)
	{
	case EVertexFormat::Float2: return VK_FORMAT_R32G32_SFLOAT;
	case EVertexFormat::Float3: return VK_FORMAT_R32G32B32_SFLOAT;
	case EVertexFormat::Float4: return VK_FORMAT_R32G32B32A32_SFLOAT;
	default: return VK_FORMAT_UNDEFINED;
	}
}

VulkanBuffer* requireBuffer(
	const std::shared_ptr<RBuffer>& Buffer,
	const VulkanDevice& Device,
	EBufferUsage Usage,
	DeviceSizeType Offset)
{
	auto* result = dynamic_cast<VulkanBuffer*>(Buffer.get());
	if (!result || &result->getDevice() != &Device || !result->isValid() ||
		!Buffer->getDescriptor().Usage.has(Usage) || Offset >= Buffer->getDescriptor().Size ||
		Buffer->getDeviceAddress() == 0)
		throw std::invalid_argument("Acceleration-structure input requires an addressable Vulkan buffer and valid offset.");
	return result;
}

struct GeometryStorage
{
	std::vector<VkAccelerationStructureGeometryKHR> Geometries;
	std::vector<VkAccelerationStructureBuildRangeInfoKHR> Ranges;
	std::vector<uint32_t> PrimitiveCounts;
	std::vector<std::shared_ptr<void>> Resources;
};

GeometryStorage convertGeometries(
	const VulkanDevice& Device,
	EAccelerationStructureType StructureType,
	std::span<const AccelerationStructureGeometry> Geometries,
	bool IncludeRanges)
{
	if (Geometries.empty()) throw std::invalid_argument("Acceleration-structure build requires geometry.");
	GeometryStorage output;
	output.Geometries.reserve(Geometries.size());
	output.Ranges.reserve(Geometries.size());
	output.PrimitiveCounts.reserve(Geometries.size());
	for (const auto& geometry : Geometries)
	{
		constexpr uint8_t known_geometry_flags =
			static_cast<uint8_t>(EAccelerationStructureGeometryFlag_t::Opaque) |
			static_cast<uint8_t>(EAccelerationStructureGeometryFlag_t::NoDuplicateAnyHitInvocation);
		if ((geometry.Flags.Value & ~known_geometry_flags) != 0)
			throw std::invalid_argument("Acceleration-structure geometry contains unknown flags.");
		if (geometry.PrimitiveCount == 0)
			throw std::invalid_argument("Acceleration-structure geometry requires primitives.");
		VkAccelerationStructureGeometryKHR converted {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
			.pNext = nullptr,
			.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
			.geometry = {},
			.flags = toVk(geometry.Flags)
		};
		if (geometry.Type == EAccelerationStructureGeometryType::Triangles)
		{
			if (StructureType != EAccelerationStructureType::BottomLevel)
				throw std::invalid_argument("Triangle geometry is only valid in a bottom-level acceleration structure.");
			const auto* triangles = std::get_if<AccelerationStructureTriangles>(&geometry.Data);
			if (!triangles || triangles->VertexStride == 0 || triangles->MaxVertex == 0)
				throw std::invalid_argument("Triangle geometry has invalid vertex metadata.");
			const VkFormat format = rayTracingVertexFormat(triangles->VertexFormat);
			if (format == VK_FORMAT_UNDEFINED)
				throw std::invalid_argument("Triangle vertex format is unsupported for Vulkan ray tracing.");
			auto* vertices = requireBuffer(triangles->VertexBuffer, Device,
				EBufferUsage::AccelerationStructureBuildInput, triangles->VertexOffset);
			VkIndexType index_type = VK_INDEX_TYPE_NONE_KHR;
			VkDeviceAddress index_address = 0;
			if (triangles->IndexFormat != EIndexFormat::None)
			{
				auto* indices = requireBuffer(triangles->IndexBuffer, Device,
					EBufferUsage::AccelerationStructureBuildInput, triangles->IndexOffset);
				index_type = triangles->IndexFormat == EIndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
				index_address = indices->getDeviceAddress() + triangles->IndexOffset;
				output.Resources.emplace_back(triangles->IndexBuffer);
			}
			VkDeviceAddress transform_address = 0;
			if (triangles->TransformBuffer)
			{
				auto* transform = requireBuffer(triangles->TransformBuffer, Device,
					EBufferUsage::AccelerationStructureBuildInput, triangles->TransformOffset);
				transform_address = transform->getDeviceAddress() + triangles->TransformOffset;
				output.Resources.emplace_back(triangles->TransformBuffer);
			}
			converted.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			converted.geometry.triangles = {
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
				.pNext = nullptr,
				.vertexFormat = format,
				.vertexData = { vertices->getDeviceAddress() + triangles->VertexOffset },
				.vertexStride = triangles->VertexStride,
				.maxVertex = triangles->MaxVertex,
				.indexType = index_type,
				.indexData = { index_address },
				.transformData = { transform_address }
			};
			output.Resources.emplace_back(triangles->VertexBuffer);
		}
		else if (geometry.Type == EAccelerationStructureGeometryType::AABBs)
		{
			if (StructureType != EAccelerationStructureType::BottomLevel)
				throw std::invalid_argument("AABB geometry is only valid in a bottom-level acceleration structure.");
			const auto* aabbs = std::get_if<AccelerationStructureAABBs>(&geometry.Data);
			if (!aabbs || aabbs->Stride < 24 || aabbs->Stride % 8 != 0)
				throw std::invalid_argument("AABB geometry stride must be at least 24 and a multiple of 8.");
			auto* buffer = requireBuffer(aabbs->Buffer, Device,
				EBufferUsage::AccelerationStructureBuildInput, aabbs->Offset);
			converted.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
			converted.geometry.aabbs = {
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
				.pNext = nullptr,
				.data = { buffer->getDeviceAddress() + aabbs->Offset },
				.stride = aabbs->Stride
			};
			output.Resources.emplace_back(aabbs->Buffer);
		}
		else
		{
			if (StructureType != EAccelerationStructureType::TopLevel)
				throw std::invalid_argument("Instance geometry is only valid in a top-level acceleration structure.");
			const auto* instances = std::get_if<AccelerationStructureInstances>(&geometry.Data);
			if (!instances) throw std::invalid_argument("Instance geometry payload does not match its type.");
			auto* buffer = requireBuffer(instances->Buffer, Device,
				EBufferUsage::AccelerationStructureBuildInput, instances->Offset);
			converted.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
			converted.geometry.instances = {
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
				.pNext = nullptr,
				.arrayOfPointers = instances->ArrayOfPointers,
				.data = { buffer->getDeviceAddress() + instances->Offset }
			};
			output.Resources.emplace_back(instances->Buffer);
		}
		output.Geometries.emplace_back(converted);
		output.PrimitiveCounts.emplace_back(geometry.PrimitiveCount);
		if (IncludeRanges)
			output.Ranges.push_back({ geometry.PrimitiveCount, geometry.PrimitiveOffset,
				geometry.FirstVertex, geometry.TransformOffset });
	}
	return output;
}

VkShaderStageFlagBits rayStage(EShaderStage_t Stage)
{
	switch (Stage)
	{
	case EShaderStage_t::RayGeneration: return VK_SHADER_STAGE_RAYGEN_BIT_KHR;
	case EShaderStage_t::AnyHit: return VK_SHADER_STAGE_ANY_HIT_BIT_KHR;
	case EShaderStage_t::ClosestHit: return VK_SHADER_STAGE_CLOSEST_HIT_BIT_KHR;
	case EShaderStage_t::Miss: return VK_SHADER_STAGE_MISS_BIT_KHR;
	case EShaderStage_t::Intersection: return VK_SHADER_STAGE_INTERSECTION_BIT_KHR;
	case EShaderStage_t::Callable: return VK_SHADER_STAGE_CALLABLE_BIT_KHR;
	default: throw std::invalid_argument("Ray-tracing pipeline contains a non-ray-tracing shader stage.");
	}
}

uint64_t hashRayTracingPipeline(const RayTracingPipelineDescriptor& Desc)
{
	uint64_t hash = 14695981039346656037ull;
	auto append = [&](const void* data, size_t size) {
		for (size_t index = 0; index < size; ++index) { hash ^= static_cast<const uint8_t*>(data)[index]; hash *= 1099511628211ull; }
	};
	const auto layout_hash = Desc.Layout->getCompatibilityHash(); append(&layout_hash, sizeof(layout_hash));
	append(&Desc.MaxRecursionDepth, sizeof(Desc.MaxRecursionDepth));
	for (const auto& stage : Desc.Stages)
	{
		const auto shader_hash = stage.Shader->getContentHash(); append(&shader_hash, sizeof(shader_hash));
		auto constants = stage.SpecializationConstants;
		std::ranges::sort(constants, {}, &SpecializationConstant::Id);
		for (const auto& constant : constants) { append(&constant.Id, sizeof(constant.Id)); append(constant.Data.data(), constant.Data.size()); }
	}
	for (const auto& group : Desc.Groups)
	{
		append(&group.Type, sizeof(group.Type)); append(&group.GeneralShader, sizeof(group.GeneralShader));
		append(&group.ClosestHitShader, sizeof(group.ClosestHitShader)); append(&group.AnyHitShader, sizeof(group.AnyHitShader));
		append(&group.IntersectionShader, sizeof(group.IntersectionShader));
	}
	append(&Desc.Compile.Flags.Value, sizeof(Desc.Compile.Flags.Value));
	return hash;
}
} // namespace

VulkanAccelerationStructure::VulkanAccelerationStructure(
	VulkanDevice& InDevice, const AccelerationStructureDescriptor& Desc)
	: Device(&InDevice), Storage(Desc.Storage), Type(Desc.Type), Size(Desc.Size)
{
	if (!Device->getFeatures().AccelerationStructure) throw std::logic_error("Vulkan acceleration structures are unsupported.");
	if (Desc.Type != EAccelerationStructureType::BottomLevel &&
		Desc.Type != EAccelerationStructureType::TopLevel)
		throw std::invalid_argument("Acceleration-structure type is invalid.");
	auto* buffer = dynamic_cast<VulkanBuffer*>(Storage.get());
	if (!buffer || &buffer->getDevice() != Device || !buffer->isValid() ||
		!Storage->getDescriptor().Usage.has(EBufferUsage::AccelerationStructureStorage) ||
		Desc.Size == 0 || Desc.Offset % 256 != 0 || Desc.Offset >= Storage->getDescriptor().Size ||
		Desc.Size > Storage->getDescriptor().Size - Desc.Offset)
		throw std::invalid_argument("Acceleration-structure storage range is invalid or not 256-byte aligned.");
	const VkAccelerationStructureCreateInfoKHR info {
		.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
		.pNext = nullptr, .createFlags = 0,
		.buffer = static_cast<VkBuffer>(buffer->getVkBuffer()),
		.offset = Desc.Offset, .size = Desc.Size, .type = toVk(Desc.Type), .deviceAddress = 0
	};
	const auto create = load<PFN_vkCreateAccelerationStructureKHR>(*Device, "vkCreateAccelerationStructureKHR");
	if (create(static_cast<VkDevice>(Device->getVkDevice()), &info, nullptr,
		reinterpret_cast<VkAccelerationStructureKHR*>(&Handle)) != VK_SUCCESS)
		throw std::runtime_error("vkCreateAccelerationStructureKHR failed.");
}

VulkanAccelerationStructure::~VulkanAccelerationStructure()
{
	if (Handle && Device)
	{
		auto destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(Device->getVkDevice().getProcAddr("vkDestroyAccelerationStructureKHR"));
		if (destroy) destroy(static_cast<VkDevice>(Device->getVkDevice()), static_cast<VkAccelerationStructureKHR>(Handle), nullptr);
	}
}

RDevice& VulkanAccelerationStructure::getDevice() const noexcept { return *Device; }
void* VulkanAccelerationStructure::getNativeHandle() const noexcept
{
	return reinterpret_cast<void*>(static_cast<VkAccelerationStructureKHR>(Handle));
}
DeviceAddress VulkanAccelerationStructure::getDeviceAddress() const noexcept
{
	if (!Handle || !Device) return 0;
	auto get_address = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
		Device->getVkDevice().getProcAddr("vkGetAccelerationStructureDeviceAddressKHR"));
	if (!get_address) return 0;
	const VkAccelerationStructureDeviceAddressInfoKHR info {
		VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr,
		static_cast<VkAccelerationStructureKHR>(Handle) };
	return get_address(static_cast<VkDevice>(Device->getVkDevice()), &info);
}

AccelerationStructureBuildSizes queryVulkanAccelerationStructureBuildSizes(
	const VulkanDevice& Device, EAccelerationStructureType Type,
	EAccelerationStructureBuildFlags Flags,
	std::span<const AccelerationStructureGeometry> Geometries)
{
	if (!Device.getFeatures().AccelerationStructure) return {};
	try
	{
		constexpr uint8_t known_build_flags =
			static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::AllowUpdate) |
			static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::AllowCompaction) |
			static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::PreferFastTrace) |
			static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::PreferFastBuild) |
			static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::LowMemory);
		if ((Flags.Value & ~known_build_flags) != 0) return {};
		auto geometry = convertGeometries(Device, Type, Geometries, false);
		const VkAccelerationStructureBuildGeometryInfoKHR info {
			.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
			.pNext = nullptr, .type = toVk(Type), .flags = toVk(Flags),
			.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
			.srcAccelerationStructure = VK_NULL_HANDLE, .dstAccelerationStructure = VK_NULL_HANDLE,
			.geometryCount = static_cast<uint32_t>(geometry.Geometries.size()),
			.pGeometries = geometry.Geometries.data(), .ppGeometries = nullptr,
			.scratchData = { 0 }
		};
		VkAccelerationStructureBuildSizesInfoKHR sizes {
			VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
		auto query = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
			Device.getVkDevice().getProcAddr("vkGetAccelerationStructureBuildSizesKHR"));
		if (!query) return {};
		query(static_cast<VkDevice>(Device.getVkDevice()),
			VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info,
			geometry.PrimitiveCounts.data(), &sizes);
		return { sizes.accelerationStructureSize, sizes.buildScratchSize, sizes.updateScratchSize };
	}
	catch (const std::invalid_argument&) { return {}; }
}

bool recordVulkanAccelerationStructureBuilds(
	VulkanDevice& Device, vk::CommandBuffer CommandBuffer,
	std::span<const AccelerationStructureBuildDescriptor> Builds,
	std::vector<std::shared_ptr<void>>& RetainedResources)
{
	if (!Device.getFeatures().AccelerationStructure || Builds.empty()) return false;
	try
	{
		std::vector<GeometryStorage> geometry_storage;
		std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos;
		std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> ranges;
		geometry_storage.reserve(Builds.size()); infos.reserve(Builds.size()); ranges.reserve(Builds.size());
		for (const auto& build : Builds)
		{
			auto destination = std::dynamic_pointer_cast<VulkanAccelerationStructure>(build.Destination);
			auto source = std::dynamic_pointer_cast<VulkanAccelerationStructure>(build.Source);
			if (!destination || &destination->getDevice() != &Device || !destination->isValid() ||
				(build.Mode == EAccelerationStructureBuildMode::Update &&
				 (!source || &source->getDevice() != &Device || source->getType() != destination->getType())))
				return false;
			if ((build.Mode == EAccelerationStructureBuildMode::Build && source) ||
				(build.Mode == EAccelerationStructureBuildMode::Update &&
				 !build.Flags.has(EAccelerationStructureBuildFlag_t::AllowUpdate)))
				return false;
			auto* scratch = requireBuffer(build.ScratchBuffer, Device, EBufferUsage::DeviceAddress, build.ScratchOffset);
			if (!build.ScratchBuffer->getDescriptor().Usage.has(EBufferUsage::Storage) ||
				(scratch->getDeviceAddress() + build.ScratchOffset) %
					Device.getLimits().MinAccelerationStructureScratchOffsetAlignment != 0)
				return false;
			const auto sizes = queryVulkanAccelerationStructureBuildSizes(Device, destination->getType(), build.Flags, build.Geometries);
			const uint64_t required_scratch = build.Mode == EAccelerationStructureBuildMode::Update
				? sizes.UpdateScratchSize : sizes.BuildScratchSize;
			if (!sizes.AccelerationStructureSize || destination->getSize() < sizes.AccelerationStructureSize ||
				build.ScratchOffset >= build.ScratchBuffer->getDescriptor().Size ||
				required_scratch > build.ScratchBuffer->getDescriptor().Size - build.ScratchOffset)
				return false;
			geometry_storage.emplace_back(convertGeometries(Device, destination->getType(), build.Geometries, true));
			auto& converted = geometry_storage.back();
			infos.push_back({
				.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
				.pNext = nullptr, .type = toVk(destination->getType()), .flags = toVk(build.Flags),
				.mode = build.Mode == EAccelerationStructureBuildMode::Update
					? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
				.srcAccelerationStructure = source ? static_cast<VkAccelerationStructureKHR>(source->getVkHandle()) : VK_NULL_HANDLE,
				.dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>(destination->getVkHandle()),
				.geometryCount = static_cast<uint32_t>(converted.Geometries.size()),
				.pGeometries = converted.Geometries.data(), .ppGeometries = nullptr,
				.scratchData = { scratch->getDeviceAddress() + build.ScratchOffset }
			});
			ranges.emplace_back(converted.Ranges.data());
			RetainedResources.emplace_back(build.Destination);
			if (build.Source) RetainedResources.emplace_back(build.Source);
			RetainedResources.emplace_back(build.ScratchBuffer);
			RetainedResources.insert(RetainedResources.end(), converted.Resources.begin(), converted.Resources.end());
		}
		// vkCmdBuildAccelerationStructuresKHR consumes one range-array pointer per build info.
		load<PFN_vkCmdBuildAccelerationStructuresKHR>(Device, "vkCmdBuildAccelerationStructuresKHR")(
			static_cast<VkCommandBuffer>(CommandBuffer), static_cast<uint32_t>(infos.size()), infos.data(), ranges.data());
		return true;
	}
	catch (const std::invalid_argument&) { return false; }
}

std::shared_ptr<RPipeline> createVulkanRayTracingPipeline(
	VulkanDevice& Device, const RayTracingPipelineDescriptor& Desc)
{
	if (!Device.getFeatures().RayTracingPipeline) return {};
	auto layout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Desc.Layout);
	if (!layout || &layout->getDevice() != &Device || !layout->isValid() || Desc.Stages.empty() || Desc.Groups.empty())
		throw std::invalid_argument("Ray-tracing pipeline layout, stages, or groups are invalid.");
	if (Desc.MaxRecursionDepth == 0 || Desc.MaxRecursionDepth > Device.getLimits().MaxRayRecursionDepth)
		throw std::invalid_argument("Ray-tracing recursion depth exceeds the device limit.");

	struct StageData { std::shared_ptr<VulkanShader> Shader; std::vector<VkSpecializationMapEntry> Map; std::vector<std::byte> Data; VkSpecializationInfo Specialization {}; VkPipelineShaderStageCreateInfo Info {}; };
	std::vector<StageData> storage(Desc.Stages.size());
	std::vector<VkPipelineShaderStageCreateInfo> stages; stages.reserve(storage.size());
	for (size_t index = 0; index < Desc.Stages.size(); ++index)
	{
		auto& out = storage[index]; const auto& in = Desc.Stages[index];
		out.Shader = std::dynamic_pointer_cast<VulkanShader>(in.Shader);
		if (!out.Shader || &out.Shader->getDevice() != &Device || !out.Shader->isValid())
			throw std::invalid_argument("Ray-tracing shader belongs to another backend or device.");
		std::unordered_set<uint32_t> ids;
		for (const auto& constant : in.SpecializationConstants)
		{
			if (constant.Data.empty() || !ids.emplace(constant.Id).second) throw std::invalid_argument("Invalid specialization constant.");
			out.Map.push_back({ constant.Id, static_cast<uint32_t>(out.Data.size()), constant.Data.size() });
			out.Data.insert(out.Data.end(), constant.Data.begin(), constant.Data.end());
		}
		out.Specialization = { static_cast<uint32_t>(out.Map.size()), out.Map.data(), out.Data.size(), out.Data.data() };
		out.Info = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
			rayStage(out.Shader->getStage()), static_cast<VkShaderModule>(out.Shader->getVkShaderModule()),
			out.Shader->getEntryPoint().c_str(), out.Map.empty() ? nullptr : &out.Specialization };
		stages.emplace_back(out.Info);
	}

	std::vector<VkRayTracingShaderGroupCreateInfoKHR> groups; groups.reserve(Desc.Groups.size());
	auto valid = [&](uint32_t shader, std::initializer_list<EShaderStage_t> allowed) {
		if (shader == RayTracingShaderGroup::UnusedShader) return false;
		if (shader >= storage.size()) throw std::invalid_argument("Ray-tracing group shader index is out of range.");
		return std::ranges::find(allowed, storage[shader].Shader->getStage()) != allowed.end();
	};
	uint32_t ray_generation_group_count = 0;
	for (const auto& group : Desc.Groups)
	{
		VkRayTracingShaderGroupCreateInfoKHR out { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
		out.generalShader = out.closestHitShader = out.anyHitShader = out.intersectionShader = VK_SHADER_UNUSED_KHR;
		if (group.Type == ERayTracingShaderGroupType::General)
		{
			if (!valid(group.GeneralShader, { EShaderStage_t::RayGeneration, EShaderStage_t::Miss, EShaderStage_t::Callable }) ||
				group.ClosestHitShader != RayTracingShaderGroup::UnusedShader || group.AnyHitShader != RayTracingShaderGroup::UnusedShader || group.IntersectionShader != RayTracingShaderGroup::UnusedShader)
				throw std::invalid_argument("General ray-tracing group has invalid shader roles.");
			out.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; out.generalShader = group.GeneralShader;
			if (storage[group.GeneralShader].Shader->getStage() == EShaderStage_t::RayGeneration)
				++ray_generation_group_count;
		}
		else
		{
			if (group.GeneralShader != RayTracingShaderGroup::UnusedShader ||
				(group.ClosestHitShader != RayTracingShaderGroup::UnusedShader && !valid(group.ClosestHitShader, { EShaderStage_t::ClosestHit })) ||
				(group.AnyHitShader != RayTracingShaderGroup::UnusedShader && !valid(group.AnyHitShader, { EShaderStage_t::AnyHit })))
				throw std::invalid_argument("Hit group has invalid shader roles.");
			out.type = group.Type == ERayTracingShaderGroupType::ProceduralHitGroup
				? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
			out.closestHitShader = group.ClosestHitShader; out.anyHitShader = group.AnyHitShader;
			if (group.Type == ERayTracingShaderGroupType::ProceduralHitGroup)
			{
				if (!valid(group.IntersectionShader, { EShaderStage_t::Intersection })) throw std::invalid_argument("Procedural hit group requires an intersection shader.");
				out.intersectionShader = group.IntersectionShader;
			}
			else if (group.IntersectionShader != RayTracingShaderGroup::UnusedShader) throw std::invalid_argument("Triangle hit group cannot contain an intersection shader.");
		}
		groups.emplace_back(out);
	}
	if (ray_generation_group_count == 0)
		throw std::invalid_argument("Ray-tracing pipeline requires a ray-generation shader group.");

	VkPipelineCreateFlags flags = Desc.Compile.Flags.has(EPipelineCompileFlag_t::Optimize) ? 0 : VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
	#ifdef VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
	if (Desc.Compile.Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded)) flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
	#endif
	VulkanPipelineCache* cache = nullptr;
	if (Desc.Compile.Cache)
	{
		cache = dynamic_cast<VulkanPipelineCache*>(Desc.Compile.Cache.get());
		if (!cache || &cache->getDevice() != &Device || !cache->isValid()) throw std::invalid_argument("Ray-tracing pipeline cache is invalid.");
	}
	const VkRayTracingPipelineCreateInfoKHR info {
		.sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR, .pNext = nullptr,
		.flags = flags, .stageCount = static_cast<uint32_t>(stages.size()), .pStages = stages.data(),
		.groupCount = static_cast<uint32_t>(groups.size()), .pGroups = groups.data(),
		.maxPipelineRayRecursionDepth = Desc.MaxRecursionDepth,
		.pLibraryInfo = nullptr, .pLibraryInterface = nullptr, .pDynamicState = nullptr,
		.layout = static_cast<VkPipelineLayout>(layout->getVkPipelineLayout()),
		.basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1
	};
	std::unique_lock<std::mutex> cache_lock;
	if (cache) cache_lock = std::unique_lock(cache->getMutex());
	VkPipeline native = VK_NULL_HANDLE;
	const VkResult result = load<PFN_vkCreateRayTracingPipelinesKHR>(Device, "vkCreateRayTracingPipelinesKHR")(
		static_cast<VkDevice>(Device.getVkDevice()), VK_NULL_HANDLE,
		cache ? static_cast<VkPipelineCache>(cache->getVkPipelineCache()) : VK_NULL_HANDLE, 1, &info, nullptr, &native);
	if (result == VK_PIPELINE_COMPILE_REQUIRED) return {};
	if (result != VK_SUCCESS) throw std::runtime_error("vkCreateRayTracingPipelinesKHR failed.");
	vk::UniquePipeline pipeline(
		vk::Pipeline(native),
		vk::detail::ObjectDestroy<vk::Device, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>(Device.getVkDevice()));
	return std::make_shared<VulkanPipeline>(Device, EPipelineType::RayTracing, Desc.Layout,
		RenderingSignature {}, EDynamicStates {}, EPrimitiveTopology::TriangleList, false,
		hashRayTracingPipeline(Desc), Desc.DebugName, std::move(pipeline), static_cast<uint32_t>(groups.size()));
}

} // namespace rhi