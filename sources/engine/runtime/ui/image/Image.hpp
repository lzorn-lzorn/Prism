#pragma once

#include "UIDrawList.hpp"
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace ui
{
// 材质抽象: 未来用于对 Image(RenderTarget)做后处理/着色. 目前仅保留扩展接口.
class IMaterial
{
public:
    virtual ~IMaterial() = default;
};

// Image: 一个 RenderTarget 在 CPU 层面的表示 —— 一块 RGBA8 像素缓冲.
// 既可作为普通图片显示, 也可作为渲染目标(未来配合材质/Shader 做离屏渲染).
class Image
{
public:
    Image() = default;
    Image(std::uint32_t Width, std::uint32_t Height);

    std::uint32_t getWidth() const noexcept { return Width; }
    std::uint32_t getHeight() const noexcept { return Height; }
    bool isEmpty() const noexcept { return Width == 0 || Height == 0 || Pixels.empty(); }

    // 像素缓冲(RGBA8, 长度 = Width * Height * 4).
    std::span<std::byte> getPixels() noexcept;
    std::span<const std::byte> getPixels() const noexcept;

    // 用单一颜色填充整张图像.
    void clear(Color Value);

    // 材质扩展接口(暂不实现材质逻辑).
    void setMaterial(std::shared_ptr<IMaterial> Material) noexcept;
    std::shared_ptr<IMaterial> getMaterial() const noexcept;

    // 转成纹理数据, 供渲染器上传.
    UITextureData toTextureData() const noexcept;

private:
    std::uint32_t Width{};
    std::uint32_t Height{};
    std::vector<std::byte> Pixels;
    std::shared_ptr<IMaterial> Material;
};

}
