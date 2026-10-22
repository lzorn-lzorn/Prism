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
    auto* Result = dynamic_cast<VulkanSemaphore*>(Semaphore.get());
    if (!Result || &Result->getDevice() != &Device)
        throw std::invalid_argument("Semaphore belongs to another RHI device or backend.");
    return *Result;
}

VulkanFence& fenceOf(const std::shared_ptr<RFence>& Fence, VulkanDevice& Device)
{
    auto* Result = dynamic_cast<VulkanFence*>(Fence.get());
    if (!Result || &Result->getDevice() != &Device)
        throw std::invalid_argument("Fence belongs to another RHI device or backend.");
    return *Result;
}

vk::QueryPipelineStatisticFlags pipelineStatistics(EPipelineStatistics Statistics)
{
    vk::QueryPipelineStatisticFlags Result;
    if (Statistics.has(EPipelineStatistic_t::InputAssemblyVertices))
        Result |= vk::QueryPipelineStatisticFlagBits::eInputAssemblyVertices;
    if (Statistics.has(EPipelineStatistic_t::InputAssemblyPrimitives))
        Result |= vk::QueryPipelineStatisticFlagBits::eInputAssemblyPrimitives;
    if (Statistics.has(EPipelineStatistic_t::VertexShaderInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eVertexShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::GeometryShaderInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eGeometryShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::GeometryShaderPrimitives))
        Result |= vk::QueryPipelineStatisticFlagBits::eGeometryShaderPrimitives;
    if (Statistics.has(EPipelineStatistic_t::ClippingInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eClippingInvocations;
    if (Statistics.has(EPipelineStatistic_t::ClippingPrimitives))
        Result |= vk::QueryPipelineStatisticFlagBits::eClippingPrimitives;
    if (Statistics.has(EPipelineStatistic_t::PixelShaderInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eFragmentShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::HullShaderPatches))
        Result |= vk::QueryPipelineStatisticFlagBits::eTessellationControlShaderPatches;
    if (Statistics.has(EPipelineStatistic_t::DomainShaderInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eTessellationEvaluationShaderInvocations;
    if (Statistics.has(EPipelineStatistic_t::ComputeShaderInvocations))
        Result |= vk::QueryPipelineStatisticFlagBits::eComputeShaderInvocations;
    return Result;
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
    vk::Result Result;
    try { Result = Context->Device->waitForFences(Fence.get(), true, TimeoutNanoseconds); }
    catch (const vk::SystemError& Error)
    {
        if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
        {
            Context->markDeviceLost();
            return false;
        }
        throw;
    }
    if (Result == vk::Result::eTimeout)
        return false;
    if (Result != vk::Result::eSuccess)
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
    vk::SemaphoreTypeCreateInfo TypeInfo(
        Timeline ? vk::SemaphoreType::eTimeline : vk::SemaphoreType::eBinary,
        Timeline ? InitialValue : 0);
    vk::SemaphoreCreateInfo CreateInfo;
    CreateInfo.pNext = Timeline ? &TypeInfo : nullptr;
    Semaphore = Context->Device->createSemaphoreUnique(CreateInfo);
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
    vk::Semaphore SemaphoreHandle = Semaphore.get();
    const vk::SemaphoreWaitInfo WaitInfo({}, 1, &SemaphoreHandle, &Value);
    vk::Result Result;
    try { Result = Context->Device->waitSemaphores(WaitInfo, TimeoutNanoseconds); }
    catch (const vk::SystemError& Error)
    {
        if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
        {
            Context->markDeviceLost();
            return false;
        }
        throw;
    }
    if (Result == vk::Result::eTimeout)
        return false;
    if (Result != vk::Result::eSuccess)
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
    vk::QueryType Type;
    switch (Descriptor.Type)
    {
    case EQueryType::Occlusion: Type = vk::QueryType::eOcclusion; break;
    case EQueryType::PipelineStatistics: Type = vk::QueryType::ePipelineStatistics; break;
    case EQueryType::Timestamp:
    default: Type = vk::QueryType::eTimestamp; break;
    }
    // VkQueryPoolCreateInfo::pipelineStatistics defines ascending-bit result word order.
    Pool = Context->Device->createQueryPoolUnique(vk::QueryPoolCreateInfo(
        {}, Type, Descriptor.Count, pipelineStatistics(Descriptor.PipelineStatistics)));
}

RDevice& VulkanQueryPool::getDevice() const noexcept { return *Device; }

bool VulkanQueryPool::getResults(
    uint32_t FirstQuery,
    std::span<uint64_t> Results,
    bool Wait) const
{
    if (Results.empty())
        return true;
    const uint32_t ValuesPerQuery = getResultValuesPerQuery();
    if (Results.size() % ValuesPerQuery != 0)
        throw std::invalid_argument("Legacy query readback requires complete query records.");
    EQueryResultFlags Flags;
    Flags.set(EQueryResultFlag_t::Result64);
    if (Wait) Flags.set(EQueryResultFlag_t::Wait);
    return getResults(FirstQuery,
        static_cast<uint32_t>(Results.size() / ValuesPerQuery), Results, Flags);
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
    const uint32_t Values = getResultValuesPerQuery();
    const bool RequestedAvailability = Flags.has(EQueryResultFlag_t::WithAvailability);
    const size_t OutputStride = Values + (RequestedAvailability ? 1u : 0u);
    if (Results.size() != static_cast<size_t>(QueryCount) * OutputStride)
        throw std::invalid_argument("Query destination size does not match count and result stride.");

    // Always ask vkGetQueryPoolResults for availability internally so the bool contract is exact.
    const size_t NativeStride = Values + 1u;
    std::vector<uint64_t> Temporary(static_cast<size_t>(QueryCount) * NativeStride, 0);
    vk::QueryResultFlags VkFlags = vk::QueryResultFlagBits::e64 |
        vk::QueryResultFlagBits::eWithAvailability;
    if (Flags.has(EQueryResultFlag_t::Wait)) VkFlags |= vk::QueryResultFlagBits::eWait;
    if (Flags.has(EQueryResultFlag_t::Partial)) VkFlags |= vk::QueryResultFlagBits::ePartial;
    const vk::Result Result = Context->Device->getQueryPoolResults(
        Pool.get(), FirstQuery, QueryCount, Temporary.size() * sizeof(uint64_t), Temporary.data(),
        NativeStride * sizeof(uint64_t), VkFlags);
    if (Result != vk::Result::eSuccess && Result != vk::Result::eNotReady)
        throw std::runtime_error("Failed to retrieve Vulkan query results.");

    bool AllAvailable = true;
    for (uint32_t Query = 0; Query < QueryCount; ++Query)
    {
        const auto Native = Temporary.data() + static_cast<size_t>(Query) * NativeStride;
        auto Output = Results.data() + static_cast<size_t>(Query) * OutputStride;
        const bool Available = Native[Values] != 0;
        AllAvailable &= Available;
        if (Available || Flags.has(EQueryResultFlag_t::Partial))
            std::copy_n(Native, Values, Output);
        if (RequestedAvailability)
            Output[Values] = Native[Values];
    }
    return AllAvailable;
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
    std::scoped_lock Lock(Mutex);
    collectCompleted();
    if (Desc.CommandLists.empty())
        throw std::invalid_argument("A queue submission requires at least one command list.");

    PendingSubmission Submission;
    Submission.CommandLists.assign(Desc.CommandLists.begin(), Desc.CommandLists.end());
    Submission.WaitSemaphores.assign(Desc.WaitSemaphores.begin(), Desc.WaitSemaphores.end());
    Submission.SignalSemaphores.assign(Desc.SignalSemaphores.begin(), Desc.SignalSemaphores.end());
    Submission.ExternalFence = Desc.Fence;

    std::vector<vk::CommandBufferSubmitInfo> CommandInfos;
    std::vector<vk::CommandBuffer> CommandBuffers;
    CommandInfos.reserve(Submission.CommandLists.size());
    CommandBuffers.reserve(Submission.CommandLists.size());
    for (const auto& Command : Submission.CommandLists)
    {
        auto* VkCommand = dynamic_cast<VulkanCommandList*>(Command.get());
        if (!VkCommand || &VkCommand->getDevice() != Device)
            throw std::invalid_argument("Command list belongs to another RHI device or backend.");
        if (Command->getQueueType() != Type || Command->getState() != ECommandListState::Executable)
            throw std::logic_error("Submitted command list has the wrong queue type or state.");
        if (Command->getLevel() != ECommandListLevel::Primary)
            throw std::logic_error("A secondary command list cannot be submitted directly.");
        CommandInfos.emplace_back(VkCommand->getVkCommandBuffer(), 0);
        CommandBuffers.emplace_back(VkCommand->getVkCommandBuffer());
    }

    // synchronization2 不可用时回滚到 1.0 的 vkQueueSubmit(二元/时间线信号量并存).
    const bool UseSynchronization2 = Device->getFeatures().Synchronization2;
    std::vector<vk::SemaphoreSubmitInfo> WaitInfos;
    std::vector<vk::SemaphoreSubmitInfo> SignalInfos;
    std::vector<vk::Semaphore> WaitSemaphores;
    std::vector<vk::PipelineStageFlags> WaitStages;
    std::vector<uint64_t> WaitValues;
    std::vector<vk::Semaphore> SignalSemaphores;
    std::vector<uint64_t> SignalValues;
    WaitInfos.reserve(Submission.WaitSemaphores.size());
    SignalInfos.reserve(Submission.SignalSemaphores.size());

    for (const auto& Info : Submission.WaitSemaphores)
    {
        auto& Semaphore = semaphoreOf(Info.Semaphore, *Device);
        if (!Semaphore.isTimeline() && Info.Value != 0)
            throw std::invalid_argument("Binary semaphore submit values must be zero.");
        if (UseSynchronization2)
        {
            WaitInfos.emplace_back(
                Semaphore.getVkSemaphore(),
                Semaphore.isTimeline() ? Info.Value : 0,
                vk::PipelineStageFlagBits2::eAllCommands,
                0);
        }
        else
        {
            WaitSemaphores.emplace_back(Semaphore.getVkSemaphore());
            // 时间线信号量忽略等待阶段; 二元信号量在颜色输出阶段等待交换链图像.
            WaitStages.emplace_back(vk::PipelineStageFlagBits::eColorAttachmentOutput);
            if (Semaphore.isTimeline())
                WaitValues.emplace_back(Info.Value);
        }
    }
    for (const auto& Info : Submission.SignalSemaphores)
    {
        auto& Semaphore = semaphoreOf(Info.Semaphore, *Device);
        if (!Semaphore.isTimeline() && Info.Value != 0)
            throw std::invalid_argument("Binary semaphore submit values must be zero.");
        if (UseSynchronization2)
        {
            SignalInfos.emplace_back(
                Semaphore.getVkSemaphore(),
                Semaphore.isTimeline() ? Info.Value : 0,
                vk::PipelineStageFlagBits2::eAllCommands,
                0);
        }
        else
        {
            SignalSemaphores.emplace_back(Semaphore.getVkSemaphore());
            if (Semaphore.isTimeline())
                SignalValues.emplace_back(Info.Value);
        }
    }

    vk::Fence Fence;
    if (Submission.ExternalFence)
        Fence = fenceOf(Submission.ExternalFence, *Device).getVkFence();
    else
    {
        Submission.InternalFence = Context->Device->createFenceUnique({});
        Fence = Submission.InternalFence.get();
    }

    try
    {
        if (UseSynchronization2)
        {
            const vk::SubmitInfo2 SubmitInfo(
                {},
                static_cast<uint32_t>(WaitInfos.size()), WaitInfos.data(),
                static_cast<uint32_t>(CommandInfos.size()), CommandInfos.data(),
                static_cast<uint32_t>(SignalInfos.size()), SignalInfos.data());
            Queue.submit2(SubmitInfo, Fence);
        }
        else
        {
            const bool HasTimeline = !WaitValues.empty() || !SignalValues.empty();
            vk::TimelineSemaphoreSubmitInfo TimelineInfo(
                static_cast<uint32_t>(WaitValues.size()),
                WaitValues.empty() ? nullptr : WaitValues.data(),
                static_cast<uint32_t>(SignalValues.size()),
                SignalValues.empty() ? nullptr : SignalValues.data());
            const vk::SubmitInfo SubmitInfo(
                static_cast<uint32_t>(WaitSemaphores.size()),
                WaitSemaphores.empty() ? nullptr : WaitSemaphores.data(),
                WaitStages.empty() ? nullptr : WaitStages.data(),
                static_cast<uint32_t>(CommandBuffers.size()),
                CommandBuffers.empty() ? nullptr : CommandBuffers.data(),
                static_cast<uint32_t>(SignalSemaphores.size()),
                SignalSemaphores.empty() ? nullptr : SignalSemaphores.data(),
                HasTimeline ? static_cast<const void*>(&TimelineInfo) : nullptr);
            Queue.submit(SubmitInfo, Fence);
        }
    }
    catch (const vk::SystemError& Error)
    {
        if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
            Context->markDeviceLost();
        throw;
    }
    for (const auto& Command : Submission.CommandLists)
        static_cast<VulkanCommandList*>(Command.get())->markSubmitted();
    Pending.emplace_back(std::move(Submission));
}

EPresentStatus VulkanQueue::present(const PresentDescriptor& Desc)
{
    if (Context->isDeviceLost()) return EPresentStatus::DeviceLost;
    std::scoped_lock Lock(Mutex);
    collectCompleted();
    auto* Swapchain = dynamic_cast<VulkanSwapchain*>(Desc.Swapchain.get());
    if (!Swapchain || &Swapchain->getDevice() != Device)
        throw std::invalid_argument("Swapchain belongs to another RHI device or backend.");
    if (Desc.Generation == 0 || Desc.Generation != Swapchain->getGeneration())
        return EPresentStatus::OutOfDate;
    if (Desc.ImageIndex >= Swapchain->getImageCount())
        throw std::out_of_range("Present image index exceeds the swapchain.");

    std::vector<vk::Semaphore> Waits;
    Waits.reserve(Desc.WaitSemaphores.size());
    for (const auto& Semaphore : Desc.WaitSemaphores)
    {
        auto& Native = semaphoreOf(Semaphore, *Device);
        if (Native.isTimeline())
            throw std::invalid_argument("Vulkan presentation requires binary wait semaphores.");
        Waits.emplace_back(Native.getVkSemaphore());
    }
    const vk::SwapchainKHR NativeSwapchain = Swapchain->getVkSwapchain();
    const vk::PresentInfoKHR Info(
        static_cast<uint32_t>(Waits.size()), Waits.data(),
        1, &NativeSwapchain, &Desc.ImageIndex);
    try
    {
        const vk::Result Result = PresentQueue.presentKHR(Info);
        const auto Status = vulkan_wsi::mapPresent(Result);
        if (!Status) throw std::runtime_error("Unexpected Vulkan presentation result.");
        if (const auto Health = vulkan_wsi::mapStatus(Result)) Swapchain->recordStatus(*Health);
        return *Status;
    }
    catch (const vk::SystemError& Error)
    {
        const auto Result = vulkan_wsi::resultFromSystemError(Error);
        const auto Status = vulkan_wsi::mapPresent(Result);
        if (!Status) throw;
        if (const auto Health = vulkan_wsi::mapStatus(Result)) Swapchain->recordStatus(*Health);
        return *Status;
    }
}

void VulkanQueue::poll()
{
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    std::scoped_lock Lock(Mutex);
    collectCompleted();
}

void VulkanQueue::waitIdle()
{
    if (Context->isDeviceLost())
        throw std::runtime_error("Vulkan device is lost; rebuild the RHI/device and all resources.");
    std::scoped_lock Lock(Mutex);
    try
    {
        Queue.waitIdle();
        if (PresentQueue != Queue) PresentQueue.waitIdle();
    }
    catch (const vk::SystemError& Error)
    {
        if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
            Context->markDeviceLost();
        throw;
    }
    for (auto& Pending : Pending)
        complete(Pending);
    Pending.clear();
}

void* VulkanQueue::getNativeHandle() const noexcept { return static_cast<VkQueue>(Queue); }

void VulkanQueue::collectCompleted()
{
    auto Iterator = Pending.begin();
    while (Iterator != Pending.end())
    {
        const vk::Fence Fence = Iterator->ExternalFence
            ? fenceOf(Iterator->ExternalFence, *Device).getVkFence()
            : Iterator->InternalFence.get();
        vk::Result Status;
        try { Status = Context->Device->getFenceStatus(Fence); }
        catch (const vk::SystemError& Error)
        {
            if (vulkan_wsi::resultFromSystemError(Error) == vk::Result::eErrorDeviceLost)
                Context->markDeviceLost();
            throw;
        }
        if (Status == vk::Result::eSuccess)
        {
            complete(*Iterator);
            Iterator = Pending.erase(Iterator);
        }
        else
            ++Iterator;
    }
}

void VulkanQueue::complete(PendingSubmission& Submission)
{
    for (const auto& Command : Submission.CommandLists)
        static_cast<VulkanCommandList*>(Command.get())->markComplete();
}

} // namespace rhi
