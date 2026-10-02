// renderer/MaterialSystem.hpp
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <core/functions/HandleManager.hpp>
#include <RHI.hpp>

namespace renderer
{

CORE_DEFINE_HANDLE(MaterialTemplate);   // -> struct MaterialTemplate; using MaterialTemplateHandle
CORE_DEFINE_HANDLE(MaterialInstance);   // -> struct MaterialInstance; using MaterialInstanceHandle

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

    rhi::VertexInputState   VertexInput;
    rhi::EPrimitiveTopology Topology { rhi::EPrimitiveTopology::TriangleList };

    rhi::RasterizerState   Rasterizer;
    rhi::MultisampleState  Multisample;
    rhi::DepthStencilState DepthStencil;
    rhi::BlendState        Blend;

    uint32_t                                 MaxRecursionDepth { 1 };
    std::vector<rhi::RayTracingShaderGroup>  ShaderGroups;

    rhi::RenderingSignature    Rendering;
    rhi::EPipelineCompileFlags CompileFlags {};
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
    [[nodiscard]] const std::shared_ptr<rhi::RPipeline>&        getPipeline() const noexcept { return Pipeline; }
    [[nodiscard]] const std::shared_ptr<rhi::RPipelineLayout>&  getPipelineLayout() const noexcept { return PipelineLayout; }
    [[nodiscard]] const std::shared_ptr<rhi::RBindGroupLayout>& getMaterialSetLayout() const noexcept { return MaterialSetLayout; }

    [[nodiscard]] uint32_t getMaterialSetIndex() const noexcept { return MaterialSetIndex; }
    [[nodiscard]] uint32_t getUniformBlockSize() const noexcept { return Descriptor.MaterialUniforms.Size; }
    [[nodiscard]] bool     isValid() const noexcept { return Pipeline != nullptr; }

    // ---- 参数查询（用索引，避免裸指针失效） ----
    [[nodiscard]] const ParameterDescriptor* findParameter(std::string_view Name) const noexcept
    {
        auto it = ParameterLookup.find(std::string(Name));
        if (it == ParameterLookup.end()) return nullptr;
        return &Descriptor.MaterialUniforms.Parameters[it->second];
    }

    [[nodiscard]] const TextureBindingDescriptor* findTexture(std::string_view Name) const noexcept
    {
        auto it = TextureLookup.find(std::string(Name));
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

    virtual void createPipeline(rhi::RDevice& Device) = 0;

protected:
    MaterialTemplateDescriptor Descriptor;

    std::shared_ptr<rhi::RBindGroupLayout> MaterialSetLayout;
    std::shared_ptr<rhi::RPipelineLayout>  PipelineLayout;
    std::shared_ptr<rhi::RPipeline>        Pipeline;

    uint32_t MaterialSetIndex { 0 };

    std::unordered_map<std::string, uint32_t> ParameterLookup;
    std::unordered_map<std::string, uint32_t> TextureLookup;

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

    // ---- 参数写入 ----
    void setFloat(std::string_view Name, float V) noexcept
    {
        writeParameter(Name, EParameterType::Float, &V, sizeof(V));
    }
    void setInt(std::string_view Name, int32_t V) noexcept
    {
        writeParameter(Name, EParameterType::Int, &V, sizeof(V));
    }
    void setUInt(std::string_view Name, uint32_t V) noexcept
    {
        writeParameter(Name, EParameterType::UInt, &V, sizeof(V));
    }
    void setBool(std::string_view Name, bool V) noexcept
    {
        uint32_t I = V ? 1u : 0u;
        writeParameter(Name, EParameterType::Bool, &I, sizeof(I));
    }
    void setVec2(std::string_view Name, float X, float Y) noexcept
    {
        float V[2] = { X, Y };
        writeParameter(Name, EParameterType::Vec2, V, sizeof(V));
    }
    void setVec3(std::string_view Name, float X, float Y, float Z) noexcept
    {
        float V[3] = { X, Y, Z };
        writeParameter(Name, EParameterType::Vec3, V, sizeof(V));
    }
    void setVec4(std::string_view Name, const std::array<float, 4>& V) noexcept
    {
        writeParameter(Name, EParameterType::Vec4, V.data(), sizeof(float) * 4);
    }
    void setVec4(std::string_view Name, float X, float Y, float Z, float W) noexcept
    {
        float V[4] = { X, Y, Z, W };
        writeParameter(Name, EParameterType::Vec4, V, sizeof(V));
    }
    void setMat4(std::string_view Name, const float M[16]) noexcept
    {
        writeParameter(Name, EParameterType::Mat4, M, sizeof(float) * 16);
    }
    void writeRaw(std::string_view Name, std::span<const std::byte> Data) noexcept
    {
        auto* Tmpl = getTemplate();
        if (!Tmpl) return;
        auto* P = Tmpl->findParameter(Name);
        if (!P) return;
        const uint32_t TotalSize = P->Size * P->ArrayCount;
        if (Data.size() > TotalSize) return;
        if (P->Offset + TotalSize > UniformData.size()) return;
        std::memcpy(UniformData.data() + P->Offset, Data.data(), Data.size());
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
    void writeParameter(std::string_view Name, EParameterType Expected,
                        const void* Data, size_t Size) noexcept
    {
        auto* Tmpl = getTemplate();
        if (!Tmpl) return;
        auto* P = Tmpl->findParameter(Name);
        if (!P || P->Type != Expected || P->Offset + Size > UniformData.size())
            return;
        std::memcpy(UniformData.data() + P->Offset, Data, Size);
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

    std::vector<std::byte>                                UniformData;
    std::unordered_map<uint32_t, rhi::BindGroupResource>  TextureBindings;
    std::shared_ptr<rhi::RBindGroup>                      BindGroup;

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
        createPipeline(Device);
    }

    void createPipeline(rhi::RDevice& Device) override
    {
        std::shared_ptr<rhi::RShader> VS, FS;
        if (Descriptor.Shaders.Vertex) VS = createShader(Device, *Descriptor.Shaders.Vertex);
        if (Descriptor.Shaders.Pixel)  FS = createShader(Device, *Descriptor.Shaders.Pixel);

        rhi::GraphicsPipelineDescriptor GP {
            .Layout        = PipelineLayout,
            .Vertex        = { .Shader = VS },
            .Pixel         = { .Shader = FS },
            .VertexInput   = Descriptor.VertexInput,
            .InputAssembly = { .Topology = Descriptor.Topology },
            .Rasterizer    = Descriptor.Rasterizer,
            .Multisample   = Descriptor.Multisample,
            .DepthStencil  = Descriptor.DepthStencil,
            .Blend         = Descriptor.Blend,
            .Rendering     = Descriptor.Rendering,
            .DynamicStates = rhi::EDynamicStates(rhi::EDynamicState_t::Viewport)
                           | rhi::EDynamicState_t::Scissor,
            .Compile       = { .Flags = Descriptor.CompileFlags },
            .DebugName     = Descriptor.Name
        };

        Pipeline = Device.createGraphicsPipeline(GP);
    }
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
        createPipeline(Device);
    }

    void createPipeline(rhi::RDevice& Device) override
    {
        std::shared_ptr<rhi::RShader> CS;
        if (Descriptor.Shaders.Compute) CS = createShader(Device, *Descriptor.Shaders.Compute);

        rhi::ComputePipelineDescriptor CP {
            .Layout    = PipelineLayout,
            .Compute   = { .Shader = CS },
            .Compile   = { .Flags = Descriptor.CompileFlags },
            .DebugName = Descriptor.Name
        };

        Pipeline = Device.createComputePipeline(CP);
    }
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
        createPipeline(Device);
    }

