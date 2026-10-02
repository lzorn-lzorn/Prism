// renderer/MaterialSystem.hpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <core/functions/HandleManager.hpp>
#include <RHI.hpp>

namespace renderer
{

struct TransparentStringHash
{
    using is_transparent = void;

    [[nodiscard]] size_t operator()(std::string_view Value) const noexcept
    {
        return std::hash<std::string_view>{}(Value);
    }

    [[nodiscard]] size_t operator()(const std::string& Value) const noexcept
    {
        return (*this)(std::string_view(Value));
    }

    [[nodiscard]] size_t operator()(const char* Value) const noexcept
    {
        return (*this)(std::string_view(Value));
    }
};

CORE_DEFINE_HANDLE(MaterialTemplate);   // -> struct MaterialTemplate; using MaterialTemplateHandle
CORE_DEFINE_HANDLE(MaterialInstance);   // -> struct MaterialInstance; using MaterialInstanceHandle
CORE_DEFINE_HANDLE(VertexFactory);      // -> struct VertexFactory; using VertexFactoryHandle

// ============================================================
// 参数类型
// ============================================================
enum class EParameterType : uint8_t
{
    None,
    Float, Vec2, Vec3, Vec4, Mat3, Mat4,
    Int, Int2, Int3, Int4,
    UInt, UInt2, UInt3, UInt4,
    Bool,
    Texture2D, Texture3D, TextureCube, Texture2DArray,
    Sampler,
    CombinedImageSampler2D,
    CombinedImageSampler3D,
    CombinedImageSamplerCube,
    AccelerationStructure,
};

[[nodiscard]] constexpr bool isUniformParam(EParameterType T) noexcept
{
    switch (T)
    {
        case EParameterType::Texture2D:
        case EParameterType::Texture3D:
        case EParameterType::TextureCube:
        case EParameterType::Texture2DArray:
        case EParameterType::Sampler:
        case EParameterType::CombinedImageSampler2D:
        case EParameterType::CombinedImageSampler3D:
        case EParameterType::CombinedImageSamplerCube:
        case EParameterType::AccelerationStructure:
            return false;
        default:
            return true;
    }
}

[[nodiscard]] constexpr uint32_t getParamSize(EParameterType T) noexcept
{
    switch (T)
    {
        case EParameterType::Float:  return 4;
        case EParameterType::Vec2:   return 8;
        case EParameterType::Vec3:   return 12;
        case EParameterType::Vec4:   return 16;
        case EParameterType::Mat3:   return 36;
        case EParameterType::Mat4:   return 64;
        case EParameterType::Int:    return 4;
        case EParameterType::Int2:   return 8;
        case EParameterType::Int3:   return 12;
        case EParameterType::Int4:   return 16;
        case EParameterType::UInt:   return 4;
        case EParameterType::UInt2:  return 8;
        case EParameterType::UInt3:  return 12;
        case EParameterType::UInt4:  return 16;
        case EParameterType::Bool:   return 4;
        default: return 0;
    }
}

struct ParameterDescriptor
{
    std::string    Name;
    EParameterType Type { EParameterType::None };

    uint32_t Offset     { 0 };
    uint32_t Size       { 0 };
    uint32_t ArrayCount { 1 };
    uint32_t Binding    { 0 };

    rhi::EShaderStage Visibility {
        rhi::EShaderStage_t::Vertex | rhi::EShaderStage_t::Pixel
    };
};

struct TextureBindingDescriptor
{
    std::string    Name;
    EParameterType Type { EParameterType::CombinedImageSampler2D };
    uint32_t       Binding { 1 };
    uint32_t       ArrayCount { 1 };
    rhi::EShaderStage Visibility { rhi::EShaderStage_t::Pixel };
};

struct UniformBlockDescriptor
{
    std::string                      Name;
    uint32_t                         Size { 0 };
    std::vector<ParameterDescriptor> Parameters;
};

struct ShaderSetDescriptor
{
    std::optional<rhi::ShaderDescriptor> Vertex;
    std::optional<rhi::ShaderDescriptor> Pixel;
    std::optional<rhi::ShaderDescriptor> Compute;
    std::optional<rhi::ShaderDescriptor> RayGeneration;
    std::optional<rhi::ShaderDescriptor> Miss;
    std::optional<rhi::ShaderDescriptor> ClosestHit;
    std::optional<rhi::ShaderDescriptor> AnyHit;
    std::optional<rhi::ShaderDescriptor> Intersection;
    std::optional<rhi::ShaderDescriptor> Callable;
};

enum class EPassType : uint8_t
{
    PrePass,
    BasePass,
    ShadowDepth,
    Velocity,
    CustomDepth,
    Translucency,
    PostProcess,
    Count
};

inline constexpr size_t MaterialPassCount = static_cast<size_t>(EPassType::Count);

struct MaterialPassOverrides
{
    ShaderSetDescriptor Shaders;

    std::optional<rhi::RasterizerState>   Rasterizer;
    std::optional<rhi::MultisampleState>  Multisample;
    std::optional<rhi::DepthStencilState> DepthStencil;
    std::optional<rhi::BlendState>        Blend;

    std::optional<rhi::RenderingSignature>    Rendering;
    std::optional<rhi::EPipelineCompileFlags> CompileFlags;
};

struct VertexFactoryPassOverrides
{
    std::optional<rhi::ShaderDescriptor> VertexShader;
    std::optional<rhi::VertexInputState> VertexInput;
    std::optional<rhi::EPrimitiveTopology> Topology;
};

struct VertexFactoryDescriptor
{
    std::string Name;
    std::string Category;

    std::optional<rhi::ShaderDescriptor> VertexShader;
    rhi::VertexInputState VertexInput;
    rhi::EPrimitiveTopology Topology { rhi::EPrimitiveTopology::TriangleList };

    std::array<VertexFactoryPassOverrides, MaterialPassCount> Passes {};
};

class VertexFactoryBase
{
public:
    virtual ~VertexFactoryBase() = default;

    VertexFactoryBase(const VertexFactoryBase&)            = delete;
    VertexFactoryBase& operator=(const VertexFactoryBase&) = delete;
    VertexFactoryBase(VertexFactoryBase&&)                 = delete;
    VertexFactoryBase& operator=(VertexFactoryBase&&)      = delete;

