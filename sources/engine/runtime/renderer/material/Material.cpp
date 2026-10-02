
#include "Material.hpp"

#include <cstdint>
#include <type_traits>

namespace renderer
{

namespace
{

template <typename T>
constexpr uint64_t toU64(T Value) noexcept
{
    if constexpr (std::is_enum_v<T>)
    {
        return static_cast<uint64_t>(std::underlying_type_t<T>(Value));
    }
    else
    {
        return static_cast<uint64_t>(Value);
    }
}

constexpr void hashCombine(uint64_t& Seed, uint64_t Value) noexcept
{
    Seed ^= Value + 0x9e3779b97f4a7c15ull + (Seed << 6) + (Seed >> 2);
}

template <typename T>
void hashCombine(uint64_t& Seed, T Value) noexcept
{
    hashCombine(Seed, toU64(Value));
}

[[nodiscard]] uint64_t hashRenderingSignature(const rhi::RenderingSignature& Signature) noexcept
{
    uint64_t Hash = 0;
    hashCombine(Hash, Signature.ColorAttachmentCount);
    for (const auto Format : Signature.ColorFormats)
    {
        hashCombine(Hash, Format);
    }
    hashCombine(Hash, Signature.DepthFormat);
    hashCombine(Hash, Signature.StencilFormat);
    hashCombine(Hash, Signature.SampleCount);
    hashCombine(Hash, Signature.ViewMask);
    return Hash;
}

[[nodiscard]] uint64_t hashVertexInputState(const rhi::VertexInputState& State) noexcept
{
    uint64_t Hash = 0;

    hashCombine(Hash, State.Buffers.size());
    for (const auto& Buffer : State.Buffers)
    {
        hashCombine(Hash, Buffer.Binding);
        hashCombine(Hash, Buffer.Stride);
        hashCombine(Hash, Buffer.InputRate);
    }

    hashCombine(Hash, State.Attributes.size());
    for (const auto& Attr : State.Attributes)
    {
        hashCombine(Hash, Attr.Location);
        hashCombine(Hash, Attr.Binding);
        hashCombine(Hash, Attr.Format);
        hashCombine(Hash, Attr.Offset);
    }

    return Hash;
}

} // namespace

size_t MaterialSystem::PipelineCacheKeyHash::operator()(const PipelineCacheKey& Key) const noexcept
{
    uint64_t Hash = 0;
    hashCombine(Hash, Key.MaterialId);
    hashCombine(Hash, Key.MaterialVersion);
    hashCombine(Hash, Key.VertexFactoryId);
    hashCombine(Hash, Key.VertexFactoryVersion);
    hashCombine(Hash, Key.Pass);

    hashCombine(Hash, Key.HasRenderingOverride ? 1ull : 0ull);
    if (Key.HasRenderingOverride)
    {
        hashCombine(Hash, hashRenderingSignature(Key.RenderingOverride));
    }

    hashCombine(Hash, Key.HasVertexInputOverride ? 1ull : 0ull);
    if (Key.HasVertexInputOverride)
    {
        hashCombine(Hash, Key.VertexInputHash);
    }

    hashCombine(Hash, Key.HasTopologyOverride ? 1ull : 0ull);
    if (Key.HasTopologyOverride)
    {
        hashCombine(Hash, Key.TopologyOverride);
    }

    hashCombine(Hash, Key.HasCompileFlagsOverride ? 1ull : 0ull);
    if (Key.HasCompileFlagsOverride)
    {
        hashCombine(Hash, Key.CompileFlagsOverride.Value);
    }

    return static_cast<size_t>(Hash);
}

size_t MaterialSystem::BindGroupCacheKeyHash::operator()(const BindGroupCacheKey& Key) const noexcept
{
    uint64_t Hash = 0;
    hashCombine(Hash, Key.MaterialId);
    hashCombine(Hash, Key.MaterialVersion);
    hashCombine(Hash, reinterpret_cast<uint64_t>(Key.UniformBuffer));
    hashCombine(Hash, Key.UniformBufferOffset);
    hashCombine(Hash, Key.TextureSetHash);
    return static_cast<size_t>(Hash);
}

MaterialInstanceBase::ParameterHandle MaterialInstanceBase::getParameterHandle(std::string_view Name) noexcept
{
    return findAndCacheParameterHandle(Name);
}

MaterialInstanceBase::ParameterHandle MaterialInstanceBase::findAndCacheParameterHandle(std::string_view Name) noexcept
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return {};

