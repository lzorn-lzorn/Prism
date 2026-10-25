#pragma once

#include "IGenericWindow.hpp"
#include <infra/math/Vector.hpp>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace ui
{
struct Color
{
    float R{}, G{}, B{}, A{1.0f};
};

using Vector2 = infra::Vector<float, 2>;

struct UIPoint
{
    float X{}, Y{};
};

struct UIRect
{
    float X{}, Y{}, Width{}, Height{};
    constexpr Vector2 getPosition() const noexcept { return {X, Y}; }
    constexpr Vector2 getSize() const noexcept { return {Width, Height}; }
};

struct UITransform
{
    float XX{1}, XY{}, YX{}, YY{1}, TX{}, TY{};
    UIPoint transformPoint(UIPoint Point) const noexcept;
    UITransform compose(const UITransform& Local) const noexcept;
};

// 渲染变换: 仅影响渲染与命中(缩放/旋转/平移), 不影响布局(测量/排列).
// Scale 缩放; Rotation 弧度(逆时针为正); Translation 平移(逻辑单位); Pivot 归一化轴心(0..1).
struct RenderTransform
{
    Vector2 Scale{1, 1};
    float Rotation{0};
    Vector2 Translation{0, 0};
    Vector2 Pivot{0, 0};

    bool isIdentity() const noexcept
    {
        return Scale.x() == 1 && Scale.y() == 1 && Rotation == 0 &&
               Translation.x() == 0 && Translation.y() == 0;
    }
};

struct TextureHandle
{
    std::uint64_t Value{};
    bool operator==(const TextureHandle&) const = default;
};

struct UIVertex
{
    UIPoint Position;
    UIPoint UV;
    Color Tint;
};

struct UIDrawBatch
{
    std::uint32_t FirstIndex{}, IndexCount{};
    TextureHandle Texture;
    std::optional<UIRect> Clip;
};

struct UIDrawList
{
    std::vector<UIVertex> Vertices;
    std::vector<std::uint32_t> Indices;
    std::vector<UIDrawBatch> Batches;

    void clear() noexcept;
    void validate() const;
};

struct UITextureData
{
    std::uint32_t Width{}, Height{};
    std::span<const std::byte> Pixels;
};

struct UITextureUpdate
{
    std::uint32_t X{}, Y{}, Width{}, Height{};
    std::span<const std::byte> Pixels;
};

/**
 * @brief 一帧内需要送到 GPU 的纹理数据块(行已紧密排列, 步长 = UploadWidth * 4).
 *
 * 由 UI 后端在帧内累积脏区, 渲染侧的 UIRenderPass 在进入 render scope 之前消费:
 *   - Full == true  : 首次上传, 整张纹理都要拷贝(此时 X=Y=0 且尺寸等于 Width/Height);
 *   - Full == false : 只更新 [X, Y, UploadWidth, UploadHeight] 子矩形.
 *
 * @note 纹理格式固定为 RGBA8(直通 alpha), 与 FontService 图集/Image 像素布局一致.
 */
struct UISampledTextureUpload
{
    std::uint64_t Handle {};
    std::uint32_t Width {};
    std::uint32_t Height {};
    std::uint32_t X {};
    std::uint32_t Y {};
    std::uint32_t UploadWidth {};
    std::uint32_t UploadHeight {};
    bool          Full {};
    std::vector<std::byte> Pixels;
};

struct UIFrameInfo
{
    UISize LogicalSize;
    UISize PixelSize;
    Color ClearColor;
};
}