    [[nodiscard]] const std::string& getName() const noexcept { return Descriptor.Name; }
    [[nodiscard]] const std::string& getCategory() const noexcept { return Descriptor.Category; }
    [[nodiscard]] const VertexFactoryDescriptor& getDescriptor() const noexcept { return Descriptor; }

    [[nodiscard]] const std::shared_ptr<rhi::RShader>& getVertexShader(EPassType Pass) const noexcept
    {
        return PassVertexShaders[static_cast<size_t>(Pass)];
    }

    [[nodiscard]] rhi::VertexInputState getVertexInput(EPassType Pass) const
    {
        const auto& Override = Descriptor.Passes[static_cast<size_t>(Pass)].VertexInput;
        return Override.value_or(Descriptor.VertexInput);
    }

    [[nodiscard]] rhi::EPrimitiveTopology getTopology(EPassType Pass) const noexcept
    {
        const auto& Override = Descriptor.Passes[static_cast<size_t>(Pass)].Topology;
        return Override.value_or(Descriptor.Topology);
    }

    [[nodiscard]] bool isValid() const noexcept
    {
        return true;
    }

    [[nodiscard]] core::HandleIdType getStableId() const noexcept { return StableId; }
    [[nodiscard]] core::HandleGenerationType getVersion() const noexcept { return Version; }

protected:
    explicit VertexFactoryBase(VertexFactoryDescriptor InDescriptor)
        : Descriptor(std::move(InDescriptor)) {}

    void setCacheIdentity(core::HandleIdType InStableId, core::HandleGenerationType InVersion) noexcept
    {
        StableId = InStableId;
        Version = InVersion;
    }

    [[nodiscard]] std::shared_ptr<rhi::RShader> createShader(
        rhi::RDevice& Device,
        const rhi::ShaderDescriptor& Desc)
    {
        return Device.createShader(Desc);
    }

    void buildPassShaders(rhi::RDevice& Device)
    {
        std::shared_ptr<rhi::RShader> DefaultVS;
        if (Descriptor.VertexShader) DefaultVS = createShader(Device, *Descriptor.VertexShader);

        for (size_t Index = 0; Index < MaterialPassCount; ++Index)
        {
            const auto& Pass = Descriptor.Passes[Index];
            PassVertexShaders[Index] = Pass.VertexShader
                ? createShader(Device, *Pass.VertexShader)
                : DefaultVS;
        }
    }

protected:
    friend class MaterialSystem;

    VertexFactoryDescriptor Descriptor;
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassVertexShaders {};
    core::HandleIdType StableId { core::InvalidHandleId };
    core::HandleGenerationType Version { core::InvalidHandleGeneration };
};

class GraphicsVertexFactory final : public VertexFactoryBase
{
public:
    static std::shared_ptr<GraphicsVertexFactory> create(
        rhi::RDevice& Device,
        VertexFactoryDescriptor InDescriptor)
    {
        auto Factory = std::shared_ptr<GraphicsVertexFactory>(
            new GraphicsVertexFactory(std::move(InDescriptor)));
        Factory->buildPassShaders(Device);
        return Factory;
    }

private:
    explicit GraphicsVertexFactory(VertexFactoryDescriptor D)
        : VertexFactoryBase(std::move(D)) {}
};

struct MaterialPipelineRequest
{
    EPassType Pass { EPassType::BasePass };
    VertexFactoryHandle VertexFactory {};
    std::shared_ptr<rhi::RShader> VertexShaderOverride;
    std::optional<rhi::RenderingSignature>    RenderingOverride;
    std::optional<rhi::VertexInputState>      VertexInputOverride;
    std::optional<rhi::EPrimitiveTopology>    TopologyOverride;
    std::optional<rhi::EPipelineCompileFlags> CompileFlagsOverride;
};

struct MaterialTemplateDescriptor
{
    std::string Name;
    std::string Category;

    ShaderSetDescriptor Shaders;

    UniformBlockDescriptor                MaterialUniforms;
    std::vector<TextureBindingDescriptor> MaterialTextures;

    uint32_t          PushConstantSize { 0 };
    rhi::EShaderStage PushConstantStages {
        rhi::EShaderStage_t::Vertex | rhi::EShaderStage_t::Pixel
    };

    rhi::RasterizerState   Rasterizer;
    rhi::MultisampleState  Multisample;
    rhi::DepthStencilState DepthStencil;
    rhi::BlendState        Blend;

    uint32_t                                 MaxRecursionDepth { 1 };
    std::vector<rhi::RayTracingShaderGroup>  ShaderGroups;

    rhi::RenderingSignature    Rendering;
    rhi::EPipelineCompileFlags CompileFlags {};

    std::array<MaterialPassOverrides, MaterialPassCount> Passes {};
};


class MaterialTemplateBase
{
public:
    virtual ~MaterialTemplateBase() = default;

    MaterialTemplateBase(const MaterialTemplateBase&)            = delete;
    MaterialTemplateBase& operator=(const MaterialTemplateBase&) = delete;
    MaterialTemplateBase(MaterialTemplateBase&&)                 = delete;
    MaterialTemplateBase& operator=(MaterialTemplateBase&&)      = delete;

    // ---- 类型查询 ----
    [[nodiscard]] virtual rhi::EPipelineType getPipelineType() const noexcept = 0;
    [[nodiscard]] const std::string& getName() const noexcept { return Descriptor.Name; }
    [[nodiscard]] const std::string& getCategory() const noexcept { return Descriptor.Category; }
    [[nodiscard]] const MaterialTemplateDescriptor& getDescriptor() const noexcept { return Descriptor; }

    // ---- RHI 资源 ----
    [[nodiscard]] const std::shared_ptr<rhi::RPipelineLayout>&  getPipelineLayout() const noexcept { return PipelineLayout; }
    [[nodiscard]] const std::shared_ptr<rhi::RBindGroupLayout>& getMaterialSetLayout() const noexcept { return MaterialSetLayout; }
    [[nodiscard]] const MaterialPassOverrides& getPassOverrides(EPassType Pass) const noexcept
    {
        return Descriptor.Passes[static_cast<size_t>(Pass)];
    }

