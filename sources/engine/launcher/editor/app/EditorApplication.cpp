#include "EditorApplication.hpp"

#include "GenericApplication.hpp"
#include "Window.hpp"
#include "RHIUIRenderer.hpp"
#include "FlexibleSlot.hpp"
#include "HorizontalBox.hpp"
#include "TextWidget.hpp"
#include "VerticalBox.hpp"

#include <renderer/RendererServer.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace editor
{
namespace
{
// ============================================================================
//  调色板(编辑器配色)
// ============================================================================
constexpr ui::Color PanelBackground{0.13f, 0.14f, 0.16f, 1.0f};
constexpr ui::Color PanelHeaderBackground{0.17f, 0.18f, 0.21f, 1.0f};
constexpr ui::Color PanelBorder{0.26f, 0.28f, 0.32f, 1.0f};
constexpr ui::Color TextPrimary{0.90f, 0.91f, 0.93f, 1.0f};
constexpr ui::Color TextMuted{0.58f, 0.61f, 0.66f, 1.0f};
constexpr ui::Color TextAccent{0.40f, 0.78f, 0.92f, 1.0f};
constexpr ui::Color TextValue{0.70f, 0.86f, 0.55f, 1.0f};

// ============================================================================
//  数学辅助(行主序矩阵 + 列向量约定, 与 HLSL 的 mul(M, v) 一致)
// ============================================================================

/** @brief 平移矩阵. */
[[nodiscard]] infra::Matrix4x4 makeTranslation(infra::Vec3f Translation)
{
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][3] = Translation.x();
    Result[1][3] = Translation.y();
    Result[2][3] = Translation.z();
    return Result;
}

/** @brief 绕 Y 轴旋转(弧度). */
[[nodiscard]] infra::Matrix4x4 makeRotationY(float Radians)
{
    const float C = std::cos(Radians);
    const float S = std::sin(Radians);
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][0] = C;
    Result[0][2] = S;
    Result[2][0] = -S;
    Result[2][2] = C;
    return Result;
}

/** @brief 绕 X 轴旋转(弧度). */
[[nodiscard]] infra::Matrix4x4 makeRotationX(float Radians)
{
    const float C = std::cos(Radians);
    const float S = std::sin(Radians);
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[1][1] = C;
    Result[1][2] = -S;
    Result[2][1] = S;
    Result[2][2] = C;
    return Result;
}

/** @brief 等比缩放矩阵. */
[[nodiscard]] infra::Matrix4x4 makeScale(float UniformScale)
{
    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    Result[0][0] = UniformScale;
    Result[1][1] = UniformScale;
    Result[2][2] = UniformScale;
    return Result;
}

/**
 * @brief 构造 Vulkan 约定的透视投影矩阵.
 *
 * Vulkan 的裁剪空间为 Y 向下、深度 0..1, 因此 Y 轴取负、深度按 0..1 映射
 * (与 OpenGL 的 -1..1 不同).
 */
[[nodiscard]] infra::Matrix4x4 makePerspective(float FovYRadians, float Aspect, float Near, float Far)
{
    const float F = 1.0f / std::tan(FovYRadians * 0.5f);
    infra::Matrix4x4 Result {};
    Result[0][0] = F / std::max(Aspect, 0.0001f);
    Result[1][1] = -F;
    Result[2][2] = Far / (Near - Far);
    Result[2][3] = (Far * Near) / (Near - Far);
    Result[3][2] = -1.0f;
    return Result;
}

