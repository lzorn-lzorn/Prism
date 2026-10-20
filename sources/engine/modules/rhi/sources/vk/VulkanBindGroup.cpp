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
    constexpr uint32_t minimum_count = DescriptorSetsPerPool;
    if (Count > std::numeric_limits<uint32_t>::max() / DescriptorSetsPerPool)
        return std::numeric_limits<uint32_t>::max();
    return std::max(minimum_count, Count * DescriptorSetsPerPool);
}

const BindGroupLayoutEntry& findLayoutEntry(
    std::span<const BindGroupLayoutEntry> Entries,
    uint32_t Binding)
{
    const auto iterator = std::ranges::lower_bound(Entries, Binding, {}, &BindGroupLayoutEntry::Binding);
    if (iterator == Entries.end() || iterator->Binding != Binding)
        throw std::invalid_argument("BindGroup entry references a binding absent from its layout.");
    return *iterator;
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
    uint32_t requested_descriptors = 0;
    for (size_t index = 0; index < Entries.size(); ++index)
    {
        const auto& entry = Entries[index];
        const uint32_t count = entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? VariableArrayCount
            : entry.ArrayCount;
        if (requested_descriptors > std::numeric_limits<uint32_t>::max() - count)
            throw std::overflow_error("Descriptor pool size overflow.");
        requested_descriptors += count;
    }
    const uint32_t descriptors_per_type = scaledDescriptorCount(requested_descriptors);
    // Pages are intentionally type-complete: the arena may reuse a page for layouts
    // created later, so sizing it only for the first layout would be invalid.
    const std::array descriptor_types {
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
    std::vector<vk::DescriptorPoolSize> sizes;
    sizes.reserve(descriptor_types.size());
    for (const auto type : descriptor_types)
        sizes.emplace_back(type, descriptors_per_type);
    if (Device->getFeatures().AccelerationStructure)
        sizes.emplace_back(vk::DescriptorType::eAccelerationStructureKHR, descriptors_per_type);

    vk::DescriptorPoolCreateFlags flags = vk::DescriptorPoolCreateFlagBits::eFreeDescriptorSet;
    if (UpdateAfterBind)
        flags |= vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind;
    return Device->getVkDevice().createDescriptorPoolUnique(vk::DescriptorPoolCreateInfo(
        flags, DescriptorSetsPerPool, sizes));
}

vk::DescriptorSet VulkanDescriptorAllocator::tryAllocate(
    vk::DescriptorPool Pool,
    vk::DescriptorSetLayout Layout,
    uint32_t VariableArrayCount) const
{
    VkDescriptorSetVariableDescriptorCountAllocateInfo variable_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorSetCount = 1,
        .pDescriptorCounts = &VariableArrayCount
    };
    const VkDescriptorSetLayout layout = static_cast<VkDescriptorSetLayout>(Layout);
    VkDescriptorSetAllocateInfo allocate_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = VariableArrayCount == 0 ? nullptr : &variable_info,
        .descriptorPool = static_cast<VkDescriptorPool>(Pool),
        .descriptorSetCount = 1,
        .pSetLayouts = &layout
    };
    VkDescriptorSet result = VK_NULL_HANDLE;
    const VkResult allocation_result = vkAllocateDescriptorSets(
        static_cast<VkDevice>(Device->getVkDevice()), &allocate_info, &result);
    if (allocation_result == VK_SUCCESS)
        return result;
    if (allocation_result == VK_ERROR_OUT_OF_POOL_MEMORY || allocation_result == VK_ERROR_FRAGMENTED_POOL)
        return {};
    throw std::runtime_error("Failed to allocate a Vulkan descriptor set.");
}