    [[nodiscard]] uint32_t getMaterialSetIndex() const noexcept { return MaterialSetIndex; }
    [[nodiscard]] uint32_t getUniformBlockSize() const noexcept { return Descriptor.MaterialUniforms.Size; }
    [[nodiscard]] bool     isValid() const noexcept { return PipelineLayout != nullptr && MaterialSetLayout != nullptr; }
    [[nodiscard]] core::HandleIdType getStableId() const noexcept { return StableId; }
    [[nodiscard]] uint32_t getVersion() const noexcept { return Version; }

    [[nodiscard]] virtual std::shared_ptr<rhi::RPipeline> createPipeline(
        rhi::RDevice& Device,
        const MaterialPipelineRequest& Request) const = 0;

    // ---- 参数查询（用索引，避免裸指针失效） ----
    [[nodiscard]] const ParameterDescriptor* findParameter(std::string_view Name) const noexcept
    {
        auto it = ParameterLookup.find(Name);
        if (it == ParameterLookup.end()) return nullptr;
        return &Descriptor.MaterialUniforms.Parameters[it->second];
    }

    [[nodiscard]] const TextureBindingDescriptor* findTexture(std::string_view Name) const noexcept
    {
        auto it = TextureLookup.find(Name);
        if (it == TextureLookup.end()) return nullptr;
        return &Descriptor.MaterialTextures[it->second];
    }

    [[nodiscard]] const std::vector<TextureBindingDescriptor>& getTextureBindings() const noexcept
    {
        return Descriptor.MaterialTextures;
    }

    // ---- 实例引用计数（供 MaterialSystem / MaterialInstanceBase 使用） ----
    [[nodiscard]] uint32_t getInstanceRefCount() const noexcept
    {
        return InstanceRefCount.load(std::memory_order_relaxed);
    }

    void addInstanceRef() noexcept
    {
        InstanceRefCount.fetch_add(1, std::memory_order_relaxed);
    }

    void releaseInstanceRef() noexcept
    {
        InstanceRefCount.fetch_sub(1, std::memory_order_relaxed);
    }

    // 仅供热重载时继承旧模板的计数
    void setInstanceRefCount(uint32_t V) noexcept
    {
        InstanceRefCount.store(V, std::memory_order_relaxed);
    }

protected:
    explicit MaterialTemplateBase(MaterialTemplateDescriptor InDescriptor)
        : Descriptor(std::move(InDescriptor)) {}

    void setCacheIdentity(core::HandleIdType InStableId, uint32_t InVersion) noexcept
    {
        StableId = InStableId;
        Version = InVersion;
    }

    void buildMaterialSetLayout(rhi::RDevice& Device)
    {
        std::vector<rhi::BindGroupLayoutEntry> Entries;

        if (Descriptor.MaterialUniforms.Size > 0)
        {
            Entries.push_back({
                .Binding    = 0,
                .Type       = rhi::EDescriptorType::UniformBuffer,
                .ArrayCount = 1,
                .Visibility = rhi::EShaderStage_t::Vertex | rhi::EShaderStage_t::Pixel,
                .Flags      = rhi::EDescriptorBindingFlags(
                                  rhi::EDescriptorBindingFlag_t::DynamicOffset)
            });
        }

        for (const auto& Tex : Descriptor.MaterialTextures)
        {
            Entries.push_back({
                .Binding    = Tex.Binding,
                .Type       = toDescriptorType(Tex.Type),
                .ArrayCount = Tex.ArrayCount,
                .Visibility = Tex.Visibility,
                .Flags      = {}
            });
        }

        MaterialSetLayout = Device.createBindGroupLayout({ .Entries = std::move(Entries) });
    }

    void buildPipelineLayout(
        rhi::RDevice& Device,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts,
        uint32_t MaterialSetIndex)
    {
        this->MaterialSetIndex = MaterialSetIndex;

        std::vector<std::shared_ptr<rhi::RBindGroupLayout>> Layouts;
        Layouts.reserve(2 + ExtraLayouts.size());
        Layouts.push_back(FrameLayout);
        Layouts.push_back(MaterialSetLayout);
        for (const auto& L : ExtraLayouts) Layouts.push_back(L);

        std::vector<rhi::PushConstantRange> PushRanges;
        if (Descriptor.PushConstantSize > 0)
        {
            PushRanges.push_back({
                .Stages = Descriptor.PushConstantStages,
                .Offset = 0,
                .Size   = Descriptor.PushConstantSize
            });
        }

        PipelineLayout = Device.createPipelineLayout({
            .BindGroupLayouts   = std::move(Layouts),
            .PushConstantRanges = std::move(PushRanges),
            .DebugName          = Descriptor.Name + ".PipelineLayout"
        });
    }

    void buildParameterLookup()
    {
        ParameterLookup.clear();
        TextureLookup.clear();

        for (uint32_t i = 0; i < Descriptor.MaterialUniforms.Parameters.size(); ++i)
        {
            ParameterLookup[Descriptor.MaterialUniforms.Parameters[i].Name] = i;
        }
        for (uint32_t i = 0; i < Descriptor.MaterialTextures.size(); ++i)
        {
            TextureLookup[Descriptor.MaterialTextures[i].Name] = i;
        }
    }

    [[nodiscard]] std::shared_ptr<rhi::RShader> createShader(
        rhi::RDevice& Device,
        const rhi::ShaderDescriptor& Desc)
    {
        return Device.createShader(Desc);
    }

protected:
    friend class MaterialSystem;

    MaterialTemplateDescriptor Descriptor;

    std::shared_ptr<rhi::RBindGroupLayout> MaterialSetLayout;
    std::shared_ptr<rhi::RPipelineLayout>  PipelineLayout;

    uint32_t MaterialSetIndex { 0 };

    std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> ParameterLookup;
    std::unordered_map<std::string, uint32_t, TransparentStringHash, std::equal_to<>> TextureLookup;
    core::HandleIdType StableId { core::InvalidHandleId };
    uint32_t Version { 1 };

private:
    std::atomic<uint32_t> InstanceRefCount { 0 };

