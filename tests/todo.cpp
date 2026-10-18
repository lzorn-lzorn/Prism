#include <RHI.hpp>
#include <vk/VulkanWSI.hpp>

#include <cassert>
#include <iostream>
#include <type_traits>

namespace
{
using namespace rhi;

constexpr bool flagsContract()
{
	EBufferUsageFlags usage(EBufferUsage::Vertex);
	usage = usage | EBufferUsage::TransferDst;
	if (!usage.has(EBufferUsage::Vertex) || !usage.has(EBufferUsage::TransferDst))
		return false;
	usage.set(EBufferUsage::DeviceAddress);
	usage.clear(EBufferUsage::Vertex);
	return usage.has(EBufferUsage::DeviceAddress) && !usage.has(EBufferUsage::Vertex) &&
		static_cast<bool>(usage);
}

constexpr RenderingSignature colorSignature(EFormat Format)
{
	RenderingSignature signature;
	signature.ColorFormats[0] = Format;
	signature.ColorAttachmentCount = 1;
	return signature;
}

static_assert(flagsContract());
static_assert(std::is_same_v<EBufferUsageFlags::enum_type, EBufferUsage>);
static_assert(std::is_same_v<EPipelineStatistics::underlying, uint32_t>);
static_assert(static_cast<uint32_t>(EBufferUsage::DeviceAddress) == (1u << 9));
static_assert(static_cast<uint64_t>(EDynamicState_t::VertexInput) == (1ull << 16));
static_assert(EAcquireStatus::DeviceLost != EAcquireStatus::SurfaceLost);
static_assert(EPresentStatus::OutOfDate != EPresentStatus::Suboptimal);
static_assert(vulkan_wsi::mapAcquire(vk::Result::eTimeout) == EAcquireStatus::NotReady);
static_assert(vulkan_wsi::mapPresent(vk::Result::eErrorDeviceLost) == EPresentStatus::DeviceLost);
static_assert(vulkan_wsi::mapStatus(vk::Result::eErrorUnknown) == std::nullopt);
static_assert(isRenderingCompatible(colorSignature(EFormat::BGRA8_UNorm),
	colorSignature(EFormat::BGRA8_UNorm)));
static_assert(!isRenderingCompatible(colorSignature(EFormat::BGRA8_UNorm),
	colorSignature(EFormat::RGBA8_UNorm)));
static_assert(timestampTicksToNanoseconds(4, 2.5) == 10.0);
static_assert(timestampTicksToNanoseconds(4, 0.0) == 0.0);
static_assert(RayTracingShaderGroup::UnusedShader == std::numeric_limits<uint32_t>::max());
}

