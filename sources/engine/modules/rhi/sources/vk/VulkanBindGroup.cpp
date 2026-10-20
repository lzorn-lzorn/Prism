#include "VulkanBindGroup.hpp"

#include "VulkanBuffer.hpp"
#include "VulkanDevice.hpp"
#include "VulkanImageView.hpp"
#include "VulkanPipeline.hpp"
#include "VulkanRHI.hpp"
#include "VulkanRayTracing.hpp"
#include "VulkanSampler.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <unordered_set>

namespace rhi
{
namespace
{
constexpr uint32_t DescriptorSetsPerPool = 64;

uint32_t scaledDescriptorCount(uint32_t Count)
{
    constexpr uint32_t MinimumCount = DescriptorSetsPerPool;
    if (Count > std::numeric_limits<uint32_t>::max() / DescriptorSetsPerPool)
        return std::numeric_limits<uint32_t>::max();
    return std::max(MinimumCount, Count * DescriptorSetsPerPool);
}

const BindGroupLayoutEntry& findLayoutEntry(
    std::span<const BindGroupLayoutEntry> Entries,
    uint32_t Binding)
{
    const auto Iterator = std::ranges::lower_bound(Entries, Binding, {}, &BindGroupLayoutEntry::Binding);
    if (Iterator == Entries.end() || Iterator->Binding != Binding)
        throw std::invalid_argument("BindGroup entry references a binding absent from its layout.");
    return *Iterator;
}

bool isBufferDescriptor(EDescriptorType Type)
{
    return Type == EDescriptorType::UniformBuffer ||
        Type == EDescriptorType::ReadOnlyStorageBuffer ||
        Type == EDescriptorType::ReadWriteStorageBuffer;
}

void validateBufferUsage(const RBuffer::Descriptor_t& Desc, EDescriptorType Type)
{
    if (Type == EDescriptorType::UniformBuffer && !Desc.Usage.has(EBufferUsage::Uniform))
        throw std::invalid_argument("Uniform-buffer binding requires Uniform buffer usage.");
    if ((Type == EDescriptorType::ReadOnlyStorageBuffer ||
        Type == EDescriptorType::ReadWriteStorageBuffer) &&
        !Desc.Usage.has(EBufferUsage::Storage))
    {
        throw std::invalid_argument("Storage-buffer binding requires Storage buffer usage.");
    }
}
} // namespace

VulkanDescriptorAllocator::VulkanDescriptorAllocator(VulkanDevice& InDevice) noexcept
    : Device(&InDevice)
{
}

vk::UniqueDescriptorPool VulkanDescriptorAllocator::createPool(
    std::span<const BindGroupLayoutEntry> Entries,
    uint32_t VariableArrayCount,
    bool UpdateAfterBind) const
{
    uint32_t RequestedDescriptors = 0;
    for (size_t Index = 0; Index < Entries.size(); ++Index)
    {
        const auto& Entry = Entries[Index];
        const uint32_t Count = Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? VariableArrayCount
            : Entry.ArrayCount;
        if (RequestedDescriptors > std::numeric_limits<uint32_t>::max() - Count)
            throw std::overflow_error("Descriptor pool size overflow.");
        RequestedDescriptors += Count;
    }
    const uint32_t DescriptorsPerType = scaledDescriptorCount(RequestedDescriptors);
    // Pages are intentionally type-complete: the arena may reuse a page for layouts
    // created later, so sizing it only for the first layout would be invalid.
    const std::array DescriptorTypes {
        vk::DescriptorType::eSampler,
        vk::DescriptorType::eCombinedImageSampler,
        vk::DescriptorType::eSampledImage,
        vk::DescriptorType::eStorageImage,
        vk::DescriptorType::eUniformTexelBuffer,
        vk::DescriptorType::eStorageTexelBuffer,
        vk::DescriptorType::eUniformBuffer,
        vk::DescriptorType::eStorageBuffer,
        vk::DescriptorType::eUniformBufferDynamic,
        vk::DescriptorType::eStorageBufferDynamic,
        vk::DescriptorType::eInputAttachment
    };
    std::vector<vk::DescriptorPoolSize> Sizes;
    Sizes.reserve(DescriptorTypes.size());
    for (const auto Type : DescriptorTypes)
        Sizes.emplace_back(Type, DescriptorsPerType);
    if (Device->getFeatures().AccelerationStructure)
        Sizes.emplace_back(vk::DescriptorType::eAccelerationStructureKHR, DescriptorsPerType);

    vk::DescriptorPoolCreateFlags Flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    if (UpdateAfterBind)
        Flags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
    return Device->getVkDevice().createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(
        Flags, DescriptorSetsPerPool, Sizes));
}