    static rhi::EDescriptorType toDescriptorType(EParameterType T) noexcept
    {
        using DT = rhi::EDescriptorType;
        switch (T) {
            case EParameterType::Texture2D:
            case EParameterType::Texture3D:
            case EParameterType::TextureCube:
            case EParameterType::Texture2DArray:
                return DT::SampledTexture;
            case EParameterType::Sampler:
                return DT::Sampler;
            case EParameterType::CombinedImageSampler2D:
            case EParameterType::CombinedImageSampler3D:
            case EParameterType::CombinedImageSamplerCube:
                return DT::CombinedImageSampler;
            case EParameterType::AccelerationStructure:
                return DT::AccelerationStructure;
            default:
                return DT::UniformBuffer;
        }
    }
};

// ============================================================
// 材质实例基类：通过 Handle 引用模板
// ============================================================
class MaterialSystem;

class MaterialInstanceBase
{
public:
    MaterialInstanceBase(MaterialSystem* InSystem, MaterialTemplateHandle InTemplateHandle);
    virtual ~MaterialInstanceBase();

    MaterialInstanceBase(const MaterialInstanceBase&)            = delete;
    MaterialInstanceBase& operator=(const MaterialInstanceBase&) = delete;
    MaterialInstanceBase(MaterialInstanceBase&&)                 = delete;
    MaterialInstanceBase& operator=(MaterialInstanceBase&&)      = delete;

    // ---- 模板查询（热重载后自动返回新模板） ----
    [[nodiscard]] MaterialTemplateHandle getTemplateHandle() const noexcept { return TemplateHandle; }
    [[nodiscard]] const MaterialTemplateBase* getTemplate() const noexcept;
    [[nodiscard]] const std::shared_ptr<rhi::RBindGroup>& getBindGroup() const noexcept { return BindGroup; }

    struct ParameterHandle
    {
        uint32_t Index { std::numeric_limits<uint32_t>::max() };
        uint32_t Offset { 0 };
        uint32_t Size { 0 };
        uint32_t ArrayCount { 0 };
        EParameterType Type { EParameterType::None };

        [[nodiscard]] bool isValid() const noexcept
        {
            return Type != EParameterType::None;
        }
    };

    [[nodiscard]] ParameterHandle getParameterHandle(std::string_view Name) noexcept;

    // ---- 参数写入 ----
    void setFloat(std::string_view Name, float V) noexcept
    {
        setFloat(getParameterHandle(Name), V);
    }
    void setInt(std::string_view Name, int32_t V) noexcept
    {
        setInt(getParameterHandle(Name), V);
    }
    void setUInt(std::string_view Name, uint32_t V) noexcept
    {
        setUInt(getParameterHandle(Name), V);
    }
    void setBool(std::string_view Name, bool V) noexcept
    {
        uint32_t I = V ? 1u : 0u;
        writeParameter(getParameterHandle(Name), EParameterType::Bool, &I, sizeof(I));
    }
    void setVec2(std::string_view Name, float X, float Y) noexcept
    {
        float V[2] = { X, Y };
        writeParameter(getParameterHandle(Name), EParameterType::Vec2, V, sizeof(V));
    }
    void setVec3(std::string_view Name, float X, float Y, float Z) noexcept
    {
        float V[3] = { X, Y, Z };
        writeParameter(getParameterHandle(Name), EParameterType::Vec3, V, sizeof(V));
    }
    void setVec4(std::string_view Name, const std::array<float, 4>& V) noexcept
    {
        setVec4(getParameterHandle(Name), V);
    }
    void setVec4(std::string_view Name, float X, float Y, float Z, float W) noexcept
    {
        float V[4] = { X, Y, Z, W };
        writeParameter(getParameterHandle(Name), EParameterType::Vec4, V, sizeof(V));
    }
    void setMat4(std::string_view Name, const float M[16]) noexcept
    {
        writeParameter(getParameterHandle(Name), EParameterType::Mat4, M, sizeof(float) * 16);
    }

    void setFloat(ParameterHandle Handle, float V) noexcept
    {
        writeParameter(Handle, EParameterType::Float, &V, sizeof(V));
    }
    void setInt(ParameterHandle Handle, int32_t V) noexcept
    {
        writeParameter(Handle, EParameterType::Int, &V, sizeof(V));
    }
    void setUInt(ParameterHandle Handle, uint32_t V) noexcept
    {
        writeParameter(Handle, EParameterType::UInt, &V, sizeof(V));
    }
    void setVec4(ParameterHandle Handle, const std::array<float, 4>& V) noexcept
    {
        writeParameter(Handle, EParameterType::Vec4, V.data(), sizeof(float) * 4);
    }

    void writeRaw(std::string_view Name, std::span<const std::byte> Data) noexcept
    {
        const auto Handle = getParameterHandle(Name);
        if (!Handle.isValid()) return;

        const uint32_t TotalSize = Handle.Size * Handle.ArrayCount;
        if (Data.size() > TotalSize) return;
        if (Handle.Offset + TotalSize > UniformData.size()) return;

        std::memcpy(UniformData.data() + Handle.Offset, Data.data(), Data.size());
        IsUniformDirty = true;
    }

    // ---- 纹理绑定 ----
    void setTexture(std::string_view Name, std::shared_ptr<rhi::RImageView> View);
    void setSampler(std::string_view Name, std::shared_ptr<rhi::RSampler> Sampler);
    void setCombinedImageSampler(
        std::string_view Name,
        std::shared_ptr<rhi::RImageView> View,
        std::shared_ptr<rhi::RSampler>   Sampler);
    void setAccelerationStructure(
        std::string_view Name,
        std::shared_ptr<rhi::RAccelerationStructure> AS);

    // ---- 提交：写动态 UBO + 更新 BindGroup ----
    void commit(rhi::RDevice& Device,
                const std::shared_ptr<rhi::RBuffer>& DynamicUBO,
                uint32_t UBOOffset);

    [[nodiscard]] bool isDirty() const noexcept
    {
        return IsUniformDirty || IsTextureDirty;
    }

    [[nodiscard]] uint64_t computeTextureSetHash() const noexcept;

private:
    [[nodiscard]] ParameterHandle findAndCacheParameterHandle(std::string_view Name) noexcept;

