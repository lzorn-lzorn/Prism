#pragma once

#include "Layout.hpp"
#include <memory>
#include <optional>

namespace ui
{
class Widget;

/**
* @brief 容器拥有插槽, 插槽独占其子控件；反向指针仅用于观察, 不拥有对象。
* 子控件脱离时会丢弃与父容器相关的布局元数据。
*/
class Slot : public IVisual
{
public:
    ~Slot() override;
    Slot(const Slot&) = delete;
    Slot& operator=(const Slot&) = delete;
    Widget* getChild() noexcept;
    const Widget* getChild() const noexcept;
    Widget* getParent() noexcept;
    const Widget* getParent() const noexcept;
    std::unique_ptr<Widget> takeChild() noexcept;
protected:
    Slot(Widget& Parent, std::unique_ptr<Widget> Child);
private:
    Widget* Parent;
    std::unique_ptr<Widget> Child;
};

class PanelSlot : public Slot
{
public:
    PanelSlot(Widget& Parent, std::unique_ptr<Widget> Child);
    void setLayout(const Layout& Value);
    const Layout& getLayout() const noexcept;
    bool hasLayout() const noexcept;
    void clearLayout() noexcept;
    void setAutoSize(bool Value) noexcept;
    bool isAutoSize() const noexcept;
    // 通用面板无显式 Z 序（返回 0，即按添加顺序）；CanvasSlot 覆盖为自身 Z 序。
    virtual int getZOrder() const noexcept { return 0; }
    Vector2 measure(Vector2 AvailableSize) const override;
    void arrange(UIRect ParentContent) override;
private:
    Layout Anchor{};
    bool HasAnchor{};
    bool EnableAutoSize{};
};

enum class Alignment
{
    Start, Center, End, Fill
};

struct Padding
{
    float Left{}, Top{}, Right{}, Bottom{};
};

/**
* @brief 旧式锚点布局优先于对齐布局。
* 初始使用手动分配区域；设置内边距或对齐方式后启用相应布局分配。
* 禁用布局后恢复手动分配区域。
*/
class LeafSlot : public PanelSlot
{
public:
    LeafSlot(Widget& Parent, std::unique_ptr<Widget> Child);
    void setPadding(Padding Value);
    Padding getPadding() const noexcept;
    void setHorizontalAlignment(Alignment Value);
    Alignment getHorizontalAlignment() const noexcept;
    void setVerticalAlignment(Alignment Value);
    Alignment getVerticalAlignment() const noexcept;
    void setLayoutEnabled(bool Value) noexcept;
    bool isLayoutEnabled() const noexcept;
    Vector2 measure(Vector2 AvailableSize) const override;
    void arrange(UIRect ParentContent) override;
private:
    Padding Insets{};
    Alignment Horizontal{Alignment::Fill};
    Alignment Vertical{Alignment::Fill};
    bool LayoutEnabled{};
};
}