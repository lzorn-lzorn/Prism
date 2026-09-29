#pragma once

#include <RHI.hpp>
#include <memory>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

/** @brief Owns a Vulkan buffer and its bound device memory. */
class VulkanBuffer final : public RBuffer
{
public:
	/**
	 * @brief Creates a buffer and allocates compatible memory.
	 * @param Device Device that owns the resource.
	 * @param Desc Immutable buffer description.
	 * @return A fully bound Vulkan buffer.
	 */
	[[nodiscard]] static std::shared_ptr<VulkanBuffer> create(VulkanDevice& Device, const Descriptor_t& Desc);

	/**
	 * @brief 只创建 VkBuffer, 不绑定显存(placed resource 的第一步).
	 * @param Device 所属设备
	 * @param Desc 不可变描述
	 * @return 未绑定显存的 Buffer
	 */
	[[nodiscard]] static std::shared_ptr<VulkanBuffer> createUnbound(
		VulkanDevice& Device,
		const Descriptor_t& Desc);

	~VulkanBuffer() override = default;

	/** @brief 查询驱动给出的显存需求(size / alignment / memoryTypeBits). */
	[[nodiscard]] MemoryRequirements getMemoryRequirements() const;

	/**
	 * @brief 记录 placed resource 所绑定的显存.
	 * @note 必须在 Device::bindBufferMemory 之后调用; 该对象只保持引用, 不负责释放堆.
	 */
	void bindPlacedMemory(std::shared_ptr<DeviceMemory> InMemory);

	[[nodiscard]] RDevice& getDevice() const noexcept override;
	[[nodiscard]] const Descriptor_t& getDescriptor() const noexcept override { return Descriptor; }
	[[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(Buffer); }
	[[nodiscard]] void* getNativeHandle() const noexcept override;
	[[nodiscard]] DeviceAddress getDeviceAddress() const noexcept override;
	[[nodiscard]] void* map(DeviceSizeType Offset = 0, DeviceSizeType Size = 0) override;
	void unmap() override;
	void flush(DeviceSizeType Offset, DeviceSizeType Size) override;
	void invalidate(DeviceSizeType Offset, DeviceSizeType Size) override;

	[[nodiscard]] vk::Buffer getVkBuffer() const noexcept { return Buffer.get(); }

private:
	VulkanBuffer(VulkanDevice& Device, Descriptor_t Desc, std::shared_ptr<DeviceMemory> Memory, vk::UniqueBuffer Buffer);

	VulkanDevice* Device { nullptr };
	Descriptor_t Descriptor;
	// Declared before Buffer so the Vulkan object is destroyed before its bound memory.
	std::shared_ptr<DeviceMemory> Memory;
	vk::UniqueBuffer Buffer;
	bool IsMapped { false };
	/** @brief placed resource 的显存由堆持有, 此处只记录引用. */
	bool OwnsAllocation { true };
};

} // namespace rhi