    void writeParameter(ParameterHandle Handle, EParameterType Expected,
                        const void* Data, size_t Size) noexcept
    {
        if (!Handle.isValid()) return;
        if (Handle.Type != Expected) return;
        if (Handle.Offset + Size > UniformData.size())
            return;
        std::memcpy(UniformData.data() + Handle.Offset, Data, Size);
        IsUniformDirty = true;
    }

    static bool isCombinedType(EParameterType T) noexcept
    {
        return T == EParameterType::CombinedImageSampler2D
            || T == EParameterType::CombinedImageSampler3D
            || T == EParameterType::CombinedImageSamplerCube;
    }

private:
    friend class MaterialSystem;

    MaterialSystem*        System { nullptr };
    MaterialTemplateHandle TemplateHandle {};

    std::vector<std::byte> UniformData;
    std::unordered_map<uint32_t, rhi::BindGroupResource> TextureBindings;
    std::shared_ptr<rhi::RBindGroup> BindGroup;
    std::unordered_map<std::string, ParameterHandle, TransparentStringHash, std::equal_to<>> CachedParameterHandles;
    core::HandleIdType CachedParameterTemplateId { core::InvalidHandleId };
    uint32_t CachedParameterTemplateVersion { 0 };
    core::HandleIdType CachedBindGroupTemplateId { core::InvalidHandleId };
    uint32_t CachedBindGroupTemplateVersion { 0 };