/** @brief 构造右手系视图矩阵(相机看向 Target). */
[[nodiscard]] infra::Matrix4x4 makeLookAt(infra::Vec3f Eye, infra::Vec3f Target, infra::Vec3f Up)
{
    const infra::Vec3f Backward = (Eye - Target).normalized({0.0f, 0.0f, 1.0f});
    const infra::Vec3f Right = infra::Vec3f::cross(Up, Backward).normalized({1.0f, 0.0f, 0.0f});
    const infra::Vec3f CameraUp = infra::Vec3f::cross(Backward, Right);

    infra::Matrix4x4 Result = infra::Matrix4x4::Identity();
    for (std::size_t Column = 0; Column < 3; ++Column)
    {
        Result[0][Column] = Right[Column];
        Result[1][Column] = CameraUp[Column];
        Result[2][Column] = Backward[Column];
    }
    Result[0][3] = -Right.dot(Eye);
    Result[1][3] = -CameraUp.dot(Eye);
    Result[2][3] = -Backward.dot(Eye);
    return Result;
}

// ============================================================================
//  编辑器控件
// ============================================================================

/**
 * @brief 带背景与边框的面板: 内部只放一个铺满的子控件.
 *
 * 弹性布局的容器本身不绘制, 因此编辑器的每一块区域都用它来"画底色 + 分隔线".
 */
class EditorPanel final : public ui::PanelWidget
{
public:
    EditorPanel(std::unique_ptr<ui::Widget> Content, ui::Color Fill, ui::Color BorderColor)
        : Fill(Fill), BorderColor(BorderColor)
    {
        auto& ContentSlot = addSlot(std::move(Content));
        ui::Layout Stretch;
        Stretch.setPreset(ui::LayoutPreset::StretchAll);
        ContentSlot.setLayout(Stretch);
    }

protected:
    void onPaint(ui::PaintContext& Context) const override
    {
        const ui::UIRect Bounds = getBounds();
        const ui::UIRect Local{0, 0, Bounds.Width, Bounds.Height};
        Context.fillRect(Local, Fill);
        Context.drawRect(Local, BorderColor, 1.0f);
    }

private:
    ui::Color Fill;
    ui::Color BorderColor;
};

/**
 * @brief 3D 场景占位控件: 自身不绘制任何内容.
 *
 * 它的绝对矩形就是"引擎 3D Pass 允许光栅化的区域"; 因为这一块没有被 UI 覆盖,
 * 下层已经渲染好的 3D 场景就会直接透出来.
 */
class SceneViewportWidget final : public ui::LeafWidget
{
protected:
    ui::Vector2 onMeasure(ui::Vector2) const override { return {0, 0}; }
    void onPaint(ui::PaintContext&) const override {}
};

/** @brief 取控件的绝对矩形(逐级累加父控件的位置). */
[[nodiscard]] ui::UIRect absoluteBounds(const ui::Widget& Target)
{
    ui::UIRect Result = Target.getBounds();
    const ui::Slot* ParentSlot = Target.getSlot();
    while (ParentSlot)
    {
        const ui::Widget* Parent = ParentSlot->getParent();
        if (!Parent)
            break;
        const ui::UIRect ParentBounds = Parent->getBounds();
        Result.X += ParentBounds.X;
        Result.Y += ParentBounds.Y;
        ParentSlot = Parent->getSlot();
    }
    return Result;
}

// ============================================================================
//  演示场景: 网格构造
// ============================================================================

/** @brief 场景网格顶点: Position(12B) + Normal(12B) + Color(16B) = 40B. */
struct SceneVertex
{
    float Position[3];
    float Normal[3];
    float Color[4];
};

/** @brief 由源数据创建 host-visible 顶点/索引缓冲(小场景直接上传, 无需 staging). */
std::shared_ptr<rhi::RBuffer> createVertexBuffer(rhi::RDevice& Device,
    std::span<const SceneVertex> Data, const char* DebugName)
{
    rhi::RBuffer::Descriptor_t Desc;
    Desc.Size = Data.size_bytes();
    Desc.Usage = rhi::EBufferUsage::Vertex;
    Desc.MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent;
    Desc.DebugName = DebugName;
    auto Buffer = Device.createBuffer(Desc);
    if (auto* Dst = static_cast<std::byte*>(Buffer->map(0, Data.size_bytes())))
    {
        std::memcpy(Dst, Data.data(), Data.size_bytes());
        Buffer->unmap();
    }
    return Buffer;
}

