#pragma once

#include <renderer/Culling.hpp>

#include <RHI.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

/**
 * ============================================================================
 *  runtime::renderer - GPU 视锥剔除与间接绘制(GpuCuller)
 * ============================================================================
 *
 *  对应 shaders/cull.comp.hlsl: 对实例包围球做 6 平面保守剔除, 把可见实例压缩后
 *  写回 DrawIndexedIndirect 参数(indexCount/instanceCount/firstIndex/vertexOffset)
 *  与可见实例下标缓冲, 供 vkCmdDrawIndexedIndirect 消费.
 *
 *  缓冲约定:
 *    - InstanceBounds : SSBO, 每实例 float4(center.xyz + radius), 只读.
 *    - ParamsUBO      : UBO,  6 个视锥平面 float4 + uint 实例数.
 *    - IndirectArgs   : SSBO, 4 × uint32(indexCount/instanceCount/firstIndex/vertexOffset).
 *    - VisibleIndices : SSBO, 压缩后的可见实例下标.
 *
 *  [集成说明]
 *  间接绘制需要"实例化渲染"(同一份几何 + 逐实例变换缓冲), 与当前按对象逐个
 *  DrawPacket 录制的前向路径不同. 本组件提供了完整的剔除 compute 与间接绘制
 *  录制入口; 场景侧切换到实例化几何后, 在 Opaque Pass 里先 recordCull() 再
 *  recordIndirectDraw() 即可.
 * ============================================================================
 */
namespace runtime::renderer
{

class GpuCuller
{
public:
    GpuCuller() = default;
    GpuCuller(const GpuCuller&) = delete;
    GpuCuller& operator=(const GpuCuller&) = delete;

    /** @brief 加载 cull.comp.spv 并创建 compute 管线与缓冲. */
    bool initialize(rhi::RDevice& device)
    {
        if (Ready)
            return true;
        Device = &device;

        // 读 SPIR-V.
        const std::string path = std::string(SEED_SCENE_SHADER_DIR) + "/cull.comp.spv";
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            return false;
        const std::streamsize size = stream.tellg();
        if (size <= 0)
            return false;
        stream.seekg(0, std::ios::beg);
        std::vector<std::byte> bytecode(static_cast<size_t>(size));
        stream.read(reinterpret_cast<char*>(bytecode.data()), size);

        rhi::ShaderDescriptor cs;
        cs.Stage = rhi::EShaderStage_t::Compute;
        cs.ByteCode = std::move(bytecode);
        cs.EntryPoint = "main";
        cs.DebugName = "GpuCullCS";
        const auto shader = device.createShader(cs);
        if (!shader)
            return false;

        // Set 0(只读): b0 = ParamsUBO, t0 = InstanceBounds(SSBO).
        rhi::BindGroupLayoutDescriptor params_layout_desc;
        params_layout_desc.Entries = {
            { .Binding = 0, .Type = rhi::EDescriptorType::UniformBuffer, .ArrayCount = 1,
              .Visibility = rhi::EShaderStage_t::Compute },
            { .Binding = 1, .Type = rhi::EDescriptorType::ReadOnlyStorageBuffer, .ArrayCount = 1,
              .Visibility = rhi::EShaderStage_t::Compute },
        };
        params_layout_desc.DebugName = "GpuCullParamsLayout";
        ParamsLayout = device.createBindGroupLayout(params_layout_desc);

        // Set 1(读写): u0 = IndirectArgs, u1 = VisibleIndices.
        rhi::BindGroupLayoutDescriptor output_layout_desc;
        output_layout_desc.Entries = {
            { .Binding = 0, .Type = rhi::EDescriptorType::ReadWriteStorageBuffer, .ArrayCount = 1,
              .Visibility = rhi::EShaderStage_t::Compute },
            { .Binding = 1, .Type = rhi::EDescriptorType::ReadWriteStorageBuffer, .ArrayCount = 1,
              .Visibility = rhi::EShaderStage_t::Compute },
        };
        output_layout_desc.DebugName = "GpuCullOutputLayout";
        OutputLayout = device.createBindGroupLayout(output_layout_desc);

        rhi::PipelineLayoutDescriptor layout_desc;
        layout_desc.BindGroupLayouts = { ParamsLayout, OutputLayout };
        layout_desc.DebugName = "GpuCullLayout";
        Layout = device.createPipelineLayout(layout_desc);
        if (!Layout)
            return false;

        rhi::ComputePipelineDescriptor pipe_desc;
        pipe_desc.Layout = Layout;
        pipe_desc.Compute.Shader = shader;
        pipe_desc.DebugName = "GpuCullPipeline";
        Pipeline = device.createComputePipeline(pipe_desc);
        if (!Pipeline)
            return false;

        // ParamsUBO: 7 个 float4(6 平面 + 实例数), 持久映射.
        ParamsUBO = device.createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = 7 * sizeof(float) * 4,
            .Usage = rhi::EBufferUsage::Uniform,
            .MemoryUsage = rhi::EMemoryUsage::CPUToGPU,
            .MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent,
            .PersistentlyMapped = true,
            .DebugName = "GpuCullParamsUBO",
        });

