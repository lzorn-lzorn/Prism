#pragma once

#include "VulkanContext.hpp"
#include <RHI.hpp>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;
class VulkanCommandList;

class VulkanFence final : public RFence
{
public:
    VulkanFence(VulkanDevice& Device, VulkanContextPtr Context, bool Signaled);
    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] bool isSignaled() const override;
    [[nodiscard]] bool wait(uint64_t TimeoutNanoseconds) override;
    void reset() override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::Fence getVkFence() const noexcept { return Fence.get(); }

private:
    VulkanDevice* Device;
    VulkanContextPtr Context;
    vk::UniqueFence Fence;
};

class VulkanSemaphore final : public RSemaphore
{
public:
    VulkanSemaphore(VulkanDevice& Device, VulkanContextPtr Context, bool Timeline, uint64_t InitialValue);
    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] bool isTimeline() const noexcept override { return Timeline; }
    [[nodiscard]] uint64_t getCompletedValue() const override;
    void signal(uint64_t Value) override;
    [[nodiscard]] bool wait(uint64_t Value, uint64_t TimeoutNanoseconds) override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::Semaphore getVkSemaphore() const noexcept { return Semaphore.get(); }

private:
    VulkanDevice* Device;
    VulkanContextPtr Context;
    bool Timeline;
    vk::UniqueSemaphore Semaphore;
};

class VulkanQueryPool final : public RQueryPool
{
public:
    VulkanQueryPool(VulkanDevice& Device, VulkanContextPtr Context, QueryPoolDescriptor Desc);
    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] const QueryPoolDescriptor& getDescriptor() const noexcept override { return Descriptor; }
    [[nodiscard]] bool getResults(uint32_t FirstQuery, std::span<uint64_t> Results, bool Wait) const override;
    [[nodiscard]] bool getResults(uint32_t FirstQuery, uint32_t QueryCount,
        std::span<uint64_t> Results, EQueryResultFlags Flags) const override;
    [[nodiscard]] double timestampTicksToNanoseconds(uint64_t TickDelta) const noexcept override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::QueryPool getVkQueryPool() const noexcept { return Pool.get(); }
    [[nodiscard]] uint32_t getResultValuesPerQuery() const noexcept;

private:
    VulkanDevice* Device;
    VulkanContextPtr Context;
    QueryPoolDescriptor Descriptor;
    vk::UniqueQueryPool Pool;
};

class VulkanQueue final : public RQueue
{
public:
    VulkanQueue(VulkanDevice& Device, VulkanContextPtr Context, ECommandQueueType Type);
    ~VulkanQueue() override;
    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] ECommandQueueType getType() const noexcept override { return Type; }
    void submit(const QueueSubmitDescriptor& Desc) override;
    [[nodiscard]] EPresentStatus present(const PresentDescriptor& Desc) override;
    void poll() override;
    void waitIdle() override;
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::Queue getVkQueue() const noexcept { return Queue; }

private:
    struct PendingSubmission
    {
        vk::UniqueFence InternalFence;
        std::shared_ptr<RFence> ExternalFence;
        std::vector<std::shared_ptr<RCommandList>> CommandLists;
        std::vector<SemaphoreSubmitInfo> WaitSemaphores;
        std::vector<SemaphoreSubmitInfo> SignalSemaphores;
    };

    void collectCompleted();
    void complete(PendingSubmission& Submission);

    VulkanDevice* Device;
    VulkanContextPtr Context;
    ECommandQueueType Type;
    vk::Queue Queue;
    vk::Queue PresentQueue;
    std::mutex Mutex;
    std::vector<PendingSubmission> Pending;
};

} // namespace rhi
