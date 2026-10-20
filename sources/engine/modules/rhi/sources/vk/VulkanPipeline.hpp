#pragma once

#include <RHI.hpp>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** Vulkan SPIR-V Shader Module. 字节码与入口点在创建后不可变.  */
class VulkanShader final : public RShader
{
public:
    VulkanShader(VulkanDevice& Device, const ShaderDescriptor& Desc);
    ~VulkanShader() override = default;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] EShaderStage_t getStage() const noexcept override { return Stage; }
    [[nodiscard]] uint64_t getContentHash() const noexcept override { return ContentHash; }
    [[nodiscard]] const std::string& getEntryPoint() const noexcept override { return EntryPoint; }
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(ShaderModule); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::ShaderModule getVkShaderModule() const noexcept { return ShaderModule.get(); }

private:
    VulkanDevice* Device { nullptr };
    EShaderStage_t Stage { EShaderStage_t::Vertex };
    uint64_t ContentHash { 0 };
    std::string EntryPoint;
    std::string DebugName;
    vk::UniqueShaderModule ShaderModule;
};

class VulkanBindGroupLayout final : public RBindGroupLayout
{
public:
    VulkanBindGroupLayout(VulkanDevice& Device, const BindGroupLayoutDescriptor& Desc);
    ~VulkanBindGroupLayout() override = default;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] uint64_t getCompatibilityHash() const noexcept override { return CompatibilityHash; }
    [[nodiscard]] std::span<const std::byte> getCompatibilityKey() const noexcept override
    {
        return CompatibilityKey;
    }
    [[nodiscard]] std::span<const BindGroupLayoutEntry> getEntries() const noexcept override
    {
        return Entries;
    }
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(DescriptorSetLayout); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::DescriptorSetLayout getVkDescriptorSetLayout() const noexcept
    {
        return DescriptorSetLayout.get();
    }

private:
    VulkanDevice* Device { nullptr };
    uint64_t CompatibilityHash { 0 };
    std::vector<std::byte> CompatibilityKey;
    std::vector<BindGroupLayoutEntry> Entries;
    std::string DebugName;
    vk::UniqueDescriptorSetLayout DescriptorSetLayout;
};

class VulkanPipelineLayout final : public RPipelineLayout
{
public:
    VulkanPipelineLayout(VulkanDevice& Device, const PipelineLayoutDescriptor& Desc);
    ~VulkanPipelineLayout() override = default;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] uint64_t getCompatibilityHash() const noexcept override { return CompatibilityHash; }
    [[nodiscard]] std::span<const std::byte> getCompatibilityKey() const noexcept override
    {
        return CompatibilityKey;
    }
    [[nodiscard]] uint32_t getBindGroupLayoutCount() const noexcept override
    {
        return static_cast<uint32_t>(BindGroupLayouts.size());
    }
    [[nodiscard]] const std::shared_ptr<RBindGroupLayout>& getBindGroupLayout(
        uint32_t GroupIndex) const override;
    [[nodiscard]] bool supportsPushConstants(
        EShaderStage Stages,
        uint32_t Offset,
        uint32_t Size) const noexcept override;
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(PipelineLayout); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::PipelineLayout getVkPipelineLayout() const noexcept { return PipelineLayout.get(); }

private:
    VulkanDevice* Device { nullptr };
    uint64_t CompatibilityHash { 0 };
    std::vector<std::byte> CompatibilityKey;
    std::string DebugName;
    std::vector<std::shared_ptr<RBindGroupLayout>> BindGroupLayouts;
    std::vector<PushConstantRange> PushConstantRanges;
    vk::UniquePipelineLayout PipelineLayout;
};

/** VkPipelineCache 需要外部同步，因此 merge/serialize 在对象内部串行化.  */
class VulkanPipelineCache final : public RPipelineCache
{
public:
    VulkanPipelineCache(VulkanDevice& Device, const PipelineCacheDescriptor& Desc);
    ~VulkanPipelineCache() override = default;

    void merge(std::span<const std::shared_ptr<RPipelineCache>> Sources) override;
    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] std::vector<std::byte> serialize() const override;
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(PipelineCache); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::PipelineCache getVkPipelineCache() const noexcept { return PipelineCache.get(); }
    [[nodiscard]] std::mutex& getMutex() const noexcept { return Mutex; }

private:
    VulkanDevice* Device { nullptr };
    std::string DebugName;
    mutable std::mutex Mutex;
    vk::UniquePipelineCache PipelineCache;
};

class VulkanPipeline final : public RPipeline
{
public:
    VulkanPipeline(
        VulkanDevice& Device,
        EPipelineType Type,
        std::shared_ptr<RPipelineLayout> Layout,
        RenderingSignature Rendering,
        EDynamicStates DynamicStates,
        EPrimitiveTopology PrimitiveTopology,
        bool UsesMeshShaders,
        uint64_t CacheKey,
        std::string DebugName,
        vk::UniquePipeline Pipeline,
        uint32_t RayTracingGroupCount = 0);
    ~VulkanPipeline() override = default;

    [[nodiscard]] RDevice& getDevice() const noexcept override;
    [[nodiscard]] EPipelineType getType() const noexcept override { return Type; }
    [[nodiscard]] const std::shared_ptr<RPipelineLayout>& getLayout() const noexcept override
    {
        return Layout;
    }
    [[nodiscard]] const RenderingSignature* getRenderingSignature() const noexcept override
    {
        return Type == EPipelineType::Graphics ? &Rendering : nullptr;
    }
    [[nodiscard]] EDynamicStates getDynamicStates() const noexcept override { return DynamicStates; }
    [[nodiscard]] bool usesMeshShaders() const noexcept override { return UsesMeshShaders; }
    [[nodiscard]] uint64_t getCacheKey() const noexcept override { return CacheKey; }
    [[nodiscard]] const std::string& getDebugName() const noexcept override { return DebugName; }
    [[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(Pipeline); }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::Pipeline getVkPipeline() const noexcept { return Pipeline.get(); }
    [[nodiscard]] EPrimitiveTopology getPrimitiveTopology() const noexcept { return PrimitiveTopology; }
    [[nodiscard]] uint32_t getRayTracingGroupCount() const noexcept { return RayTracingGroupCount; }

private:
    VulkanDevice* Device { nullptr };
    EPipelineType Type { EPipelineType::None };
    std::shared_ptr<RPipelineLayout> Layout;
    RenderingSignature Rendering {};
    EDynamicStates DynamicStates {};
    EPrimitiveTopology PrimitiveTopology { EPrimitiveTopology::TriangleList };
    bool UsesMeshShaders { false };
    uint64_t CacheKey { 0 };
    std::string DebugName;
    vk::UniquePipeline Pipeline;
    uint32_t RayTracingGroupCount { 0 };
};

[[nodiscard]] std::shared_ptr<RPipeline> createVulkanGraphicsPipeline(
    VulkanDevice& Device,
    const GraphicsPipelineDescriptor& Desc);
[[nodiscard]] std::shared_ptr<RPipeline> createVulkanComputePipeline(
    VulkanDevice& Device,
    const ComputePipelineDescriptor& Desc);
[[nodiscard]] std::shared_ptr<RPipeline> createVulkanRayTracingPipeline(
    VulkanDevice& Device,
    const RayTracingPipelineDescriptor& Desc);

} // namespace rhi