    if (CachedParameterTemplateId != Tmpl->getStableId()
        || CachedParameterTemplateVersion != Tmpl->getVersion())
    {
        CachedParameterHandles.clear();
        CachedParameterTemplateId = Tmpl->getStableId();
        CachedParameterTemplateVersion = Tmpl->getVersion();

        if (UniformData.size() != Tmpl->getUniformBlockSize())
        {
            UniformData.resize(Tmpl->getUniformBlockSize(), std::byte{ 0 });
            IsUniformDirty = true;
        }
    }

    if (auto It = CachedParameterHandles.find(Name); It != CachedParameterHandles.end())
    {
        return It->second;
    }

    ParameterHandle Handle;
    if (const auto* Param = Tmpl->findParameter(Name))
    {
        Handle.Index = static_cast<uint32_t>(
            Param - Tmpl->getDescriptor().MaterialUniforms.Parameters.data());
        Handle.Offset = Param->Offset;
        Handle.Size = Param->Size;
        Handle.ArrayCount = Param->ArrayCount;
        Handle.Type = Param->Type;
    }

    CachedParameterHandles.emplace(std::string(Name), Handle);
    return Handle;
}

void MaterialInstanceBase::setSampler(
    std::string_view Name, std::shared_ptr<rhi::RSampler> Sampler)
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return;
    auto* Desc = Tmpl->findTexture(Name);
    if (!Desc) return;

    if (isCombinedType(Desc->Type))
    {
        auto& Binding = TextureBindings[Desc->Binding];
        if (auto* CIS = std::get_if<rhi::CombinedImageSamplerBinding>(&Binding))
        {
            CIS->Sampler = std::move(Sampler);
        }
        else
        {
            Binding = rhi::CombinedImageSamplerBinding{
                .View    = nullptr,
                .Sampler = std::move(Sampler),
                .Layout  = rhi::EDescriptorImageLayout::ShaderReadOnly
            };
        }
    }
    else
    {
        TextureBindings[Desc->Binding] = rhi::SamplerBinding{
            .Sampler = std::move(Sampler)
        };
    }
    IsTextureDirty = true;
}

void MaterialInstanceBase::setCombinedImageSampler(
    std::string_view Name,
    std::shared_ptr<rhi::RImageView> View,
    std::shared_ptr<rhi::RSampler>   Sampler)
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return;
    auto* Desc = Tmpl->findTexture(Name);
    if (!Desc) return;
    TextureBindings[Desc->Binding] = rhi::CombinedImageSamplerBinding{
        .View    = std::move(View),
        .Sampler = std::move(Sampler),
        .Layout  = rhi::EDescriptorImageLayout::ShaderReadOnly
    };
    IsTextureDirty = true;
}

void MaterialInstanceBase::setAccelerationStructure(
    std::string_view Name,
    std::shared_ptr<rhi::RAccelerationStructure> AS)
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return;
    auto* Desc = Tmpl->findTexture(Name);
    if (!Desc) return;
    TextureBindings[Desc->Binding] = rhi::AccelerationStructureBinding{
        .AccelerationStructure = std::move(AS)
    };
    IsTextureDirty = true;
}

