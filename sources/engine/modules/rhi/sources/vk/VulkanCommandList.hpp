#pragma once

#include <RHI.hpp>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;
class VulkanQueue;

class VulkanCommandList final : public RCommandList
{
public:
    VulkanCommandList(
        VulkanDevice& Device,
        uint32_t QueueFamilyIndex,
        const CommandListDescriptor& Desc);
    ~VulkanCommandList() override = default;
    [[nodiscard]] RDevice& getDevice() const noexcept override;

    [[nodiscard]] ECommandQueueType getQueueType() const noexcept override
    {
        return Descriptor.QueueType;
    }
    [[nodiscard]] ECommandListLevel getLevel() const noexcept override
    {
        return Descriptor.Level;
    }
    [[nodiscard]] ECommandListState getState() const noexcept override { return State; }
    [[nodiscard]] void* getNativeHandle() const noexcept override;
    [[nodiscard]] vk::CommandBuffer getVkCommandBuffer() const noexcept
    {
        return CommandBuffer.get();
    }

    void begin() override;
    void end() override;
    void reset() override;

    void barriers(
        std::span<const GlobalBarrier> GlobalBarriers,
        std::span<const BufferBarrier> BufferBarriers,
        std::span<const ImageBarrier> ImageBarriers) override;
    void beginRendering(const RenderingInfo& Info) override;
    void endRendering() override;

    void setViewports(std::span<const Viewport> Viewports) override;
    void setScissors(std::span<const RenderArea> Scissors) override;
    void setBlendConstants(const std::array<float, 4>& Constants) override;
    void setStencilReference(uint32_t FrontReference, uint32_t BackReference) override;
    void setDepthBias(float ConstantFactor, float Clamp, float SlopeFactor) override;
    void setLineWidth(float Width) override;
    [[nodiscard]] bool setCullMode(ECullMode Mode) override;
    [[nodiscard]] bool setFrontFace(EFrontFace Face) override;
    [[nodiscard]] bool setPrimitiveTopology(EPrimitiveTopology Topology) override;
    [[nodiscard]] bool setDepthTestEnable(bool Enable) override;
    [[nodiscard]] bool setDepthWriteEnable(bool Enable) override;
    [[nodiscard]] bool setDepthCompareOp(ECompareOp Operation) override;
    [[nodiscard]] bool setStencilTestEnable(bool Enable) override;
    [[nodiscard]] bool setStencilOperations(const StencilFaceState& Front, const StencilFaceState& Back) override;
    [[nodiscard]] bool setStencilCompareMask(uint32_t FrontMask, uint32_t BackMask) override;
    [[nodiscard]] bool setStencilWriteMask(uint32_t FrontMask, uint32_t BackMask) override;
    [[nodiscard]] bool setVertexInput(const VertexInputState& State) override;
    void pushConstants(
        const std::shared_ptr<RPipelineLayout>& Layout,
        EShaderStage Stages,
        uint32_t Offset,
        std::span<const std::byte> Data) override;
    void bindPipeline(const std::shared_ptr<RPipeline>& Pipeline) override;
    void bindBindGroups(
        EPipelineType PipelineType,
        const std::shared_ptr<RPipelineLayout>& Layout,
        uint32_t FirstGroup,
        std::span<const std::shared_ptr<RBindGroup>> Groups,
        std::span<const uint32_t> DynamicOffsets = {}) override;
    void bindVertexBuffers(uint32_t FirstBinding, std::span<const VertexBufferBinding> Bindings) override;
    void bindIndexBuffer(const std::shared_ptr<RBuffer>& Buffer, DeviceSizeType Offset, EIndexFormat Format) override;
    void copyBuffer(const std::shared_ptr<RBuffer>& Source, const std::shared_ptr<RBuffer>& Destination, std::span<const BufferCopyRegion> Regions) override;
    void copyBufferToImage(const std::shared_ptr<RBuffer>& Source, const std::shared_ptr<RImage>& Destination, std::span<const BufferImageCopyRegion> Regions) override;
    void copyImageToBuffer(const std::shared_ptr<RImage>& Source, const std::shared_ptr<RBuffer>& Destination, std::span<const BufferImageCopyRegion> Regions) override;
    void copyImage(const std::shared_ptr<RImage>& Source, const std::shared_ptr<RImage>& Destination, std::span<const ImageCopyRegion> Regions) override;
    void blitImage(const std::shared_ptr<RImage>& Source, const std::shared_ptr<RImage>& Destination, std::span<const ImageBlitRegion> Regions, EFilterMode Filter = EFilterMode::Linear) override;
    void fillBuffer(const std::shared_ptr<RBuffer>& Buffer, DeviceSizeType Offset, DeviceSizeType Size, uint32_t Value) override;

