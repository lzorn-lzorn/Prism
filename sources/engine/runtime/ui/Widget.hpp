#pragma once
#include "PlatformEvents.hpp"
#include "IUIRenderer.hpp"
#include "PaintContext.hpp"
#include "Invalidation.hpp"
#include "Layout.hpp"
#include "Slot.hpp"
#include <memory>
#include <cstddef>
#include <optional>
#include <vector>

namespace ui
{

// 控件可见性: 该可见性仅影响渲染; 命中与事件接收由 AcceptEvents 控制. 
enum class EVisibility
{
    Visible, // 渲染
    Hidden // 不渲染
};

// 事件处理结果: Handled 表示控件已消费该事件, 调用方应停止后续默认处理。
enum class EventReply
{
    Handled,
    Unhandled
};

class Widget : public IVisual
{
public:
    ~Widget() override = default;
    Widget() = default;
    Widget(const Widget&) = delete;
    Widget& operator=(const Widget&) = delete;
    void setBounds(UIRect Bounds);
    UIRect getBounds() const noexcept;
    Slot* getSlot() noexcept;
    const Slot* getSlot() const noexcept;
    void setLayout(const Layout& Value);
    const Layout& getLayout() const noexcept;
    bool hasLayout() const noexcept;
    void clearLayout() noexcept;
    // @note 仅由 Slot 在挂接/脱离时维护背向指针与暂存锚点，普通代码不应直接调用。
    void setParentSlot(Slot* Parent) noexcept;
    std::optional<Layout> takePendingAnchor() noexcept;
    void clearPendingAnchor() noexcept;

    // 可见性与交互状态
    void setVisibility(EVisibility Value) noexcept;
    EVisibility getVisibility() const noexcept;
    bool isVisible() const noexcept;

    // 为真时该控件不被祖先区域裁剪（只被自身矩形裁剪), 可绘制到父区域之外。
    void setEnableOuterRender(bool Value) noexcept;
    bool isEnableOuterRender() const noexcept;

    // 是否接受（消费）指针事件: 为真时该控件自身可成为命中目标并接收事件；
    // 为假时事件穿透给子控件, 若没有子控件接住则返回 Unhandled（向外传递）。
    void setAcceptEvents(bool Value) noexcept;
    bool isAcceptEvents() const noexcept;
    // 自身与整棵子树的透明度, 取值范围 [0, 1]; 1 为不透明.
    void setOpacity(float Value);
    float getOpacity() const noexcept;
    // @note 仅由 Slot 在排列时同步轴心与旋转; 轴心为归一化 (0..1), 角度为弧度.
    void setPivot(Vector2 Value) noexcept;
    Vector2 getPivot() const noexcept;
    void setRotation(float Value) noexcept;
    float getRotation() const noexcept;
    // 渲染变换(仅影响渲染与命中): 缩放/旋转/平移/轴心.
    void setRenderTransform(const RenderTransform& Value) noexcept;
    const RenderTransform& getRenderTransform() const noexcept;
    // 为真时该控件可获得键盘焦点; 为假时点击不会聚焦到它。
    void setFocusable(bool Value) noexcept;
    bool isFocusable() const noexcept;

    // 命中测试: Point 使用本控件的父坐标系（根控件即窗口逻辑坐标)。返回最深命中的可交互控件。
    Widget* hitTest(UIPoint Point);
    const Widget* hitTest(UIPoint Point) const;
    // 把窗口/父坐标系点换算为自身局部坐标（沿祖先链减去 Bounds 偏移)。
    UIPoint localizePoint(UIPoint Point) const noexcept;

    // 失效与增量渲染: 标记需要重算的结果, 帧循环消费后清除。
    void invalidate(EInvalidationReason Reason) noexcept;
    EInvalidationReason getInvalidationReason() const noexcept;
    bool isInvalidated(EInvalidationReason Reason = EInvalidationReason::All) const noexcept;

    Vector2 measure(Vector2 AvailableSize) const final;
    void arrange(UIRect Bounds) final;
    void paint(PaintContext& Context) const;
    void paint(IUIRenderer& Renderer) const;
    virtual size_t getChildCount() const noexcept { return 0; }

