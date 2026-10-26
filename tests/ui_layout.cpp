#include "Widget.hpp"
#include "slots/FlexibleSlot.hpp"
#include "widgets/FlexibleBox.hpp"
#include "widgets/HorizontalBox.hpp"
#include "widgets/VerticalBox.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace
{
// 一个不依赖字体/渲染的叶子控件，用于纯布局验证。
class ProbeWidget final : public ui::LeafWidget
{
public:
    ProbeWidget(float Width, float Height)
        : Width(Width), Height(Height)
    {
    }

protected:
    ui::Vector2 onMeasure(ui::Vector2) const override { return {Width, Height}; }

private:
    float Width;
    float Height;
};

// 复刻编辑器里"面板 = 底色 + 铺满子控件"的结构：内部子控件用 StretchAll 锚点。
class ProbePanel final : public ui::PanelWidget
{
public:
    explicit ProbePanel(std::unique_ptr<ui::Widget> Content)
    {
        auto& Slot = addSlot(std::move(Content));
        ui::Layout Stretch;
        Stretch.setPreset(ui::LayoutPreset::StretchAll);
        Slot.setLayout(Stretch);
    }
};

bool nearEqual(float A, float B)
{
    return std::fabs(A - B) < 0.01f;
}

#define CHECK(Expr)                                                  \
    do                                                               \
    {                                                                \
        if (!(Expr))                                                 \
        {                                                            \
            std::cerr << "FAIL: " << #Expr << " (line " << __LINE__ << ")\n"; \
            return 1;                                                \
        }                                                            \
    } while (false)
} // namespace

int main()
{
    // ---- 与 EditorApplication::buildUI 相同的"等比伸缩"flex 配置 ----
    auto Root = std::make_unique<ui::VerticalBox>();
    Root->setSpacing(0);
    Root->setOverflow(ui::EFlexOverflow::Shrink);

    {
        auto Bar = std::make_unique<ui::HorizontalBox>();
        Bar->addChild(std::make_unique<ProbeWidget>(100, 14));
        auto& Slot = Root->addChild(std::make_unique<ProbePanel>(std::move(Bar)));
        Slot.setFlexBasis(32);
        Slot.setFlexGrow(32);
    }

    {
        auto Body = std::make_unique<ui::HorizontalBox>();
        Body->setCrossAxisAlignment(ui::ECrossAxisAlignment::Stretch);
        Body->setOverflow(ui::EFlexOverflow::Shrink);

        {
            auto& Slot = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(80, 20)));
            Slot.setFlexBasis(180);
            Slot.setFlexGrow(180);
        }
        {
            auto& Slot = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(0, 0)));
            Slot.setFlexBasis(880);
            Slot.setFlexGrow(880);
        }
        {
            auto& Slot = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(120, 20)));
            Slot.setFlexBasis(220);
            Slot.setFlexGrow(220);
        }

        auto& Slot = Root->addChild(std::move(Body));
        Slot.setFlexBasis(664);
        Slot.setFlexGrow(664);
    }

    {
        auto Status = std::make_unique<ui::HorizontalBox>();
        Status->addChild(std::make_unique<ProbeWidget>(200, 11));
        auto& Slot = Root->addChild(std::make_unique<ProbePanel>(std::move(Status)));
        Slot.setFlexBasis(24);
        Slot.setFlexGrow(24);
    }

    const auto& Body = [&]() -> const ui::PanelWidget&
    {
        return static_cast<const ui::PanelWidget&>(Root->getChild(1));
    };

    // ---- 参考尺寸 1280x720: 布局必须与改动前完全一致 ----
    Root->arrange({0, 0, 1280, 720});
    CHECK(nearEqual(Root->getChild(0).getBounds().Height, 32));
    CHECK(nearEqual(Root->getChild(1).getBounds().Height, 664));
    CHECK(nearEqual(Root->getChild(2).getBounds().Height, 24));
    CHECK(nearEqual(Body().getChild(0).getBounds().Width, 180));
    CHECK(nearEqual(Body().getChild(1).getBounds().Width, 880));
    CHECK(nearEqual(Body().getChild(2).getBounds().Width, 220));

    // ---- 放大到 1920x1080: 各栏按 1.5 倍等比放大 ----
    Root->arrange({0, 0, 1920, 1080});
    CHECK(nearEqual(Root->getChild(0).getBounds().Height, 48));
    CHECK(nearEqual(Root->getChild(1).getBounds().Height, 996));
    CHECK(nearEqual(Root->getChild(2).getBounds().Height, 36));
    CHECK(nearEqual(Body().getChild(0).getBounds().Width, 270));
    CHECK(nearEqual(Body().getChild(1).getBounds().Width, 1320));
    CHECK(nearEqual(Body().getChild(2).getBounds().Width, 330));

    // ---- 缩小到 640x360: 各栏按 0.5 倍等比收缩, 不裁剪 ----
    Root->arrange({0, 0, 640, 360});
    CHECK(nearEqual(Root->getChild(0).getBounds().Height, 16));
    CHECK(nearEqual(Root->getChild(1).getBounds().Height, 332));
    CHECK(nearEqual(Root->getChild(2).getBounds().Height, 12));
    CHECK(nearEqual(Body().getChild(0).getBounds().Width, 90));
    CHECK(nearEqual(Body().getChild(1).getBounds().Width, 440));
    CHECK(nearEqual(Body().getChild(2).getBounds().Width, 110));

    std::cout << "PASS\n";
    return 0;
}
