#include "GenericApplication.hpp"
#include "ApplicationCommandLine.hpp"
#include "CommandLineParser.hpp"
#include "CanvasPanel.hpp"
#include "TextWidget.hpp"
#include "FlexibleSlot.hpp"
#include "HorizontalBox.hpp"
#include "VerticalBox.hpp"
#include "ImageWidget.hpp"
#include "ImageImporter.hpp"
#include "Style.hpp"
#include "ModuleLoader.hpp"
#include "RTGlobal.hpp"
#include "game/Game.hpp"
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <utility>

namespace
{
    constexpr ui::Color Red{0.86f, 0.20f, 0.22f, 1};
    constexpr ui::Color Green{0.20f, 0.72f, 0.38f, 1};
    constexpr ui::Color Blue{0.24f, 0.51f, 0.88f, 1};
    constexpr ui::Color Yellow{0.92f, 0.74f, 0.22f, 1};
    constexpr ui::Color Purple{0.56f, 0.34f, 0.78f, 1};
    constexpr ui::Color Cyan{0.20f, 0.70f, 0.76f, 1};
    constexpr ui::Color Orange{0.92f, 0.52f, 0.18f, 1};
    constexpr ui::Color Gray{0.42f, 0.46f, 0.50f, 1};
    constexpr ui::Color Border{0.08f, 0.08f, 0.08f, 1};

    // 按系数提亮颜色, 用于选中态高亮.
    ui::Color Brighten(ui::Color Color, float Factor)
    {
        return {std::min(Color.R * Factor, 1.0f), std::min(Color.G * Factor, 1.0f),
                std::min(Color.B * Factor, 1.0f), Color.A};
    }

    // 固定尺寸的彩色盒子: 参与 flex 测量(返回固定尺寸), 并绘制填充 + 描边.
    class SizedBox final : public ui::LeafWidget
    {
    public:
        SizedBox(ui::Vector2 Size, ui::Color Fill, ui::Color Stroke, float Thickness = 1.5f)
            : Size(Size), Fill(Fill), Stroke(Stroke), Thickness(Thickness) {}

    protected:
        ui::Vector2 onMeasure(ui::Vector2) const override { return Size; }

        void onPaint(ui::PaintContext& Context) const override
        {
            const auto Bounds = getBounds();
            Context.fillRect({0, 0, Bounds.Width, Bounds.Height}, Fill);
            Context.drawRect({0, 0, Bounds.Width, Bounds.Height}, Stroke, Thickness);
        }

    private:
        ui::Vector2 Size;
        ui::Color Fill;
        ui::Color Stroke;
        float Thickness;
    };

    // 可交互的带标签盒子: 悬停时高亮边框, 点击时切换选中并打印命中的盒子,
    // 用于验证 Z 序命中(重叠处应命中 Z 更高者).
    class LabeledBox final : public ui::PanelWidget
    {
    public:
        LabeledBox(std::string Tag, ui::Color Fill, std::unique_ptr<ui::TextWidget> Label)
            : Tag(std::move(Tag)), Fill(Fill)
        {
            setAcceptEvents(true);
            auto& TextSlot = addSlot(std::move(Label));
            ui::Layout Center;
            Center.setPreset(ui::LayoutPreset::Center);
            TextSlot.setLayout(Center);
            TextSlot.setAutoSize(true);
        }

    protected:
        void onPaint(ui::PaintContext& Context) const override
        {
            const auto Bounds = getBounds();
            const ui::Color CurrentFill = Selected ? Brighten(Fill, 1.35f) : Fill;
            const ui::Color CurrentBorder = (Selected || Hovered) ? ui::Color{1, 1, 1, 1} : Border;
            const float Thickness = (Selected || Hovered) ? 3.0f : 2.0f;
            Context.fillRect({0, 0, Bounds.Width, Bounds.Height}, CurrentFill);
            Context.drawRect({0, 0, Bounds.Width, Bounds.Height}, CurrentBorder, Thickness);
        }

        ui::EventReply onMouseEnter(const ui::MouseMoveEvent&, ui::UIPoint) override
        {
            Hovered = true;
            invalidate(ui::EInvalidationReason::Paint);
            return ui::EventReply::Handled;
        }