uint64_t MaterialInstanceBase::computeTextureSetHash() const noexcept
{
    uint64_t H = 14695981039346656037ull;
    auto* Tmpl = getTemplate();
    if (!Tmpl) return 0;

    for (const auto& Tex : Tmpl->getTextureBindings())
    {
        auto it = TextureBindings.find(Tex.Binding);
        uint64_t SlotHash = 0;
        hashCombine(H, it != TextureBindings.end() ? 1ull : 0ull);
        if (it != TextureBindings.end())
        {
            std::visit([&](const auto& R) {
                using T = std::decay_t<decltype(R)>;
                if constexpr (std::is_same_v<T, rhi::CombinedImageSamplerBinding>)
                {
                    SlotHash = reinterpret_cast<uint64_t>(R.View.get())
                             ^ (reinterpret_cast<uint64_t>(R.Sampler.get()) << 1);
                }
                else if constexpr (std::is_same_v<T, rhi::TextureBinding>)
                {
                    SlotHash = reinterpret_cast<uint64_t>(R.View.get());
                }
                else if constexpr (std::is_same_v<T, rhi::SamplerBinding>)
                {
                    SlotHash = reinterpret_cast<uint64_t>(R.Sampler.get());
                }
                else if constexpr (std::is_same_v<T, rhi::AccelerationStructureBinding>)
                {
                    SlotHash = reinterpret_cast<uint64_t>(R.AccelerationStructure.get());
                }
            }, it->second);
        }
        hashCombine(H, SlotHash);
    }
    return H;
}

void MaterialInstanceBase::commit(
    rhi::RDevice& Device,
    const std::shared_ptr<rhi::RBuffer>& DynamicUBO,
    uint32_t UBOOffset)
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return;

    if (CachedBindGroupTemplateId != Tmpl->getStableId()
        || CachedBindGroupTemplateVersion != Tmpl->getVersion())
    {
        BindGroup.reset();
        IsTextureDirty = true;
        CachedBindGroupTemplateId = Tmpl->getStableId();
        CachedBindGroupTemplateVersion = Tmpl->getVersion();

        const uint32_t ReloadedBlockSize = Tmpl->getUniformBlockSize();
        if (UniformData.size() != ReloadedBlockSize)
        {
            UniformData.resize(ReloadedBlockSize, std::byte { 0 });
            IsUniformDirty = true;
        }
    }

    const uint32_t BlockSize = Tmpl->getUniformBlockSize();
    const uint64_t UniformAlign = Device.getLimits().MinUniformBufferOffsetAlignment;
    if (BlockSize > 0 && UniformAlign > 0 && (UBOOffset % UniformAlign) != 0)
        return;

    if (BlockSize > 0 && IsUniformDirty)
    {
        void* Mapped = DynamicUBO->map(UBOOffset, BlockSize);
        std::memcpy(Mapped, UniformData.data(), BlockSize);
        DynamicUBO->unmap();
        DynamicUBO->flush(UBOOffset, BlockSize);
        IsUniformDirty = false;
    }

    if (!BindGroup || IsTextureDirty)
    {
        if (System)
        {
            BindGroup = System->getOrCreateMaterialBindGroup(
                *Tmpl,
                DynamicUBO,
                UBOOffset,
                TextureBindings,
                computeTextureSetHash());
        }

        if (!BindGroup)
        {
            rhi::BindGroupDescriptor Desc;
            Desc.Layout = Tmpl->getMaterialSetLayout();

            if (BlockSize > 0)
            {
                Desc.Entries.push_back({
                    .Binding      = 0,
                    .ArrayElement = 0,
                    .Resource     = rhi::BufferBinding{
                        .Buffer = DynamicUBO,
                        .Offset = UBOOffset,
                        .Size   = BlockSize
                    }
                });
            }

            for (const auto& Tex : Tmpl->getTextureBindings())
            {
                auto It = TextureBindings.find(Tex.Binding);
                if (It == TextureBindings.end()) continue;

                Desc.Entries.push_back({
                    .Binding      = Tex.Binding,
                    .ArrayElement = 0,
                    .Resource     = It->second
                });
            }

            Desc.DebugName = Tmpl->getName() + ".BindGroup";
            BindGroup = Device.createBindGroup(Desc);
        }

        IsTextureDirty = false;
    }
}