vk::DescriptorSet VulkanDescriptorAllocator::tryAllocate(
    vk::DescriptorPool Pool,
    vk::DescriptorSetLayout Layout,
    uint32_t VariableArrayCount) const
{
    VkDescriptorSetVariableDescriptorCountAllocateInfo VariableInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &VariableArrayCount
    };
    const VkDescriptorSetLayout VkLayout = static_cast<VkDescriptorSetLayout>(Layout);
    VkDescriptorSetAllocateInfo AllocateInfo {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = VariableArrayCount == 0 ? nullptr : &VariableInfo,
        .descriptorPool = static_cast<VkDescriptorPool>(Pool),
        .descriptorSetCount = 1,
        .pSetLayouts = &VkLayout
    };
    VkDescriptorSet Result = VK_NULL_HANDLE;
    const VkResult AllocationResult = vkAllocateDescriptorSets(
        static_cast<VkDevice>(Device->getVkDevice()), &AllocateInfo, &Result);
    if (AllocationResult == VK_SUCCESS)
        return Result;
    if (AllocationResult == VK_ERROR_OUT_OF_POOL_MEMORY || AllocationResult == VK_ERROR_FRAGMENTED_POOL)
        return {};
    throw std::runtime_error("Failed to allocate a Vulkan descriptor set.");
}

VulkanDescriptorAllocation VulkanDescriptorAllocator::allocate(
    vk::DescriptorSetLayout Layout,
    std::span<const BindGroupLayoutEntry> Entries,
    uint32_t VariableArrayCount,
    bool UpdateAfterBind)
{
    std::scoped_lock Lock(Mutex);
    for (auto& Page : Pools)
    {
        if (Page.UpdateAfterBind != UpdateAfterBind)
            continue;
        if (const auto Set = tryAllocate(Page.Pool.get(), Layout, VariableArrayCount))
            return { Page.Pool.get(), Set };
    }

    PoolPage Page {
        .Pool = createPool(Entries, VariableArrayCount, UpdateAfterBind),
        .UpdateAfterBind = UpdateAfterBind
    };
    const auto Set = tryAllocate(Page.Pool.get(), Layout, VariableArrayCount);
    if (!Set)
        throw std::runtime_error("A newly-created Vulkan descriptor pool could not allocate a set.");
    const auto Pool = Page.Pool.get();
    Pools.emplace_back(std::move(Page));
    return { Pool, Set };
}

void VulkanDescriptorAllocator::release(VulkanDescriptorAllocation Allocation) noexcept
{
    if (!Allocation.Pool || !Allocation.Set || !Device)
        return;
    std::scoped_lock Lock(Mutex);
    const VkDescriptorSet Set = static_cast<VkDescriptorSet>(Allocation.Set);
    (void)vkFreeDescriptorSets(
        static_cast<VkDevice>(Device->getVkDevice()),
        static_cast<VkDescriptorPool>(Allocation.Pool),
        1,
        &Set);
}

