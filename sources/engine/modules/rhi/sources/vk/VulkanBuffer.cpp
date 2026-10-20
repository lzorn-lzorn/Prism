#include "VulkanBuffer.hpp"

#include "VulkanDevice.hpp"
#include "VulkanDeviceMemory.hpp"
#include "VulkanRHI.hpp"

#include <stdexcept>
#include <utility>

namespace rhi
{

namespace
{
/** @brief 校验 Buffer 描述; 所有创建路径共用, 避免 placed 路径漏检. */
void validateBufferDescriptor(VulkanDevice& Device, const RBuffer::Descriptor_t& Desc)
{
    if (Desc.Size == 0)
        throw std::invalid_argument("Vulkan buffer size must be non-zero.");
    if (toVk(Desc.Usage) == vk::BufferUsageFlags{})
        throw std::invalid_argument("Vulkan buffer usage must not be empty.");
    if (Desc.MemoryProperty.has(EMemoryProperty::HostCoherent) &&
        !Desc.MemoryProperty.has(EMemoryProperty::HostVisible))
    {
        throw std::invalid_argument("Host-coherent buffer memory must also be host-visible.");
    }
    if (Desc.MemoryProperty.has(EMemoryProperty::HostCached) &&
        !Desc.MemoryProperty.has(EMemoryProperty::HostVisible))
    {
        throw std::invalid_argument("Host-cached buffer memory must also be host-visible.");
    }
    const bool MemoryUsageIsHostVisible = Desc.MemoryUsage == EMemoryUsage::CPUToGPU ||
        Desc.MemoryUsage == EMemoryUsage::GPUToCPU || Desc.MemoryUsage == EMemoryUsage::CPUOnly;
    if (Desc.PersistentlyMapped && !Desc.MemoryProperty.has(EMemoryProperty::HostVisible) &&
        !MemoryUsageIsHostVisible)
        throw std::invalid_argument("Persistently mapped Vulkan buffers must be host-visible.");
    const bool RequiresDeviceAddress =
        Desc.Usage.has(EBufferUsage::DeviceAddress) ||
        Desc.Usage.has(EBufferUsage::AccelerationStructureBuildInput) ||
        Desc.Usage.has(EBufferUsage::AccelerationStructureStorage) ||
        Desc.Usage.has(EBufferUsage::ShaderBindingTable);
    if (RequiresDeviceAddress && !Device.getFeatures().BufferDeviceAddress)
        throw std::invalid_argument("Vulkan buffer device address was requested but is not enabled.");
    if ((Desc.Usage.has(EBufferUsage::AccelerationStructureBuildInput) ||
        Desc.Usage.has(EBufferUsage::AccelerationStructureStorage)) &&
        !Device.getFeatures().AccelerationStructure)
        throw std::invalid_argument("Vulkan acceleration-structure buffer usage is not enabled.");
    if (Desc.Usage.has(EBufferUsage::ShaderBindingTable) &&
        !Device.getFeatures().RayTracingPipeline)
        throw std::invalid_argument("Vulkan shader-binding-table usage is not enabled.");
}
} // namespace

VulkanBuffer::VulkanBuffer(VulkanDevice& InDevice, RBuffer::Descriptor_t Desc, std::shared_ptr<DeviceMemory> InMemory, vk::UniqueBuffer InBuffer)
    : Device(&InDevice)
    , Descriptor(std::move(Desc))
    , Memory(std::move(InMemory))
    , Buffer(std::move(InBuffer))
{
}

std::shared_ptr<VulkanBuffer> VulkanBuffer::createUnbound(
    VulkanDevice& Device,
    const Descriptor_t& Desc)
{
    validateBufferDescriptor(Device, Desc);
    auto Buffer = Device.getVkDevice().createBufferUnique(vk::BufferCreateInfo(
        {}, Desc.Size, toVk(Desc.Usage), vk::SharingMode::eExclusive));
    return std::shared_ptr<VulkanBuffer>(new VulkanBuffer(Device, Desc, {}, std::move(Buffer)));
}

MemoryRequirements VulkanBuffer::getMemoryRequirements() const
{
    if (!Buffer)
        throw std::logic_error("Vulkan buffer is invalid.");
    vk::MemoryDedicatedRequirements DedicatedRequirements;
    vk::MemoryRequirements2 VkRequirements;
    VkRequirements.pNext = &DedicatedRequirements;
    const vk::BufferMemoryRequirementsInfo2 RequirementsInfo(Buffer.get());
    Device->getVkDevice().getBufferMemoryRequirements2(&RequirementsInfo, &VkRequirements);
    return MemoryRequirements {
        .Size = VkRequirements.memoryRequirements.size,
        .Alignment = VkRequirements.memoryRequirements.alignment,
        .MemoryTypeBits = VkRequirements.memoryRequirements.memoryTypeBits,
        .PrefersDedicatedAllocation = DedicatedRequirements.prefersDedicatedAllocation == VK_TRUE ||
            Descriptor.DedicatedAllocation,
        .RequiresDedicatedAllocation = DedicatedRequirements.requiresDedicatedAllocation == VK_TRUE
    };
}

void VulkanBuffer::bindPlacedMemory(std::shared_ptr<DeviceMemory> InMemory)
{
    Memory = std::move(InMemory);
    OwnsAllocation = false;
}

std::shared_ptr<VulkanBuffer> VulkanBuffer::create(
    VulkanDevice& Device,
    const RBuffer::Descriptor_t& Desc)
{
    validateBufferDescriptor(Device, Desc);

    auto Buffer = Device.getVkDevice().createBufferUnique(vk::BufferCreateInfo(
        {}, Desc.Size, toVk(Desc.Usage), vk::SharingMode::eExclusive));
    vk::MemoryDedicatedRequirements DedicatedRequirements;
    vk::MemoryRequirements2 VkRequirements;
    VkRequirements.pNext = &DedicatedRequirements;
    const vk::BufferMemoryRequirementsInfo2 RequirementsInfo(Buffer.get());
    Device.getVkDevice().getBufferMemoryRequirements2(&RequirementsInfo, &VkRequirements);
    const MemoryRequirements Requirements {
        .Size = VkRequirements.memoryRequirements.size,
        .Alignment = VkRequirements.memoryRequirements.alignment,
        .MemoryTypeBits = VkRequirements.memoryRequirements.memoryTypeBits,
        .PrefersDedicatedAllocation = DedicatedRequirements.prefersDedicatedAllocation == VK_TRUE || Desc.DedicatedAllocation,
        .RequiresDedicatedAllocation = DedicatedRequirements.requiresDedicatedAllocation == VK_TRUE
    };
    const bool MemoryUsageIsHostVisible = Desc.MemoryUsage == EMemoryUsage::CPUToGPU ||
        Desc.MemoryUsage == EMemoryUsage::GPUToCPU || Desc.MemoryUsage == EMemoryUsage::CPUOnly;
    const bool RequiresDeviceAddress =
        Desc.Usage.has(EBufferUsage::DeviceAddress) ||
        Desc.Usage.has(EBufferUsage::AccelerationStructureBuildInput) ||
        Desc.Usage.has(EBufferUsage::AccelerationStructureStorage) ||
        Desc.Usage.has(EBufferUsage::ShaderBindingTable);
    auto RequiredProperties = Desc.MemoryProperty;
    // RBuffer::Descriptor_t defaults to GPU-local memory. An explicit CPU allocation intent
    // replaces that default unless the caller supplied additional required properties.
    if (MemoryUsageIsHostVisible &&
        RequiredProperties == EMemoryPropertyFlags(EMemoryProperty::DeviceLocal))
        RequiredProperties.clear(EMemoryProperty::DeviceLocal);
    auto Memory = Device.allocateBufferMemory(MemoryAllocationDescriptor {
        .Requirements = Requirements,
        .Usage = Desc.MemoryUsage,
        .RequiredProperties = RequiredProperties,
        .PreferredProperties = Desc.PreferredMemoryProperty,
        .Priority = Desc.MemoryPriority,
        .PersistentlyMapped = Desc.PersistentlyMapped,
        .DebugName = Desc.DebugName
    }, RequiresDeviceAddress, Buffer.get());
    auto VulkanMemory = std::dynamic_pointer_cast<VulkanMemoryAllocation>(Memory);
    if (!VulkanMemory)
        throw std::logic_error("Vulkan device returned non-Vulkan memory for a buffer.");
    const auto AllocationInfo = VulkanMemory->getAllocationInfo();
    Device.getVkDevice().bindBufferMemory(
        Buffer.get(), VulkanMemory->getVkDeviceMemory(), AllocationInfo->Offset);

    return std::shared_ptr<VulkanBuffer>(new VulkanBuffer(
        Device, Desc, std::move(Memory), std::move(Buffer)));
}

RDevice& VulkanBuffer::getDevice() const noexcept
{
    return *Device;
}

void* VulkanBuffer::getNativeHandle() const noexcept
{
    return reinterpret_cast<void*>(static_cast<VkBuffer>(Buffer.get()));
}

DeviceAddress VulkanBuffer::getDeviceAddress() const noexcept
{
    if (!Buffer || !Device || !Device->getFeatures().BufferDeviceAddress ||
        (!Descriptor.Usage.has(EBufferUsage::DeviceAddress) &&
         !Descriptor.Usage.has(EBufferUsage::AccelerationStructureBuildInput) &&
         !Descriptor.Usage.has(EBufferUsage::AccelerationStructureStorage) &&
         !Descriptor.Usage.has(EBufferUsage::ShaderBindingTable)))
        return 0;
    // Vulkan 1.3 includes the Vulkan 1.2 bufferDeviceAddress feature; the KHR path has
    // equivalent requirements. Address-capable allocations carry DEVICE_ADDRESS_BIT.
    const VkBufferDeviceAddressInfo Info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
        .pNext = nullptr,
        .buffer = static_cast<VkBuffer>(Buffer.get())
    };
    return vkGetBufferDeviceAddress(
        static_cast<VkDevice>(Device->getVkDevice()), &Info);
}