MaterialSystem::~MaterialSystem()
{
    // 池归 HandleManager，不再 clear(). 
    // 这里只清理本系统维护的映射. 
    //
    // 注意：调用方需要在销毁 MaterialSystem 之前，先销毁所有实例与模板，
    //       否则实例析构时访问已销毁的 System 会出问题. 
    TemplateNameToHandle.clear();
    NamedInstances.clear();
    VertexFactoryNameToHandle.clear();
    TemplateLayouts.clear();

    {
        std::lock_guard Lock(PipelineCacheMutex);
        PipelineCache.clear();
    }
    {
        std::lock_guard Lock(BindGroupCacheMutex);
        BindGroupCache.clear();
    }
}

MaterialTemplateBase* MaterialSystem::getTemplate(TemplateHandle H) noexcept
{
    auto Ptr = TemplatePool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

const MaterialTemplateBase* MaterialSystem::getTemplate(TemplateHandle H) const noexcept
{
    auto Ptr = TemplatePool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

std::shared_ptr<MaterialTemplateBase>
MaterialSystem::getTemplateShared(TemplateHandle H) const noexcept
{
    auto* Ptr = TemplatePool.get(H);
    return Ptr ? *Ptr : nullptr;
}

MaterialTemplateHandle MaterialSystem::registerTemplateInternal(
    const MaterialTemplateDescriptor& Desc,
    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts)
{
    if (auto Existing = TemplateNameToHandle.find(Desc.Name); Existing != TemplateNameToHandle.end())
    {
        if (TemplatePool.isValid(Existing->second))
            return Existing->second;
    }

    std::shared_ptr<MaterialTemplateBase> Tmpl;

    if (Desc.Shaders.Compute)
        Tmpl = ComputeMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);
    else if (Desc.Shaders.RayGeneration)
        Tmpl = RayTracingMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);
    else
        Tmpl = GraphicsMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);

    if (!Tmpl || !Tmpl->isValid())
        return {};

    auto H = TemplatePool.create(std::move(Tmpl));
    if (auto* Created = TemplatePool.get(H); Created && *Created)
    {
        (*Created)->setCacheIdentity(H.Id, 1);
    }
    TemplateNameToHandle[Desc.Name] = H;

    TemplateLayoutCache Cache;
    Cache.FrameLayout = FrameLayout;
    for (const auto& L : ExtraLayouts) Cache.ExtraLayouts.push_back(L);
    TemplateLayouts[H.Id] = std::move(Cache);

    return H;
}

MaterialTemplateHandle MaterialSystem::registerTemplate(
    const MaterialTemplateDescriptor& Desc,
    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts)
{
    // 同名模板先走热重载路径，避免误销毁
    auto it = TemplateNameToHandle.find(Desc.Name);
    if (it != TemplateNameToHandle.end())
    {
        reloadTemplate(Desc.Name, Desc, FrameLayout, ExtraLayouts);
        return it->second;
    }
    return registerTemplateInternal(Desc, FrameLayout, ExtraLayouts);
}

VertexFactoryHandle MaterialSystem::registerVertexFactory(const VertexFactoryDescriptor& Desc)
{
    if (auto It = VertexFactoryNameToHandle.find(Desc.Name); It != VertexFactoryNameToHandle.end())
    {
        auto* Slot = VertexFactoryPool.get(It->second);
        if (!Slot) return {};

        auto NewFactory = GraphicsVertexFactory::create(Device, Desc);
        if (!NewFactory || !NewFactory->isValid()) return {};

        evictVertexFactoryCaches(It->second);
        NewFactory->setCacheIdentity(It->second.Id, It->second.Generation);
        *Slot = std::move(NewFactory);
        return It->second;
    }

    auto Factory = GraphicsVertexFactory::create(Device, Desc);
    if (!Factory || !Factory->isValid()) return {};

    auto H = VertexFactoryPool.create(std::move(Factory));
    if (auto* Created = VertexFactoryPool.get(H); Created && *Created)
    {
        (*Created)->setCacheIdentity(H.Id, H.Generation);
    }
    VertexFactoryNameToHandle[Desc.Name] = H;
    return H;
}

