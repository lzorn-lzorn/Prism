#pragma once

#include "UIDrawList.hpp"

namespace ui
{
// UI 渲染模式. 
enum class EUIMode
{
    // 立即模式: 每帧重新布局, 重新生成顶点并上传. 
    Immediate,
    // 保留模式(默认): 只在失效时重算，复用上次的布局, 绘制数据和 GPU 缓冲. 
    Retained
};

class IUIRenderer
{
public:
    virtual ~IUIRenderer() = default;
    // @note 成功时开始帧，必须配对调用 endFrame；返回 false 时未开始帧. 
    virtual bool canBeginFrame(const UIFrameInfo& Frame) = 0;
    /**
     * @brief 提交基础图元，须在成功开始帧后, 结束帧前调用；各次调用按顺序绘制. 
     * @param Start 直线起点，使用逻辑坐标. 
     * @param End 直线终点；重合端点不生成网格. 
     * @param Tint 线条颜色. 
     * @param Thickness 有限正数，表示居中描边的宽度. 
     * @return 无返回值；非法参数或帧状态抛出异常. 
     */
    void drawLine(UIPoint Start, UIPoint End, Color Tint, float Thickness = 1);
    // @note 矩形使用逻辑坐标，按 Tint 颜色向内描边，Thickness 为有限正数. 
    void drawRect(UIRect Rect, Color Tint, float Thickness = 1);
    // @note 三个顶点使用逻辑坐标，按 Tint 颜色和 Thickness 宽度沿封闭路径居中描边. 
    void drawTriangle(UIPoint First, UIPoint Second, UIPoint Third, Color Tint, float Thickness = 1);
    // @note 圆心使用逻辑坐标，半径为有限非负数；按 Tint 和 Thickness 向内描边. 
    void drawCircle(UIPoint Center, float Radius, Color Tint, float Thickness = 1);
    // @note 在逻辑坐标 Position 绘制 Tint 颜色的实心圆点，Thickness 为有限正数直径. 
    void drawPoint(UIPoint Position, Color Tint, float Thickness = 1);
    // @note 虚线图元: 逻辑坐标；DashLength/GapLength 为有限正数. 
    void drawDashedLine(UIPoint Start, UIPoint End, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    void drawDashedRect(UIRect Rect, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    void drawDashedCircle(UIPoint Center, float Radius, Color Tint, float Thickness = 1,
        float DashLength = 4, float GapLength = 4);
    /**
     * @brief 提交一份绘制列表，须在成功开始帧后, 结束帧前调用. 
     * @param DrawList 顶点, 索引与批次；提交时复制，调用后可复用或清空. 
     * @param ReuseBuffers 保留模式提示: 数据与上一帧完全一致，可复用 GPU 缓冲, 跳过上传. 
     */
    virtual void submitDrawList(const UIDrawList& DrawList, bool ReuseBuffers = false) = 0;
    virtual void endFrame() = 0;
    // @note 渲染器当前所处的 UI 模式，决定 Window 是否启用增量布局与绘制. 
    virtual EUIMode getUIMode() const noexcept = 0;
    virtual TextureHandle createTexture(const UITextureData& Data) = 0;
    virtual void updateTexture(TextureHandle Texture, const UITextureUpdate& Update) = 0;
    virtual void destroyTexture(TextureHandle Texture) = 0;

protected:
    IUIRenderer() = default;
};
}
