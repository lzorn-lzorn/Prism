#pragma once
#include <RHI.hpp>
#include <list>
#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;
class VulkanMemoryBlock;
class VulkanTransientHeap;

/** @brief A buffer allocation backed by a device-owned best-fit memory page. */
class VulkanMemoryAllocation final : public DeviceMemory
{
public:
	~VulkanMemoryAllocation() override;
	VulkanMemoryAllocation(const VulkanMemoryAllocation&) = delete;
	VulkanMemoryAllocation& operator=(const VulkanMemoryAllocation&) = delete;

	void* map(DeviceSizeType Offset = 0, DeviceSizeType Size = 0) override;
	void unmap() override;
	void flush(DeviceSizeType Offset, DeviceSizeType Size) override;
	void invalidate(DeviceSizeType Offset, DeviceSizeType Size) override;
	void release() override;
	[[nodiscard]] MemoryRequirements getMemoryRequirements() const override { return Requirements; }
	[[nodiscard]] EMemoryProperty getMemoryProperty() const override { return Info.Properties; }
	[[nodiscard]] std::optional<MemoryAllocationInfo> getAllocationInfo() const override { return Info; }
	[[nodiscard]] vk::DeviceMemory getVkDeviceMemory() const noexcept;

private:
	friend class VulkanMemoryAllocator;
	VulkanMemoryAllocation(
		std::shared_ptr<VulkanMemoryBlock> Block,
		MemoryRequirements Requirements,
		MemoryAllocationInfo Info,
		DeviceSizeType ReservedSize);

	std::shared_ptr<VulkanMemoryBlock> Block;
	MemoryRequirements Requirements {};
	MemoryAllocationInfo Info {};
	DeviceSizeType ReservedSize { 0 };
	std::mutex MapMutex;
	bool IsMapped { false };
};

/**
 * @brief Device-owned Vulkan memory page allocator used by buffers.
 *
 * Pages use aligned best-fit placement and sorted free ranges that coalesce on free.
 * Large, required-dedicated, and preferred-dedicated resources receive private blocks.
 */
class VulkanMemoryAllocator final
{
public:
	explicit VulkanMemoryAllocator(VulkanDevice& Device);
	~VulkanMemoryAllocator();
	VulkanMemoryAllocator(const VulkanMemoryAllocator&) = delete;
	VulkanMemoryAllocator& operator=(const VulkanMemoryAllocator&) = delete;

	[[nodiscard]] std::shared_ptr<VulkanMemoryAllocation> allocate(
		const MemoryAllocationDescriptor& Desc,
		bool RequireDeviceAddress,
		vk::Buffer DedicatedBuffer = {},
		vk::Image DedicatedImage = {});
	void releaseCachedPages();

private:
	VulkanDevice* Device { nullptr };
	std::mutex Mutex;
	std::vector<std::shared_ptr<VulkanMemoryBlock>> Pages;
};

class VulkanDeviceMemory final : public DeviceMemory
{
	friend class VulkanDevice;
	friend class VulkanDeviceMemoryDeleter;
	friend class VulkanDeviceMemoryPool;
	friend class VulkanDeviceMemoryAllocator;
public:
	VulkanDeviceMemory() 
		: VkDeviceMemory(VK_NULL_HANDLE)
		, Requirements({})
		, Property(EMemoryProperty::enum_type::None)
		, OwnerDevice(nullptr)
	{}
	~VulkanDeviceMemory() override = default;
	VulkanDeviceMemory(const VulkanDeviceMemory&) = delete;
	VulkanDeviceMemory& operator=(const VulkanDeviceMemory&) = delete;

	VulkanDeviceMemory(VulkanDeviceMemory&&);
	VulkanDeviceMemory& operator=(VulkanDeviceMemory&&);
	VulkanDeviceMemory(MemoryRequirements InRequirements, EMemoryProperty InProperty)
		: Requirements(InRequirements), Property(InProperty) {}

	void* map(DeviceSizeType Offset = 0, DeviceSizeType Size = 0) override;
	void unmap() override;
	void flush(DeviceSizeType Offset, DeviceSizeType Size) override;
	void invalidate(DeviceSizeType Offset, DeviceSizeType Size) override;
	void release() override;
	
	
	VulkanDeviceMemory& setVkDeviceMemory(vk::DeviceMemory InVkDeviceMemory) { VkDeviceMemory = InVkDeviceMemory; return *this; }