/** @brief 创建索引缓冲. */
std::shared_ptr<rhi::RBuffer> createIndexBuffer(rhi::RDevice& Device,
    std::span<const std::uint32_t> Data, const char* DebugName)
{
    rhi::RBuffer::Descriptor_t Desc;
    Desc.Size = Data.size_bytes();
    Desc.Usage = rhi::EBufferUsage::Index;
    Desc.MemoryProperty = rhi::EMemoryProperty::HostVisible | rhi::EMemoryProperty::HostCoherent;
    Desc.DebugName = DebugName;
    auto Buffer = Device.createBuffer(Desc);
    if (auto* Dst = static_cast<std::byte*>(Buffer->map(0, Data.size_bytes())))
    {
        std::memcpy(Dst, Data.data(), Data.size_bytes());
        Buffer->unmap();
    }
    return Buffer;
}

/** @brief 构造一个以原点为中心、边长 1 的立方体(24 顶点 / 36 索引, 逐面法线与逐面颜色). */
runtime::renderer::StaticMesh makeUnitBoxMesh(rhi::RDevice& Device,
    const std::array<ui::Color, 6>& FaceColors)
{
    constexpr float H = 0.5f;
    // 六个面: +X, -X, +Y, -Y, +Z, -Z; 每个面 4 个角点(逆时针).
    const std::array<std::array<float, 3>, 6> Normals {{
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } }};
    const std::array<std::array<std::array<float, 3>, 4>, 6> Corners {{
        {{ { H,-H,-H }, { H,-H, H }, { H, H, H }, { H, H,-H } }},
        {{ {-H,-H, H }, {-H,-H,-H }, {-H, H,-H }, {-H, H, H } }},
        {{ {-H, H,-H }, { H, H,-H }, { H, H, H }, {-H, H, H } }},
        {{ {-H,-H, H }, { H,-H, H }, { H,-H,-H }, {-H,-H,-H } }},
        {{ {-H,-H, H }, {-H, H, H }, { H, H, H }, { H,-H, H } }},
        {{ { H,-H,-H }, { H, H,-H }, {-H, H,-H }, {-H,-H,-H } }} }};

    std::vector<SceneVertex> Vertices;
    std::vector<std::uint32_t> Indices;
    Vertices.reserve(24);
    Indices.reserve(36);
    for (std::size_t Face = 0; Face < 6; ++Face)
    {
        const auto Base = static_cast<std::uint32_t>(Vertices.size());
        for (std::size_t Corner = 0; Corner < 4; ++Corner)
        {
            SceneVertex Vertex {};
            std::memcpy(Vertex.Position, Corners[Face][Corner].data(), sizeof(float) * 3);
            std::memcpy(Vertex.Normal, Normals[Face].data(), sizeof(float) * 3);
            const ui::Color& Color = FaceColors[Face];
            Vertex.Color[0] = Color.R;
            Vertex.Color[1] = Color.G;
            Vertex.Color[2] = Color.B;
            Vertex.Color[3] = Color.A;
            Vertices.push_back(Vertex);
        }
        Indices.insert(Indices.end(), { Base + 0, Base + 1, Base + 2, Base + 0, Base + 2, Base + 3 });
    }

    runtime::renderer::StaticMesh Mesh;
    Mesh.VertexBuffer = createVertexBuffer(Device, Vertices, "EditorSceneBoxVB");
    Mesh.IndexBuffer = createIndexBuffer(Device, Indices, "EditorSceneBoxIB");
    Mesh.IndexFormat = rhi::EIndexFormat::UInt32;
    Mesh.IndexCount = static_cast<std::uint32_t>(Indices.size());
    Mesh.BoundsRadius = 0.866f;
    return Mesh;
}