    void createPipeline(rhi::RDevice& Device) override
    {
        std::vector<rhi::PipelineShaderStage> Stages;
        auto addStage = [&](const std::optional<rhi::ShaderDescriptor>& S)
        {
            if (S) Stages.push_back({ .Shader = createShader(Device, *S) });
        };

        addStage(Descriptor.Shaders.RayGeneration);
        addStage(Descriptor.Shaders.Miss);
        addStage(Descriptor.Shaders.ClosestHit);
        addStage(Descriptor.Shaders.AnyHit);
        addStage(Descriptor.Shaders.Intersection);
        addStage(Descriptor.Shaders.Callable);

        rhi::RayTracingPipelineDescriptor RT {
            .Layout            = PipelineLayout,
            .Stages            = std::move(Stages),
            .Groups            = Descriptor.ShaderGroups,
            .MaxRecursionDepth = Descriptor.MaxRecursionDepth,
            .Compile           = { .Flags = Descriptor.CompileFlags },
            .DebugName         = Descriptor.Name
        };

        Pipeline = Device.createRayTracingPipeline(RT);
    }
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

    [[nodiscard]] uint32_t allocate()
    {
        uint32_t Off = static_cast<uint32_t>(CurrentFrameBase + Cursor);
        Cursor += SlotSizeAligned;
        return Off;
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

    std::shared_ptr<rhi::RBuffer> Buffer;
    void*    MappedBase { nullptr };
    uint32_t SlotSizeAligned { 0 };
    uint32_t SlotsPerFrame { 0 };
    uint64_t CurrentFrameBase { 0 };
    uint64_t Cursor { 0 };
};

class MaterialSystem
{
public:
    using TemplateHandle = MaterialTemplateHandle;
    using InstanceHandle = MaterialInstanceHandle;

    using TemplatePoolType = core::HandlePool<
        MaterialTemplate, std::shared_ptr<MaterialTemplateBase>>;
    using InstancePoolType = core::HandlePool<
        MaterialInstance, std::shared_ptr<MaterialInstanceBase>>;

    explicit MaterialSystem(rhi::RDevice& InDevice)
        : Device(InDevice)
        , TemplatePool(core::HandleManager::self()
              .getPool<MaterialTemplate, std::shared_ptr<MaterialTemplateBase>>())
        , InstancePool(core::HandleManager::self()
              .getPool<MaterialInstance, std::shared_ptr<MaterialInstanceBase>>())
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
    // 统计 / 调试
    // --------------------------------------------------------
    [[nodiscard]] size_t getTemplateCount() const noexcept { return TemplatePool.size(); }
    [[nodiscard]] size_t getInstanceCount() const noexcept { return InstancePool.size(); }

    [[nodiscard]] rhi::RDevice& getDevice() noexcept { return Device; }

    // 供 MaterialInstanceBase 访问模板
    [[nodiscard]] const MaterialTemplateBase*
    getTemplateForInstance(MaterialTemplateHandle H) const noexcept
    {
        return getTemplate(H);
    }

private:
    TemplateHandle registerTemplateInternal(
        const MaterialTemplateDescriptor& Desc,
        const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
        std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts);

private:
    rhi::RDevice& Device;

    // 引用 HandleManager 中的池，不作为所有权成员
    TemplatePoolType& TemplatePool;
    InstancePoolType& InstancePool;

    std::unordered_map<std::string, TemplateHandle> TemplateNameToHandle;
    std::unordered_map<std::string, InstanceHandle> NamedInstances;

    struct TemplateLayoutCache
    {
        std::shared_ptr<rhi::RBindGroupLayout>              FrameLayout;
        std::vector<std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts;
    };
    std::unordered_map<core::HandleIdType, TemplateLayoutCache> TemplateLayouts;
};

} // namespace renderer