void MaterialSystem::unregisterVertexFactory(VertexFactoryHandle H)
{
    if (!VertexFactoryPool.isValid(H)) return;

    auto* Slot = VertexFactoryPool.get(H);
    if (!Slot || !*Slot) return;

    evictVertexFactoryCaches(H);
    VertexFactoryNameToHandle.erase((*Slot)->getName());
    VertexFactoryPool.destroy(H);
}

VertexFactoryBase* MaterialSystem::getVertexFactory(VertexFactoryHandle H) noexcept
{
    auto Ptr = VertexFactoryPool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

const VertexFactoryBase* MaterialSystem::getVertexFactory(VertexFactoryHandle H) const noexcept
{
    auto Ptr = VertexFactoryPool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

std::shared_ptr<VertexFactoryBase> MaterialSystem::getVertexFactoryShared(VertexFactoryHandle H) const noexcept
{
    auto* Ptr = VertexFactoryPool.get(H);
    return Ptr ? *Ptr : nullptr;
}

VertexFactoryHandle MaterialSystem::findVertexFactoryByName(std::string_view Name) const noexcept
{
    auto It = VertexFactoryNameToHandle.find(Name);
    return It != VertexFactoryNameToHandle.end() ? It->second : VertexFactoryHandle{};
}

void MaterialSystem::unregisterTemplate(TemplateHandle H)
{
    if (!TemplatePool.isValid(H)) return;

    auto* Slot = TemplatePool.get(H);
    if (!Slot || !*Slot) return;

    // 有实例引用时拒绝卸载
    if ((*Slot)->getInstanceRefCount() > 0) return;

    evictTemplateCaches((*Slot)->getStableId());
    TemplateNameToHandle.erase((*Slot)->getName());
    TemplateLayouts.erase(H.Id);
    TemplatePool.destroy(H);
}

MaterialTemplateHandle MaterialSystem::findTemplateByName(
    std::string_view Name) const noexcept
{
    auto it = TemplateNameToHandle.find(Name);
    return it != TemplateNameToHandle.end() ? it->second : MaterialTemplateHandle{};
}

std::vector<MaterialTemplateHandle> MaterialSystem::getTemplatesByCategory(
    std::string_view Category) const
{
    std::vector<MaterialTemplateHandle> Result;
    for (const auto& Entry : TemplateNameToHandle)
    {
        auto H = Entry.second;
        if (auto* Tmpl = getTemplate(H))
        {
            if (Tmpl->getCategory() == Category)
                Result.push_back(H);
        }
    }
    return Result;
}

bool MaterialSystem::reloadTemplate(
    std::string_view Name,
    const MaterialTemplateDescriptor& NewDesc,
    const std::shared_ptr<rhi::RBindGroupLayout>& FrameLayout,
    std::span<const std::shared_ptr<rhi::RBindGroupLayout>> ExtraLayouts)
{
    auto H = findTemplateByName(Name);
    if (!H.isValid()) return false;

    auto* Slot = TemplatePool.get(H);
    if (!Slot || !*Slot) return false;

    // 1. 创建新模板（不进名字表）
    std::shared_ptr<MaterialTemplateBase> NewTmpl;
    if (NewDesc.Shaders.Compute)
        NewTmpl = ComputeMaterialTemplate::create(Device, NewDesc, FrameLayout, ExtraLayouts);
    else if (NewDesc.Shaders.RayGeneration)
        NewTmpl = RayTracingMaterialTemplate::create(Device, NewDesc, FrameLayout, ExtraLayouts);
    else
        NewTmpl = GraphicsMaterialTemplate::create(Device, NewDesc, FrameLayout, ExtraLayouts);

    if (!NewTmpl || !NewTmpl->isValid()) return false;

    MaterialTemplateBase* OldTemplate = Slot->get();

    // 2. 接管旧模板上的实例引用计数
    NewTmpl->setInstanceRefCount((*Slot)->getInstanceRefCount());

    // 3. 失效旧模板缓存，再原地替换 shared_ptr
    const core::HandleIdType StableId = OldTemplate->getStableId();
    const uint32_t NextVersion = OldTemplate->getVersion() + 1;
    evictTemplateCaches(StableId);
    NewTmpl->setCacheIdentity(StableId, NextVersion);
    *Slot = std::move(NewTmpl);

    // 4. 更新 Layout 缓存
    TemplateLayoutCache Cache;
    Cache.FrameLayout = FrameLayout;
    for (const auto& L : ExtraLayouts) Cache.ExtraLayouts.push_back(L);
    TemplateLayouts[H.Id] = std::move(Cache);

    // 5. 名字映射保持不变（同句柄, 同 Generation）
    return true;
}

MaterialInstanceHandle MaterialSystem::createInstance(TemplateHandle H)
{
    if (!TemplatePool.isValid(H)) return {};

    auto Ptr = std::make_shared<MaterialInstanceBase>(this, H);
    return InstancePool.create(std::move(Ptr));
}

MaterialInstanceHandle MaterialSystem::createInstance(std::string_view TemplateName)
{
    auto H = findTemplateByName(TemplateName);
    if (!H.isValid()) return {};
    return createInstance(H);
}

void MaterialSystem::destroyInstance(InstanceHandle H)
{
    InstancePool.destroy(H);
}

MaterialInstanceBase* MaterialSystem::getInstance(InstanceHandle H) noexcept
{
    auto Ptr = InstancePool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

const MaterialInstanceBase* MaterialSystem::getInstance(InstanceHandle H) const noexcept
{
    auto Ptr = InstancePool.get(H);
    return Ptr ? Ptr->get() : nullptr;
}

MaterialInstanceHandle MaterialSystem::getOrCreateNamed(
    std::string_view TemplateName, std::string_view InstanceName)
{
    if (auto it = NamedInstances.find(InstanceName); it != NamedInstances.end())
    {
        if (InstancePool.isValid(it->second))
            return it->second;

        NamedInstances.erase(it);
    }

    auto H = createInstance(TemplateName);
    if (H.isValid()) NamedInstances[std::string(InstanceName)] = H;
    return H;
}

std::shared_ptr<rhi::RPipeline> MaterialSystem::getOrCreatePipeline(
    TemplateHandle H,
    const MaterialPipelineRequest& Request)
{
    if (!TemplatePool.isValid(H)) return nullptr;

    auto Tmpl = getTemplateShared(H);
    if (!Tmpl) return nullptr;

    return getOrCreatePipeline(*Tmpl, Request);
}

std::shared_ptr<rhi::RPipeline> MaterialSystem::getOrCreatePipeline(
    TemplateHandle H,
    VertexFactoryHandle VF,
    const MaterialPipelineRequest& Request)
{
    MaterialPipelineRequest Resolved = Request;
    Resolved.VertexFactory = VF;
    return getOrCreatePipeline(H, Resolved);
}

std::shared_ptr<rhi::RPipeline> MaterialSystem::getOrCreatePipeline(
    const MaterialTemplateBase& Template,
    const MaterialPipelineRequest& Request)
{
    MaterialPipelineRequest ResolvedRequest = Request;
    core::TypedHandle<VertexFactory> ResolvedVFHandle {};
    if (Request.VertexFactory.isValid())
    {
        auto VF = getVertexFactoryShared(Request.VertexFactory);
        if (VF)
        {
            ResolvedVFHandle = Request.VertexFactory;

            if (!ResolvedRequest.VertexShaderOverride)
                ResolvedRequest.VertexShaderOverride = VF->getVertexShader(Request.Pass);

            if (!ResolvedRequest.VertexInputOverride)
                ResolvedRequest.VertexInputOverride = VF->getVertexInput(Request.Pass);

            if (!ResolvedRequest.TopologyOverride)
                ResolvedRequest.TopologyOverride = VF->getTopology(Request.Pass);
        }
    }

    if (Template.getPipelineType() == rhi::EPipelineType::Graphics)
    {
        const bool HasVertexInput = ResolvedRequest.VertexInputOverride.has_value();
        const bool HasTopology = ResolvedRequest.TopologyOverride.has_value();
        if (!HasVertexInput || !HasTopology)
            return nullptr;
    }

    PipelineCacheKey Key;
    Key.MaterialId = Template.getStableId();
    Key.MaterialVersion = Template.getVersion();
    Key.VertexFactoryId = ResolvedVFHandle.Id;
    Key.VertexFactoryVersion = ResolvedVFHandle.Generation;
    Key.Pass = ResolvedRequest.Pass;

    if (ResolvedRequest.RenderingOverride)
    {
        Key.HasRenderingOverride = true;
        Key.RenderingOverride = *ResolvedRequest.RenderingOverride;
    }
    if (ResolvedRequest.VertexInputOverride)
    {
        Key.HasVertexInputOverride = true;
        Key.VertexInputHash = hashVertexInputState(*ResolvedRequest.VertexInputOverride);
    }
    if (ResolvedRequest.TopologyOverride)
    {
        Key.HasTopologyOverride = true;
        Key.TopologyOverride = *ResolvedRequest.TopologyOverride;
    }
    if (ResolvedRequest.CompileFlagsOverride)
    {
        Key.HasCompileFlagsOverride = true;
        Key.CompileFlagsOverride = *ResolvedRequest.CompileFlagsOverride;
    }

    {
        std::lock_guard Lock(PipelineCacheMutex);
        if (auto It = PipelineCache.find(Key); It != PipelineCache.end())
        {
            return It->second;
        }
    }

    auto Pipeline = Template.createPipeline(Device, ResolvedRequest);
    if (!Pipeline) return nullptr;

    std::lock_guard Lock(PipelineCacheMutex);
    auto [It, Inserted] = PipelineCache.emplace(Key, Pipeline);
    if (!Inserted)
        return It->second;

    return Pipeline;
}

void MaterialSystem::warmupPipelines(
    TemplateHandle H,
    std::span<const MaterialPipelineRequest> Requests)
{
    for (const auto& Request : Requests)
    {
        static_cast<void>(getOrCreatePipeline(H, Request));
    }
}

void MaterialSystem::warmupPipelines(
    TemplateHandle H,
    VertexFactoryHandle VF,
    std::span<const MaterialPipelineRequest> Requests)
{
    for (const auto& Request : Requests)
    {
        static_cast<void>(getOrCreatePipeline(H, VF, Request));
    }
}

std::shared_ptr<rhi::RBindGroup> MaterialSystem::getOrCreateMaterialBindGroup(
    const MaterialTemplateBase& Template,
    const std::shared_ptr<rhi::RBuffer>& DynamicUBO,
    uint32_t UBOOffset,
    const std::unordered_map<uint32_t, rhi::BindGroupResource>& TextureBindings,
    uint64_t TextureSetHash)
{
    const uint32_t BlockSize = Template.getUniformBlockSize();

    BindGroupCacheKey Key {
        .MaterialId = Template.getStableId(),
        .MaterialVersion = Template.getVersion(),
        .UniformBuffer = BlockSize > 0 ? DynamicUBO.get() : nullptr,
        .UniformBufferOffset = BlockSize > 0 ? UBOOffset : 0,
        .TextureSetHash = TextureSetHash
    };

    {
        std::lock_guard Lock(BindGroupCacheMutex);
        if (auto It = BindGroupCache.find(Key); It != BindGroupCache.end())
        {
            if (auto Cached = It->second.lock())
                return Cached;

            BindGroupCache.erase(It);
        }
    }

    rhi::BindGroupDescriptor Desc;
    Desc.Layout = Template.getMaterialSetLayout();

    if (BlockSize > 0)
    {
        Desc.Entries.push_back({
            .Binding      = 0,
            .ArrayElement = 0,
            .Resource     = rhi::BufferBinding{
                .Buffer = DynamicUBO,
                .Offset = UBOOffset,
                .Size   = BlockSize
            }
        });
    }

    for (const auto& Tex : Template.getTextureBindings())
    {
        auto It = TextureBindings.find(Tex.Binding);
        if (It == TextureBindings.end()) continue;

        Desc.Entries.push_back({
            .Binding      = Tex.Binding,
            .ArrayElement = 0,
            .Resource     = It->second
        });
    }

    Desc.DebugName = Template.getName() + ".BindGroup";
    auto BindGroup = Device.createBindGroup(Desc);
    if (!BindGroup) return nullptr;

    {
        std::lock_guard Lock(BindGroupCacheMutex);
        BindGroupCache[Key] = BindGroup;
    }

    return BindGroup;
}

void MaterialSystem::evictTemplateCaches(core::HandleIdType TemplateStableId) noexcept
{
    if (TemplateStableId == core::InvalidHandleId) return;

    {
        std::lock_guard Lock(PipelineCacheMutex);
        for (auto It = PipelineCache.begin(); It != PipelineCache.end();)
        {
            if (It->first.MaterialId == TemplateStableId)
                It = PipelineCache.erase(It);
            else
                ++It;
        }
    }

    {
        std::lock_guard Lock(BindGroupCacheMutex);
        for (auto It = BindGroupCache.begin(); It != BindGroupCache.end();)
        {
            if (It->first.MaterialId == TemplateStableId)
                It = BindGroupCache.erase(It);
            else
                ++It;
        }
    }
}

void MaterialSystem::evictVertexFactoryCaches(VertexFactoryHandle VertexFactory) noexcept
{
    if (!VertexFactory.isValid()) return;

    std::lock_guard Lock(PipelineCacheMutex);
    for (auto It = PipelineCache.begin(); It != PipelineCache.end();)
    {
        if (It->first.VertexFactoryId == VertexFactory.Id
            && It->first.VertexFactoryVersion == VertexFactory.Generation)
            It = PipelineCache.erase(It);
        else
            ++It;
    }
}


MaterialInstanceBase::MaterialInstanceBase(
    MaterialSystem* InSystem, MaterialTemplateHandle InTemplateHandle)
    : System(InSystem)
    , TemplateHandle(InTemplateHandle)
{
    if (System)
    {
        if (auto Tmpl = System->getTemplateShared(TemplateHandle))
        {
            UniformData.resize(Tmpl->getUniformBlockSize(), std::byte{ 0 });
            CachedParameterTemplateId = Tmpl->getStableId();
            CachedParameterTemplateVersion = Tmpl->getVersion();
            CachedBindGroupTemplateId = Tmpl->getStableId();
            CachedBindGroupTemplateVersion = Tmpl->getVersion();
            Tmpl->addInstanceRef();
        }
    }
}

MaterialInstanceBase::~MaterialInstanceBase()
{
    if (System)
    {
        if (auto Tmpl = System->getTemplateShared(TemplateHandle))
        {
            Tmpl->releaseInstanceRef();
        }
    }
}

const MaterialTemplateBase* MaterialInstanceBase::getTemplate() const noexcept
{
    return System ? System->getTemplate(TemplateHandle) : nullptr;
}

void MaterialInstanceBase::setTexture(
    std::string_view Name, std::shared_ptr<rhi::RImageView> View)
{
    auto* Tmpl = getTemplate();
    if (!Tmpl) return;
    auto* Desc = Tmpl->findTexture(Name);
    if (!Desc) return;

    if (isCombinedType(Desc->Type))
    {
        auto& Binding = TextureBindings[Desc->Binding];
        if (auto* CIS = std::get_if<rhi::CombinedImageSamplerBinding>(&Binding))
        {
            CIS->View = std::move(View);
        }
        else
        {
            Binding = rhi::CombinedImageSamplerBinding{
                .View    = std::move(View),
                .Sampler = nullptr,
                .Layout  = rhi::EDescriptorImageLayout::ShaderReadOnly
            };
        }
    }
    else
    {
        TextureBindings[Desc->Binding] = rhi::TextureBinding{
            .View   = std::move(View),
            .Layout = rhi::EDescriptorImageLayout::ShaderReadOnly
        };
    }
    IsTextureDirty = true;
}

}