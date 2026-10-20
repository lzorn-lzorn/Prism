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
    auto Proc = reinterpret_cast<Function>(Device.getVkDevice().getProcAddr(Name));
    if (!Proc) throw std::logic_error(std::string("Missing Vulkan ray-tracing command: ") + Name);
    return Proc;
}

VkAccelerationStructureTypeKHR toVk(EAccelerationStructureType Type)
{
    return Type == EAccelerationStructureType::TopLevel
        ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
        : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
}

VkBuildAccelerationStructureFlagsKHR toVk(EAccelerationStructureBuildFlags Flags)
{
    VkBuildAccelerationStructureFlagsKHR Result = 0;
    if (Flags.has(EAccelerationStructureBuildFlag_t::AllowUpdate)) Result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    if (Flags.has(EAccelerationStructureBuildFlag_t::AllowCompaction)) Result |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    if (Flags.has(EAccelerationStructureBuildFlag_t::PreferFastTrace)) Result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (Flags.has(EAccelerationStructureBuildFlag_t::PreferFastBuild)) Result |= VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR;
    if (Flags.has(EAccelerationStructureBuildFlag_t::LowMemory)) Result |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    return Result;
}

VkGeometryFlagsKHR toVk(EAccelerationStructureGeometryFlags Flags)
{
    VkGeometryFlagsKHR Result = 0;
    if (Flags.has(EAccelerationStructureGeometryFlag_t::Opaque)) Result |= VK_GEOMETRY_OPAQUE_BIT_KHR;
    if (Flags.has(EAccelerationStructureGeometryFlag_t::NoDuplicateAnyHitInvocation)) Result |= VK_GEOMETRY_NO_DUPLICATE_ANY_HIT_INVOCATION_BIT_KHR;
    return Result;
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
    auto* Result = dynamic_cast<VulkanBuffer*>(Buffer.get());
    if (!Result || &Result->getDevice() != &Device || !Result->isValid() ||
        !Buffer->getDescriptor().Usage.has(Usage) || Offset >= Buffer->getDescriptor().Size ||
        Buffer->getDeviceAddress() == 0)
        throw std::invalid_argument("Acceleration-structure input requires an addressable Vulkan buffer and valid offset.");
    return Result;
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
    GeometryStorage Output;
    Output.Geometries.reserve(Geometries.size());
    Output.Ranges.reserve(Geometries.size());
    Output.PrimitiveCounts.reserve(Geometries.size());
    for (const auto& Geometry : Geometries)
    {
        constexpr uint8_t KnownGeometryFlags =
            static_cast<uint8_t>(EAccelerationStructureGeometryFlag_t::Opaque) |
            static_cast<uint8_t>(EAccelerationStructureGeometryFlag_t::NoDuplicateAnyHitInvocation);
        if ((Geometry.Flags.Value & ~KnownGeometryFlags) != 0)
            throw std::invalid_argument("Acceleration-structure geometry contains unknown flags.");
        if (Geometry.PrimitiveCount == 0)
            throw std::invalid_argument("Acceleration-structure geometry requires primitives.");
        VkAccelerationStructureGeometryKHR Converted {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .pNext = nullptr,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .geometry = {},
            .flags = toVk(Geometry.Flags)
        };
        if (Geometry.Type == EAccelerationStructureGeometryType::Triangles)
        {
            if (StructureType != EAccelerationStructureType::BottomLevel)
                throw std::invalid_argument("Triangle geometry is only valid in a bottom-level acceleration structure.");
            const auto* Triangles = std::get_if<AccelerationStructureTriangles>(&Geometry.Data);
            if (!Triangles || Triangles->VertexStride == 0 || Triangles->MaxVertex == 0)
                throw std::invalid_argument("Triangle geometry has invalid vertex metadata.");
            const VkFormat Format = rayTracingVertexFormat(Triangles->VertexFormat);
            if (Format == VK_FORMAT_UNDEFINED)
                throw std::invalid_argument("Triangle vertex format is unsupported for Vulkan ray tracing.");
            auto* Vertices = requireBuffer(Triangles->VertexBuffer, Device,
                EBufferUsage::AccelerationStructureBuildInput, Triangles->VertexOffset);
            VkIndexType IndexType = VK_INDEX_TYPE_NONE_KHR;
            VkDeviceAddress IndexAddress = 0;
            if (Triangles->IndexFormat != EIndexFormat::None)
            {
                auto* Indices = requireBuffer(Triangles->IndexBuffer, Device,
                    EBufferUsage::AccelerationStructureBuildInput, Triangles->IndexOffset);
                IndexType = Triangles->IndexFormat == EIndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
                IndexAddress = Indices->getDeviceAddress() + Triangles->IndexOffset;
                Output.Resources.emplace_back(Triangles->IndexBuffer);
            }
            VkDeviceAddress TransformAddress = 0;
            if (Triangles->TransformBuffer)
            {
                auto* Transform = requireBuffer(Triangles->TransformBuffer, Device,
                    EBufferUsage::AccelerationStructureBuildInput, Triangles->TransformOffset);
                TransformAddress = Transform->getDeviceAddress() + Triangles->TransformOffset;
                Output.Resources.emplace_back(Triangles->TransformBuffer);
            }
            Converted.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
            Converted.geometry.triangles = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .pNext = nullptr,
                .vertexFormat = Format,
                .vertexData = { Vertices->getDeviceAddress() + Triangles->VertexOffset },
                .vertexStride = Triangles->VertexStride,
                .maxVertex = Triangles->MaxVertex,
                .indexType = IndexType,
                .indexData = { IndexAddress },
                .transformData = { TransformAddress }
            };
            Output.Resources.emplace_back(Triangles->VertexBuffer);
        }
        else if (Geometry.Type == EAccelerationStructureGeometryType::AABBs)
        {
            if (StructureType != EAccelerationStructureType::BottomLevel)
                throw std::invalid_argument("AABB geometry is only valid in a bottom-level acceleration structure.");
            const auto* Aabbs = std::get_if<AccelerationStructureAABBs>(&Geometry.Data);
            if (!Aabbs || Aabbs->Stride < 24 || Aabbs->Stride % 8 != 0)
                throw std::invalid_argument("AABB geometry stride must be at least 24 and a multiple of 8.");
            auto* Buffer = requireBuffer(Aabbs->Buffer, Device,
                EBufferUsage::AccelerationStructureBuildInput, Aabbs->Offset);
            Converted.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            Converted.geometry.aabbs = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR,
                .pNext = nullptr,
                .data = { Buffer->getDeviceAddress() + Aabbs->Offset },
                .stride = Aabbs->Stride
            };
            Output.Resources.emplace_back(Aabbs->Buffer);
        }
        else
        {
            if (StructureType != EAccelerationStructureType::TopLevel)
                throw std::invalid_argument("Instance geometry is only valid in a top-level acceleration structure.");
            const auto* Instances = std::get_if<AccelerationStructureInstances>(&Geometry.Data);
            if (!Instances) throw std::invalid_argument("Instance geometry payload does not match its type.");
            auto* Buffer = requireBuffer(Instances->Buffer, Device,
                EBufferUsage::AccelerationStructureBuildInput, Instances->Offset);
            Converted.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
            Converted.geometry.instances = {
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
                .pNext = nullptr,
                .arrayOfPointers = Instances->ArrayOfPointers,
                .data = { Buffer->getDeviceAddress() + Instances->Offset }
            };
            Output.Resources.emplace_back(Instances->Buffer);
        }
        Output.Geometries.emplace_back(Converted);
        Output.PrimitiveCounts.emplace_back(Geometry.PrimitiveCount);
        if (IncludeRanges)
            Output.Ranges.push_back({ Geometry.PrimitiveCount, Geometry.PrimitiveOffset,
                Geometry.FirstVertex, Geometry.TransformOffset });
    }
    return Output;
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
    uint64_t Hash = 14695981039346656037ull;
    auto Append = [&](const void* Data, size_t Size) {
        for (size_t Index = 0; Index < Size; ++Index) { Hash ^= static_cast<const uint8_t*>(Data)[Index]; Hash *= 1099511628211ull; }
    };
    const auto LayoutHash = Desc.Layout->getCompatibilityHash(); Append(&LayoutHash, sizeof(LayoutHash));
    Append(&Desc.MaxRecursionDepth, sizeof(Desc.MaxRecursionDepth));
    for (const auto& Stage : Desc.Stages)
    {
        const auto ShaderHash = Stage.Shader->getContentHash(); Append(&ShaderHash, sizeof(ShaderHash));
        auto Constants = Stage.SpecializationConstants;
        std::ranges::sort(Constants, {}, &SpecializationConstant::Id);
        for (const auto& Constant : Constants) { Append(&Constant.Id, sizeof(Constant.Id)); Append(Constant.Data.data(), Constant.Data.size()); }
    }
    for (const auto& Group : Desc.Groups)
    {
        Append(&Group.Type, sizeof(Group.Type)); Append(&Group.GeneralShader, sizeof(Group.GeneralShader));
        Append(&Group.ClosestHitShader, sizeof(Group.ClosestHitShader)); Append(&Group.AnyHitShader, sizeof(Group.AnyHitShader));
        Append(&Group.IntersectionShader, sizeof(Group.IntersectionShader));
    }
    Append(&Desc.Compile.Flags.Value, sizeof(Desc.Compile.Flags.Value));
    return Hash;
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
    auto* Buffer = dynamic_cast<VulkanBuffer*>(Storage.get());
    if (!Buffer || &Buffer->getDevice() != Device || !Buffer->isValid() ||
        !Storage->getDescriptor().Usage.has(EBufferUsage::AccelerationStructureStorage) ||
        Desc.Size == 0 || Desc.Offset % 256 != 0 || Desc.Offset >= Storage->getDescriptor().Size ||
        Desc.Size > Storage->getDescriptor().Size - Desc.Offset)
        throw std::invalid_argument("Acceleration-structure storage range is invalid or not 256-byte aligned.");
    const VkAccelerationStructureCreateInfoKHR Info {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
        .pNext = nullptr, .createFlags = 0,
        .buffer = static_cast<VkBuffer>(Buffer->getVkBuffer()),
        .offset = Desc.Offset, .size = Desc.Size, .type = toVk(Desc.Type), .deviceAddress = 0
    };
    const auto Create = load<PFN_vkCreateAccelerationStructureKHR>(*Device, "vkCreateAccelerationStructureKHR");
    if (Create(static_cast<VkDevice>(Device->getVkDevice()), &Info, nullptr,
        reinterpret_cast<VkAccelerationStructureKHR*>(&Handle)) != VK_SUCCESS)
        throw std::runtime_error("vkCreateAccelerationStructureKHR failed.");
}

