#pragma once

#include "IUIRenderer.hpp"
#include <memory>
#include <string>

namespace ui
{
enum class EGpuBackend
{
    Auto,
    Vulkan,
    Direct3D12,
    Metal
};

class UIRenderer final : public IUIRenderer
{
  public:
    explicit UIRenderer(IGenericWindow &Window, EGpuBackend Backend = EGpuBackend::Auto, bool Debug = false);
    ~UIRenderer() override;
    UIRenderer(const UIRenderer &) = delete;
    UIRenderer &operator=(const UIRenderer &) = delete;
    std::string getBackendName() const;
    // @note 设置 UI 渲染模式（默认保留模式）；须在创建纹理与开始帧之前设置。
    void setUIMode(EUIMode Mode) noexcept;
    // @note 清空并呈现空帧，不是纯查询；返回操作是否成功。
    bool canClear(Color ClearColor);
    /**
     * @brief 成功时实际开始一帧，不是纯查询。
     * 成功调用后必须配对调用 endFrame。在 endFrame 前禁止创建、更新和销毁纹理，
     * 即使提交被拒绝也不例外。提交的数据会被复制并按提交顺序渲染。
     * 批次中值为零的纹理句柄采样白色；零不是可修改的纹理。
     * 所有调用必须在 SDL 窗口线程上串行执行。纹理采用紧密排列的非预乘透明度
     * RGBA8 格式，每个维度最多为 8192 像素。
     * @param Frame 待开始帧的信息。
     * @return 是否成功开始一帧；成功后必须调用 endFrame。
     */
    bool canBeginFrame(const UIFrameInfo &Frame) override;
    void submitDrawList(const UIDrawList &DrawList, bool ReuseBuffers = false) override;
    void endFrame() override;
    EUIMode getUIMode() const noexcept override;
    TextureHandle createTexture(const UITextureData &Data) override;
    void updateTexture(TextureHandle Texture, const UITextureUpdate &Update) override;
    void destroyTexture(TextureHandle Texture) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> ImplPtr;
};
}
