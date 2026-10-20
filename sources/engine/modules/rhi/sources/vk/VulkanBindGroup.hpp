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

/** @brief 由共享的 Vulkan 描述符池 arena 返回的描述符集分配。 */
struct VulkanDescriptorAllocation
{
    vk::DescriptorPool Pool;
    vk::DescriptorSet Set;
};

/**
 * @brief 线程安全、只增不减的描述符池 arena，支持按集合回收。
 *
 * 池由多个 BindGroup 共享，以避免每个分组各做一次原生池分配。
 * 已耗尽或碎片化的页面会被跳过，并以几何级数添加新页面。
 */
class VulkanDescriptorAllocator final
{
public:
    explicit VulkanDescriptorAllocator(VulkanDevice& Device) noexcept;
    ~VulkanDescriptorAllocator() = default;

    VulkanDescriptorAllocator(const VulkanDescriptorAllocator&) = delete;
    VulkanDescriptorAllocator& operator=(const VulkanDescriptorAllocator&) = delete;

    /**
     * @brief 分配一个描述符集。
     * @param Layout 原生描述符集布局。
     * @param Entries 规范化的 RHI 布局条目。
     * @param VariableArrayCount 可变描述符绑定的运行时数量，或为零。
     * @param UpdateAfterBind 该布局是否需要 update-after-bind 池。
     * @return BindGroup 使用的池与描述符集句柄。
     */
    [[nodiscard]] VulkanDescriptorAllocation allocate(
        vk::DescriptorSetLayout Layout,
        std::span<const BindGroupLayoutEntry> Entries,
        uint32_t VariableArrayCount,
        bool UpdateAfterBind);

    /** @brief 将一个描述符集归还给其所属的池。 */
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

/** @brief 不可变的 Vulkan 描述符集及其引用的资源。 */
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