VulkanBindGroup::VulkanBindGroup(
    VulkanDevice& InDevice,
    std::shared_ptr<VulkanDescriptorAllocator> InAllocator,
    const BindGroupDescriptor& Desc)
    : Device(&InDevice)
    , Allocator(std::move(InAllocator))
    , Layout(Desc.Layout)
    , Entries(Desc.Entries)
    , DebugName(Desc.DebugName)
{
    auto VkLayout = std::dynamic_pointer_cast<VulkanBindGroupLayout>(Layout);
    if (!VkLayout || &VkLayout->getDevice() != Device || !VkLayout->isValid())
        throw std::invalid_argument("BindGroup layout belongs to another backend or device.");
    if (!Allocator)
        throw std::invalid_argument("BindGroup requires a descriptor allocator.");

    const auto LayoutEntries = VkLayout->getEntries();
    const auto VariableEntry = std::ranges::find_if(LayoutEntries, [](const auto& Entry)
    {
        return Entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount);
    });
    if (Desc.VariableArrayCount != 0 && VariableEntry == LayoutEntries.end())
        throw std::invalid_argument("VariableArrayCount was supplied for a fixed-size BindGroup layout.");
    const uint32_t VariableCount = VariableEntry == LayoutEntries.end()
        ? 0
        : (Desc.VariableArrayCount == 0 ? VariableEntry->ArrayCount : Desc.VariableArrayCount);
    if (VariableEntry != LayoutEntries.end() && VariableCount > VariableEntry->ArrayCount)
        throw std::invalid_argument("BindGroup variable descriptor count exceeds its layout maximum.");
    VariableArrayCount = VariableCount;

    const bool UpdateAfterBind = std::ranges::any_of(LayoutEntries, [](const auto& Entry)
    {
        return Entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind);
    });
    Allocation = Allocator->allocate(
        VkLayout->getVkDescriptorSetLayout(), LayoutEntries, VariableCount, UpdateAfterBind);
    struct AllocationGuard
    {
        std::shared_ptr<VulkanDescriptorAllocator> Allocator;
        VulkanDescriptorAllocation* Allocation { nullptr };
        bool Active { true };
        ~AllocationGuard()
        {
            if (Active)
                Allocator->release(*Allocation);
        }
    } AllocationGuard { Allocator, &Allocation };

    std::unordered_set<uint64_t> WrittenElements;
    WrittenElements.reserve(Entries.size());
    std::vector<vk::DescriptorBufferInfo> BufferInfos;
    std::vector<vk::DescriptorImageInfo> ImageInfos;
    std::vector<vk::BufferView> TexelViews;
    std::vector<vk::WriteDescriptorSet> Writes;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> AccelerationWrites;
    std::vector<VkAccelerationStructureKHR> AccelerationHandles;
    BufferInfos.reserve(Entries.size());
    ImageInfos.reserve(Entries.size());
    TexelViews.reserve(Entries.size());
    Writes.reserve(Entries.size());
    AccelerationWrites.reserve(Entries.size());
    AccelerationHandles.reserve(Entries.size());
    TexelBufferViews.reserve(Entries.size());

    for (const auto& Entry : Entries)
    {
        const auto& LayoutEntry = findLayoutEntry(LayoutEntries, Entry.Binding);
        const uint32_t DescriptorCount = LayoutEntry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? VariableCount
            : LayoutEntry.ArrayCount;
        if (Entry.ArrayElement >= DescriptorCount)
            throw std::out_of_range("BindGroup array element exceeds the declared descriptor count.");
        const uint64_t ElementKey = (static_cast<uint64_t>(Entry.Binding) << 32u) | Entry.ArrayElement;
        if (!WrittenElements.emplace(ElementKey).second)
            throw std::invalid_argument("BindGroup contains a duplicate binding array element.");

        vk::WriteDescriptorSet Write;
        Write
            .setDstSet(Allocation.Set)
            .setDstBinding(Entry.Binding)
            .setDstArrayElement(Entry.ArrayElement)
            .setDescriptorCount(1)
            .setDescriptorType(toVk(LayoutEntry.Type, LayoutEntry.Flags));

        if (isBufferDescriptor(LayoutEntry.Type))
        {
            const auto* Binding = std::get_if<BufferBinding>(&Entry.Resource);
            auto Buffer = Binding ? std::dynamic_pointer_cast<VulkanBuffer>(Binding->Buffer) : nullptr;
            if (!Binding || !Buffer || &Buffer->getDevice() != Device || !Buffer->isValid())
                throw std::invalid_argument("Buffer descriptor requires a valid Vulkan buffer from this device.");
            const auto& BufferDesc = Buffer->getDescriptor();
            validateBufferUsage(BufferDesc, LayoutEntry.Type);
            if (Binding->Offset >= BufferDesc.Size ||
                (Binding->Size != 0 && Binding->Size > BufferDesc.Size - Binding->Offset))
                throw std::out_of_range("Buffer binding range is out of bounds.");
            const DeviceSizeType Alignment = LayoutEntry.Type == EDescriptorType::UniformBuffer
                ? Device->getLimits().MinUniformBufferOffsetAlignment
                : Device->getLimits().MinStorageBufferOffsetAlignment;
            if (Binding->Offset % Alignment != 0)
                throw std::invalid_argument("Buffer binding offset does not satisfy device alignment.");
            BufferInfos.emplace_back(
                Buffer->getVkBuffer(),
                Binding->Offset,
                Binding->Size == 0 ? BufferDesc.Size - Binding->Offset : Binding->Size);
            Write.pBufferInfo = &BufferInfos.back();
        }
        else if (LayoutEntry.Type == EDescriptorType::UniformTexelBuffer ||
            LayoutEntry.Type == EDescriptorType::StorageTexelBuffer)
        {
            const auto* Binding = std::get_if<TexelBufferBinding>(&Entry.Resource);
            auto Buffer = Binding ? std::dynamic_pointer_cast<VulkanBuffer>(Binding->Buffer) : nullptr;
            if (!Binding || !Buffer || &Buffer->getDevice() != Device || !Buffer->isValid() ||
                Binding->Format == EFormat::Undefined)
                throw std::invalid_argument("Texel-buffer descriptor requires a valid buffer and format.");
            const auto& BufferDesc = Buffer->getDescriptor();
            const auto RequiredUsage = LayoutEntry.Type == EDescriptorType::UniformTexelBuffer
                ? EBufferUsage::UniformTexel
                : EBufferUsage::StorageTexel;
            if (!BufferDesc.Usage.has(RequiredUsage))
                throw std::invalid_argument("Texel-buffer binding has incompatible buffer usage.");
            if (Binding->Offset >= BufferDesc.Size ||
                (Binding->Size != 0 && Binding->Size > BufferDesc.Size - Binding->Offset) ||
                Binding->Offset % Device->getLimits().MinTexelBufferOffsetAlignment != 0)
                throw std::out_of_range("Texel-buffer range or alignment is invalid.");
            TexelBufferViews.emplace_back(Device->getVkDevice().createBufferViewUnique(vk::BufferViewCreateInfo(
                {}, Buffer->getVkBuffer(), toVk(Binding->Format), Binding->Offset,
                Binding->Size == 0 ? BufferDesc.Size - Binding->Offset : Binding->Size)));
            TexelViews.emplace_back(TexelBufferViews.back().get());
            Write.pTexelBufferView = &TexelViews.back();
        }
        else if (LayoutEntry.Type == EDescriptorType::Sampler ||
            LayoutEntry.Type == EDescriptorType::ComparisonSampler)
        {
            const auto* Binding = std::get_if<SamplerBinding>(&Entry.Resource);
            auto Sampler = Binding ? std::dynamic_pointer_cast<VulkanSampler>(Binding->Sampler) : nullptr;
            if (!Binding || !Sampler || &Sampler->getDevice() != Device || !Sampler->isValid())
                throw std::invalid_argument("Sampler descriptor requires a valid Vulkan sampler from this device.");
            if (LayoutEntry.Type == EDescriptorType::ComparisonSampler &&
                !Sampler->getDescriptor().CompareEnable)
                throw std::invalid_argument("ComparisonSampler requires comparison sampling to be enabled.");
            ImageInfos.emplace_back(Sampler->getVkSampler(), vk::ImageView{}, vk::ImageLayout::eUndefined);
            Write.pImageInfo = &ImageInfos.back();
        }
        else if (LayoutEntry.Type == EDescriptorType::CombinedImageSampler)
        {
            const auto* Binding = std::get_if<CombinedImageSamplerBinding>(&Entry.Resource);
            auto View = Binding ? std::dynamic_pointer_cast<VulkanImageView>(Binding->View) : nullptr;
            auto Sampler = Binding ? std::dynamic_pointer_cast<VulkanSampler>(Binding->Sampler) : nullptr;
            if (!Binding || !View || !Sampler || &View->getDevice() != Device ||
                &Sampler->getDevice() != Device || !View->isValid() || !Sampler->isValid())
                throw std::invalid_argument("Combined descriptor requires a valid image view and sampler.");
            ImageInfos.emplace_back(Sampler->getVkSampler(), View->getVkImageView(), toVk(Binding->Layout));
            Write.pImageInfo = &ImageInfos.back();
        }
        else if (LayoutEntry.Type == EDescriptorType::SampledTexture ||
            LayoutEntry.Type == EDescriptorType::StorageTexture ||
            LayoutEntry.Type == EDescriptorType::InputAttachment)
        {
            const auto* Binding = std::get_if<TextureBinding>(&Entry.Resource);
            auto View = Binding ? std::dynamic_pointer_cast<VulkanImageView>(Binding->View) : nullptr;
            if (!Binding || !View || &View->getDevice() != Device || !View->isValid())
                throw std::invalid_argument("Texture descriptor requires a valid Vulkan image view from this device.");
            if (LayoutEntry.Type == EDescriptorType::StorageTexture &&
                Binding->Layout != EDescriptorImageLayout::General)
                throw std::invalid_argument("Storage textures require the General descriptor image layout.");
            ImageInfos.emplace_back(vk::Sampler{}, View->getVkImageView(), toVk(Binding->Layout));
            Write.pImageInfo = &ImageInfos.back();
        }
        else if (LayoutEntry.Type == EDescriptorType::AccelerationStructure)
        {
            const auto* Binding = std::get_if<AccelerationStructureBinding>(&Entry.Resource);
            auto Structure = Binding
                ? std::dynamic_pointer_cast<VulkanAccelerationStructure>(Binding->AccelerationStructure)
                : nullptr;
            if (!Device->getFeatures().AccelerationStructure || !Structure ||
                &Structure->getDevice() != Device || !Structure->isValid())
                throw std::invalid_argument("Acceleration-structure descriptor requires a valid Vulkan acceleration structure.");
            AccelerationHandles.emplace_back(static_cast<VkAccelerationStructureKHR>(Structure->getVkHandle()));
            AccelerationWrites.push_back({
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                nullptr, 1, &AccelerationHandles.back()
            });
            Write.pNext = &AccelerationWrites.back();
        }
        else
        {
            throw std::invalid_argument("Acceleration-structure BindGroups require the ray-tracing backend path.");
        }
        Writes.emplace_back(Write);
    }

    for (const auto& LayoutEntry : LayoutEntries)
    {
        if (LayoutEntry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound))
            continue;
        const uint32_t Count = LayoutEntry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? VariableCount
            : LayoutEntry.ArrayCount;
        for (uint32_t ArrayElement = 0; ArrayElement < Count; ++ArrayElement)
        {
            const uint64_t Key = (static_cast<uint64_t>(LayoutEntry.Binding) << 32u) | ArrayElement;
            if (!WrittenElements.contains(Key))
                throw std::invalid_argument("BindGroup does not initialize every required descriptor.");
        }
    }
    Device->getVkDevice().updateDescriptorSets(Writes, {});
    AllocationGuard.Active = false;
}

VulkanBindGroup::~VulkanBindGroup()
{
    if (Allocator)
        Allocator->release(Allocation);
}

RDevice& VulkanBindGroup::getDevice() const noexcept
{
    return *Device;
}

void* VulkanBindGroup::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkDescriptorSet>(Allocation.Set));
}

} // namespace rhi