VulkanAccelerationStructure::~VulkanAccelerationStructure()
{
    if (Handle && Device)
    {
        auto Destroy = reinterpret_cast<PFN_vkDestroyAccelerationStructureKHR>(Device->getVkDevice().getProcAddr("vkDestroyAccelerationStructureKHR"));
        if (Destroy) Destroy(static_cast<VkDevice>(Device->getVkDevice()), static_cast<VkAccelerationStructureKHR>(Handle), nullptr);
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
    auto GetAddress = reinterpret_cast<PFN_vkGetAccelerationStructureDeviceAddressKHR>(
        Device->getVkDevice().getProcAddr("vkGetAccelerationStructureDeviceAddressKHR"));
    if (!GetAddress) return 0;
    const VkAccelerationStructureDeviceAddressInfoKHR Info {
        VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, nullptr,
        static_cast<VkAccelerationStructureKHR>(Handle) };
    return GetAddress(static_cast<VkDevice>(Device->getVkDevice()), &Info);
}

AccelerationStructureBuildSizes queryVulkanAccelerationStructureBuildSizes(
    const VulkanDevice& Device, EAccelerationStructureType Type,
    EAccelerationStructureBuildFlags Flags,
    std::span<const AccelerationStructureGeometry> Geometries)
{
    if (!Device.getFeatures().AccelerationStructure) return {};
    try
    {
        constexpr uint8_t KnownBuildFlags =
            static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::AllowUpdate) |
            static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::AllowCompaction) |
            static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::PreferFastTrace) |
            static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::PreferFastBuild) |
            static_cast<uint8_t>(EAccelerationStructureBuildFlag_t::LowMemory);
        if ((Flags.Value & ~KnownBuildFlags) != 0) return {};
        auto Geometry = convertGeometries(Device, Type, Geometries, false);
        const VkAccelerationStructureBuildGeometryInfoKHR Info {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .pNext = nullptr, .type = toVk(Type), .flags = toVk(Flags),
            .mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .srcAccelerationStructure = VK_NULL_HANDLE, .dstAccelerationStructure = VK_NULL_HANDLE,
            .geometryCount = static_cast<uint32_t>(Geometry.Geometries.size()),
            .pGeometries = Geometry.Geometries.data(), .ppGeometries = nullptr,
            .scratchData = { 0 }
        };
        VkAccelerationStructureBuildSizesInfoKHR Sizes {
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
        auto Query = reinterpret_cast<PFN_vkGetAccelerationStructureBuildSizesKHR>(
            Device.getVkDevice().getProcAddr("vkGetAccelerationStructureBuildSizesKHR"));
        if (!Query) return {};
        Query(static_cast<VkDevice>(Device.getVkDevice()),
            VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &Info,
            Geometry.PrimitiveCounts.data(), &Sizes);
        return { Sizes.accelerationStructureSize, Sizes.buildScratchSize, Sizes.updateScratchSize };
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
        std::vector<GeometryStorage> GeometryStorage;
        std::vector<VkAccelerationStructureBuildGeometryInfoKHR> Infos;
        std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> Ranges;
        GeometryStorage.reserve(Builds.size()); Infos.reserve(Builds.size()); Ranges.reserve(Builds.size());
        for (const auto& Build : Builds)
        {
            auto Destination = std::dynamic_pointer_cast<VulkanAccelerationStructure>(Build.Destination);
            auto Source = std::dynamic_pointer_cast<VulkanAccelerationStructure>(Build.Source);
            if (!Destination || &Destination->getDevice() != &Device || !Destination->isValid() ||
                (Build.Mode == EAccelerationStructureBuildMode::Update &&
                 (!Source || &Source->getDevice() != &Device || Source->getType() != Destination->getType())))
                return false;
            if ((Build.Mode == EAccelerationStructureBuildMode::Build && Source) ||
                (Build.Mode == EAccelerationStructureBuildMode::Update &&
                 !Build.Flags.has(EAccelerationStructureBuildFlag_t::AllowUpdate)))
                return false;
            auto* Scratch = requireBuffer(Build.ScratchBuffer, Device, EBufferUsage::DeviceAddress, Build.ScratchOffset);
            if (!Build.ScratchBuffer->getDescriptor().Usage.has(EBufferUsage::Storage) ||
                (Scratch->getDeviceAddress() + Build.ScratchOffset) %
                    Device.getLimits().MinAccelerationStructureScratchOffsetAlignment != 0)
                return false;
            const auto Sizes = queryVulkanAccelerationStructureBuildSizes(Device, Destination->getType(), Build.Flags, Build.Geometries);
            const uint64_t RequiredScratch = Build.Mode == EAccelerationStructureBuildMode::Update
                ? Sizes.UpdateScratchSize : Sizes.BuildScratchSize;
            if (!Sizes.AccelerationStructureSize || Destination->getSize() < Sizes.AccelerationStructureSize ||
                Build.ScratchOffset >= Build.ScratchBuffer->getDescriptor().Size ||
                RequiredScratch > Build.ScratchBuffer->getDescriptor().Size - Build.ScratchOffset)
                return false;
            GeometryStorage.emplace_back(convertGeometries(Device, Destination->getType(), Build.Geometries, true));
            auto& Converted = GeometryStorage.back();
            Infos.push_back({
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                .pNext = nullptr, .type = toVk(Destination->getType()), .flags = toVk(Build.Flags),
                .mode = Build.Mode == EAccelerationStructureBuildMode::Update
                    ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                .srcAccelerationStructure = Source ? static_cast<VkAccelerationStructureKHR>(Source->getVkHandle()) : VK_NULL_HANDLE,
                .dstAccelerationStructure = static_cast<VkAccelerationStructureKHR>(Destination->getVkHandle()),
                .geometryCount = static_cast<uint32_t>(Converted.Geometries.size()),
                .pGeometries = Converted.Geometries.data(), .ppGeometries = nullptr,
                .scratchData = { Scratch->getDeviceAddress() + Build.ScratchOffset }
            });
            Ranges.emplace_back(Converted.Ranges.data());
            RetainedResources.emplace_back(Build.Destination);
            if (Build.Source) RetainedResources.emplace_back(Build.Source);
            RetainedResources.emplace_back(Build.ScratchBuffer);
            RetainedResources.insert(RetainedResources.end(), Converted.Resources.begin(), Converted.Resources.end());
        }
        // vkCmdBuildAccelerationStructuresKHR consumes one range-array pointer per build info.
        load<PFN_vkCmdBuildAccelerationStructuresKHR>(Device, "vkCmdBuildAccelerationStructuresKHR")(
            static_cast<VkCommandBuffer>(CommandBuffer), static_cast<uint32_t>(Infos.size()), Infos.data(), Ranges.data());
        return true;
    }
    catch (const std::invalid_argument&) { return false; }
}