void* VulkanBuffer::map(DeviceSizeType Offset, DeviceSizeType Size)
{
    if (!Memory->getMemoryProperty().has(EMemoryProperty::HostVisible))
        throw std::logic_error("Only host-visible Vulkan buffers can be mapped.");
    if (IsMapped)
        throw std::logic_error("Vulkan buffer memory is already mapped.");
    if (Offset >= Descriptor.Size || (Size != 0 && Size > Descriptor.Size - Offset))
        throw std::out_of_range("Vulkan buffer map range is out of bounds.");
    void* Result = Memory->map(Offset, Size == 0 ? Descriptor.Size - Offset : Size);
    IsMapped = true;
    return Result;
}

void VulkanBuffer::unmap()
{
    if (!IsMapped)
        throw std::logic_error("Vulkan buffer memory is not mapped.");
    Memory->unmap();
    IsMapped = false;
}

void VulkanBuffer::flush(DeviceSizeType Offset, DeviceSizeType Size)
{
    if (!IsMapped)
        throw std::logic_error("Vulkan buffer must be mapped before flushing it.");
    if (Offset >= Descriptor.Size || (Size != 0 && Size > Descriptor.Size - Offset))
        throw std::out_of_range("Vulkan buffer flush range is out of bounds.");
    Memory->flush(Offset, Size == 0 ? Descriptor.Size - Offset : Size);
}

void VulkanBuffer::invalidate(DeviceSizeType Offset, DeviceSizeType Size)
{
    if (!IsMapped)
        throw std::logic_error("Vulkan buffer must be mapped before invalidating it.");
    if (Offset >= Descriptor.Size || (Size != 0 && Size > Descriptor.Size - Offset))
        throw std::out_of_range("Vulkan buffer invalidate range is out of bounds.");
    Memory->invalidate(Offset, Size == 0 ? Descriptor.Size - Offset : Size);
}

} // namespace rhi
