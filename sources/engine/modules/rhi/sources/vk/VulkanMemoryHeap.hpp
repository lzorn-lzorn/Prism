#pragma once

#include <RHI.hpp>

#include <memory>
#include <mutex>
#include <vector>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;
class VulkanMemoryBlock;

/**
 * @brief 一段用于显存别名的大块 Vulkan 设备显存.
 *
 * RDG 之类的帧图会把"时间上不重叠"的瞬态资源放进同一个堆:
 *
 *      VkImage A <-- bind(heap, 0)
 *      VkImage B <-- bind(heap, 0)   // 与 A 复用同一段显存
 *
 * Vulkan 要求复用同一段显存的资源都带 VK_IMAGE_CREATE_ALIAS_BIT, 并且
 * 在复用点插入 oldLayout=UNDEFINED 的布局转换. 这两件事分别由
 * VulkanImage 的创建路径与 RDG 的屏障推导负责, 本类只负责"切显存".
 */
class VulkanTransientHeap final : public RTransientHeap
{
public:
	~VulkanTransientHeap() override;

	VulkanTransientHeap(const VulkanTransientHeap&) = delete;
	VulkanTransientHeap& operator=(const VulkanTransientHeap&) = delete;

	/** @brief 申请真实显存; 失败时返回 nullptr 而不是抛出, 由调用方回退到独立分配. */
	[[nodiscard]] static std::shared_ptr<VulkanTransientHeap> create(
		VulkanDevice& Device,
		const MemoryHeapDescriptor& Desc);

	[[nodiscard]] RDevice& getDevice() const noexcept override;
	[[nodiscard]] const MemoryHeapDescriptor& getDescriptor() const noexcept override { return Descriptor; }
	[[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(Memory); }
	[[nodiscard]] DeviceSizeType getSize() const noexcept override { return Descriptor.Size; }
	[[nodiscard]] void* getNativeHandle() const noexcept override;
	[[nodiscard]] vk::DeviceMemory getVkDeviceMemory() const noexcept { return Memory; }
	/** @brief 返回堆实际选中的 Vulkan 内存类型下标; 子分配必须与其一致. */
	[[nodiscard]] uint32_t getMemoryTypeIndex() const noexcept { return MemoryTypeIndex; }

	/**
	 * @brief 返回同时满足 TypeBits / Alignment 且未被占用的偏移.
	 * @param Size 需要的字节数
	 * @param Alignment 需要的对齐
	 * @return 偏移; 放不下返回 std::nullopt
	 */
	[[nodiscard]] std::optional<DeviceSizeType> suballocate(
		DeviceSizeType Size,
		DeviceSizeType Alignment,
		uint32_t TypeBits);

	/** @brief 归还一段偏移. */
	void release(DeviceSizeType Offset, DeviceSizeType Size);

	// RTransientHeap
	[[nodiscard]] std::optional<DeviceSizeType> suballocate(
		DeviceSizeType Size,
		DeviceSizeType Alignment) override
	{
		return suballocate(Size, Alignment, 0);
	}
	void releaseSuballocation(DeviceSizeType Offset, DeviceSizeType Size) override
	{
		release(Offset, Size);
	}

private:
	VulkanTransientHeap(VulkanDevice& Device, MemoryHeapDescriptor Desc, vk::DeviceMemory Memory);

	struct Range
	{
		DeviceSizeType Begin { 0 };
		DeviceSizeType End { 0 };
	};

	VulkanDevice*          Device { nullptr };
	MemoryHeapDescriptor   Descriptor;
	vk::DeviceMemory       Memory { VK_NULL_HANDLE };
	/** @brief 堆创建时确定的内存类型; 后续子分配必须与其一致. */
	uint32_t               MemoryTypeIndex { 0 };
	mutable std::mutex     Mutex;
	std::vector<Range>     FreeRanges;
};

} // namespace rhi