/** @brief 构造一块水平地面(y = Height, 覆盖 ±HalfExtent). */
runtime::renderer::StaticMesh makeGroundMesh(rhi::RDevice& Device, float Height, float HalfExtent)
{
    const std::array<SceneVertex, 4> Vertices {{
        { { -HalfExtent, Height, -HalfExtent }, { 0, 1, 0 }, { 0.20f, 0.21f, 0.24f, 1.0f } },
        { { -HalfExtent, Height,  HalfExtent }, { 0, 1, 0 }, { 0.20f, 0.21f, 0.24f, 1.0f } },
        { {  HalfExtent, Height,  HalfExtent }, { 0, 1, 0 }, { 0.20f, 0.21f, 0.24f, 1.0f } },
        { {  HalfExtent, Height, -HalfExtent }, { 0, 1, 0 }, { 0.20f, 0.21f, 0.24f, 1.0f } } }};
    const std::array<std::uint32_t, 6> Indices {{ 0, 1, 2, 0, 2, 3 }};

    runtime::renderer::StaticMesh Mesh;
    Mesh.VertexBuffer = createVertexBuffer(Device, Vertices, "EditorSceneGroundVB");
    Mesh.IndexBuffer = createIndexBuffer(Device, Indices, "EditorSceneGroundIB");
    Mesh.IndexFormat = rhi::EIndexFormat::UInt32;
    Mesh.IndexCount = static_cast<std::uint32_t>(Indices.size());
    Mesh.BoundsRadius = HalfExtent;
    return Mesh;
}

// ============================================================================
//  演示场景: 代理注册与逐帧动画
// ============================================================================

/** @brief 场景中的一个可动画物体. */
struct SceneObject
{
    std::uint32_t ProxyIndex { 0 };
    float Radius { 0.0f };
    float Height { 0.0f };
    float SpinSpeed { 0.0f };
    float OrbitSpeed { 0.0f };
    float Phase { 0.0f };
    float Scale { 1.0f };
    float Tilt { 0.0f };
};

/** @brief 演示场景: 地面 + 若干绕中心公转/自转的彩色立方体. */
struct DemoScene
{
    std::vector<SceneObject> Objects;
    float Elapsed { 0.0f };
};

/** @brief 把一个静态网格注册成不透明网格代理, 返回其代理索引. */
std::uint32_t addMeshProxy(runtime::renderer::RendererServer& Renderer,
    const std::shared_ptr<runtime::renderer::StaticMesh>& Mesh,
    const infra::Matrix4x4& Transform, std::uint32_t StableOrder, float BoundsRadius)
{
    runtime::renderer::MeshRenderProxy Proxy;
    Proxy.Header.EntityId = StableOrder + 1;
    Proxy.Header.Kind = runtime::renderer::ERenderProxyKind::OpaqueMesh;
    Proxy.Header.MaterialId = 0;
    Proxy.Header.LocalToWorld = Transform;
    Proxy.Header.BoundsCenterWS = { 0.0f, 0.0f, 0.0f };
    Proxy.Header.BoundsRadius = BoundsRadius;
    Proxy.Header.StableOrder = StableOrder;
    Proxy.Mesh = Mesh;
    Proxy.CastShadow = false;
    return Renderer.getRenderProxyWorld().addProxy(std::move(Proxy));
}

