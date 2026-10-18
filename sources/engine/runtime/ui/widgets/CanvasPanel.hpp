#pragma once

#include "Widget.hpp"

namespace ui
{
class CanvasPanel;

/**
* @brief CanvasPanel 专用插槽, 对标 UE 的 UCanvasPanelSlot: 
* 在 PanelSlot 的锚点/自动尺寸基础上, 增加 UE 风格粒度 API 与显式 Z 序. 
*/
class CanvasSlot : public PanelSlot
{
public:
    CanvasSlot(Widget& Parent, std::unique_ptr<Widget> Child);

    // 锚点(归一化 0..1；Min 与 Max 相等为固定轴, 不相等为拉伸轴). 
    void setAnchors(const Anchors& Value);
    void setAnchors(Vector2 Min, Vector2 Max);
    Anchors getAnchors() const noexcept;
    // 位置与尺寸(固定轴生效；拉伸轴忽略 Position/Size). 
    void setPosition(Vector2 Value);
    Vector2 getPosition() const noexcept;
    void setSize(Vector2 Value);
    Vector2 getSize() const noexcept;
    // 对齐(即 Layout::Pivot, 0..1: 锚点处落在自身哪个比例位置). 
    void setAlignment(Vector2 Value);
    Vector2 getAlignment() const noexcept;
    // 拉伸轴两端偏移(StartMargin/EndMargin；固定轴忽略). 
    void setOffsets(Vector2 StartMargin, Vector2 EndMargin);
    // 旋转(绕轴心, 弧度, 逆时针为正). 
    void setRotation(float Value);
    float getRotation() const noexcept;
    // Z 序: 值越大越靠前(后绘制、先命中)；相等时按添加顺序. 
    void setZOrder(int Value) noexcept;
    int getZOrder() const noexcept override;

private:
    Layout layout() const noexcept;
    int ZOrder{};
};

/**
* @brief 对标 UE 的 UCanvasPanel: 子控件按各自的锚点/偏移/对齐/尺寸放置, 并支持显式 Z 序. 
* 基于 PanelWidget, 额外提供 UE 风格入口 addChildToCanvas 与调试边框. 
*/
class CanvasPanel : public PanelWidget
{
public:
    // UE 风格: 把子控件加入画布并返回其专用 CanvasSlot, 便于随后配置锚点/偏移/尺寸/Z 序. 
    CanvasSlot& addChildToCanvas(std::unique_ptr<Widget> Child);

#ifndef NDEBUG // debug 模式下多余的调试信息
    // 是否绘制矩形的边框(实线, 向内描边). 
    void setEnablePaintBorder(bool Value) noexcept;
    bool isEnablePaintBorder() const noexcept;
    // 是否绘制虚线边框. 
    void setEnablePaintDashedBorder(bool Value) noexcept;
    bool isEnablePaintDashedBorder() const noexcept;
    // 边框颜色. 
    void setBorderColor(Color Value);
    Color getBorderColor() const noexcept;
    // 边框粗细(有限正数). 
    void setBorderThickness(float Value);
    float getBorderThickness() const noexcept;
#endif

protected:
    std::unique_ptr<PanelSlot> createSlot(std::unique_ptr<Widget> Child) override;
    void onPaint(PaintContext& Context) const override;

private:
#ifndef NDEBUG // debug 模式下多余的调试信息
    bool EnablePaintBorder{false};
    bool EnablePaintDashedBorder{false};
    Color BorderColor{1, 0, 0, 1};
    float BorderThickness{1.0f};
#endif
};
}
