// 端到端验证: 最大化窗口后, Window::drawFrame 是否按新安全区重新布局内容根.
#include "Window.hpp"
#include "Widget.hpp"
#include "slots/FlexibleSlot.hpp"
#include "widgets/FlexibleBox.hpp"
#include "widgets/HorizontalBox.hpp"
#include "widgets/VerticalBox.hpp"
#include "IUIRenderer.hpp"
#include <IGenericPlatform.hpp>

#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>

namespace
{
class ProbeWidget final : public ui::LeafWidget
{
public:
    ProbeWidget(float W, float H) : W(W), H(H) {}
protected:
    ui::Vector2 onMeasure(ui::Vector2) const override { return {W, H}; }
private:
    float W, H;
};

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

class FakeRenderer final : public ui::IUIRenderer
{
public:
    bool canBeginFrame(const ui::UIFrameInfo&) override { return true; }
    void submitDrawList(const ui::UIDrawList&, bool) override { ++Submitted; }
    void endFrame() override {}
    ui::EUIMode getUIMode() const noexcept override { return ui::EUIMode::Retained; }
    ui::TextureHandle createTexture(const ui::UITextureData&) override { return {}; }
    void updateTexture(ui::TextureHandle, const ui::UITextureUpdate&) override {}
    void destroyTexture(ui::TextureHandle) override {}
    int Submitted = 0;
};

void pump(runtime::platform::IGenericPlatform& Platform)
{
    while (Platform.pollEvent()) {}
}
} // namespace

int main()
{
    auto Platform = runtime::platform::createPlatform();
    if (!Platform)
    {
        std::printf("SKIP: no platform\n");
        return 0;
    }

    auto Root = std::make_unique<ui::VerticalBox>();
    Root->setSpacing(0);
    {
        auto Bar = std::make_unique<ui::HorizontalBox>();
        Bar->addChild(std::make_unique<ProbeWidget>(100, 14));
        auto& Slot = Root->addChild(std::make_unique<ProbePanel>(std::move(Bar)));
        Slot.setFlexBasis(32);
    }
    {
        auto Body = std::make_unique<ui::HorizontalBox>();
        Body->setCrossAxisAlignment(ui::ECrossAxisAlignment::Stretch);
        auto& S0 = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(80, 20)));
        S0.setFlexBasis(180);
        auto& S1 = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(0, 0)));
        S1.setFlexGrow(1);
        S1.setFlexBasis(0);
        auto& S2 = Body->addChild(std::make_unique<ProbePanel>(std::make_unique<ProbeWidget>(120, 20)));
        S2.setFlexBasis(220);
        auto& Slot = Root->addChild(std::move(Body));
        Slot.setFlexGrow(1);
        Slot.setFlexBasis(0);
    }
    {
        auto Status = std::make_unique<ui::HorizontalBox>();
        Status->addChild(std::make_unique<ProbeWidget>(200, 11));
        auto& Slot = Root->addChild(std::make_unique<ProbePanel>(std::move(Status)));
        Slot.setFlexBasis(24);
    }

    runtime::platform::WindowConfigs Configs;
    Configs.Title = "WindowResizeTest";
    Configs.Width = 800;
    Configs.Height = 600;

    ui::Window Window(*Platform, Configs, std::move(Root));
    FakeRenderer Renderer;

    auto report = [&](const char* Label)
    {
        const auto Safe = Window.getNativeWindow().getSafeArea();
        const auto Size = Window.getNativeWindow().getSize();
        const auto& RootPanel = static_cast<const ui::PanelWidget&>(*Window.getContentRoot());
        const auto& BodyPanel = static_cast<const ui::PanelWidget&>(RootPanel.getChild(1));
        const float ViewportWidth = BodyPanel.getChild(1).getBounds().Width;
        const float BodyHeight = BodyPanel.getBounds().Height;
        std::printf("%s: safe=%dx%d size=%dx%d bodyHeight=%.0f viewportWidth=%.0f\n",
            Label, Safe.Width, Safe.Height, Size.Width, Size.Height, BodyHeight, ViewportWidth);
    };

    // 初始布局.
    Window.drawFrame(&Renderer);
    report("initial");

    // 最大化 + 泵事件.
    auto* Native = static_cast<SDL_Window*>(Window.getNativeWindow().getNativeHandle());
    SDL_MaximizeWindow(Native);
    pump(*Platform);
    report("after maximize (before draw)");

    // 再画一帧.
    Window.drawFrame(&Renderer);
    report("after maximize (after draw)");

    const auto& RootPanel = static_cast<const ui::PanelWidget&>(*Window.getContentRoot());
    const auto& BodyPanel = static_cast<const ui::PanelWidget&>(RootPanel.getChild(1));
    const float ViewportWidth = BodyPanel.getChild(1).getBounds().Width;

    // 初始 800 宽: 视口 = 800 - 180 - 220 = 400. 最大化后应显著变大.
    const bool Ok = ViewportWidth > 400.0f;
    std::printf("viewport width = %.0f (>400 expected after maximize): %s\n",
        ViewportWidth, Ok ? "PASS" : "FAIL");
    return Ok ? 0 : 1;
}