/** @brief 构建演示场景(需要渲染器已初始化, 因为要创建 GPU 缓冲). */
DemoScene createDemoScene(runtime::renderer::RendererServer& Renderer)
{
    DemoScene Scene;
    const std::shared_ptr<rhi::RDevice> Device = Renderer.getDevice();
    if (!Device)
        return Scene;

    // 地面: 一块深色地板, 让立方体有"落地"的参照.
    const auto Ground = std::make_shared<runtime::renderer::StaticMesh>(
        makeGroundMesh(*Device, -1.0f, 9.0f));
    (void)addMeshProxy(Renderer, Ground, infra::Matrix4x4::Identity(), 0, 9.0f);

    // 三个不同颜色的立方体, 各自公转 + 自转, 用于证明场景是逐帧在跑的.
    // 每个立方体六个面用略有差异的同色系, 让立体感在光照下更清楚.
    const std::array<std::array<ui::Color, 6>, 3> Palettes {{
        {{ ui::Color{0.88f,0.30f,0.30f,1}, ui::Color{0.70f,0.22f,0.24f,1},
           ui::Color{0.96f,0.42f,0.42f,1}, ui::Color{0.55f,0.18f,0.20f,1},
           ui::Color{0.92f,0.36f,0.36f,1}, ui::Color{0.62f,0.20f,0.22f,1} }},
        {{ ui::Color{0.26f,0.72f,0.42f,1}, ui::Color{0.20f,0.58f,0.34f,1},
           ui::Color{0.34f,0.84f,0.50f,1}, ui::Color{0.16f,0.46f,0.28f,1},
           ui::Color{0.30f,0.78f,0.46f,1}, ui::Color{0.18f,0.52f,0.30f,1} }},
        {{ ui::Color{0.30f,0.52f,0.90f,1}, ui::Color{0.24f,0.42f,0.74f,1},
           ui::Color{0.40f,0.62f,0.98f,1}, ui::Color{0.20f,0.34f,0.60f,1},
           ui::Color{0.34f,0.56f,0.94f,1}, ui::Color{0.22f,0.38f,0.66f,1} }},
    }};
    for (std::size_t Index = 0; Index < Palettes.size(); ++Index)
    {
        const auto Box = std::make_shared<runtime::renderer::StaticMesh>(
            makeUnitBoxMesh(*Device, Palettes[Index]));
        SceneObject Object;
        Object.Radius = 2.1f;
        Object.Height = -0.55f;
        Object.SpinSpeed = 0.9f + 0.35f * static_cast<float>(Index);
        Object.OrbitSpeed = 0.35f + 0.12f * static_cast<float>(Index);
        Object.Phase = 6.2831853f * static_cast<float>(Index) / static_cast<float>(Palettes.size());
        Object.Scale = 1.15f;
        Object.Tilt = 0.15f * static_cast<float>(Index);
        Object.ProxyIndex = addMeshProxy(Renderer, Box, infra::Matrix4x4::Identity(),
            static_cast<std::uint32_t>(Index + 1), 1.0f);
        Scene.Objects.push_back(Object);
    }
    return Scene;
}

/** @brief 推进演示场景的动画: 立方体绕中心公转并自转. */
void tickDemoScene(DemoScene& Scene, runtime::renderer::RendererServer& Renderer, float DeltaSeconds)
{
    Scene.Elapsed += DeltaSeconds;
    for (const SceneObject& Object : Scene.Objects)
    {
        const float Orbit = Object.Phase + Scene.Elapsed * Object.OrbitSpeed;
        const infra::Vec3f Position {
            std::cos(Orbit) * Object.Radius, Object.Height, std::sin(Orbit) * Object.Radius };
        const infra::Matrix4x4 Transform =
            makeTranslation(Position) * makeRotationY(Scene.Elapsed * Object.SpinSpeed)
            * makeRotationX(Object.Tilt) * makeScale(Object.Scale);
        Renderer.getRenderProxyWorld().updateTransform(Object.ProxyIndex, Transform);
    }
}

// ============================================================================
//  编辑器界面构建
// ============================================================================

