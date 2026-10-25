#pragma once

#include "IUIRenderer.hpp"
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ui
{

/**
 * @brief UI 渲染后端选择.
 * @note 迁移到引擎 RHI 后仅保留 Vulkan 作为实际后端; 其余枚举值仅为兼容
 *       旧配置/命令行解析, 不再创建独立后端实例.
 */
enum class EGpuBackend
{
    Auto,
    Vulkan,
    Direct3D12,
    Metal
};

/**
 * @brief 用引擎 RHI 实现的 UI 渲染器(取代 SDL_GPU 后端).
 *
 * 与 SDL GPU 的 UIRenderer 保持同一个 IUIRenderer 接口, 因此 Widget 树 /
 * FontService / ImageWidget 无需改动; 差异只在"数据落到哪":
 *   - SDL GPU 版: 直接提交 SDL_GPUDevice;
 *   - 本版: 收集 UIDrawList 与纹理, 交给 runtime::renderer::RendererServer
 *     的 UIRenderPass 消费, 走 RDG → RHI → Vulkan 的统一命令流.
 *
 * 生命周期约定与 UIRenderer 一致: canBeginFrame → submitDrawList* → endFrame.
 * 纹理采用紧密排列 RGBA8.
 */
class RHIUIRenderer final : public IUIRenderer
{
public:
    RHIUIRenderer();
    ~RHIUIRenderer() override;
    RHIUIRenderer(const RHIUIRenderer&) = delete;
    RHIUIRenderer& operator=(const RHIUIRenderer&) = delete;

    bool canBeginFrame(const UIFrameInfo& Frame) override;
    void submitDrawList(const UIDrawList& DrawList, bool ReuseBuffers = false) override;
    void endFrame() override;
    EUIMode getUIMode() const noexcept override;
    void setUIMode(EUIMode Mode) noexcept;
    TextureHandle createTexture(const UITextureData& Data) override;
    void updateTexture(TextureHandle Texture, const UITextureUpdate& Update) override;
    void destroyTexture(TextureHandle Texture) override;

    /**
     * @brief 取当前帧提交的绘制列表(供 RendererServer 的 UIRenderPass 消费).
     * @return 最近一帧的绘制数据; 无帧时为引用空列表.
     */
    [[nodiscard]] const std::vector<UIDrawList>& getDrawLists() const noexcept;

    /**
     * @brief 取纹理的像素缓冲(供 RHI 上传).
     * @param Texture 纹理句柄.
     * @return 指向像素的只读视图; 句柄无效时为空.
     */
    [[nodiscard]] const UITextureData* getTextureData(TextureHandle Texture) const noexcept;

    /**
     * @brief 取走自上次调用以来所有纹理的脏区数据并清空脏标记.
     *
     * 返回的每一块都是紧密排列的行数据(步长 = UploadWidth * 4), 可直接拷进
     * staging buffer; 渲染侧据此做局部 copyBufferToImage.
     *
     * @return 待上传的纹理数据块; 无变化时为空.
     * @note 必须在 UI 线程(提交绘制列表的同一线程)调用, 与 canBeginFrame/endFrame 同序.
     */
    [[nodiscard]] std::vector<UISampledTextureUpload> collectTextureUploads();

private:
    struct Impl;
    std::unique_ptr<Impl> ImplPtr;
};

} // namespace ui
