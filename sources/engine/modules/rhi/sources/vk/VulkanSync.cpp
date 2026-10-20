#include "VulkanSync.hpp"

#include "VulkanCommandList.hpp"
#include "VulkanDevice.hpp"
#include "VulkanSwapchain.hpp"
#include "VulkanWSI.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace rhi
{
namespace
{
VulkanSemaphore& semaphoreOf(const std::shared_ptr<RSemaphore>& Semaphore, VulkanDevice& Device)
{
    auto* result = dynamic_cast<VulkanSemaphore*>(Semaphore.get());
    if (!result || &result->getDevice() != &Device)
        throw std::invalid_argument("Semaphore belongs to another RHI device or backend.");
    return *result;
}

VulkanFence& fenceOf(const std::shared_ptr<RFence>& Fence, VulkanDevice& Device)
{
    auto* result = dynamic_cast<VulkanFence*>(Fence.get());
    if (!result || &result->getDevice() != &Device)
        throw std::invalid_argument("Fence belongs to another RHI device or backend.");
    return *result;
}

vk::QueryPipelineStatisticFlags pipelineStatistics(EPipelineStatistics Statistics)
{
    vk::QueryPipelineStatisticFlags result;
    if (Statistics.has(EPipelineStatistic_t::InputAssemblyVertices))
        result |= vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices;
    if (Statistics.has(EPipelineStatistic_t::InputAssemblyPrimitives))
        result |= vk::QueryPipelineStatisticFlagBits::eInputAssemblyPrimitives;
    if (Statistics.has(EPipelineStatistic_t::VertexShaderInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::GeometryShaderInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eGeometryShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::GeometryShaderPrimitives))
        result |= vk::QueryPipelineStatisticFlagBits::eGeometryShaderPrimitives;
    if (Statistics.has(EPipelineStatistic_t::ClippingInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eClippingInvocations;
    if (Statistics.has(EPipelineStatistic_t::ClippingPrimitives))
        result |= vk::QueryPipelineStatisticFlagBits::eClippingPrimitives;
    if (Statistics.has(EPipelineStatistic_t::PixelShaderInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::HullShaderPatches))
        result |= vk::QueryPipelineStatisticFlagBits::eTessellationControlShaderPatches;
    if (Statistics.has(EPipelineStatistic_t::DomainShaderInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eTessellationEvaluationShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::ComputeShaderInvocations))
        result |= vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
    return result;
}

constexpr uint32_t PipelineStatisticMask = (1u << 11) - 1;
constexpr uint8_t QueryResultMask = (1u << 4) - 1;
}

VulkanFence::VulkanFence(
    VulkanDevice& InDevice,
    VulkanContextPtr InContext,
    bool Signaled)
    : Device(&InDevice)
    , Context(std::move(InContext))
    , Fence(Context->Device->createFenceUnique(vk::FenceCreateInfo(
        Signaled ? vk::FenceCreateFlagBits::eSignaled : vk::FenceCreateFlags{})))
{
}

RDevice& VulkanFence::getDevice() const noexcept { return *Device; }

bool VulkanFence::isSignaled() const
{
    return Context->Device->getFenceStatus(Fence.get()) == vk::Result::eSuccess;
}

bool VulkanFence::wait(uint64_t TimeoutNanoseconds)
{
    vk::Result result;
    try { result = Context->Device->waitForFences(Fence.get(), true, TimeoutNanoseconds); }
    catch (const vk::SystemError& error)
    {
        if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
        {
            Context->markDeviceLost();
            return false;
        }
        throw;
    }
    if (result == vk::Result::eTimeout)
        return false;
    if (result != vk::Result::eSuccess)
        throw std::runtime_error("Failed to wait for a Vulkan fence.");
    return true;
}

void VulkanFence::reset() { Context->Device->resetFences(Fence.get()); }
void* VulkanFence::getNativeHandle() const noexcept { return static_cast<VkFence>(Fence.get()); }

VulkanSemaphore::VulkanSemaphore(
    VulkanDevice& InDevice,
    VulkanContextPtr InContext,
    bool InTimeline,
    uint64_t InitialValue)
    : Device(&InDevice)
    , Context(std::move(InContext))
    , Timeline(InTimeline)
{
    vk::SemaphoreTypeCreateInfo type_info(
        Timeline ? vk::SemaphoreType::eTimeline : vk::SemaphoreType::eBinary,
        Timeline ? InitialValue : 0);
    vk::SemaphoreCreateInfo create_info;
    create_info.pNext = Timeline ? &type_info : nullptr;
    Semaphore = Context->Device->createSemaphoreUnique(create_info);
}

RDevice& VulkanSemaphore::getDevice() const noexcept { return *Device; }

uint64_t VulkanSemaphore::getCompletedValue() const
{
    if (!Timeline)
        throw std::logic_error("A binary semaphore has no counter value.");
    return Context->Device->getSemaphoreCounterValue(Semaphore.get());
}

void VulkanSemaphore::signal(uint64_t Value)
{
    if (!Timeline)
        throw std::logic_error("A binary semaphore cannot be signaled by the CPU.");
    Context->Device->signalSemaphore(vk::SemaphoreSignalInfo(Semaphore.get(), Value));
}

bool VulkanSemaphore::wait(uint64_t Value, uint64_t TimeoutNanoseconds)
{
    if (!Timeline)
        throw std::logic_error("A binary semaphore cannot be waited by the CPU.");
    vk::Semaphore semaphore = Semaphore.get();
    const vk::SemaphoreWaitInfo wait_info({}, 1, &semaphore, &Value);
    vk::Result result;
    try { result = Context->Device->waitSemaphores(wait_info, TimeoutNanoseconds); }
    catch (const vk::SystemError& error)
    {
        if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
        {
            Context->markDeviceLost();
            return false;
        }
        throw;
    }
    if (result == vk::Result::eTimeout)
        return false;
    if (result != vk::Result::eSuccess)
        throw std::runtime_error("Failed to wait for a Vulkan timeline semaphore.");
    return true;
}

void* VulkanSemaphore::getNativeHandle() const noexcept
{
    return static_cast<VkSemaphore>(Semaphore.get());
}

VulkanQueryPool::VulkanQueryPool(
    VulkanDevice& InDevice,
    VulkanContextPtr InContext,
    QueryPoolDescriptor Desc)
    : Device(&InDevice)
    , Context(std::move(InContext))
    , Descriptor(std::move(Desc))
{
    if (Descriptor.Count == 0)
        throw std::invalid_argument("A query pool must contain at least one query.");
    if ((Descriptor.PipelineStatistics.Value & ~PipelineStatisticMask) != 0 ||
        (Descriptor.Type == EQueryType::PipelineStatistics) != static_cast<bool>(Descriptor.PipelineStatistics))
        throw std::invalid_argument("Pipeline-statistics queries require a non-empty, known statistics mask only.");
    vk::QueryType type;
    switch (Descriptor.Type)
    {
    case EQueryType::Occlusion: type = vk::QueryType::eOcclusion; break;
    case EQueryType::PipelineStatistics: type = vk::QueryType::ePipelineStatistics; break;
    case EQueryType::Timestamp:
    default: type = vk::QueryType::eTimestamp; break;
    }
    // VkQueryPoolCreateInfo::pipelineStatistics defines ascending-bit result word order.
    Pool = Context->Device->createQueryPoolUnique(vk::QueryPoolCreateInfo(
        {}, type, Descriptor.Count, pipelineStatistics(Descriptor.PipelineStatistics)));
}

RDevice& VulkanQueryPool::getDevice() const noexcept { return *Device; }

bool VulkanQueryPool::getResults(
    uint32_t FirstQuery,
    std::span<uint64_t> Results,
    bool Wait) const
{
    if (Results.empty())
        return true;
    const uint32_t values_per_query = getResultValuesPerQuery();
    if (Results.size() % values_per_query != 0)
        throw std::invalid_argument("Legacy query readback requires complete query records.");
    EQueryResultFlags flags;
    flags.set(EQueryResultFlag_t::Result64);
    if (Wait) flags.set(EQueryResultFlag_t::Wait);
    return getResults(FirstQuery,
        static_cast<uint32_t>(Results.size() / values_per_query), Results, flags);
}

uint32_t VulkanQueryPool::getResultValuesPerQuery() const noexcept
{
    return Descriptor.Type == EQueryType::PipelineStatistics
        ? std::popcount(Descriptor.PipelineStatistics.Value)
        : 1u;
}

bool VulkanQueryPool::getResults(
    uint32_t FirstQuery,
    uint32_t QueryCount,
    std::span<uint64_t> Results,
    EQueryResultFlags Flags) const
{
    if ((Flags.Value & ~QueryResultMask) != 0)
        throw std::invalid_argument("Query readback contains unknown flags.");
    if (Descriptor.Type == EQueryType::Timestamp && Flags.has(EQueryResultFlag_t::Partial))
        throw std::invalid_argument("Vulkan timestamp queries do not support partial results.");
    if (QueryCount == 0)
    {
        if (!Results.empty()) throw std::invalid_argument("Zero queries require an empty destination.");
        return true;
    }
    if (FirstQuery >= Descriptor.Count || QueryCount > Descriptor.Count - FirstQuery)
        throw std::out_of_range("Query result range exceeds the query pool.");
    const uint32_t values = getResultValuesPerQuery();
    const bool requested_availability = Flags.has(EQueryResultFlag_t::WithAvailability);
    const size_t output_stride = values + (requested_availability ? 1u : 0u);
    if (Results.size() != static_cast<size_t>(QueryCount) * output_stride)
        throw std::invalid_argument("Query destination size does not match count and result stride.");

    // Always ask vkGetQueryPoolResults for availability internally so the bool contract is exact.
    const size_t native_stride = values + 1u;
    std::vector<uint64_t> temporary(static_cast<size_t>(QueryCount) * native_stride, 0);
    vk::QueryResultFlags vk_flags = vk::QueryResultFlagBits::e64 |
        vk::QueryResultFlagBits::eWithAvailability;
    if (Flags.has(EQueryResultFlag_t::Wait)) vk_flags |= vk::QueryResultFlagBits::eWait;
    if (Flags.has(EQueryResultFlag_t::Partial)) vk_flags |= vk::QueryResultFlagBits::ePartial;
    const vk::Result result = Context->Device->getQueryPoolResults(
        Pool.get(), FirstQuery, QueryCount, temporary.size() * sizeof(uint64_t), temporary.data(),
        native_stride * sizeof(uint64_t), vk_flags);
    if (result != vk::Result::eSuccess && result != vk::Result::eNotReady)
        throw std::runtime_error("Failed to retrieve Vulkan query results.");

    bool all_available = true;
    for (uint32_t query = 0; query < QueryCount; ++query)
    {
        const auto native = temporary.data() + static_cast<size_t>(query) * native_stride;
        auto output = Results.data() + static_cast<size_t>(query) * output_stride;
        const bool available = native[values] != 0;
        all_available &= available;
        if (available || Flags.has(EQueryResultFlag_t::Partial))
            std::copy_n(native, values, output);
        if (requested_availability)
            output[values] = native[values];
    }
    return all_available;
}

double VulkanQueryPool::timestampTicksToNanoseconds(uint64_t TickDelta) const noexcept
{
    if (Descriptor.Type != EQueryType::Timestamp || !Device->getFeatures().TimestampQueries)
        return 0.0;
    return rhi::timestampTicksToNanoseconds(
        TickDelta, Device->getLimits().TimestampPeriodNanoseconds);
}

void* VulkanQueryPool::getNativeHandle() const noexcept
{
    return static_cast<VkQueryPool>(Pool.get());
}

VulkanQueue::VulkanQueue(
    VulkanDevice& InDevice,
    VulkanContextPtr InContext,
    ECommandQueueType InType)
    : Device(&InDevice)
    , Context(std::move(InContext))
    , Type(InType)
    , Queue(Context->Device->getQueue(Context->familyIndex(Type), 0))
    , PresentQueue(Context->Device->getQueue(Context->PresentQueueFamilyIndex, 0))
{
}

VulkanQueue::~VulkanQueue()
{
    try { waitIdle(); } catch (...) {}
}

RDevice& VulkanQueue::getDevice() const noexcept { return *Device; }

void VulkanQueue::submit(const QueueSubmitDescriptor& Desc)
{
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    std::scoped_lock lock(Mutex);
    collectCompleted();
    if (Desc.CommandLists.empty())
        throw std::invalid_argument("A queue submission requires at least one command list.");

    PendingSubmission pending;
    pending.CommandLists.assign(Desc.CommandLists.begin(), Desc.CommandLists.end());
    pending.WaitSemaphores.assign(Desc.WaitSemaphores.begin(), Desc.WaitSemaphores.end());
    pending.SignalSemaphores.assign(Desc.SignalSemaphores.begin(), Desc.SignalSemaphores.end());
    pending.ExternalFence = Desc.Fence;

    std::vector<vk::CommandBufferSubmitInfo> command_infos;
    command_infos.reserve(pending.CommandLists.size());
    for (const auto& command : pending.CommandLists)
    {
        auto* vk_command = dynamic_cast<VulkanCommandList*>(command.get());
        if (!vk_command || &vk_command->getDevice() != Device)
            throw std::invalid_argument("Command list belongs to another RHI device or backend.");
        if (command->getQueueType() != Type || command->getState() != ECommandListState::Executable)
            throw std::logic_error("Submitted command list has the wrong queue type or state.");
        if (command->getLevel() != ECommandListLevel::Primary)
            throw std::logic_error("A secondary command list cannot be submitted directly.");
        command_infos.emplace_back(vk_command->getVkCommandBuffer(), 0);
    }

    auto make_semaphore_infos = [&](const std::vector<SemaphoreSubmitInfo>& infos)
    {
        std::vector<vk::SemaphoreSubmitInfo> result;
        result.reserve(infos.size());
        for (const auto& info : infos)
        {
            auto& semaphore = semaphoreOf(info.Semaphore, *Device);
            if (!semaphore.isTimeline() && info.Value != 0)
                throw std::invalid_argument("Binary semaphore submit values must be zero.");
            result.emplace_back(
                semaphore.getVkSemaphore(),
                semaphore.isTimeline() ? info.Value : 0,
                vk::PipelineStageFlagBits2::eAllCommands,
                0);
        }
        return result;
    };
    const auto wait_infos = make_semaphore_infos(pending.WaitSemaphores);
    const auto signal_infos = make_semaphore_infos(pending.SignalSemaphores);

    vk::Fence fence;
    if (pending.ExternalFence)
        fence = fenceOf(pending.ExternalFence, *Device).getVkFence();
    else
    {
        pending.InternalFence = Context->Device->createFenceUnique({});
        fence = pending.InternalFence.get();
    }

    const vk::SubmitInfo2 submit_info(
        {},
        static_cast<uint32_t>(wait_infos.size()), wait_infos.data(),
        static_cast<uint32_t>(command_infos.size()), command_infos.data(),
        static_cast<uint32_t>(signal_infos.size()), signal_infos.data());
    try { Queue.submit2(submit_info, fence); }
    catch (const vk::SystemError& error)
    {
        if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
            Context->markDeviceLost();
        throw;
    }
    for (const auto& command : pending.CommandLists)
        static_cast<VulkanCommandList*>(command.get())->markSubmitted();
    Pending.emplace_back(std::move(pending));
}

EPresentStatus VulkanQueue::present(const PresentDescriptor& Desc)
{
    if (Context->isDeviceLost()) return EPresentStatus::DeviceLost;
    std::scoped_lock lock(Mutex);
    collectCompleted();
    auto* swapchain = dynamic_cast<VulkanSwapchain*>(Desc.Swapchain.get());
    if (!swapchain || &swapchain->getDevice() != Device)
        throw std::invalid_argument("Swapchain belongs to another RHI device or backend.");
    if (Desc.Generation == 0 || Desc.Generation != swapchain->getGeneration())
        return EPresentStatus::OutOfDate;
    if (Desc.ImageIndex >= swapchain->getImageCount())
        throw std::out_of_range("Present image index exceeds the swapchain.");

    std::vector<vk::Semaphore> waits;
    waits.reserve(Desc.WaitSemaphores.size());
    for (const auto& semaphore : Desc.WaitSemaphores)
    {
        auto& native = semaphoreOf(semaphore, *Device);
        if (native.isTimeline())
            throw std::invalid_argument("Vulkan presentation requires binary wait semaphores.");
        waits.emplace_back(native.getVkSemaphore());
    }
    const vk::SwapchainKHR native_swapchain = swapchain->getVkSwapchain();
    const vk::PresentInfoKHR info(
        static_cast<uint32_t>(waits.size()), waits.data(),
        1, &native_swapchain, &Desc.ImageIndex);
    try
    {
        const vk::Result result = PresentQueue.presentKHR(info);
        const auto status = vulkan_wsi::mapPresent(result);
        if (!status) throw std::runtime_error("Unexpected Vulkan presentation result.");
        if (const auto health = vulkan_wsi::mapStatus(result)) swapchain->recordStatus(*health);
        return *status;
    }
    catch (const vk::SystemError& error)
    {
        const auto result = vulkan_wsi::resultFromSystemError(error);
        const auto status = vulkan_wsi::mapPresent(result);
        if (!status) throw;
        if (const auto health = vulkan_wsi::mapStatus(result)) swapchain->recordStatus(*health);
        return *status;
    }
}

void VulkanQueue::poll()
{
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    std::scoped_lock lock(Mutex);
    collectCompleted();
}

void VulkanQueue::waitIdle()
{
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    std::scoped_lock lock(Mutex);
    try
    {
        Queue.waitIdle();
        if (PresentQueue != Queue) PresentQueue.waitIdle();
    }
    catch (const vk::SystemError& error)
    {
        if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
            Context->markDeviceLost();
        throw;
    }
    for (auto& pending : Pending)
        complete(pending);
    Pending.clear();
}

void* VulkanQueue::getNativeHandle() const noexcept { return static_cast<VkQueue>(Queue); }

void VulkanQueue::collectCompleted()
{
    auto iterator = Pending.begin();
    while (iterator != Pending.end())
    {
        const vk::Fence fence = iterator->ExternalFence
            ? fenceOf(iterator->ExternalFence, *Device).getVkFence()
            : iterator->InternalFence.get();
        vk::Result status;
        try { status = Context->Device->getFenceStatus(fence); }
        catch (const vk::SystemError& error)
        {
            if (vulkan_wsi::resultFromSystemError(error) == vk::Result::eErrorDeviceLost)
                Context->markDeviceLost();
            throw;
        }
        if (status == vk::Result::eSuccess)
        {
            complete(*iterator);
            iterator = Pending.erase(iterator);
        }
        else
            ++iterator;
    }
}

void VulkanQueue::complete(PendingSubmission& Submission)
{
    for (const auto& command : Submission.CommandLists)
        static_cast<VulkanCommandList*>(command.get())->markComplete();
}

} // namespace rhi