    void draw(
        uint32_t VertexCount,
        uint32_t InstanceCount,
        uint32_t FirstVertex,
        uint32_t FirstInstance) override;
    void drawIndexed(
        uint32_t IndexCount,
        uint32_t InstanceCount,
        uint32_t FirstIndex,
        int32_t VertexOffset,
        uint32_t FirstInstance) override;
    void drawMeshTasks(
        uint32_t GroupCountX,
        uint32_t GroupCountY,
        uint32_t GroupCountZ) override;
    void drawIndirect(const std::shared_ptr<RBuffer>& Buffer, DeviceSizeType Offset, uint32_t DrawCount, uint32_t Stride) override;
    void drawIndexedIndirect(const std::shared_ptr<RBuffer>& Buffer, DeviceSizeType Offset, uint32_t DrawCount, uint32_t Stride) override;
    void dispatchIndirect(const std::shared_ptr<RBuffer>& Buffer, DeviceSizeType Offset) override;
    void resetQueries(const std::shared_ptr<RQueryPool>& Pool, uint32_t First, uint32_t Count) override;
    void beginQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) override;
    void endQuery(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) override;
    void writeTimestamp(const std::shared_ptr<RQueryPool>& Pool, uint32_t Query) override;
    [[nodiscard]] bool copyQueryResults(const std::shared_ptr<RQueryPool>& Pool, uint32_t First, uint32_t Count,
        const std::shared_ptr<RBuffer>& Destination, DeviceSizeType Offset, DeviceSizeType Stride,
        EQueryResultFlags Flags) override;
    [[nodiscard]] bool buildAccelerationStructures(
        std::span<const AccelerationStructureBuildDescriptor> Builds) override;
    [[nodiscard]] bool traceRays(const TraceRaysDescriptor& Desc) override;
    void beginDebugLabel(std::string_view Name, const std::array<float, 4>& Color = {}) override;
    void endDebugLabel() override;
    void insertDebugLabel(std::string_view Name, const std::array<float, 4>& Color = {}) override;
    void executeSecondary(std::span<const std::shared_ptr<RCommandList>> CommandLists) override;

    void markSubmitted();
    void markComplete();
    void dispatch(
        uint32_t GroupCountX,
        uint32_t GroupCountY,
        uint32_t GroupCountZ) override;

private:
    void requireRecording(const char* Operation) const;
    void requireInsideRendering(const char* Operation) const;
    void requireOutsideRendering(const char* Operation) const;
    void validateRenderingInfo(const RenderingInfo& Info) const;
    void validateRenderingCompatibility(const RPipeline& Pipeline) const;
    void validateGraphicsPipeline() const;
    void validateDynamicStates(const RPipeline& Pipeline) const;
    void retainRenderingResources(const RenderingInfo& Info);

    VulkanDevice* Device { nullptr };
    CommandListDescriptor Descriptor;
    ECommandListState State { ECommandListState::Initial };
    bool InsideRendering { false };
    bool InheritedRendering { false };
    bool RenderingUsesSecondaryCommandBuffers { false };
    std::shared_ptr<RPipeline> BoundGraphicsPipeline;
    std::shared_ptr<RPipeline> BoundComputePipeline;
    std::shared_ptr<RPipeline> BoundRayTracingPipeline;
    RenderingSignature ActiveRenderingSignature {};
    EDynamicStates InitializedDynamicStates {};
    std::optional<EPrimitiveTopology> DynamicPrimitiveTopology;
    std::shared_ptr<RQueryPool> ActiveQueryPool;
    uint32_t ActiveQuery { 0 };
    std::vector<std::shared_ptr<void>> RetainedResources;
    std::vector<std::shared_ptr<VulkanCommandList>> ExecutedSecondaries;
    vk::UniqueCommandPool CommandPool;
    vk::UniqueCommandBuffer CommandBuffer;
};

} // namespace rhi
