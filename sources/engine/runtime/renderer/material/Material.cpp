
#include <Material.hpp>

namespace renderer
{

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
        H ^= SlotHash + 0x9e3779b97f4a7c15ull + (H << 6) + (H >> 2);
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

    const uint32_t BlockSize = Tmpl->getUniformBlockSize();

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
        rhi::BindGroupDescriptor Desc;
        Desc.Layout = Tmpl->getMaterialSetLayout();

        if (BlockSize > 0)
        {
            Desc.Entries.push_back({
                .Binding      = 0,
                .ArrayElement = 0,
                .Resource     = rhi::BufferBinding{
                    .Buffer = DynamicUBO,
                    .Offset = 0,
                    .Size   = BlockSize
                }
            });
        }

        for (const auto& [Binding, Resource] : TextureBindings)
        {
            Desc.Entries.push_back({
                .Binding      = Binding,
                .ArrayElement = 0,
                .Resource     = Resource
            });
        }

        Desc.DebugName = Tmpl->getName() + ".BindGroup";
        BindGroup = Device.createBindGroup(Desc);
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
    TemplateLayouts.clear();
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
    std::shared_ptr<MaterialTemplateBase> Tmpl;

    if (Desc.Shaders.Compute)
        Tmpl = ComputeMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);
    else if (Desc.Shaders.RayGeneration)
        Tmpl = RayTracingMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);
    else
        Tmpl = GraphicsMaterialTemplate::create(Device, Desc, FrameLayout, ExtraLayouts);

    auto H = TemplatePool.create(std::move(Tmpl));
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

void MaterialSystem::unregisterTemplate(TemplateHandle H)
{
    if (!TemplatePool.isValid(H)) return;

    auto* Slot = TemplatePool.get(H);
    if (!Slot || !*Slot) return;

    // 有实例引用时拒绝卸载
    if ((*Slot)->getInstanceRefCount() > 0) return;

    TemplateNameToHandle.erase((*Slot)->getName());
    TemplateLayouts.erase(H.Id);
    TemplatePool.destroy(H);
}

MaterialTemplateHandle MaterialSystem::findTemplateByName(
    std::string_view Name) const noexcept
{
    auto it = TemplateNameToHandle.find(std::string(Name));
    return it != TemplateNameToHandle.end() ? it->second : MaterialTemplateHandle{};
}

std::vector<MaterialTemplateHandle> MaterialSystem::getTemplatesByCategory(
    std::string_view Category) const
{
    std::vector<MaterialTemplateHandle> Result;
    for (auto& [Name, H] : TemplateNameToHandle)
    {
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

    // 2. 接管旧模板上的实例引用计数
    NewTmpl->setInstanceRefCount((*Slot)->getInstanceRefCount());

    // 3. 原地替换 shared_ptr（旧对象若还有外部引用，会延迟到最后一个引用释放）
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
    auto Key = std::string(InstanceName);
    auto it = NamedInstances.find(Key);
    if (it != NamedInstances.end()) return it->second;

    auto H = createInstance(TemplateName);
    if (H.isValid()) NamedInstances[Key] = H;
    return H;
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