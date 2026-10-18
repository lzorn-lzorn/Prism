#pragma once

#include "UIDrawList.hpp"

namespace ui
{
struct TextLayout;
class IUIRenderer;

class PaintContext
{
public:
    explicit PaintContext(UIDrawList& DrawList, IUIRenderer* Renderer = nullptr);
    // 可用的渲染器(供控件在绘制阶段上传纹理等); 无 GPU 场景下为空.
    IUIRenderer* getRenderer() const noexcept;
    // @note 基础图元使用逻辑坐标；粗细必须为有限正数, 点的粗细表示直径.
    void drawLine(UIPoint Start, UIPoint End, Color Tint, float Thickness = 1);
    void drawRect(UIRect Rect, Color Tint, float Thickness = 1);
    void drawTriangle(UIPoint First, UIPoint Second, UIPoint Third, Color Tint, float Thickness = 1);
    // @note 圆和矩形向内描边；圆半径为有限非负数, 零半径不生成网格.
    void drawCircle(UIPoint Center, float Radius, Color Tint, float Thickness = 1);
    void drawPoint(UIPoint Position, Color Tint, float Thickness = 1);
    void fillRect(UIRect Rect, Color Tint);
    void strokeRect(UIRect Rect, Color Tint, float Thickness = 1);
    void fillRoundedRect(UIRect Rect, float Radius, Color Tint);
    void strokeRoundedRect(UIRect Rect, float Radius, Color Tint, float Thickness = 1);
    void fillPath(std::span<const UIPoint> Points, Color Tint);
    void strokePath(std::span<const UIPoint> Points, Color Tint, float Thickness = 1,
        bool Closed = false);
    // @note 虚线图元使用逻辑坐标；DashLength/GapLength 为有限正数，实线段沿各边重新起算。
    void drawDashedLine(UIPoint Start, UIPoint End, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    void strokeDashedRect(UIRect Rect, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    void drawDashedCircle(UIPoint Center, float Radius, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    void strokeDashedPath(std::span<const UIPoint> Points, Color Tint, float Thickness = 1, float DashLength = 4, float GapLength = 4, bool Closed = false);
    void drawImage(TextureHandle Texture, UIRect Destination,
        UIRect SourceUV = {0, 0, 1, 1}, Color Tint = {1, 1, 1, 1});
    void drawTextLayout(const TextLayout& IVisual, UIPoint Origin, Color Tint);
    void pushClipRect(UIRect Rect, bool IgnoreParentClip = false);
    void popClip();
    void pushTransform(const UITransform& Transform);
    void popTransform();
    void pushOpacity(float Opacity);
    void popOpacity();

private:
    UIDrawList& DrawList;
    IUIRenderer* Renderer;
    std::vector<std::optional<UIRect>> Clips{std::nullopt};
    std::vector<UITransform> Transforms{UITransform{}};
    std::vector<float> Opacities{1};
    void appendMesh(std::span<const UIVertex> Vertices,
        std::span<const std::uint32_t> Indices, TextureHandle Texture = {});
};
}