#pragma once

#include "Image.hpp"
#include "Widget.hpp"
#include <memory>
#include <optional>

namespace ui
{
// 图片填充模式.
enum class EImageFillMode
{
    Stretch,  // 拉伸填满控件(默认)
    Repeat,   // 平铺(按原始尺寸重复)
    Center    // 居中, 按原始尺寸不缩放
};

// ImageWidget: 显示一个 Image(RenderTarget 的 CPU 表示).
// 支持: 显式尺寸(默认图片大小)、内容偏移、填充模式(拉伸/平铺/居中)、图集(UV 子区域)、溢出裁剪.
class ImageWidget : public LeafWidget
{
public:
    ImageWidget() = default;
    explicit ImageWidget(std::shared_ptr<Image> InImage);

    void setImage(std::shared_ptr<Image> Image);
    std::shared_ptr<Image> getImage() const noexcept;

    // 尺寸: 默认自动(图片大小); 显式设置后按该尺寸测量.
    void setSize(Vector2 Size) noexcept;
    void clearSize() noexcept;
    Vector2 getSize() const noexcept;

    // 图片内容在控件内的偏移(逻辑单位).
    void setPosition(Vector2 Position) noexcept;
    Vector2 getPosition() const noexcept;

    // 填充模式.
    void setFillMode(EImageFillMode Mode) noexcept;
    EImageFillMode getFillMode() const noexcept;

    // 图集: 显示纹理的 UV 子区域(归一化 0..1).
    void setSourceUV(UIRect UV) noexcept;
    UIRect getSourceUV() const noexcept;

    // 是否裁剪到自身边界(默认 true); false 时图片可超出控件仍被渲染.
    void setClipToBounds(bool Value) noexcept;
    bool isClipToBounds() const noexcept;

protected:
    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void onPaint(PaintContext& Context) const override;
    bool shouldClipSelf() const noexcept override;

private:
    std::shared_ptr<Image> ImageValue;
    std::optional<Vector2> ExplicitSize;
    Vector2 Position{0, 0};
    EImageFillMode FillMode{EImageFillMode::Stretch};
    UIRect SourceUV{0, 0, 1, 1};
    bool ClipToBounds{true};
    mutable TextureHandle Texture{};  // 懒上传后的 GPU 纹理(0 表示未上传)
};

}