        ui::EventReply onMouseLeave(const ui::MouseMoveEvent&, ui::UIPoint) override
        {
            Hovered = false;
            invalidate(ui::EInvalidationReason::Paint);
            return ui::EventReply::Handled;
        }

        ui::EventReply onMouseButtonClicked(const ui::MouseButtonEvent&, ui::UIPoint) override
        {
            Selected = !Selected;
            invalidate(ui::EInvalidationReason::Paint);
            std::cout << "Clicked box: " << Tag << " (selected=" << (Selected ? "true" : "false") << ")\n";
            return ui::EventReply::Handled;
        }

    private:
        std::string Tag;
        ui::Color Fill;
        mutable bool Hovered{};
        mutable bool Selected{};
    };
}

int main(int Argc, char** Argv)
{
    try
    {
        // 启动引擎子系统: 模块加载器 + 日志 + 引擎(先于 Editor 启动).
        ModuleLoader::self();
        GSeedEngine.preinitialize();
        // 托管一个演示游戏: 引擎会在自己的 initialize/run/destroy 中驱动其生命周期.
        GSeedEngine.attachGame(game::createDemoGame());
        GSeedEngine.initialize();

        ui::ApplicationOptions Options;
        ui::CommandLineParser Parser("Editor", "编辑器特性演示");
        ui::registerApplicationOptions(Parser, Options);
        const auto Result = Parser.parse(Argc, Argv, std::cout, std::cerr);
        if (Result.ShouldExit)
        {
            return Result.ExitCode;
        }

        auto& App = ui::GenericApplication::self();
        App.initialize(std::move(Options));
        {
            auto& Window = App.getMainWindow();

            // 调色板工具演示: 上层调用者填充颜色, 底层框架只提供 Palette/EColorRole 工具.
            ui::Palette DemoPalette;
            DemoPalette.setColor(ui::EColorRole::Window, {0.94f, 0.95f, 0.94f, 1});
            DemoPalette.setColor(ui::EColorRole::Text, {1, 1, 1, 1});
            DemoPalette.setColor(ui::EColorRole::Accent, Cyan);
            Window.setBackgroundColor(DemoPalette.color(ui::EColorRole::Window));

            // 窗口分辨率策略演示: Preset 档位 (F11 全屏; setResolution 会吸附到最近档位).
            Window.setPresetResolutions({{1280, 720}, {1600, 900}, {1920, 1080}});
            Window.setResizePolicy(ui::EWindowResizePolicy::Preset);
            std::cout << "Window resize policy: Preset, " << Window.getPresetResolutions().size() << " tiers\n";

            const auto MakeText = [&](const char32_t* Text, float Size, ui::Color Color)
            {
                return App.createTextWidget(App.encodeText(Text), Size, 0, Color);
            };
            const auto MakeBox = [&](ui::Vector2 Size, ui::Color Color)
            {
                return std::make_unique<SizedBox>(Size, Color, Border, 1.5f);
            };

            // 根布局: 一个垂直盒子, 逐段演示各类特性. 交叉轴 Stretch 让各段铺满宽度.
            auto Root = std::make_unique<ui::VerticalBox>();
            Root->setSpacing(3);

            // 标题.
            {
                auto& Slot = Root->addChild(MakeText(U"Editor 特性演示: 一维 Flex 布局 (主轴/交叉轴/方向/间距/对齐/grow) + 锚点画布", 22, DemoPalette.color(ui::EColorRole::Text)));
                Slot.setAlignSelf(ui::ECrossAxisAlignment::Center);
                Slot.setMargin({16, 8, 16, 2});
            }

            // 1. 主轴对齐 justify-content: 六种模式各一行, 盒子在行内横向分布.
            {
                auto& Caption = Root->addChild(MakeText(U"1. 主轴对齐 justify-content (HorizontalBox)", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 4, 16, 0});
            }
            const auto AddMainRow = [&](const char32_t* Name, ui::EMainAxisAlignment Mode)
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(8);
                auto& Label = Row->addChild(MakeText(Name, 13, Gray));
                Label.setFlexBasis(52);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                auto Box = std::make_unique<ui::HorizontalBox>();
                Box->setSpacing(5);
                Box->setMainAxisAlignment(Mode);
                Box->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                Box->addChild(MakeBox({34, 16}, Red));
                Box->addChild(MakeBox({46, 16}, Green));
                Box->addChild(MakeBox({28, 16}, Blue));
                auto& BoxSlot = Row->addChild(std::move(Box));
                BoxSlot.setFlexGrow(1);
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            };
            AddMainRow(U"起点", ui::EMainAxisAlignment::Start);
            AddMainRow(U"居中", ui::EMainAxisAlignment::Center);
            AddMainRow(U"末尾", ui::EMainAxisAlignment::End);
            AddMainRow(U"两端", ui::EMainAxisAlignment::SpaceBetween);
            AddMainRow(U"环绕", ui::EMainAxisAlignment::SpaceAround);
            AddMainRow(U"均分", ui::EMainAxisAlignment::SpaceEvenly);

            // 2. 交叉轴对齐 align-items: 固定行高, 盒子高度不同, 观察其纵向摆放.
            {
                auto& Caption = Root->addChild(MakeText(U"2. 交叉轴对齐 align-items (行高固定 34)", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            const auto AddCrossRow = [&](const char32_t* Name, ui::ECrossAxisAlignment Mode)
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(10);
                Row->setCrossAxisAlignment(Mode);
                auto& Label = Row->addChild(MakeText(Name, 13, Gray));
                Label.setFlexBasis(52);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                Row->addChild(MakeBox({40, 10}, Red));
                Row->addChild(MakeBox({40, 22}, Green));
                Row->addChild(MakeBox({40, 32}, Blue));
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
                RowSlot.setFlexBasis(34);
            };
            AddCrossRow(U"起点", ui::ECrossAxisAlignment::Start);
            AddCrossRow(U"居中", ui::ECrossAxisAlignment::Center);
            AddCrossRow(U"末尾", ui::ECrossAxisAlignment::End);
            AddCrossRow(U"拉伸", ui::ECrossAxisAlignment::Stretch);

            // 3. flex-grow / 间距 / 反向 / 边距.
            {
                auto& Caption = Root->addChild(MakeText(U"3. flex-grow / 间距 / 反向 / 边距 / 收缩", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            // grow: 剩余宽度按 1:2 分配给两个盒子.
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(6);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                auto& Label = Row->addChild(MakeText(U"grow", 13, Gray));
                Label.setFlexBasis(52);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                auto& G1 = Row->addChild(MakeBox({40, 22}, Purple));
                G1.setFlexGrow(1);
                auto& G2 = Row->addChild(MakeBox({40, 22}, Orange));
                G2.setFlexGrow(2);
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            }
            // 间距: spacing=16.
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(16);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                auto& Label = Row->addChild(MakeText(U"间距", 13, Gray));
                Label.setFlexBasis(52);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                Row->addChild(MakeBox({40, 22}, Red));
                Row->addChild(MakeBox({40, 22}, Green));
                Row->addChild(MakeBox({40, 22}, Blue));
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            }
            // 反向: RowReverse, 标签作为最后一个子项, 反向后落在最左侧.
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setDirection(ui::EFlexDirection::RowReverse);
                Row->setSpacing(6);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                Row->addChild(MakeBox({36, 22}, Red));
                Row->addChild(MakeBox({36, 22}, Green));
                Row->addChild(MakeBox({36, 22}, Blue));
                Row->addChild(MakeText(U"反向", 13, Gray));
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            }
            // 边距: 黄色盒子左右各加 18 的 margin.
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(6);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                auto& Label = Row->addChild(MakeText(U"边距", 13, Gray));
                Label.setFlexBasis(52);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                auto& M = Row->addChild(MakeBox({40, 22}, Yellow));
                M.setMargin({18, 0, 18, 0});
                Row->addChild(MakeBox({40, 22}, Red));
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            }
            // 收缩 Shrink: 空间不足时按 flex-shrink 权重收缩(标签不参与收缩), 对比默认裁剪.
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(6);
                Row->setOverflow(ui::EFlexOverflow::Shrink);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                auto& Label = Row->addChild(MakeText(U"收缩", 13, Gray));
                Label.setFlexBasis(52);
                Label.setFlexShrink(0);
                Label.setAlignSelf(ui::ECrossAxisAlignment::Center);
                Row->addChild(MakeBox({220, 22}, Red));
                Row->addChild(MakeBox({220, 22}, Green));
                Row->addChild(MakeBox({220, 22}, Blue));
                Row->addChild(MakeBox({220, 22}, Purple));
                Row->addChild(MakeBox({220, 22}, Orange));
                Row->addChild(MakeBox({220, 22}, Yellow));
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 0});
            }

            // 4. VerticalBox: 主轴沿 Y 的垂直布局.
            {
                auto& Caption = Root->addChild(MakeText(U"4. VerticalBox 垂直盒子 (间距 + 居中)", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            {
                auto VBox = std::make_unique<ui::VerticalBox>();
                VBox->setSpacing(6);
                VBox->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                VBox->addChild(MakeBox({120, 16}, Red));
                VBox->addChild(MakeBox({160, 16}, Green));
                VBox->addChild(MakeBox({90, 16}, Blue));
                auto& RowSlot = Root->addChild(std::move(VBox));
                RowSlot.setMargin({16, 0, 16, 4});
                RowSlot.setFlexBasis(62);
            }

            // 5. CanvasPanel: 锚点 / 位置 / 尺寸 / 对齐 / Z 序 / 旋转 / 透明度.
            {
                auto& Caption = Root->addChild(MakeText(U"5. CanvasPanel 锚点 / Z序 / 旋转 / 透明度 (悬停高亮, 点击选中)", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            {
                auto Canvas = std::make_unique<ui::CanvasPanel>();
                Canvas->setEnablePaintDashedBorder(true);
                Canvas->setBorderColor({1, 0.2f, 0.2f, 1});
                Canvas->setBorderThickness(2);

                const auto AddCorner = [&](ui::LayoutPreset Preset, ui::Color Color, const char* Tag,
                                           const char32_t* Label, ui::Vector2 Position, ui::Vector2 Size)
                {
                    auto& Slot = Canvas->addChildToCanvas(std::make_unique<LabeledBox>(Tag, Color, MakeText(Label, 13, {1, 1, 1, 1})));
                    ui::Layout IVisual;
                    IVisual.setPreset(Preset);
                    Slot.setLayout(IVisual);
                    Slot.setSize(Size);
                    Slot.setPosition(Position);
                };
                AddCorner(ui::LayoutPreset::TopLeft, Red, "TopLeft", U"左上", {10, 6}, {104, 40});
                AddCorner(ui::LayoutPreset::TopRight, Green, "TopRight", U"右上", {-114, 6}, {104, 40});
                AddCorner(ui::LayoutPreset::BottomLeft, Blue, "BottomLeft", U"左下", {10, -46}, {104, 40});
                AddCorner(ui::LayoutPreset::BottomRight, Yellow, "BottomRight", U"右下", {-114, -46}, {104, 40});

                // 旋转: 轴心设为中心, 绕轴心旋转 30 度.
                {
                    auto& Slot = Canvas->addChildToCanvas(std::make_unique<LabeledBox>("Rotate", Purple, MakeText(U"旋转30°", 13, {1, 1, 1, 1})));
                    ui::Layout IVisual;
                    IVisual.setPreset(ui::LayoutPreset::Center);
                    Slot.setLayout(IVisual);
                    Slot.setSize({104, 40});
                    Slot.setPosition({0, 0});
                    Slot.setAlignment({0.5f, 0.5f});
                    Slot.setRotation(0.5235987756f);
                }

                // Z 序 + 透明度: 顶层盒子 Z=5 半透明, 覆盖底层盒子.
                {
                    auto& Back = Canvas->addChildToCanvas(std::make_unique<LabeledBox>("Back", Gray, MakeText(U"底层 Z=0", 13, {1, 1, 1, 1})));
                    ui::Layout IVisual;
                    IVisual.setPreset(ui::LayoutPreset::Center);
                    Back.setLayout(IVisual);
                    Back.setSize({140, 64});
                    Back.setPosition({70, -12});
                    Back.setZOrder(0);

                    auto& Front = Canvas->addChildToCanvas(std::make_unique<LabeledBox>("Front", Orange, MakeText(U"顶层 Z=5 半透明", 13, {1, 1, 1, 1})));
                    Front.setLayout(IVisual);
                    Front.setSize({110, 48});
                    Front.setPosition({100, 6});
                    Front.setZOrder(5);
                    Front.getChild()->setOpacity(0.7f);
                }

                auto& RowSlot = Root->addChild(std::move(Canvas));
                RowSlot.setMargin({16, 0, 16, 12});
                RowSlot.setFlexBasis(120);
                RowSlot.setFlexGrow(1);
            }

            // 6. ImageWidget + RenderTransform.
            {
                auto& Caption = Root->addChild(MakeText(U"6. ImageWidget (RenderTarget CPU 表示) + RenderTransform", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(24);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);

                // 生成一张渐变图(CPU 像素), 演示 Image 的像素缓冲.
                auto Image = std::make_shared<ui::Image>(96, 96);
                auto Pixels = Image->getPixels();
                for (int Y = 0; Y < 96; ++Y)
                {
                    for (int X = 0; X < 96; ++X)
                    {
                        const auto G = static_cast<unsigned char>((X * 255) / 95);
                        const auto B = static_cast<unsigned char>((Y * 255) / 95);
                        const std::size_t I = (static_cast<std::size_t>(Y) * 96 + X) * 4;
                        Pixels[I] = std::byte{0x20};
                        Pixels[I + 1] = static_cast<std::byte>(G);
                        Pixels[I + 2] = static_cast<std::byte>(B);
                        Pixels[I + 3] = std::byte{0xFF};
                    }
                }

                auto Picture = std::make_unique<ui::ImageWidget>(Image);
                ui::RenderTransform RT;
                RT.Scale = {1.5f, 1.5f};
                RT.Rotation = 0.4f;
                RT.Pivot = {0.5f, 0.5f};
                Picture->setRenderTransform(RT);
                Row->addChild(std::move(Picture)).setAlignSelf(ui::ECrossAxisAlignment::Center);

                // 平铺 + 图集: 用左上 48x48 子区域平铺到 120x90.
                auto Tiled = std::make_unique<ui::ImageWidget>(Image);
                Tiled->setSize({120, 90});
                Tiled->setFillMode(ui::EImageFillMode::Repeat);
                Tiled->setSourceUV({0, 0, 0.5f, 0.5f});
                Row->addChild(std::move(Tiled)).setAlignSelf(ui::ECrossAxisAlignment::Center);

                auto Text = MakeText(U"RenderTransform 文字", 18, DemoPalette.color(ui::EColorRole::Text));
                ui::RenderTransform TextRT;
                TextRT.Rotation = -0.25f;
                TextRT.Pivot = {0.5f, 0.5f};
                Text->setRenderTransform(TextRT);
                Row->addChild(std::move(Text)).setAlignSelf(ui::ECrossAxisAlignment::Center);

                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 10});
                RowSlot.setFlexBasis(170);
            }

            // 7. 图片导入(ImageImporter 通用模块)渲染 assets/001.jpg.
            {
                auto& Caption = Root->addChild(MakeText(U"7. 图片导入 ImageImporter 渲染 assets/001.jpg", 15, DemoPalette.color(ui::EColorRole::Accent)));
                Caption.setMargin({16, 6, 16, 0});
            }
            {
                auto Row = std::make_unique<ui::HorizontalBox>();
                Row->setSpacing(16);
                Row->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                auto Photo = ui::ImageImporter::loadFromFile("assets/001.jpg");
                if (Photo)
                {
                    auto Picture = std::make_unique<ui::ImageWidget>(Photo);
                    Picture->setSize({260, 180});
                    Row->addChild(std::move(Picture)).setAlignSelf(ui::ECrossAxisAlignment::Center);
                    Row->addChild(MakeText(U"已加载 assets/001.jpg", 14, DemoPalette.color(ui::EColorRole::Text)))
                        .setAlignSelf(ui::ECrossAxisAlignment::Center);
                }
                else
                {
                    Row->addChild(MakeText(U"未找到 assets/001.jpg", 14, {1, 0.4f, 0.4f, 1}))
                        .setAlignSelf(ui::ECrossAxisAlignment::Center);
                }
                auto& RowSlot = Root->addChild(std::move(Row));
                RowSlot.setMargin({16, 0, 16, 12});
                RowSlot.setFlexBasis(200);
            }

            Window.setContentRoot(std::move(Root));
        }
        const int ExitCode = App.run();
        GSeedEngine.destroy();
        return ExitCode;
    }
    catch (const std::exception& Error)
    {
        std::cerr << "Editor error: " << Error.what() << '\n';
        return 1;
    }
}
