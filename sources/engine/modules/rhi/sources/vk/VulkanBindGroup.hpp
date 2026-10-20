#pragma once

#include <RHI.hpp>
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;
class VulkanBindGroupLayout;

/** @brief Descriptor set allocation returned by the shared Vulkan pool arena. */
struct VulkanDescriptorAllocation
{
    vk::DescriptorPool Pool;
    vk::DescriptorSet Set;
};

/**
 * @brief Thread-safe, grow-only descriptor-pool arena with per-set reclamation.
 *
 * Pools are shared by many BindGroups to avoid one native pool allocation per group.
 * Exhausted or fragmented pages are skipped and new pages are added geometrically.
 */
class VulkanDescriptorAllocator final
{
public:
    explicit VulkanDescriptorAllocator(VulkanDevice& Device) noexcept;
    ~VulkanDescriptorAllocator() = default;

    VulkanDescriptorAllocator(const VulkanDescriptorAllocator&) = delete;
    VulkanDescriptorAllocator& operator=(const VulkanDescriptorAllocator&) = delete;

    /**
     * @brief Allocates one descriptor set.
     * @param Layout Native descriptor set layout.
     * @param Entries Normalized RHI layout entries.
     * @param VariableArrayCount Runtime count of the variable descriptor binding, or zero.
     * @param UpdateAfterBind Whether the layout requires an update-after-bind pool.
     * @return Pool and set handles used by the BindGroup.
     */
    [[nodiscard]] VulkanDescriptorAllocation allocate(
        vk::DescriptorSetLayout Layout,
        std::span<const BindGroupLayoutEntry> Entries,
        uint32_t VariableArrayCount,
        bool UpdateAfterBind);

    /** @brief Returns a descriptor set to its owning pool. */
    void release(VulkanDescriptorAllocation Allocation) noexcept;

private:
    struct PoolPage
    {
        vk::UniqueDescriptorPool Pool;
        bool UpdateAfterBind { false };
    };

    [[nodiscard]] vk::UniqueDescriptorPool createPool(
        std::span<const BindGroupLayoutEntry> Entries,
        uint32_t VariableArrayCount,
        bool UpdateAfterBind) const;
    [[nodiscard]] vk::DescriptorSet tryAllocate(
        vk::DescriptorPool Pool,
        vk::DescriptorSetLayout Layout,
        uint32_t VariableArrayCount) const;

    VulkanDevice* Device { nullptr };
    std::mutex Mutex;
    std::vector<PoolPage> Pools;
};

/** @brief Immutable Vulkan descriptor set and the resources referenced by it. */
class VulkanBindGroup final : public RBindGroup
{
public:
    VulkanBindGroup(
        VulkanDevice& Device,
        std::shared_ptr<VulkanDescriptorAllocator> Allocator,
        const BindGroupDescriptor& Desc);
    ~VulkanBindGroup() override;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] const std::shared_ptr<RBindGroupLayout>& getLayout() const noexcept override
    {
        return Layout;
    }
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(Allocation.Set); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::DescriptorSet getVkDescriptorSet() const noexcept { return Allocation.Set; }
    [[nodiscard]] std::span<const BindGroupEntry> getEntries() const noexcept { return Entries; }
    [[nodiscard]] uint32_t getDescriptorCount(const BindGroupLayoutEntry& Entry) const noexcept
    {
        return Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? VariableArrayCount
            : Entry.ArrayCount;
    }

private:
    VulkanDevice* Device { nullptr };
    std::shared_ptr<VulkanDescriptorAllocator> Allocator;
    std::shared_ptr<RBindGroupLayout> Layout;
    std::vector<BindGroupEntry> Entries;
    std::vector<vk::UniqueBufferView> TexelBufferViews;
    VulkanDescriptorAllocation Allocation;
    uint32_t VariableArrayCount { 0 };
    std::string DebugName;
};

} // namespace rhi
