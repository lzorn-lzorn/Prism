#pragma once

#include <RHI.hpp>
#include <memory>
#include <variant>
#include <vulkan/vulkan.hpp>

namespace rhi
{

class VulkanDevice;

class VulkanImage final : public RImage
{
public:
	~VulkanImage() override = default;
	VulkanImage(const VulkanImage&) = delete;
	VulkanImage& operator=(const VulkanImage&) = delete;
	VulkanImage(VulkanImage&&) = delete;
	VulkanImage& operator=(VulkanImage&&) = delete;

	[[nodiscard]] static std::shared_ptr<VulkanImage> create(
		VulkanDevice& Device,
		const Descriptor_t& Desc,
		DeviceMemoryAllocator* Allocator = nullptr);
	[[nodiscard]] static std::shared_ptr<VulkanImage> createUnbound(
		VulkanDevice& Device,
		const Descriptor_t& Desc);
	/**
	 * @brief 创建未绑定显存且带 VK_IMAGE_CREATE_ALIAS_BIT 的 Image.
	 * @note 复用同一段显存的 placed resource 必须使用该入口.
	 */
	[[nodiscard]] static std::shared_ptr<VulkanImage> createUnboundAliasing(
		VulkanDevice& Device,
		const Descriptor_t& Desc);
	[[nodiscard]] static std::shared_ptr<VulkanImage> wrapExternal(
		VulkanDevice& Device,
		const Descriptor_t& Desc,
		vk::Image Image);

	void allocateAndBindMemory(DeviceMemoryAllocator* Allocator = nullptr);

	/** @brief 查询驱动给出的显存需求(size / alignment / memoryTypeBits). */
	[[nodiscard]] MemoryRequirements getMemoryRequirements() const;

	/**
	 * @brief 记录 placed resource 所绑定的显存.
	 * @note 必须在 Device::bindImageMemory 之后调用; 该对象只保持引用, 不负责释放堆.
	 */
	void bindPlacedMemory(std::shared_ptr<DeviceMemory> InMemory);

	[[nodiscard]] RDevice& getDevice() const noexcept override;
	[[nodiscard]] bool isValid() const noexcept override { return static_cast<bool>(getVkImage()); }
	[[nodiscard]] bool isMemoryBound() const noexcept override { return isExternal() || Memory != nullptr; }
	[[nodiscard]] void* getNativeHandle() const noexcept override;
	[[nodiscard]] vk::Image getVkImage() const noexcept;
	[[nodiscard]] bool isExternal() const noexcept
	{
		return std::holds_alternative<vk::Image>(Image);
	}
	[[nodiscard]] VulkanDevice& getVulkanDevice() const noexcept { return *Device; }
	[[nodiscard]] const std::shared_ptr<DeviceMemory>& getMemory() const noexcept { return Memory; }

private:
	VulkanImage(VulkanDevice& Device, const Descriptor_t& Desc, vk::UniqueImage Image);
	VulkanImage(VulkanDevice& Device, const Descriptor_t& Desc, vk::Image ExternalImage);

	static void validateDescriptor(const Descriptor_t& Desc);
	[[nodiscard]] static vk::ImageCreateInfo makeCreateInfo(
		const Descriptor_t& Desc,
		bool EnableAliasing);

	VulkanDevice* Device = nullptr;
	std::shared_ptr<DeviceMemory> Memory;
	// Memory 必须先声明：成员按逆序析构，确保自有 VkImage 先销毁，再释放绑定内存. 
	std::variant<vk::UniqueImage, vk::Image> Image;
};

} // namespace rhi