/** @brief 一行的两个文本: 左键名 + 右值. */
std::unique_ptr<ui::Widget> makePropertyRow(ui::GenericApplication& App, const char32_t* Key,
    const char32_t* Value)
{
    auto Row = std::make_unique<ui::HorizontalBox>();
    Row->setSpacing(6);
    auto& KeySlot = Row->addChild(App.createTextWidget(App.encodeText(Key), 12, 0, TextMuted));
    KeySlot.setFlexBasis(78);
    KeySlot.setAlignSelf(ui::ECrossAxisAlignment::Center);
    auto& ValueSlot = Row->addChild(App.createTextWidget(App.encodeText(Value), 12, 0, TextValue));
    ValueSlot.setFlexGrow(1);
    ValueSlot.setAlignSelf(ui::ECrossAxisAlignment::Center);
    return Row;
}

/** @brief 面板标题条文本. */
std::unique_ptr<ui::Widget> makeSectionTitle(ui::GenericApplication& App, const char32_t* Title)
{
    return App.createTextWidget(App.encodeText(Title), 13, 0, TextAccent);
}
} // namespace

// ============================================================================
//  EditorApplication 实现
// ============================================================================

struct EditorApplication::Impl
{
    ui::GenericApplication& App;
    runtime::renderer::RendererServer& Renderer;
    DemoScene Scene;
    const SceneViewportWidget* ViewportWidget{nullptr};

    Impl(ui::GenericApplication& InApp, runtime::renderer::RendererServer& InRenderer)
        : App(InApp), Renderer(InRenderer)
    {
    }

    void buildScene()
    {
        Scene = createDemoScene(Renderer);
        std::cout << "Editor scene: " << Renderer.getRenderProxyWorld().getProxyCount()
                  << " proxies\n";
    }