    bool IsUniformDirty { true };
    bool IsTextureDirty { true };
};

// ============================================================
// Graphics / Compute / RT 模板派生类
// ============================================================
class GraphicsMaterialTemplate : public MaterialTemplateBase
{
public:
    static std::shared_ptr<GraphicsMaterialTemplate> create(
        rhi::RDevice& Device,
        MaterialTemplateDescriptor InDescriptor,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts = {},
        uint32_t MaterialSetIndex = 1)
    {
        auto Tmpl = std::shared_ptr<GraphicsMaterialTemplate>(
            new GraphicsMaterialTemplate(std::move(InDescriptor)));
        Tmpl->initialize(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        return Tmpl;
    }

    [[nodiscard]] rhi::EPipelineType getPipelineType() const noexcept override
    {
        return rhi::EPipelineType::Graphics;
    }

    [[nodiscard]] std::shared_ptr<rhi::RPipeline> createPipeline(
        rhi::RDevice& Device,
        const MaterialPipelineRequest& Request) const override
    {
        if (!Request.VertexInputOverride || !Request.TopologyOverride)
            return nullptr;

        const size_t PassIndex = static_cast<size_t>(Request.Pass);
        const auto& Pass = Descriptor.Passes[PassIndex];

        const auto VertexInput = *Request.VertexInputOverride;
        const auto Topology = *Request.TopologyOverride;
        const auto Rasterizer = Pass.Rasterizer.value_or(Descriptor.Rasterizer);
        const auto Multisample = Pass.Multisample.value_or(Descriptor.Multisample);
        const auto DepthStencil = Pass.DepthStencil.value_or(Descriptor.DepthStencil);
        const auto Blend = Pass.Blend.value_or(Descriptor.Blend);
        const auto Rendering = Request.RenderingOverride
            .value_or(Pass.Rendering.value_or(Descriptor.Rendering));
        const auto CompileFlags = Request.CompileFlagsOverride
            .value_or(Pass.CompileFlags.value_or(Descriptor.CompileFlags));

        rhi::GraphicsPipelineDescriptor GP {
            .Layout        = PipelineLayout,
            .Vertex        = {
                .Shader = Request.VertexShaderOverride
                    ? Request.VertexShaderOverride
                    : PassVertexShaders[PassIndex]
            },
            .Pixel         = { .Shader = PassPixelShaders[PassIndex] },
            .VertexInput   = VertexInput,
            .InputAssembly = { .Topology = Topology },
            .Rasterizer    = Rasterizer,
            .Multisample   = Multisample,
            .DepthStencil  = DepthStencil,
            .Blend         = Blend,
            .Rendering     = Rendering,
            .DynamicStates = rhi::EDynamicStates(rhi::EDynamicState_t::Viewport)
                           | rhi::EDynamicState_t::Scissor,
            .Compile       = { .Flags = CompileFlags },
            .DebugName     = Descriptor.Name + ".Pass" + std::to_string(PassIndex)
        };

        return Device.createGraphicsPipeline(GP);
    }

private:
    explicit GraphicsMaterialTemplate(MaterialTemplateDescriptor D)
        : MaterialTemplateBase(std::move(D)) {}

    void initialize(rhi::RDevice& Device,
                    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
                    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts,
                    uint32_t MaterialSetIndex)
    {
        buildMaterialSetLayout(Device);
        buildPipelineLayout(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        buildParameterLookup();
        buildPassShaders(Device);
    }

    void buildPassShaders(rhi::RDevice& Device)
    {
        std::shared_ptr<rhi::RShader> DefaultVS;
        std::shared_ptr<rhi::RShader> DefaultPS;
        if (Descriptor.Shaders.Vertex) DefaultVS = createShader(Device, *Descriptor.Shaders.Vertex);
        if (Descriptor.Shaders.Pixel)  DefaultPS = createShader(Device, *Descriptor.Shaders.Pixel);

        for (size_t Index = 0; Index < MaterialPassCount; ++Index)
        {
            const auto& Pass = Descriptor.Passes[Index];
            PassVertexShaders[Index] = Pass.Shaders.Vertex
                ? createShader(Device, *Pass.Shaders.Vertex)
                : DefaultVS;
            PassPixelShaders[Index] = Pass.Shaders.Pixel
                ? createShader(Device, *Pass.Shaders.Pixel)
                : DefaultPS;
        }
    }

private:
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassVertexShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassPixelShaders {};
};

class ComputeMaterialTemplate : public MaterialTemplateBase
{
public:
    static std::shared_ptr<ComputeMaterialTemplate> create(
        rhi::RDevice& Device,
        MaterialTemplateDescriptor Desc,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts = {},
        uint32_t MaterialSetIndex = 1)
    {
        auto Tmpl = std::shared_ptr<ComputeMaterialTemplate>(
            new ComputeMaterialTemplate(std::move(Desc)));
        Tmpl->initialize(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        return Tmpl;
    }

    [[nodiscard]] rhi::EPipelineType getPipelineType() const noexcept override
    {
        return rhi::EPipelineType::Compute;
    }

    [[nodiscard]] std::shared_ptr<rhi::RPipeline> createPipeline(
        rhi::RDevice& Device,
        const MaterialPipelineRequest& Request) const override
    {
        const size_t PassIndex = static_cast<size_t>(Request.Pass);
        const auto& Pass = Descriptor.Passes[PassIndex];
        const auto CompileFlags = Request.CompileFlagsOverride
            .value_or(Pass.CompileFlags.value_or(Descriptor.CompileFlags));

        rhi::ComputePipelineDescriptor CP {
            .Layout    = PipelineLayout,
            .Compute   = { .Shader = PassComputeShaders[PassIndex] },
            .Compile   = { .Flags = CompileFlags },
            .DebugName = Descriptor.Name + ".Pass" + std::to_string(PassIndex)
        };

        return Device.createComputePipeline(CP);
    }

private:
    explicit ComputeMaterialTemplate(MaterialTemplateDescriptor D)
        : MaterialTemplateBase(std::move(D)) {}

    void initialize(rhi::RDevice& Device,
                    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
                    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts,
                    uint32_t MaterialSetIndex)
    {
        buildMaterialSetLayout(Device);
        buildPipelineLayout(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        buildParameterLookup();
        buildPassShaders(Device);
    }

    void buildPassShaders(rhi::RDevice& Device)
    {
        std::shared_ptr<rhi::RShader> DefaultCS;
        if (Descriptor.Shaders.Compute) DefaultCS = createShader(Device, *Descriptor.Shaders.Compute);

        for (size_t Index = 0; Index < MaterialPassCount; ++Index)
        {
            const auto& Pass = Descriptor.Passes[Index];
            PassComputeShaders[Index] = Pass.Shaders.Compute
                ? createShader(Device, *Pass.Shaders.Compute)
                : DefaultCS;
        }
    }

private:
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassComputeShaders {};
};

class RayTracingMaterialTemplate : public MaterialTemplateBase
{
public:
    static std::shared_ptr<RayTracingMaterialTemplate> create(
        rhi::RDevice& Device,
        MaterialTemplateDescriptor Desc,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts = {},
        uint32_t MaterialSetIndex = 1)
    {
        auto Tmpl = std::shared_ptr<RayTracingMaterialTemplate>(
            new RayTracingMaterialTemplate(std::move(Desc)));
        Tmpl->initialize(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        return Tmpl;
    }

    [[nodiscard]] rhi::EPipelineType getPipelineType() const noexcept override
    {
        return rhi::EPipelineType::RayTracing;
    }

    [[nodiscard]] std::shared_ptr<rhi::RPipeline> createPipeline(
        rhi::RDevice& Device,
        const MaterialPipelineRequest& Request) const override
    {
        const size_t PassIndex = static_cast<size_t>(Request.Pass);
        const auto& Pass = Descriptor.Passes[PassIndex];
        const auto CompileFlags = Request.CompileFlagsOverride
            .value_or(Pass.CompileFlags.value_or(Descriptor.CompileFlags));

        std::vector<rhi::PipelineShaderStage> Stages;
        auto addStage = [&](const std::shared_ptr<rhi::RShader>& Shader)
        {
            if (Shader) Stages.push_back({ .Shader = Shader });
        };

        addStage(PassRayGenShaders[PassIndex]);
        addStage(PassMissShaders[PassIndex]);
        addStage(PassClosestHitShaders[PassIndex]);
        addStage(PassAnyHitShaders[PassIndex]);
        addStage(PassIntersectionShaders[PassIndex]);
        addStage(PassCallableShaders[PassIndex]);

        rhi::RayTracingPipelineDescriptor RT {
            .Layout            = PipelineLayout,
            .Stages            = std::move(Stages),
            .Groups            = Descriptor.ShaderGroups,
            .MaxRecursionDepth = Descriptor.MaxRecursionDepth,
            .Compile           = { .Flags = CompileFlags },
            .DebugName         = Descriptor.Name + ".Pass" + std::to_string(PassIndex)
        };

        return Device.createRayTracingPipeline(RT);
    }

private:
    explicit RayTracingMaterialTemplate(MaterialTemplateDescriptor D)
        : MaterialTemplateBase(std::move(D)) {}

    void initialize(rhi::RDevice& Device,
                    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
                    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts,
                    uint32_t MaterialSetIndex)
    {
        buildMaterialSetLayout(Device);
        buildPipelineLayout(Device, FrameLayout, ExtraLayouts, MaterialSetIndex);
        buildParameterLookup();
        buildPassShaders(Device);
    }

    void buildPassShaders(rhi::RDevice& Device)
    {
        auto buildPassShader = [&](const std::optional<rhi::ShaderDescriptor>& Override,
                                   const std::optional<rhi::ShaderDescriptor>& Fallback)
        {
            if (Override) return createShader(Device, *Override);
            if (Fallback) return createShader(Device, *Fallback);
            return std::shared_ptr<rhi::RShader> {};
        };

        for (size_t Index = 0; Index < MaterialPassCount; ++Index)
        {
            const auto& Pass = Descriptor.Passes[Index];
            PassRayGenShaders[Index] = buildPassShader(
                Pass.Shaders.RayGeneration,
                Descriptor.Shaders.RayGeneration);
            PassMissShaders[Index] = buildPassShader(
                Pass.Shaders.Miss,
                Descriptor.Shaders.Miss);
            PassClosestHitShaders[Index] = buildPassShader(
                Pass.Shaders.ClosestHit,
                Descriptor.Shaders.ClosestHit);
            PassAnyHitShaders[Index] = buildPassShader(
                Pass.Shaders.AnyHit,
                Descriptor.Shaders.AnyHit);
            PassIntersectionShaders[Index] = buildPassShader(
                Pass.Shaders.Intersection,
                Descriptor.Shaders.Intersection);
            PassCallableShaders[Index] = buildPassShader(
                Pass.Shaders.Callable,
                Descriptor.Shaders.Callable);
        }
    }

private:
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassRayGenShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassMissShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassClosestHitShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassAnyHitShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassIntersectionShaders {};
    std::array<std::shared_ptr<rhi::RShader>, MaterialPassCount> PassCallableShaders {};
};

class DynamicUniformRing
{
public:
    void initialize(rhi::RDevice& Device, uint32_t SlotSize,
                    uint32_t SlotCount, uint32_t FramesInFlight)
    {
        const uint64_t Alignment = Device.getLimits().MinUniformBufferOffsetAlignment;
        SlotSizeAligned = static_cast<uint32_t>(alignUp(SlotSize, Alignment));
        SlotsPerFrame   = SlotCount;

        Buffer = Device.createBuffer({
            .Size               = (rhi::DeviceSizeType)SlotSizeAligned * SlotCount * FramesInFlight,
            .Usage              = rhi::EBufferUsage_t::Uniform,
            .MemoryUsage        = rhi::EMemoryUsage::CPUToGPU,
            .MemoryProperty     = rhi::EMemoryProperty_t::HostVisible
                                | rhi::EMemoryProperty_t::HostCoherent,
            .PersistentlyMapped = true,
            .DebugName          = "MaterialDynamicUBO"
        });

        MappedBase = Buffer->map();
    }

    void shutdown()
    {
        if (Buffer && MappedBase)
        {
            Buffer->unmap();
            MappedBase = nullptr;
        }
        Buffer.reset();
    }

    void beginFrame(uint32_t FrameIndex)
    {
        CurrentFrameBase = static_cast<uint64_t>(FrameIndex) * SlotsPerFrame * SlotSizeAligned;
        Cursor = 0;
    }

    void beginFrame(uint32_t FrameIndex, uint64_t CompletedFenceValue)
    {
        for (auto It = InFlightFrames.begin(); It != InFlightFrames.end();)
        {
            if (It->FenceValue <= CompletedFenceValue)
                It = InFlightFrames.erase(It);
            else
                ++It;
        }

        beginFrame(FrameIndex);
    }

    [[nodiscard]] uint32_t allocate()
    {
        uint32_t Off = static_cast<uint32_t>(CurrentFrameBase + Cursor);
        Cursor += SlotSizeAligned;
        return Off;
    }

    void endFrame(uint64_t FenceValue)
    {
        InFlightFrames.push_back({
            .FrameBase  = CurrentFrameBase,
            .FenceValue = FenceValue
        });
    }

    void write(uint32_t Offset, const void* Data, uint32_t Size)
    {
        std::memcpy(static_cast<std::byte*>(MappedBase) + Offset, Data, Size);
    }

    [[nodiscard]] const std::shared_ptr<rhi::RBuffer>& getBuffer() const noexcept
    {
        return Buffer;
    }

    [[nodiscard]] uint32_t getSlotSizeAligned() const noexcept { return SlotSizeAligned; }

private:
    static uint64_t alignUp(uint64_t V, uint64_t A) { return (V + A - 1) / A * A; }

    struct InFlightFrame
    {
        uint64_t FrameBase { 0 };
        uint64_t FenceValue { 0 };
    };

    std::shared_ptr<rhi::RBuffer> Buffer;
    void*    MappedBase { nullptr };
    uint32_t SlotSizeAligned { 0 };
    uint32_t SlotsPerFrame { 0 };
    uint64_t CurrentFrameBase { 0 };
    uint64_t Cursor { 0 };
    std::vector<InFlightFrame> InFlightFrames;
};

class MaterialSystem
{
public:
    using TemplateHandle = MaterialTemplateHandle;
    using InstanceHandle = MaterialInstanceHandle;
    using VertexFactoryHandleType = VertexFactoryHandle;

    using TemplatePoolType = core::HandlePool<
        MaterialTemplate, std::shared_ptr<MaterialTemplateBase>>;
    using InstancePoolType = core::HandlePool<
        MaterialInstance, std::shared_ptr<MaterialInstanceBase>>;
    using VertexFactoryPoolType = core::HandlePool<
        VertexFactory, std::shared_ptr<VertexFactoryBase>>;

    explicit MaterialSystem(rhi::RDevice& InDevice)
        : Device(InDevice)
        , TemplatePool(core::HandleManager::self()
              .getPool<MaterialTemplate, std::shared_ptr<MaterialTemplateBase>>())
        , InstancePool(core::HandleManager::self()
              .getPool<MaterialInstance, std::shared_ptr<MaterialInstanceBase>>())
          , VertexFactoryPool(core::HandleManager::self()
              .getPool<VertexFactory, std::shared_ptr<VertexFactoryBase>>())
    {}

    // 池归 HandleManager，析构时只清理本系统维护的映射
    ~MaterialSystem();

    // --------------------------------------------------------
    // 模板管理
    // --------------------------------------------------------
    TemplateHandle registerTemplate(
        const MaterialTemplateDescriptor& Desc,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts = {});

    // 有实例引用时会拒绝卸载
    void unregisterTemplate(TemplateHandle H);

    [[nodiscard]] MaterialTemplateBase*       getTemplate(TemplateHandle H) noexcept;
    [[nodiscard]] const MaterialTemplateBase* getTemplate(TemplateHandle H) const noexcept;

    // 返回 shared_ptr，避免热重载后裸指针悬空
    [[nodiscard]] std::shared_ptr<MaterialTemplateBase>
    getTemplateShared(TemplateHandle H) const noexcept;

    [[nodiscard]] TemplateHandle findTemplateByName(std::string_view Name) const noexcept;

    [[nodiscard]] std::vector<TemplateHandle>
    getTemplatesByCategory(std::string_view Category) const;

    bool reloadTemplate(std::string_view Name,
                        const MaterialTemplateDescriptor& NewDesc,
                        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
                        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts = {});

    // --------------------------------------------------------
    // 顶点工厂管理
    // --------------------------------------------------------
    VertexFactoryHandleType registerVertexFactory(const VertexFactoryDescriptor& Desc);
    void unregisterVertexFactory(VertexFactoryHandleType H);

    [[nodiscard]] VertexFactoryBase* getVertexFactory(VertexFactoryHandleType H) noexcept;
    [[nodiscard]] const VertexFactoryBase* getVertexFactory(VertexFactoryHandleType H) const noexcept;
    [[nodiscard]] std::shared_ptr<VertexFactoryBase> getVertexFactoryShared(VertexFactoryHandleType H) const noexcept;
    [[nodiscard]] VertexFactoryHandleType findVertexFactoryByName(std::string_view Name) const noexcept;

    // --------------------------------------------------------
    // 实例管理
    // --------------------------------------------------------
    [[nodiscard]] InstanceHandle createInstance(TemplateHandle H);
    [[nodiscard]] InstanceHandle createInstance(std::string_view TemplateName);

    void destroyInstance(InstanceHandle H);

    [[nodiscard]] MaterialInstanceBase*       getInstance(InstanceHandle H) noexcept;
    [[nodiscard]] const MaterialInstanceBase* getInstance(InstanceHandle H) const noexcept;

    [[nodiscard]] InstanceHandle getOrCreateNamed(
        std::string_view TemplateName, std::string_view InstanceName);

    // --------------------------------------------------------
    // Pipeline 缓存
    // --------------------------------------------------------
    [[nodiscard]] std::shared_ptr<rhi::RPipeline> getOrCreatePipeline(
        TemplateHandle H,
        const MaterialPipelineRequest& Request = {});

    [[nodiscard]] std::shared_ptr<rhi::RPipeline> getOrCreatePipeline(
        TemplateHandle H,
        VertexFactoryHandleType VF,
        const MaterialPipelineRequest& Request = {});

    [[nodiscard]] std::shared_ptr<rhi::RPipeline> getOrCreatePipeline(
        const MaterialTemplateBase& Template,
        const MaterialPipelineRequest& Request = {});

    void warmupPipelines(
        TemplateHandle H,
        std::span<const MaterialPipelineRequest> Requests);

    void warmupPipelines(
        TemplateHandle H,
        VertexFactoryHandleType VF,
        std::span<const MaterialPipelineRequest> Requests);

    // --------------------------------------------------------
    // 统计 / 调试
    // --------------------------------------------------------
    [[nodiscard]] size_t getTemplateCount() const noexcept { return TemplatePool.size(); }
    [[nodiscard]] size_t getInstanceCount() const noexcept { return InstancePool.size(); }
    [[nodiscard]] size_t getVertexFactoryCount() const noexcept { return VertexFactoryPool.size(); }

    [[nodiscard]] rhi::RDevice& getDevice() noexcept { return Device; }

    // 供 MaterialInstanceBase 访问模板
    [[nodiscard]] const MaterialTemplateBase*
    getTemplateForInstance(MaterialTemplateHandle H) const noexcept
    {
        return getTemplate(H);
    }

private:
    friend class MaterialInstanceBase;

    struct PipelineCacheKey
    {
        core::HandleIdType MaterialId { core::InvalidHandleId };
        uint32_t MaterialVersion { 0 };
        core::HandleIdType VertexFactoryId { core::InvalidHandleId };
        core::HandleGenerationType VertexFactoryVersion { core::InvalidHandleGeneration };
        EPassType Pass { EPassType::BasePass };

        bool HasRenderingOverride { false };
        rhi::RenderingSignature RenderingOverride {};

        bool HasVertexInputOverride { false };
        uint64_t VertexInputHash { 0 };

        bool HasTopologyOverride { false };
        rhi::EPrimitiveTopology TopologyOverride { rhi::EPrimitiveTopology::TriangleList };

        bool HasCompileFlagsOverride { false };
        rhi::EPipelineCompileFlags CompileFlagsOverride {};

        bool operator==(const PipelineCacheKey& Other) const noexcept = default;
    };

    struct PipelineCacheKeyHash
    {
        [[nodiscard]] size_t operator()(const PipelineCacheKey& Key) const noexcept;
    };

    struct BindGroupCacheKey
    {
        core::HandleIdType MaterialId { core::InvalidHandleId };
        uint32_t MaterialVersion { 0 };
        const rhi::RBuffer* UniformBuffer { nullptr };
        uint32_t UniformBufferOffset { 0 };
        uint64_t TextureSetHash { 0 };

        bool operator==(const BindGroupCacheKey& Other) const noexcept = default;
    };

    struct BindGroupCacheKeyHash
    {
        [[nodiscard]] size_t operator()(const BindGroupCacheKey& Key) const noexcept;
    };

    TemplateHandle registerTemplateInternal(
        const MaterialTemplateDescriptor& Desc,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts);

    [[nodiscard]] std::shared_ptr<rhi::RBindGroup> getOrCreateMaterialBindGroup(
        const MaterialTemplateBase& Template,
        const std::shared_ptr<rhi::RBuffer>& DynamicUBO,
        uint32_t UBOOffset,
        const std::unordered_map<uint32_t, rhi::BindGroupResource>& TextureBindings,
        uint64_t TextureSetHash);

    void evictTemplateCaches(core::HandleIdType TemplateStableId) noexcept;
    void evictVertexFactoryCaches(VertexFactoryHandleType VertexFactory) noexcept;

private:
    rhi::RDevice& Device;

    // 引用 HandleManager 中的池，不作为所有权成员
    TemplatePoolType& TemplatePool;
    InstancePoolType& InstancePool;
    VertexFactoryPoolType& VertexFactoryPool;

    std::unordered_map<std::string, TemplateHandle, TransparentStringHash, std::equal_to<>> TemplateNameToHandle;
    std::unordered_map<std::string, InstanceHandle, TransparentStringHash, std::equal_to<>> NamedInstances;
    std::unordered_map<std::string, VertexFactoryHandleType, TransparentStringHash, std::equal_to<>> VertexFactoryNameToHandle;

    struct TemplateLayoutCache
    {
        std::shared_ptr<rhi::RBindGroupLayout>              FrameLayout;
        std::vector<std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts;
    };
    std::unordered_map<core::HandleIdType, TemplateLayoutCache> TemplateLayouts;

    std::unordered_map<PipelineCacheKey, std::shared_ptr<rhi::RPipeline>, PipelineCacheKeyHash> PipelineCache;
    std::unordered_map<BindGroupCacheKey, std::weak_ptr<rhi::RBindGroup>, BindGroupCacheKeyHash> BindGroupCache;

    std::mutex PipelineCacheMutex;
    std::mutex BindGroupCacheMutex;
};

} // namespace renderer