    // 输入钩子: 默认返回 EventReply::Unhandled; LocalPoint 为自身局部坐标。
    // 按下: 按下且命中该控件时触发（一次点击的开始)。
    virtual EventReply onMouseButtonDown(const MouseButtonEvent& Event, UIPoint LocalPoint);
    // 释放: 释放且命中该控件时触发。
    virtual EventReply onMouseButtonUp(const MouseButtonEvent& Event, UIPoint LocalPoint);
    // 点击: 按下与释放都命中同一控件时触发（完整的一次点击)。
    virtual EventReply onMouseButtonClicked(const MouseButtonEvent& Event, UIPoint LocalPoint);
    // 移动: 指针在该控件上方移动时触发。
    virtual EventReply onMouseMoved(const MouseMoveEvent& Event, UIPoint LocalPoint);
    // 进入/离开（悬停): 指针跨过控件边界时各触发一次。
    virtual EventReply onMouseEnter(const MouseMoveEvent& Event, UIPoint LocalPoint);
    virtual EventReply onMouseLeave(const MouseMoveEvent& Event, UIPoint LocalPoint);
    // 滚轮: 指针在该控件上方滚动时触发。
    virtual EventReply onMouseWheel(const MouseWheelEvent& Event, UIPoint LocalPoint);
    // 键盘: 派发给聚焦控件; 按住重复时 onKeyDown 会随 Repeat 再次触发。
    virtual EventReply onKeyDown(const KeyEvent& Event);
    virtual EventReply onKeyUp(const KeyEvent& Event);

protected:
    virtual Vector2 onMeasure(Vector2 AvailableSize) const;
    virtual void onArrange(UIRect LocalContent);
    virtual void onPaint(PaintContext& Context) const;
    virtual void paintChildren(PaintContext&) const {}
    virtual void arrangeChildren(UIRect) {}
    // 命中测试的孩子遍历(后添加的优先); Point 使用父坐标系。
    virtual const Widget* hitTestChildren(UIPoint) const { return nullptr; }
    // 是否裁剪到自身边界; TextWidget 在 EnableTextOuter 时返回 false 以渲染出自身.
    virtual bool shouldClipSelf() const noexcept { return true; }
    bool containsPoint(UIPoint Point) const noexcept;

private:
    void consumeInvalidation(EInvalidationReason Reason) const noexcept;
    UIRect Bounds{};
    Vector2 DesiredSize{}; 
    Slot* ParentSlot{};
    std::optional<Layout> PendingAnchor;
    EVisibility Visibility{EVisibility::Visible};
    bool EnableOuterRender{};
    bool AcceptEvents{};
    bool Focusable{};
    float Opacity{1.0f};
    RenderTransform RenderTransformValue;

    mutable EInvalidationReason Invalidation{EInvalidationReason::All};
};

class LeafWidget : public Widget
{
public:
    size_t getChildCount() const noexcept final;
protected:
    void paintChildren(PaintContext& Context) const final;
    void arrangeChildren(UIRect Content) final;
    const Widget* hitTestChildren(UIPoint Point) const final;
};

class ContentWidget : public Widget
{
public:
    size_t getChildCount() const noexcept final;
    Widget* getChild() noexcept;
    const Widget* getChild() const noexcept;
    LeafSlot* getChildSlot() noexcept;
    const LeafSlot* getChildSlot() const noexcept;
    // @note 启用 Fill 填充分配; 旧式 setChild 保留手动设置的边界.
    LeafSlot& addSlot(std::unique_ptr<Widget> Child);
    void setChild(std::unique_ptr<Widget> Child);
    std::unique_ptr<Widget> takeChild() noexcept;
protected:
    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void paintChildren(PaintContext& Context) const final;
    void arrangeChildren(UIRect Content) final;
    const Widget* hitTestChildren(UIPoint Point) const final;
private:
    std::unique_ptr<LeafSlot> ChildSlot;
};

class PanelWidget : public Widget
{
public:
    using Widget::getSlot;
    size_t getChildCount() const noexcept final;
    size_t getSlotCount() const noexcept;
    PanelSlot& addSlot(std::unique_ptr<Widget> Child);
    Widget& addChild(std::unique_ptr<Widget> Child);
    Widget& getChild(size_t Index);
    const Widget& getChild(size_t Index) const;
    PanelSlot& getChildSlot(size_t Index);
    const PanelSlot& getChildSlot(size_t Index) const;
    /**
     * @brief 按包含空插槽的索引访问插槽; 子控件索引则跳过空插槽.
     * 直接调用 Slot::takeChild 会留下空插槽, removeSlot 才会显式移除插槽.
     * @param Index 包含空插槽的插槽索引.
     * @return 对应的插槽引用.
     */
    PanelSlot& getSlot(size_t Index);
    const PanelSlot& getSlot(size_t Index) const;
    PanelSlot* getSlotForChild(const Widget& Child) noexcept;
    const PanelSlot* getSlotForChild(const Widget& Child) const noexcept;
    std::unique_ptr<Widget> takeChild(size_t Index);
    void removeSlot(size_t Index);
protected:
    // 创建子插槽的工厂；CanvasPanel 覆盖为 CanvasSlot。
    virtual std::unique_ptr<PanelSlot> createSlot(std::unique_ptr<Widget> Child);
    Vector2 onMeasure(Vector2 AvailableSize) const override;
    void paintChildren(PaintContext& Context) const final;
    void arrangeChildren(UIRect Content) override;
    const Widget* hitTestChildren(UIPoint Point) const final;
private:
    size_t getChildSlotIndex(size_t Index) const;
    std::vector<std::unique_ptr<PanelSlot>> Slots;
};
}