    void buildUI()
    {
        auto& Window = App.getMainWindow();
        Window.setBackgroundColor(PanelBackground);

        auto Root = std::make_unique<ui::VerticalBox>();
        Root->setSpacing(0);

        // ---- 菜单栏 ----
        {
            auto Bar = std::make_unique<ui::HorizontalBox>();
            Bar->setSpacing(14);
            Bar->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
            auto& TitleSlot = Bar->addChild(
                App.createTextWidget("Prism Editor", 14, 0, TextPrimary));
            TitleSlot.setMargin({10, 0, 0, 0});
            TitleSlot.setAlignSelf(ui::ECrossAxisAlignment::Center);
            for (const char32_t* Item : { U"File", U"Edit", U"View", U"Window", U"Help" })
                Bar->addChild(App.createTextWidget(App.encodeText(Item), 12, 0, TextMuted))
                    .setAlignSelf(ui::ECrossAxisAlignment::Center);
            auto& HintSlot = Bar->addChild(
                App.createTextWidget("  F11: fullscreen   Esc: quit", 11, 0, TextMuted));
            HintSlot.setFlexGrow(1);
            HintSlot.setAlignSelf(ui::ECrossAxisAlignment::Center);

            auto& Slot = Root->addChild(
                std::make_unique<EditorPanel>(std::move(Bar), PanelHeaderBackground, PanelBorder));
            Slot.setFlexBasis(32);
        }

        // ---- 主体: 大纲 | 视口 | 检查器 ----
        {
            auto Body = std::make_unique<ui::HorizontalBox>();
            Body->setSpacing(0);
            Body->setCrossAxisAlignment(ui::ECrossAxisAlignment::Stretch);

            // 左侧大纲.
            {
                auto Content = std::make_unique<ui::VerticalBox>();
                Content->setSpacing(4);
                auto& HeadSlot = Content->addChild(makeSectionTitle(App, U"OUTLINER"));
                HeadSlot.setMargin({10, 8, 10, 2});
                for (const char32_t* Name :
                    { U"Scene", U"  Ground", U"  RedCube", U"  GreenCube", U"  BlueCube",
                      U"  DirectionalLight", U"  Camera" })
                {
                    const bool bTopLevel = Name[0] != U' ';
                    auto& Slot = Content->addChild(App.createTextWidget(
                        App.encodeText(Name), 12, 0, bTopLevel ? TextPrimary : TextMuted));
                    Slot.setMargin({10, 0, 10, 0});
                }
                auto& Slot = Body->addChild(std::make_unique<EditorPanel>(
                    std::move(Content), PanelBackground, PanelBorder));
                Slot.setFlexBasis(180);
            }

            // 中间视口列: 标题条 + 场景区域 + 相机信息.
            {
                auto Column = std::make_unique<ui::VerticalBox>();
                Column->setSpacing(0);

                // 视口标题条.
                {
                    auto Header = std::make_unique<ui::HorizontalBox>();
                    Header->setSpacing(10);
                    Header->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                    auto& NameSlot = Header->addChild(
                        App.createTextWidget("Viewport", 12, 0, TextPrimary));
                    NameSlot.setMargin({10, 0, 0, 0});
                    auto& ModeSlot = Header->addChild(
                        App.createTextWidget("Perspective", 12, 0, TextAccent));
                    ModeSlot.setFlexGrow(1);
                    auto& StatSlot = Header->addChild(
                        App.createTextWidget("RDG · 3D + UI", 11, 0, TextMuted));
                    StatSlot.setMargin({0, 0, 10, 0});
                    auto& Slot = Column->addChild(std::make_unique<EditorPanel>(
                        std::move(Header), PanelHeaderBackground, PanelBorder));
                    Slot.setFlexBasis(24);
                }

                // 场景区域: 不绘制任何 UI, 透出引擎渲染好的 3D 场景.
                {
                    auto ProbeWidget = std::make_unique<SceneViewportWidget>();
                    ViewportWidget = ProbeWidget.get();
                    auto& Slot = Column->addChild(std::move(ProbeWidget));
                    Slot.setFlexGrow(1);
                    Slot.setFlexBasis(0);
                }

                // 视口脚注.
                {
                    auto Footer = std::make_unique<ui::HorizontalBox>();
                    Footer->setSpacing(10);
                    Footer->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
                    auto& InfoSlot = Footer->addChild(App.createTextWidget(
                        "Camera  pos(6.5, 4.2, 8.4)  fov 50°  near 0.1  far 200  ·  Y-up / Vulkan NDC",
                        11, 0, TextMuted));
                    InfoSlot.setMargin({10, 0, 0, 0});
                    InfoSlot.setFlexGrow(1);
                    auto& Slot = Column->addChild(std::make_unique<EditorPanel>(
                        std::move(Footer), PanelHeaderBackground, PanelBorder));
                    Slot.setFlexBasis(22);
                }

                auto& Slot = Body->addChild(std::move(Column));
                Slot.setFlexGrow(1);
                Slot.setFlexBasis(0);
            }

            // 右侧检查器.
            {
                auto Content = std::make_unique<ui::VerticalBox>();
                Content->setSpacing(5);
                auto& HeadSlot = Content->addChild(makeSectionTitle(App, U"INSPECTOR"));
                HeadSlot.setMargin({10, 8, 10, 2});
                auto& SubSlot = Content->addChild(
                    App.createTextWidget("RedCube  (MeshRenderProxy)", 12, 0, TextPrimary));
                SubSlot.setMargin({10, 0, 10, 4});
                for (const auto& Row : { std::pair<const char32_t*, const char32_t*>{
                         U"Kind", U"OpaqueMesh" },
                         { U"Material", U"Default" }, { U"CastShadow", U"false" },
                         { U"ProxyIndex", U"1" }, { U"Vertices", U"24" },
                         { U"Indices", U"36" }, { U"Pipeline", U"DefaultMesh" } })
                {
                    auto& Slot = Content->addChild(makePropertyRow(App, Row.first, Row.second));
                    Slot.setMargin({10, 0, 10, 0});
                }
                auto& Slot = Body->addChild(std::make_unique<EditorPanel>(
                    std::move(Content), PanelBackground, PanelBorder));
                Slot.setFlexBasis(220);
            }

            auto& Slot = Root->addChild(std::move(Body));
            Slot.setFlexGrow(1);
            Slot.setFlexBasis(0);
        }

        // ---- 状态栏 ----
        {
            auto Status = std::make_unique<ui::HorizontalBox>();
            Status->setSpacing(16);
            Status->setCrossAxisAlignment(ui::ECrossAxisAlignment::Center);
            auto& Left = Status->addChild(App.createTextWidget(
                "Ready  |  3 proxies  |  DefaultMesh fallback", 11, 0, TextMuted));
            Left.setMargin({10, 0, 0, 0});
            Left.setFlexGrow(1);
            auto& Right = Status->addChild(
                App.createTextWidget("Vulkan 1.4 · RTX 5070 Ti", 11, 0, TextMuted));
            Right.setMargin({0, 0, 10, 0});
            auto& Slot = Root->addChild(std::make_unique<EditorPanel>(
                std::move(Status), PanelHeaderBackground, PanelBorder));
            Slot.setFlexBasis(24);
        }

        Window.setContentRoot(std::move(Root));
    }