        MaxIndices = 65536; // 默认上限; 调用方按需扩容.
        // 实例包围球由 CPU 每帧上传, 用 HostVisible 简化; 大规模场景应改 staging + copy.
        InstanceBounds = device.createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = static_cast<rhi::DeviceSizeType>(MaxIndices) * sizeof(float) * 4,
            .Usage = rhi::EBufferUsage::Storage,
            .MemoryUsage = rhi::EMemoryUsage::CPUToGPU,
            .MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent,
            .PersistentlyMapped = true,
            .DebugName = "GpuCullInstanceBounds",
        });
        // 间接参数缓冲(VkDrawIndexedIndirectCommand, 5 × uint32)需 CPU 每帧复位 instanceCount.
        IndirectArgs = device.createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = 5 * sizeof(uint32_t),
            .Usage = rhi::EBufferUsageFlags(rhi::EBufferUsage::Storage) | rhi::EBufferUsage::Indirect,
            .MemoryUsage = rhi::EMemoryUsage::CPUToGPU,
            .MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent,
            .PersistentlyMapped = true,
            .DebugName = "GpuCullIndirectArgs",
        });
        VisibleIndices = device.createBuffer(rhi::RBuffer::Descriptor_t {
            .Size = static_cast<rhi::DeviceSizeType>(MaxIndices) * sizeof(uint32_t),
            .Usage = rhi::EBufferUsage::Storage,
            .MemoryUsage = rhi::EMemoryUsage::GPUOnly,
            .DebugName = "GpuCullVisibleIndices",
        });
        if (!ParamsUBO || !InstanceBounds || !IndirectArgs || !VisibleIndices)
            return false;

        ParamsGroup = device.createBindGroup({
            .Layout = ParamsLayout,
            .Entries = {
                { .Binding = 0, .ArrayElement = 0,
                  .Resource = rhi::BufferBinding { ParamsUBO, 0, 7 * sizeof(float) * 4 } },
                { .Binding = 1, .ArrayElement = 0,
                  .Resource = rhi::BufferBinding { InstanceBounds, 0, 0 } },
            },
            .DebugName = "GpuCullParamsGroup",
        });
        OutputGroup = device.createBindGroup({
            .Layout = OutputLayout,
            .Entries = {
                { .Binding = 0, .ArrayElement = 0,
                  .Resource = rhi::BufferBinding { IndirectArgs, 0, 5 * sizeof(uint32_t) } },
                { .Binding = 1, .ArrayElement = 0,
                  .Resource = rhi::BufferBinding { VisibleIndices, 0, 0 } },
            },
            .DebugName = "GpuCullOutputGroup",
        });
        if (!ParamsGroup || !OutputGroup)
            return false;

        Ready = true;
        return true;
    }

    void shutdown() noexcept
    {
        ParamsGroup.reset();
        OutputGroup.reset();
        Pipeline.reset();
        Layout.reset();
        ParamsLayout.reset();
        OutputLayout.reset();
        ParamsUBO.reset();
        InstanceBounds.reset();
        IndirectArgs.reset();
        VisibleIndices.reset();
        Device = nullptr;
        Ready = false;
    }

    /** @brief 上传视锥平面与实例数到 UBO. */
    void uploadFrustum(const Frustum& Planes, uint32_t instance_count)
    {
        InstanceCount = instance_count;
        if (!ParamsUBO || !ParamsUBO->isValid())
            return;
        struct ParamsData
        {
            float Planes[6][4];
            uint32_t InstanceCount;
            uint32_t Pad[3];
        } data {};
        for (int i = 0; i < 6; ++i)
        {
            data.Planes[i][0] = Planes.Planes[i].Normal.x();
            data.Planes[i][1] = Planes.Planes[i].Normal.y();
            data.Planes[i][2] = Planes.Planes[i].Normal.z();
            data.Planes[i][3] = Planes.Planes[i].Distance;
        }
        data.InstanceCount = instance_count;
        void* mapped = ParamsUBO->map();
        std::memcpy(mapped, &data, sizeof(data));
        ParamsUBO->unmap();
    }

    /** @brief 上传实例包围球(center.xyz + radius), 每实例一个 float4. */
    void uploadInstanceBounds(const std::vector<infra::Vec4f>& bounds)
    {
        if (!InstanceBounds || !InstanceBounds->isValid() || bounds.empty())
            return;
        const size_t bytes = bounds.size() * sizeof(float) * 4;
        if (bytes > static_cast<size_t>(MaxIndices) * sizeof(float) * 4)
            return; // 超上限, 调用方应扩容(此处保守拒绝).
        void* mapped = InstanceBounds->map();
        std::memcpy(mapped, bounds.data(), bytes);
        InstanceBounds->unmap();
    }

    /** @brief 复位间接绘制参数(instanceCount 清零, 其余由调用方给定). */
    void resetIndirectArgs(uint32_t index_count, uint32_t first_index, int32_t vertex_offset)
    {
        if (!IndirectArgs || !IndirectArgs->isValid())
            return;
        const uint32_t args[5] = { index_count, 0u, first_index,
            static_cast<uint32_t>(vertex_offset), 0u };
        void* mapped = IndirectArgs->map();
        std::memcpy(mapped, args, sizeof(args));
        IndirectArgs->unmap();
    }

    /** @brief 录制剔除 dispatch. */
    void recordCull(rhi::RCommandList& command_list)
    {
        if (!Ready || InstanceCount == 0)
            return;
        command_list.bindPipeline(Pipeline);
        const std::array groups { ParamsGroup, OutputGroup };
        command_list.bindBindGroups(rhi::EPipelineType::Compute, Layout, 0, groups);
        const uint32_t group_count = (InstanceCount + 63) / 64;
        command_list.dispatch(group_count, 1, 1);
    }

    /**
     * @brief 录制间接索引绘制(供实例化几何路径调用).
     *
     * 可见实例下标缓冲作为 per-instance 顶点属性绑定到 slot 1, 实例化顶点着色器据此
     * 从实例数据缓冲取变换/材质. 需在 recordCull() 之后、同一命令流内调用.
     */
    void recordIndirectDraw(rhi::RCommandList& command_list,
        const std::shared_ptr<rhi::RBuffer>& index_buffer, rhi::EIndexFormat format)
    {
        if (!Ready)
            return;
        if (index_buffer)
            command_list.bindIndexBuffer(index_buffer, 0, format);
        const rhi::VertexBufferBinding instance_binding { VisibleIndices, 0 };
        command_list.bindVertexBuffers(1,
            std::span<const rhi::VertexBufferBinding>(&instance_binding, 1));
        command_list.drawIndexedIndirect(IndirectArgs, 0, 1, 5 * sizeof(uint32_t));
    }

    /** @brief 取间接参数缓冲(供外部在 RDG 中导入做屏障). */
    [[nodiscard]] const std::shared_ptr<rhi::RBuffer>& getIndirectArgsBuffer() const noexcept
    {
        return IndirectArgs;
    }

private:
    rhi::RDevice* Device { nullptr };
    std::shared_ptr<rhi::RPipelineLayout> Layout;
    std::shared_ptr<rhi::RBindGroupLayout> ParamsLayout;
    std::shared_ptr<rhi::RBindGroupLayout> OutputLayout;
    std::shared_ptr<rhi::RPipeline> Pipeline;
    std::shared_ptr<rhi::RBuffer> ParamsUBO;
    std::shared_ptr<rhi::RBuffer> InstanceBounds;
    std::shared_ptr<rhi::RBuffer> IndirectArgs;
    std::shared_ptr<rhi::RBuffer> VisibleIndices;
    std::shared_ptr<rhi::RBindGroup> ParamsGroup;
    std::shared_ptr<rhi::RBindGroup> OutputGroup;
    uint32_t InstanceCount { 0 };
    uint32_t MaxIndices { 0 };
    bool Ready { false };
};

} // namespace runtime::renderer