std::shared_ptr<RPipeline> createVulkanRayTracingPipeline(
    VulkanDevice& Device, const RayTracingPipelineDescriptor& Desc)
{
    if (!Device.getFeatures().RayTracingPipeline) return {};
    auto Layout = std::dynamic_pointer_cast<VulkanPipelineLayout>(Desc.Layout);
    if (!Layout || &Layout->getDevice() != &Device || !Layout->isValid() || Desc.Stages.empty() || Desc.Groups.empty())
        throw std::invalid_argument("Ray-tracing pipeline layout, stages, or groups are invalid.");
    if (Desc.MaxRecursionDepth == 0 || Desc.MaxRecursionDepth > Device.getLimits().MaxRayRecursionDepth)
        throw std::invalid_argument("Ray-tracing recursion depth exceeds the device limit.");

    struct StageData { std::shared_ptr<VulkanShader> Shader; std::vector<VkSpecializationMapEntry> Map; std::vector<std::byte> Data; VkSpecializationInfo Specialization {}; VkPipelineShaderStageCreateInfo Info {}; };
    std::vector<StageData> Storage(Desc.Stages.size());
    std::vector<VkPipelineShaderStageCreateInfo> Stages; Stages.reserve(Storage.size());
    for (size_t Index = 0; Index < Desc.Stages.size(); ++Index)
    {
        auto& Out = Storage[Index]; const auto& In = Desc.Stages[Index];
        Out.Shader = std::dynamic_pointer_cast<VulkanShader>(In.Shader);
        if (!Out.Shader || &Out.Shader->getDevice() != &Device || !Out.Shader->isValid())
            throw std::invalid_argument("Ray-tracing shader belongs to another backend or device.");
        std::unordered_set<uint32_t> Ids;
        for (const auto& Constant : In.SpecializationConstants)
        {
            if (Constant.Data.empty() || !Ids.emplace(Constant.Id).second) throw std::invalid_argument("Invalid specialization constant.");
            Out.Map.push_back({ Constant.Id, static_cast<uint32_t>(Out.Data.size()), Constant.Data.size() });
            Out.Data.insert(Out.Data.end(), Constant.Data.begin(), Constant.Data.end());
        }
        Out.Specialization = { static_cast<uint32_t>(Out.Map.size()), Out.Map.data(), Out.Data.size(), Out.Data.data() };
        Out.Info = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
            rayStage(Out.Shader->getStage()), static_cast<VkShaderModule>(Out.Shader->getVkShaderModule()),
            Out.Shader->getEntryPoint().c_str(), Out.Map.empty() ? nullptr : &Out.Specialization };
        Stages.emplace_back(Out.Info);
    }

    std::vector<VkRayTracingShaderGroupCreateInfoKHR> Groups; Groups.reserve(Desc.Groups.size());
    auto Valid = [&](uint32_t Shader, std::initializer_list<EShaderStage_t> Allowed) {
        if (Shader == RayTracingShaderGroup::UnusedShader) return false;
        if (Shader >= Storage.size()) throw std::invalid_argument("Ray-tracing group shader index is out of range.");
        return std::ranges::find(Allowed, Storage[Shader].Shader->getStage()) != Allowed.end();
    };
    uint32_t RayGenerationGroupCount = 0;
    for (const auto& Group : Desc.Groups)
    {
        VkRayTracingShaderGroupCreateInfoKHR Out { VK_STRUCTURE_TYPE_RAY_TRACING_SHADER_GROUP_CREATE_INFO_KHR };
        Out.generalShader = Out.closestHitShader = Out.anyHitShader = Out.intersectionShader = VK_SHADER_UNUSED_KHR;
        if (Group.Type == ERayTracingShaderGroupType::General)
        {
            if (!Valid(Group.GeneralShader, { EShaderStage_t::RayGeneration, EShaderStage_t::Miss, EShaderStage_t::Callable }) ||
                Group.ClosestHitShader != RayTracingShaderGroup::UnusedShader || Group.AnyHitShader != RayTracingShaderGroup::UnusedShader || Group.IntersectionShader != RayTracingShaderGroup::UnusedShader)
                throw std::invalid_argument("General ray-tracing group has invalid shader roles.");
            Out.type = VK_RAY_TRACING_SHADER_GROUP_TYPE_GENERAL_KHR; Out.generalShader = Group.GeneralShader;
            if (Storage[Group.GeneralShader].Shader->getStage() == EShaderStage_t::RayGeneration)
                ++RayGenerationGroupCount;
        }
        else
        {
            if (Group.GeneralShader != RayTracingShaderGroup::UnusedShader ||
                (Group.ClosestHitShader != RayTracingShaderGroup::UnusedShader && !Valid(Group.ClosestHitShader, { EShaderStage_t::ClosestHit })) ||
                (Group.AnyHitShader != RayTracingShaderGroup::UnusedShader && !Valid(Group.AnyHitShader, { EShaderStage_t::AnyHit })))
                throw std::invalid_argument("Hit group has invalid shader roles.");
            Out.type = Group.Type == ERayTracingShaderGroupType::ProceduralHitGroup
                ? VK_RAY_TRACING_SHADER_GROUP_TYPE_PROCEDURAL_HIT_GROUP_KHR : VK_RAY_TRACING_SHADER_GROUP_TYPE_TRIANGLES_HIT_GROUP_KHR;
            Out.closestHitShader = Group.ClosestHitShader; Out.anyHitShader = Group.AnyHitShader;
            if (Group.Type == ERayTracingShaderGroupType::ProceduralHitGroup)
            {
                if (!Valid(Group.IntersectionShader, { EShaderStage_t::Intersection })) throw std::invalid_argument("Procedural hit group requires an intersection shader.");
                Out.intersectionShader = Group.IntersectionShader;
            }
            else if (Group.IntersectionShader != RayTracingShaderGroup::UnusedShader) throw std::invalid_argument("Triangle hit group cannot contain an intersection shader.");
        }
        Groups.emplace_back(Out);
    }
    if (RayGenerationGroupCount == 0)
        throw std::invalid_argument("Ray-tracing pipeline requires a ray-generation shader group.");

    VkPipelineCreateFlags Flags = Desc.Compile.Flags.has(EPipelineCompileFlag_t::Optimize) ? 0 : VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    #ifdef VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT
    if (Desc.Compile.Flags.has(EPipelineCompileFlag_t::FailIfCompileNeeded)) flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    #endif
    VulkanPipelineCache* Cache = nullptr;
    if (Desc.Compile.Cache)
    {
        Cache = dynamic_cast<VulkanPipelineCache*>(Desc.Compile.Cache.get());
        if (!Cache || &Cache->getDevice() != &Device || !Cache->isValid()) throw std::invalid_argument("Ray-tracing pipeline cache is invalid.");
    }
    const VkRayTracingPipelineCreateInfoKHR Info {
        .sType = VK_STRUCTURE_TYPE_RAY_TRACING_PIPELINE_CREATE_INFO_KHR, .pNext = nullptr,
        .flags = Flags, .stageCount = static_cast<uint32_t>(Stages.size()), .pStages = Stages.data(),
        .groupCount = static_cast<uint32_t>(Groups.size()), .pGroups = Groups.data(),
        .maxPipelineRayRecursionDepth = Desc.MaxRecursionDepth,
        .pLibraryInfo = nullptr, .pLibraryInterface = nullptr, .pDynamicState = nullptr,
        .layout = static_cast<VkPipelineLayout>(Layout->getVkPipelineLayout()),
        .basePipelineHandle = VK_NULL_HANDLE, .basePipelineIndex = -1
    };
    std::unique_lock<std::mutex> CacheLock;
    if (Cache) CacheLock = std::unique_lock(Cache->getMutex());
    VkPipeline Native = VK_NULL_HANDLE;
    const VkResult Result = load<PFN_vkCreateRayTracingPipelinesKHR>(Device, "vkCreateRayTracingPipelinesKHR")(
        static_cast<VkDevice>(Device.getVkDevice()), VK_NULL_HANDLE,
        Cache ? static_cast<VkPipelineCache>(Cache->getVkPipelineCache()) : VK_NULL_HANDLE, 1, &Info, nullptr, &Native);
    if (Result == VK_PIPELINE_COMPILE_REQUIRED) return {};
    if (Result != VK_SUCCESS) throw std::runtime_error("vkCreateRayTracingPipelinesKHR failed.");
    vk::UniquePipeline Pipeline(
        vk::Pipeline(Native),
        vk::detail::ObjectDestroy<vk::Device, VULKAN_HPP_DEFAULT_DISPATCHER_TYPE>(Device.getVkDevice()));
    return std::make_shared<VulkanPipeline>(Device, EPipelineType::RayTracing, Desc.Layout,
        RenderingSignature {}, EDynamicStates {}, EPrimitiveTopology::TriangleList, false,
        hashRayTracingPipeline(Desc), Desc.DebugName, std::move(Pipeline), static_cast<uint32_t>(Groups.size()));
}

} // namespace rhi