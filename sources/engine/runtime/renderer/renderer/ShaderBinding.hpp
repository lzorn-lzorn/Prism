#pragma once

#include <RHI.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace runtime::renderer
{

using ResourceId = uint64_t;

/**
 * @brief 为着色器资源名生成确定性标识符。
 * @param Name 着色器编译器输出的 UTF-8 资源名。
 * @return 稳定的 FNV-1a 标识符。冲突由 ResourceBindingTable 检查。
 */
[[nodiscard]] constexpr ResourceId makeResourceId(std::string_view Name) noexcept
{
    ResourceId hash = 14695981039346656037ull;
    for (const char value : Name)
    {
        hash ^= static_cast<uint8_t>(value);
        hash *= 1099511628211ull;
    }
    return hash;
}

/** @brief 通过反射发现的一个着色器可见描述符。 */
struct ShaderResourceBinding
{
    ResourceId Id { 0 };
    std::string Name;
    uint32_t Group { 0 };
    uint32_t Binding { 0 };
    uint32_t ArrayCount { 1 };
    uint32_t BlockSize { 0 };
    rhi::EDescriptorType Type { rhi::EDescriptorType::UniformBuffer };
    rhi::EShaderStage Visibility {};
    bool RuntimeArray { false };
};

/** @brief 一个 SPIR-V 入口点的反射结果。 */
struct ShaderInterface
{
    rhi::EShaderStage_t Stage { rhi::EShaderStage_t::Vertex };
    std::string EntryPoint;
    std::vector<ShaderResourceBinding> Resources;
    std::vector<rhi::PushConstantRange> PushConstants;
};

/** @brief 控制从反射得到的运行时数组到有限 RHI 布局的转换。 */
struct PipelineInterfaceBuildOptions
{
    uint32_t RuntimeArrayMaxCount { 1024 };
    bool RuntimeArraysArePartiallyBound { true };
    bool RuntimeArraysUseUpdateAfterBind { false };
};

/**
 * @brief 合并后的着色器 ABI，用于创建 RHI 布局并解析具体资源。
 *
 * BindGroups 按着色器 set/space 索引。当着色器 set 稀疏时保留空组，
 * 使后续所有 set 编号保持其 ABI 位置。
 */
struct PipelineInterface
{
    std::vector<rhi::BindGroupLayoutDescriptor> BindGroups;
    std::vector<ShaderResourceBinding> Resources;
    std::vector<rhi::PushConstantRange> PushConstants;
};

/** @brief 由 Renderer 而非底层 RHI 持有的 SPIR-V 反射前端。 */
class SpirvShaderReflector final
{
public:
    /**
     * @brief 反射某个入口点使用的描述符和 push constant。
     * @param ByteCode 完整的 SPIR-V 模块字节。
     * @param Stage EntryPoint 期望的 RHI 阶段。
     * @param EntryPoint 要检查的入口点名称。
     * @return 归一化的渲染器侧着色器 ABI。
     * @throws std::invalid_argument 如果 SPIR-V 或入口点元数据无效。
     */
    [[nodiscard]] static ShaderInterface reflect(
        std::span<const std::byte> ByteCode,
        rhi::EShaderStage_t Stage,
        std::string_view EntryPoint = "main");
};

/** @brief 校验并合并多个着色器阶段接口为一个管线 ABI。 */
class PipelineInterfaceBuilder final
{
public:
    /**
     * @brief 跨阶段合并描述符可见性与 push-constant 可见性。
     * @param Interfaces 属于同一管线的各阶段接口。
     * @param Options 无界描述符数组的策略。
     * @return 按 group、binding 与 push-constant 偏移排序的规范接口。
     */
    [[nodiscard]] static PipelineInterface merge(
        std::span<const ShaderInterface> Interfaces,
        const PipelineInterfaceBuildOptions& Options = {});
};

/**
 * @brief 将反射得到的资源名映射到具体 RHI 资源的语义表。
 *
 * 该表与描述符集编号无关。这样当着色器绑定在资源编译期间被重新分配时，
 * 材质与帧代码能保持稳定。
 */
class ResourceBindingTable final
{
public:
    /** @brief 通过着色器可见名称添加或替换一个资源数组元素。 */
    void set(std::string_view Name, rhi::BindGroupResource Resource, uint32_t ArrayElement = 0);

    /** @brief 通过预计算的稳定 ID 添加或替换一个资源数组元素。 */
    void set(ResourceId Id, std::string_view Name, rhi::BindGroupResource Resource,
        uint32_t ArrayElement = 0);

    /** @return 所请求数组元素处的资源，不存在时为 nullptr。 */
    [[nodiscard]] const rhi::BindGroupResource* find(
        ResourceId Id,
        std::string_view ExpectedName,
        uint32_t ArrayElement = 0) const;

    /** @return 最高已填充元素加一，资源不存在时为零。 */
    [[nodiscard]] uint32_t getElementCount(ResourceId Id, std::string_view ExpectedName) const;

private:
    struct Slot
    {
        std::string Name;
        std::unordered_map<uint32_t, rhi::BindGroupResource> Elements;
    };
    std::unordered_map<ResourceId, Slot> Slots;
};

/** @brief 将 PipelineInterface 针对资源表进行解析的结果。 */
struct ResolvedPipelineBindings
{
    std::shared_ptr<rhi::RPipelineLayout> PipelineLayout;
    std::vector<std::shared_ptr<rhi::RBindGroupLayout>> BindGroupLayouts;
    std::vector<std::shared_ptr<rhi::RBindGroup>> BindGroups;
};

/**
 * @brief 创建并复用 RHI 布局，然后实例化不可变 BindGroup。
 *
 * 布局按规范 ABI 字节缓存。具体 BindGroup 有意不缓存，
 * 因为资源的生命周期/版本策略属于材质以及本服务之上的渲染图。
 */
class ShaderBindingResolver final
{
public:
    explicit ShaderBindingResolver(rhi::RDevice& Device) noexcept : Device(&Device) {}

    /**
     * @brief 解析所有反射资源并创建可绑定的 RHI 对象。
     * @param Interface 合并后的管线 ABI。
     * @param Resources 语义资源表。
     * @return 按 set 顺序排列的管线布局、组布局与具体组。
     */
    [[nodiscard]] ResolvedPipelineBindings resolve(
        const PipelineInterface& Interface,
        const ResourceBindingTable& Resources);

private:
    [[nodiscard]] std::shared_ptr<rhi::RPipelineLayout> getOrCreatePipelineLayout(
        const PipelineInterface& Interface);

    rhi::RDevice* Device { nullptr };
    std::mutex CacheMutex;
    std::unordered_map<std::string, std::weak_ptr<rhi::RPipelineLayout>> LayoutCache;
};

} // namespace runtime::renderer