VulkanDescriptorAllocation VulkanDescriptorAllocator::allocate(
    vk::DescriptorSetLayout Layout,
    std::span<const BindGroupLayoutEntry> Entries,
    uint32_t VariableArrayCount,
    bool UpdateAfterBind)
{
    std::scoped_lock lock(Mutex);
    for (auto& page : Pools)
    {
        if (page.UpdateAfterBind != UpdateAfterBind)
            continue;
        if (const auto set = tryAllocate(page.Pool.get(), Layout, VariableArrayCount))
            return { page.Pool.get(), set };
    }

    PoolPage page {
        .Pool = createPool(Entries, VariableArrayCount, UpdateAfterBind),
        .UpdateAfterBind = UpdateAfterBind
    };
    const auto set = tryAllocate(page.Pool.get(), Layout, VariableArrayCount);
    if (!set)
        throw std::runtime_error("A newly-created Vulkan descriptor pool could not allocate a set.");
    const auto pool = page.Pool.get();
    Pools.emplace_back(std::move(page));
    return { pool, set };
}

void VulkanDescriptorAllocator::release(VulkanDescriptorAllocation Allocation) noexcept
{
    if (!Allocation.Pool || !Allocation.Set || !Device)
        return;
    std::scoped_lock lock(Mutex);
    const VkDescriptorSet set = static_cast<VkDescriptorSet>(Allocation.Set);
    (void)vkFreeDescriptorSets(
        static_cast<VkDevice>(Device->getVkDevice()),
        static_cast<VkDescriptorPool>(Allocation.Pool),
        1,
        &set);
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
    auto vk_layout = std::dynamic_pointer_cast<VulkanBindGroupLayout>(Layout);
    if (!vk_layout || &vk_layout->getDevice() != Device || !vk_layout->isValid())
        throw std::invalid_argument("BindGroup layout belongs to another backend or device.");
    if (!Allocator)
        throw std::invalid_argument("BindGroup requires a descriptor allocator.");

    const auto layout_entries = vk_layout->getEntries();
    const auto variable_entry = std::ranges::find_if(layout_entries, [](const auto& entry)
    {
        return entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount);
    });
    if (Desc.VariableArrayCount != 0 && variable_entry == layout_entries.end())
        throw std::invalid_argument("VariableArrayCount was supplied for a fixed-size BindGroup layout.");
    const uint32_t variable_count = variable_entry == layout_entries.end()
        ? 0
        : (Desc.VariableArrayCount == 0 ? variable_entry->ArrayCount : Desc.VariableArrayCount);
    if (variable_entry != layout_entries.end() && variable_count > variable_entry->ArrayCount)
        throw std::invalid_argument("BindGroup variable descriptor count exceeds its layout maximum.");
    VariableArrayCount = variable_count;

    const bool update_after_bind = std::ranges::any_of(layout_entries, [](const auto& entry)
    {
        return entry.Flags.has(EDescriptorBindingFlag_t::UpdateAfterBind);
    });
    Allocation = Allocator->allocate(
        vk_layout->getVkDescriptorSetLayout(), layout_entries, variable_count, update_after_bind);
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
    } allocation_guard { Allocator, &Allocation };

    std::unordered_set<uint64_t> written_elements;
    written_elements.reserve(Entries.size());
    std::vector<vk::DescriptorBufferInfo> buffer_infos;
    std::vector<vk::DescriptorImageInfo> image_infos;
    std::vector<vk::BufferView> texel_views;
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<VkWriteDescriptorSetAccelerationStructureKHR> acceleration_writes;
    std::vector<VkAccelerationStructureKHR> acceleration_handles;
    buffer_infos.reserve(Entries.size());
    image_infos.reserve(Entries.size());
    texel_views.reserve(Entries.size());
    writes.reserve(Entries.size());
    acceleration_writes.reserve(Entries.size());
    acceleration_handles.reserve(Entries.size());
    TexelBufferViews.reserve(Entries.size());

    for (const auto& entry : Entries)
    {
        const auto& layout_entry = findLayoutEntry(layout_entries, entry.Binding);
        const uint32_t descriptor_count = layout_entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? variable_count
            : layout_entry.ArrayCount;
        if (entry.ArrayElement >= descriptor_count)
            throw std::out_of_range("BindGroup array element exceeds the declared descriptor count.");
        const uint64_t element_key = (static_cast<uint64_t>(entry.Binding) << 32u) | entry.ArrayElement;
        if (!written_elements.emplace(element_key).second)
            throw std::invalid_argument("BindGroup contains a duplicate binding array element.");

        vk::WriteDescriptorSet write;
        write
            .setDstSet(Allocation.Set)
            .setDstBinding(entry.Binding)
            .setDstArrayElement(entry.ArrayElement)
            .setDescriptorCount(1)
            .setDescriptorType(toVk(layout_entry.Type, layout_entry.Flags));

        if (isBufferDescriptor(layout_entry.Type))
        {
            const auto* binding = std::get_if<BufferBinding>(&entry.Resource);
            auto buffer = binding ? std::dynamic_pointer_cast<VulkanBuffer>(binding->Buffer) : nullptr;
            if (!binding || !buffer || &buffer->getDevice() != Device || !buffer->isValid())
                throw std::invalid_argument("Buffer descriptor requires a valid Vulkan buffer from this device.");
            const auto& buffer_desc = buffer->getDescriptor();
            validateBufferUsage(buffer_desc, layout_entry.Type);
            if (binding->Offset >= buffer_desc.Size ||
                (binding->Size != 0 && binding->Size > buffer_desc.Size - binding->Offset))
                throw std::out_of_range("Buffer binding range is out of bounds.");
            const DeviceSizeType alignment = layout_entry.Type == EDescriptorType::UniformBuffer
                ? Device->getLimits().MinUniformBufferOffsetAlignment
                : Device->getLimits().MinStorageBufferOffsetAlignment;
            if (binding->Offset % alignment != 0)
                throw std::invalid_argument("Buffer binding offset does not satisfy device alignment.");
            buffer_infos.emplace_back(
                buffer->getVkBuffer(),
                binding->Offset,
                binding->Size == 0 ? buffer_desc.Size - binding->Offset : binding->Size);
            write.pBufferInfo = &buffer_infos.back();
        }
        else if (layout_entry.Type == EDescriptorType::UniformTexelBuffer ||
            layout_entry.Type == EDescriptorType::StorageTexelBuffer)
        {
            const auto* binding = std::get_if<TexelBufferBinding>(&entry.Resource);
            auto buffer = binding ? std::dynamic_pointer_cast<VulkanBuffer>(binding->Buffer) : nullptr;
            if (!binding || !buffer || &buffer->getDevice() != Device || !buffer->isValid() ||
                binding->Format == EFormat::Undefined)
                throw std::invalid_argument("Texel-buffer descriptor requires a valid buffer and format.");
            const auto& buffer_desc = buffer->getDescriptor();
            const auto required_usage = layout_entry.Type == EDescriptorType::UniformTexelBuffer
                ? EBufferUsage::UniformTexel
                : EBufferUsage::StorageTexel;
            if (!buffer_desc.Usage.has(required_usage))
                throw std::invalid_argument("Texel-buffer binding has incompatible buffer usage.");
            if (binding->Offset >= buffer_desc.Size ||
                (binding->Size != 0 && binding->Size > buffer_desc.Size - binding->Offset) ||
                binding->Offset % Device->getLimits().MinTexelBufferOffsetAlignment != 0)
                throw std::out_of_range("Texel-buffer range or alignment is invalid.");
            TexelBufferViews.emplace_back(Device->getVkDevice().createBufferViewUnique(vk::BufferViewCreateInfo(
                {}, buffer->getVkBuffer(), toVk(binding->Format), binding->Offset,
                binding->Size == 0 ? buffer_desc.Size - binding->Offset : binding->Size)));
            texel_views.emplace_back(TexelBufferViews.back().get());
            write.pTexelBufferView = &texel_views.back();
        }
        else if (layout_entry.Type == EDescriptorType::Sampler ||
            layout_entry.Type == EDescriptorType::ComparisonSampler)
        {
            const auto* binding = std::get_if<SamplerBinding>(&entry.Resource);
            auto sampler = binding ? std::dynamic_pointer_cast<VulkanSampler>(binding->Sampler) : nullptr;
            if (!binding || !sampler || &sampler->getDevice() != Device || !sampler->isValid())
                throw std::invalid_argument("Sampler descriptor requires a valid Vulkan sampler from this device.");
            if (layout_entry.Type == EDescriptorType::ComparisonSampler &&
                !sampler->getDescriptor().CompareEnable)
                throw std::invalid_argument("ComparisonSampler requires comparison sampling to be enabled.");
            image_infos.emplace_back(sampler->getVkSampler(), vk::ImageView{}, vk::ImageLayout::eUndefined);
            write.pImageInfo = &image_infos.back();
        }
        else if (layout_entry.Type == EDescriptorType::CombinedImageSampler)
        {
            const auto* binding = std::get_if<CombinedImageSamplerBinding>(&entry.Resource);
            auto view = binding ? std::dynamic_pointer_cast<VulkanImageView>(binding->View) : nullptr;
            auto sampler = binding ? std::dynamic_pointer_cast<VulkanSampler>(binding->Sampler) : nullptr;
            if (!binding || !view || !sampler || &view->getDevice() != Device ||
                &sampler->getDevice() != Device || !view->isValid() || !sampler->isValid())
                throw std::invalid_argument("Combined descriptor requires a valid image view and sampler.");
            image_infos.emplace_back(sampler->getVkSampler(), view->getVkImageView(), toVk(binding->Layout));
            write.pImageInfo = &image_infos.back();
        }
        else if (layout_entry.Type == EDescriptorType::SampledTexture ||
            layout_entry.Type == EDescriptorType::StorageTexture ||
            layout_entry.Type == EDescriptorType::InputAttachment)
        {
            const auto* binding = std::get_if<TextureBinding>(&entry.Resource);
            auto view = binding ? std::dynamic_pointer_cast<VulkanImageView>(binding->View) : nullptr;
            if (!binding || !view || &view->getDevice() != Device || !view->isValid())
                throw std::invalid_argument("Texture descriptor requires a valid Vulkan image view from this device.");
            if (layout_entry.Type == EDescriptorType::StorageTexture &&
                binding->Layout != EDescriptorImageLayout::General)
                throw std::invalid_argument("Storage textures require the General descriptor image layout.");
            image_infos.emplace_back(vk::Sampler{}, view->getVkImageView(), toVk(binding->Layout));
            write.pImageInfo = &image_infos.back();
        }
        else if (layout_entry.Type == EDescriptorType::AccelerationStructure)
        {
            const auto* binding = std::get_if<AccelerationStructureBinding>(&entry.Resource);
            auto structure = binding
                ? std::dynamic_pointer_cast<VulkanAccelerationStructure>(binding->AccelerationStructure)
                : nullptr;
            if (!Device->getFeatures().AccelerationStructure || !structure ||
                &structure->getDevice() != Device || !structure->isValid())
                throw std::invalid_argument("Acceleration-structure descriptor requires a valid Vulkan acceleration structure.");
            acceleration_handles.emplace_back(static_cast<VkAccelerationStructureKHR>(structure->getVkHandle()));
            acceleration_writes.push_back({
                VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
                nullptr, 1, &acceleration_handles.back()
            });
            write.pNext = &acceleration_writes.back();
        }
        else
        {
            throw std::invalid_argument("Acceleration-structure BindGroups require the ray-tracing backend path.");
        }
        writes.emplace_back(write);
    }

    for (const auto& layout_entry : layout_entries)
    {
        if (layout_entry.Flags.has(EDescriptorBindingFlag_t::PartiallyBound))
            continue;
        const uint32_t count = layout_entry.Flags.has(EDescriptorBindingFlag_t::VariableArrayCount)
            ? variable_count
            : layout_entry.ArrayCount;
        for (uint32_t array_element = 0; array_element < count; ++array_element)
        {
            const uint64_t key = (static_cast<uint64_t>(layout_entry.Binding) << 32u) | array_element;
            if (!written_elements.contains(key))
                throw std::invalid_argument("BindGroup does not initialize every required descriptor.");
        }
    }
    Device->getVkDevice().updateDescriptorSets(writes, {});
    allocation_guard.Active = false;
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