int main()
{
	using namespace rhi;

	EBufferUsageFlags buffer_usage(EBufferUsage::Vertex);
	buffer_usage = buffer_usage | EBufferUsage::TransferDst;
	assert(buffer_usage.has(EBufferUsage::Vertex));
	assert(buffer_usage.has(EBufferUsage::TransferDst));
	assert(!buffer_usage.has(EBufferUsage::Index));
	buffer_usage.set(EBufferUsage::DeviceAddress);
	assert(buffer_usage.has(EBufferUsage::DeviceAddress));

	const RBuffer::Descriptor_t modern_buffer;
	assert(modern_buffer.MemoryUsage == EMemoryUsage::Auto);
	assert(!modern_buffer.DedicatedAllocation);
	assert(!modern_buffer.PersistentlyMapped);
	assert(DeviceFeatures {}.DeferredRelease == false);
	assert(DeviceFeatures {}.ExtendedDynamicState == false);
	assert(DeviceFeatures {}.ExtendedDynamicState2 == false);
	assert(DeviceFeatures {}.ExtendedDynamicState3 == false);
	assert(DeviceFeatures {}.DynamicVertexInput == false);
	assert(DeviceFeatures {}.SecondaryCommandLists == false);
	assert(DeviceFeatures {}.QueryResultCopy == false);
	assert(DeviceFeatures {}.BufferDeviceAddress == false);
	assert(DeviceFeatures {}.AccelerationStructure == false);
	assert(DeviceFeatures {}.RayTracingPipeline == false);
	assert(DeviceFeatures {}.RayQuery == false);
	assert(DeviceFeatures {}.SwapchainStatus == false);
	assert(DeviceFeatures {}.HDRSwapchain == false);
	assert(DeviceFeatures {}.HDRMetadata == false);
	assert(DeviceLimits {}.ShaderBindingTableAlignment == 1);
	assert(DeviceLimits {}.ShaderGroupHandleAlignment == 1);
	assert(DeviceLimits {}.ShaderGroupHandleSize == 0);
	assert(DeviceLimits {}.MaxShaderGroupStride == 0);
	assert(DeviceLimits {}.MaxRayRecursionDepth == 0);
	assert(DeviceLimits {}.MaxRayDispatchInvocationCount == 0);
	const MemoryRequirements memory_requirements { 1, 1, 1 };
	assert(!memory_requirements.RequiresDedicatedAllocation);

	const ImageBarrier image_barrier;
	assert(image_barrier.Before == EResourceState::Undefined);
	assert(image_barrier.Range.MipLevelCount == 1);
	assert(!image_barrier.SourceQueue.has_value());

	const BufferBarrier buffer_barrier;
	assert(buffer_barrier.Size == 0);
	assert(buffer_barrier.Before == EResourceState::Common);

	const SwapchainDescriptor swapchain;
	assert(swapchain.MinimumImageCount >= 2);
	assert(swapchain.PreferredPresentMode == EPresentMode::Mailbox);
	assert(!swapchain.RequireExactFormatAndColorSpace);
	SwapchainDescriptor minimized_swapchain = swapchain;
	minimized_swapchain.Width = 0;
	minimized_swapchain.Height = 0;
	assert(minimized_swapchain.Width == 0 && minimized_swapchain.Height == 0);
	const AcquireResult no_acquire;
	assert(no_acquire.Generation == 0);
	const PresentDescriptor no_present;
	assert(no_present.Generation == 0);

	using namespace vulkan_wsi;
	assert(mapAcquire(vk::Result::eSuccess) == EAcquireStatus::Success);
	assert(mapAcquire(vk::Result::eSuboptimalKHR) == EAcquireStatus::Suboptimal);
	assert(mapAcquire(vk::Result::eErrorOutOfDateKHR) == EAcquireStatus::OutOfDate);
	assert(mapAcquire(vk::Result::eErrorSurfaceLostKHR) == EAcquireStatus::SurfaceLost);
	assert(mapAcquire(vk::Result::eErrorDeviceLost) == EAcquireStatus::DeviceLost);
	assert(mapAcquire(vk::Result::eTimeout) == EAcquireStatus::NotReady);
	assert(mapPresent(vk::Result::eErrorOutOfDateKHR) == EPresentStatus::OutOfDate);
	assert(mapPresent(vk::Result::eErrorSurfaceLostKHR) == EPresentStatus::SurfaceLost);
	assert(mapPresent(vk::Result::eErrorDeviceLost) == EPresentStatus::DeviceLost);
	assert(mapStatus(vk::Result::eSuboptimalKHR) == ESwapchainStatus::Suboptimal);
	assert(!mapStatus(vk::Result::eErrorUnknown).has_value());

	const CommandListDescriptor commands;
	assert(commands.QueueType == ECommandQueueType::Graphics);
	assert(commands.Level == ECommandListLevel::Primary);
	assert(commands.OneTimeSubmit);
	assert(!commands.RenderingInheritance.has_value());

	const RenderingInfo rendering;
	assert(!rendering.SecondaryCommandBuffers);
	const SecondaryCommandListInheritance inheritance;
	assert(inheritance.ColorAttachmentCount == 0);
	assert(inheritance.DepthFormat == EFormat::Undefined);
	assert(inheritance.SampleCount == ESampleCount::Count1);

	const QueryPoolDescriptor queries;
	assert(queries.Type == EQueryType::Timestamp);
	assert(queries.Count == 1);
	assert(!queries.PipelineStatistics);
	assert(timestampTicksToNanoseconds(4, 2.5) == 10.0);
	assert(timestampTicksToNanoseconds(4, 0.0) == 0.0);

	EQueryResultFlags query_flags(EQueryResultFlag_t::Wait);
	query_flags.set(EQueryResultFlag_t::WithAvailability);
	assert(query_flags.has(EQueryResultFlag_t::Wait));
	assert(query_flags.has(EQueryResultFlag_t::WithAvailability));
	query_flags.clear(EQueryResultFlag_t::Wait);
	assert(!query_flags.has(EQueryResultFlag_t::Wait));
	EPipelineStatistics statistics(EPipelineStatistic_t::VertexShaderInvocations);
	statistics = statistics | EPipelineStatistic_t::PixelShaderInvocations;
	assert(statistics.has(EPipelineStatistic_t::VertexShaderInvocations));
	assert(statistics.has(EPipelineStatistic_t::PixelShaderInvocations));

	const AccelerationStructureBuildSizes build_sizes;
	assert(build_sizes.AccelerationStructureSize == 0);
	const AccelerationStructureGeometry geometry;
	assert(geometry.Type == EAccelerationStructureGeometryType::Triangles);
	assert(geometry.PrimitiveCount == 0);
	const AccelerationStructureBuildDescriptor build;
	assert(build.Mode == EAccelerationStructureBuildMode::Build);
	assert(build.Geometries.empty());
	const RayTracingShaderGroup general_group;
	assert(general_group.Type == ERayTracingShaderGroupType::General);
	assert(general_group.GeneralShader == RayTracingShaderGroup::UnusedShader);
	const RayTracingPipelineDescriptor ray_pipeline;
	assert(ray_pipeline.MaxRecursionDepth == 1);
	const TraceRaysDescriptor trace;
	assert(trace.Width == 1 && trace.Height == 1 && trace.Depth == 1);
	assert(trace.RayGeneration.Address == 0 && trace.RayGeneration.Size == 0);

	std::cout << "SeedEngine RHI contract tests passed.\n";
	return 0;
}