	vk::DeviceMemory getVkDeviceMemory() const { return VkDeviceMemory; } 
	vk::DeviceMemory getVkDeviceMemory() { return VkDeviceMemory; } 
	MemoryRequirements getMemoryRequirements() const override;
	EMemoryProperty getMemoryProperty() const override;
	
private:
	void reset();
	MemoryRequirements Requirements;
	vk::DeviceMemory VkDeviceMemory { VK_NULL_HANDLE };	
	vk::Device OwnerDevice { VK_NULL_HANDLE };
	EMemoryProperty Property;
	
};

// @note: 线程不安全只能在 RHI 线程中使用
class VulkanDeviceMemoryPool final
{
	using value_type = VulkanDeviceMemory;
	using pointer = VulkanDeviceMemory*;
	constexpr static int32_t InitialSize = 512;
public:
	static VulkanDeviceMemoryPool& self() {
		static VulkanDeviceMemoryPool instance;
		return instance;
	}

	~VulkanDeviceMemoryPool();
	VulkanDeviceMemoryPool(const VulkanDeviceMemoryPool&) = delete;
	VulkanDeviceMemoryPool& operator=(const VulkanDeviceMemoryPool&) = delete;

	VulkanDeviceMemory& allocateMemory();
	
	void reclaim(VulkanDeviceMemory* Memory);

	struct Block {
		pointer MemoryBlock;
		size_t Count;
	};
private:
	VulkanDeviceMemoryPool();

	void expand();
	std::list<pointer> FreeList;
	std::vector<Block> Blocks; // 原始内存块及对象数量
	std::list<pointer> UsedList;                     // 已分配出去的对象
	size_t Capacity { InitialSize };                 // 当前池总容量
};

class VulkanDeviceMemoryDeleter
{
public:
    VulkanDeviceMemoryDeleter() = default;
	explicit VulkanDeviceMemoryDeleter(VulkanDeviceMemoryPool* InPool) : Pool(InPool) {}

    void operator()(VulkanDeviceMemory* ptr) const;

private:
    VulkanDeviceMemoryPool* Pool = nullptr;
};

class VulkanDeviceMemoryAllocator final : public DeviceMemoryAllocator
{
public:
	explicit VulkanDeviceMemoryAllocator(VulkanDevice* InDevice = nullptr) noexcept
		: Device(InDevice) {}
	~VulkanDeviceMemoryAllocator() override = default;

	// note: 从池中分配一个 VulkanDeviceMemory, 
	// note: 在 GPU 上分配一段显存, 将其与 VulkanDeviceMemory 绑定, 并设置内部的对应的所有权字段
	std::shared_ptr<DeviceMemory> allocateMemory(MemoryRequirements Requirements, EMemoryProperty Property) override;
	void freeMemory(std::shared_ptr<DeviceMemory> Memory) override;

	void setDevice(VulkanDevice* InDevice) noexcept { Device = InDevice; }
	[[nodiscard]] VulkanDevice* getDevice() const noexcept { return Device; }
private:
	VulkanDevice* Device = nullptr;
};

/**
 * @brief placed resource 持有的显存视图.
 *
 * 真正的 VkDeviceMemory 由 VulkanTransientHeap 拥有, 本对象只是让
 * RBuffer / RImage 能通过 DeviceMemory 接口报告"我绑在哪段显存上",
 * 并在 release() 时把区间归还给堆的空闲表.
 */
class VulkanHeapSubAllocation final : public DeviceMemory
{
public:
	VulkanHeapSubAllocation(
		std::shared_ptr<VulkanTransientHeap> InHeap,
		DeviceSizeType InOffset,
		DeviceSizeType InSize) noexcept
		: Heap(std::move(InHeap)), Offset(InOffset), Size(InSize)
	{
		OwnershipState = EState::ReadOnly;
	}

	void* map(DeviceSizeType, DeviceSizeType) override
	{
		throw std::logic_error("Placed resource heaps are not host mappable.");
	}
	void unmap() override {}
	void flush(DeviceSizeType, DeviceSizeType) override {}
	void invalidate(DeviceSizeType, DeviceSizeType) override {}
	void release() override;
	[[nodiscard]] MemoryRequirements getMemoryRequirements() const override
	{
		return MemoryRequirements { .Size = Size, .Alignment = 1, .MemoryTypeBits = 0 };
	}
	[[nodiscard]] EMemoryProperty getMemoryProperty() const override
	{
		return EMemoryProperty(EMemoryProperty_t::DeviceLocal);
	}

	[[nodiscard]] DeviceSizeType getOffset() const noexcept { return Offset; }
	[[nodiscard]] const std::shared_ptr<VulkanTransientHeap>& getHeap() const noexcept { return Heap; }

private:
	std::shared_ptr<VulkanTransientHeap> Heap;
	DeviceSizeType                       Offset { 0 };
	DeviceSizeType                       Size { 0 };
};
	
} // namespace rhi