    void renderFrame(float DeltaSeconds)
    {
        // (1) 场景动画: 立方体公转 + 自转.
        tickDemoScene(Scene, Renderer, std::min(DeltaSeconds, 0.1f));

        // (2) 3D 场景矩形 = 视口控件的绝对矩形(逻辑坐标, 与 UI 顶点同一坐标空间).
        ui::UIRect SceneRect { 0, 0, 0, 0 };
        if (ViewportWidget)
            SceneRect = absoluteBounds(*ViewportWidget);

        // (3) 相机: 视图 + 投影(投影按视口宽高比计算, 保证不拉伸).
        {
            const float Width = static_cast<float>(std::max(1.0f, SceneRect.Width));
            const float Height = static_cast<float>(std::max(1.0f, SceneRect.Height));
            const infra::Vec3f Eye { 6.5f, 4.2f, 8.4f };
            runtime::renderer::RenderView View;
            View.View = makeLookAt(Eye, { 0.0f, 0.1f, 0.0f }, { 0.0f, 1.0f, 0.0f });
            View.Projection = makePerspective(0.8726646f /* 50° */, Width / Height, 0.1f, 200.0f);
            View.ViewProjection = View.Projection * View.View;
            View.CameraPosition = Eye;
            Renderer.setRenderView(View);
        }

        // (4) 告诉渲染器"3D 只画在这个矩形里".
        Renderer.setSceneViewport(rhi::RenderArea {
            static_cast<std::int32_t>(SceneRect.X), static_cast<std::int32_t>(SceneRect.Y),
            static_cast<std::uint32_t>(std::max(0.0f, SceneRect.Width)),
            static_cast<std::uint32_t>(std::max(0.0f, SceneRect.Height)) });

        // (5) 提交 UI 绘制列表与纹理脏区(编辑器界面画在场景之上, 中间留空).
        if (auto* UI = dynamic_cast<ui::RHIUIRenderer*>(&App.getRenderer()))
            Renderer.submitUIDrawLists(UI->getDrawLists(), UI->collectTextureUploads());

        // (6) 跑一帧 RDG: Shadow → DepthPre → Opaque → Transparent → Post → UI.
        (void)Renderer.renderFrameGraph();
    }
};

EditorApplication::EditorApplication(ui::GenericApplication& App, runtime::renderer::RendererServer& Renderer)
    : ImplPtr(std::make_unique<Impl>(App, Renderer))
{
}

EditorApplication::~EditorApplication() = default;

void EditorApplication::initialize()
{
    ImplPtr->buildScene();
    ImplPtr->buildUI();
}

void EditorApplication::tick(float DeltaTime)
{
    ImplPtr->renderFrame(DeltaTime);
}

void EditorApplication::destroy()
{
    // 场景代理由 RenderProxyWorld 持有, 随 Renderer 关闭一起回收; 此处无需额外清理.
}

} // namespace editor
