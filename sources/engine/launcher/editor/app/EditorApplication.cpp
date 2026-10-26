#include "EditorApplication.hpp"

#include "GenericApplication.hpp"
#include "Window.hpp"
#include "RHIUIRenderer.hpp"
#include "FlexibleSlot.hpp"
#include "HorizontalBox.hpp"
#include "TextWidget.hpp"
#include "VerticalBox.hpp"
#include "PlatformEvents.hpp"

#include "engine/IGameInstance.hpp"
#include "engine/IPlaySession.hpp"
#include "engine/SimulationClock.hpp"
#include <logger/Logger.hpp>
#include <renderer/RendererServer.hpp>
#include <renderer/WorldRenderBridge.hpp>
#include <world/WorldSnapshot.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
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

constexpr float Pi = 3.14159265358979f;

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
//  演示场景: 网格构造(共享几何; 编辑世界与运行世界引用同一份)
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
        { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }}};
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
//  EditorApplication 实现(编辑器会话宿主)
// ============================================================================

struct EditorApplication::Impl
{
    enum class EMode : std::uint8_t
    {
        Edit,      // 编辑: 无运行世界; 渲染编辑世界, 用编辑器相机.
        Play,      // 运行: 运行世界 tick; 渲染运行世界, 用游戏相机.
        Simulate,  // 模拟: 运行世界 tick; 渲染运行世界, 用编辑器相机.
    };

    ui::GenericApplication& App;
    runtime::renderer::RendererServer& Renderer;
    std::unique_ptr<runtime::IGameInstance> GameInstance;

    // 模拟时钟(暂停/单步/慢放/固定步长/脱离渲染模拟).
    runtime::SimulationClock Clock;

    // 共享几何: 构建一次, 编辑世界与运行世界引用同一份 GPU 几何.
    std::array<std::shared_ptr<runtime::renderer::StaticMesh>, 3> BoxMeshes {};
    std::shared_ptr<runtime::renderer::StaticMesh> GroundMesh {};

    // 编辑世界(可编辑关卡, 编辑器所有); 运行世界是其副本(由 PlaySession 持有).
    runtime::world::WorldSnapshot EditorWorld;

    // 资源解析器(GeometryId -> StaticMesh) + 桥(逻辑快照 -> 渲染代理).
    struct DemoResolver final : runtime::renderer::IRenderResourceResolver
    {
        std::array<std::shared_ptr<runtime::renderer::StaticMesh>, 3> Boxes;
        std::shared_ptr<runtime::renderer::StaticMesh> Ground;

        std::shared_ptr<runtime::renderer::StaticMesh> resolveStaticMesh(std::uint64_t GeometryId) const override
        {
            switch (GeometryId)
            {
            case 1: return Boxes[0];
            case 2: return Boxes[1];
            case 3: return Boxes[2];
            case 4: return Ground;
            default: return nullptr;
            }
        }
        bool resolveTerrainPatch(std::uint64_t, runtime::renderer::TerrainPatchData&) const override { return false; }
        bool resolveFluidSurface(std::uint64_t, runtime::renderer::FluidSurfaceData&) const override { return false; }
        bool resolveGasVolume(std::uint64_t, runtime::renderer::GasVolumeData&) const override { return false; }
    };
    std::unique_ptr<DemoResolver> Resolver;
    runtime::renderer::WorldRenderBridge Bridge { nullptr };

    std::unique_ptr<runtime::IPlaySession> PlaySession;
    EMode Mode { EMode::Edit };
    bool SessionFailed { false };
    std::string LastStatus;

    const SceneViewportWidget* ViewportWidget { nullptr };
    ui::TextWidget* StatusText { nullptr };

    Impl(ui::GenericApplication& InApp, runtime::renderer::RendererServer& InRenderer,
        std::unique_ptr<runtime::IGameInstance> InGameInstance)
        : App(InApp), Renderer(InRenderer), GameInstance(std::move(InGameInstance))
    {
    }

    void buildMeshes()
    {
        const std::shared_ptr<rhi::RDevice> Device = Renderer.getDevice();
        if (!Device)
            return;

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
        for (std::size_t I = 0; I < Palettes.size(); ++I)
        {
            BoxMeshes[I] = std::make_shared<runtime::renderer::StaticMesh>(
                makeUnitBoxMesh(*Device, Palettes[I]));
        }
        GroundMesh = std::make_shared<runtime::renderer::StaticMesh>(
            makeGroundMesh(*Device, -1.0f, 9.0f));
    }

    void buildResolver()
    {
        Resolver = std::make_unique<DemoResolver>();
        Resolver->Boxes = BoxMeshes;
        Resolver->Ground = GroundMesh;
        Bridge.setResolver(Resolver.get());
    }

    /** @brief 构建编辑世界(可编辑关卡, 编辑器所有)。运行世界是它的副本. */
    void buildEditorWorld()
    {
        EditorWorld.clear();
        EditorWorld.Revision = 1;

        const auto AddMesh = [&](std::uint64_t EntityId, runtime::world::ELogicObjectKind Kind,
            std::uint64_t GeometryId, std::uint32_t StableOrder,
            const infra::Matrix4x4& Transform, float BoundsRadius) {
            runtime::world::LogicObjectSnapshot Object;
            Object.Header.Id = EntityId;
            Object.Header.Kind = Kind;
            Object.Header.LocalToWorld = Transform;
            Object.Header.PreviousLocalToWorld = Transform;
            Object.Header.BoundsCenterWS = { 0.0f, 0.0f, 0.0f };
            Object.Header.BoundsRadius = BoundsRadius;
            Object.Header.StableOrder = StableOrder;
            runtime::world::LogicRenderableRef Ref;
            Ref.GeometryId = GeometryId;
            Ref.MaterialId = 0;
            Ref.Transparent = false;
            Ref.CastShadow = false;
            Object.Payload = Ref;
            EditorWorld.Objects.push_back(std::move(Object));
        };

        // 地面: 静态.
        AddMesh(1, runtime::world::ELogicObjectKind::StaticRenderable, 4, 0,
            infra::Matrix4x4::Identity(), 9.0f);

        // 三个彩色立方体: 动态可渲染, 由运行世界(Play 会话)动画; 初始为轨道起点.
        for (std::size_t I = 0; I < 3; ++I)
        {
            const float Index = static_cast<float>(I);
            const float Radius = 2.1f;
            const float Height = -0.55f;
            const float Phase = 2.0f * Pi * Index / 3.0f;
            const float Tilt = 0.15f * Index;
            const float Scale = 1.15f;
            const infra::Vec3f Position {
                std::cos(Phase) * Radius, Height, std::sin(Phase) * Radius };
            const infra::Matrix4x4 Transform =
                makeTranslation(Position) * makeRotationX(Tilt) * makeScale(Scale);
            AddMesh(2 + I, runtime::world::ELogicObjectKind::DynamicRenderable,
                1 + I, static_cast<std::uint32_t>(I + 1), Transform, 1.0f);
        }
    }

    void buildUI()
    {
        auto& Window = App.getMainWindow();
        Window.setBackgroundColor(PanelBackground);

        auto Root = std::make_unique<ui::VerticalBox>();
        Root->setSpacing(0);
        Root->setOverflow(ui::EFlexOverflow::Shrink);

        // 布局采用"等比伸缩"而非固定像素: 每个 flex 子项同时设置 flexBasis=参考尺寸 与
        // flexGrow=同一参考尺寸, 使得 1280x720 下布局与原来完全一致, 而窗口放大后各栏按
        // 同一比例同步增长(而不是只有中间视口吞掉全部增量).
        // 参考尺寸为默认窗口 1280x720:
        //   水平(主体)  左大纲 : 中间视口 : 右检查器 = 180 : 880 : 220
        //   垂直(根)    菜单栏 : 主体 : 状态栏       =  32 : 664 :  24
        //   垂直(视口列) 标题条 : 场景 : 脚注        =  24 : 618 :  22

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
                App.createTextWidget("  F5 Play  ·  F6 Pause  ·  F7 Step  ·  F9 Simulate  ·  1-4 speed", 11, 0, TextMuted));
            HintSlot.setFlexGrow(1);
            HintSlot.setAlignSelf(ui::ECrossAxisAlignment::Center);

            auto& Slot = Root->addChild(
                std::make_unique<EditorPanel>(std::move(Bar), PanelHeaderBackground, PanelBorder));
            Slot.setFlexBasis(32);
            Slot.setFlexGrow(32);
        }

        // ---- 主体: 大纲 | 视口 | 检查器 ----
        {
            auto Body = std::make_unique<ui::HorizontalBox>();
            Body->setSpacing(0);
            Body->setCrossAxisAlignment(ui::ECrossAxisAlignment::Stretch);
            Body->setOverflow(ui::EFlexOverflow::Shrink);

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
                Slot.setFlexGrow(180);
            }

            // 中间视口列: 标题条 + 场景区域 + 相机信息.
            {
                auto Column = std::make_unique<ui::VerticalBox>();
                Column->setSpacing(0);
                Column->setOverflow(ui::EFlexOverflow::Shrink);

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
                    Slot.setFlexGrow(24);
                }

                // 场景区域: 不绘制任何 UI, 透出引擎渲染好的 3D 场景.
                {
                    auto ProbeWidget = std::make_unique<SceneViewportWidget>();
                    ViewportWidget = ProbeWidget.get();
                    auto& Slot = Column->addChild(std::move(ProbeWidget));
                    Slot.setFlexBasis(618);
                    Slot.setFlexGrow(618);
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
                    Slot.setFlexGrow(22);
                }

                auto& Slot = Body->addChild(std::move(Column));
                Slot.setFlexBasis(880);
                Slot.setFlexGrow(880);
            }

            // 右侧检查器.
            {
                auto Content = std::make_unique<ui::VerticalBox>();
                Content->setSpacing(5);
                auto& HeadSlot = Content->addChild(makeSectionTitle(App, U"INSPECTOR"));
                HeadSlot.setMargin({10, 8, 10, 2});
                auto& SubSlot = Content->addChild(
                    App.createTextWidget("RedCube  (DynamicRenderable)", 12, 0, TextPrimary));
                SubSlot.setMargin({10, 0, 10, 4});
                for (const auto& Row : { std::pair<const char32_t*, const char32_t*>{
                         U"Kind", U"Dynamic" },
                         { U"Material", U"Default" }, { U"CastShadow", U"false" },
                         { U"GeometryId", U"1" }, { U"Vertices", U"24" },
                         { U"Indices", U"36" }, { U"Pipeline", U"DefaultMesh" } })
                {
                    auto& Slot = Content->addChild(makePropertyRow(App, Row.first, Row.second));
                    Slot.setMargin({10, 0, 10, 0});
                }
                auto& Slot = Body->addChild(std::make_unique<EditorPanel>(
                    std::move(Content), PanelBackground, PanelBorder));
                Slot.setFlexBasis(220);
                Slot.setFlexGrow(220);
            }

            auto& Slot = Root->addChild(std::move(Body));
            Slot.setFlexBasis(664);
            Slot.setFlexGrow(664);
        }

        // ---- 状态栏 ----
        {
            auto Status = std::make_unique<ui::HorizontalBox>();
            Status->setSpacing(16);
            auto StatusWidget = App.createTextWidget(
                "Edit  |  Running  |  1.00x  |  Render", 11, 0, TextMuted);
            StatusText = StatusWidget.get();
            auto& Left = Status->addChild(std::move(StatusWidget));
            Left.setMargin({10, 0, 0, 0});
            Left.setFlexGrow(1);
            auto& Right = Status->addChild(
                App.createTextWidget("Vulkan 1.4 · RTX 5070 Ti", 11, 0, TextMuted));
            Right.setMargin({0, 0, 10, 0});
            auto& Slot = Root->addChild(std::make_unique<EditorPanel>(
                std::move(Status), PanelHeaderBackground, PanelBorder));
            Slot.setFlexBasis(24);
            Slot.setFlexGrow(24);
        }

        Window.setContentRoot(std::move(Root));
    }

    /** @brief 编辑器命令: Play/Stop/Pause/Step/慢放/Simulate 的按键入口. */
    bool onKey(const runtime::platform::KeyEvent& Key)
    {
        if (!Key.Pressed || Key.Repeat)
            return false;

        using runtime::platform::PhysicalKey;
        switch (Key.Physical)
        {
        case PhysicalKey::F5: // Play / Stop
            (Mode == EMode::Play) ? stopPlay() : startPlay(EMode::Play);
            return true;
        case PhysicalKey::F9: // Simulate / Stop
            (Mode == EMode::Simulate) ? stopPlay() : startPlay(EMode::Simulate);
            return true;
        case PhysicalKey::F6: // Pause / Resume
            Clock.togglePause();
            return true;
        case PhysicalKey::F7: // 单步(暂停时推进一帧)
            Clock.requestStep();
            return true;
        case PhysicalKey::F8: // 脱离渲染模拟(无头追赶)
            Clock.setSimulateWithoutRender(!Clock.isSimulateWithoutRender());
            return true;
        case PhysicalKey::Digit1: Clock.setTimeScale(0.25f); return true;
        case PhysicalKey::Digit2: Clock.setTimeScale(1.0f); return true;
        case PhysicalKey::Digit3: Clock.setTimeScale(2.0f); return true;
        case PhysicalKey::Digit4: Clock.setTimeScale(4.0f); return true;
        default:
            return false;
        }
    }

    void startPlay(EMode NewMode)
    {
        if (PlaySession)
            stopPlay();
        if (!GameInstance)
            return;

        // copy-on-play: 用编辑世界创建一份运行世界副本, 后续演化只发生在副本上.
        PlaySession = GameInstance->createPlaySession(EditorWorld);
        PlaySession->beginPlay();
        Clock.resume();
        Clock.setSimulateWithoutRender(false);
        Mode = NewMode;
        LOG_INFO(runtime::DefaultCategory, "[EditorHost] {} started (world copy of {} objects)",
            Mode == EMode::Play ? "Play" : "Simulate", EditorWorld.Objects.size());
    }

    void stopPlay()
    {
        if (!PlaySession)
            return;
        try
        {
            PlaySession->endPlay();
        }
        catch (...)
        {
            // 关闭路径不得抛出.
        }
        PlaySession.reset(); // 丢弃运行世界副本; 编辑世界不受影响.
        Mode = EMode::Edit;
        Clock.resume();
        LOG_INFO(runtime::DefaultCategory, "[EditorHost] Play stopped, editor world restored");
    }

    [[nodiscard]] runtime::renderer::RenderView computeView() const
    {
        ui::UIRect SceneRect { 0, 0, 0, 0 };
        if (ViewportWidget)
            SceneRect = absoluteBounds(*ViewportWidget);
        const float Width = static_cast<float>(std::max(1.0f, SceneRect.Width));
        const float Height = static_cast<float>(std::max(1.0f, SceneRect.Height));

        runtime::SessionCamera Camera;
        bool UseGameCamera = (Mode == EMode::Play) && PlaySession;
        if (UseGameCamera)
        {
            const auto GameCamera = PlaySession->getActiveCamera();
            if (GameCamera)
                Camera = *GameCamera;
            else
                UseGameCamera = false;
        }
        if (!UseGameCamera)
        {
            // 编辑器相机(编辑/模拟): 固定视角.
            Camera.View = makeLookAt({ 6.5f, 4.2f, 8.4f }, { 0.0f, 0.1f, 0.0f }, { 0.0f, 1.0f, 0.0f });
            Camera.Position = { 6.5f, 4.2f, 8.4f };
        }

        // 投影由宿主按实际视口重算(会话只提供取景).
        Camera.Projection = makePerspective(0.8726646f /* 50° */, Width / Height, 0.1f, 200.0f);
        Camera.ViewProjection = Camera.Projection * Camera.View;

        runtime::renderer::RenderView View;
        View.View = Camera.View;
        View.Projection = Camera.Projection;
        View.ViewProjection = Camera.ViewProjection;
        View.CameraPosition = Camera.Position;
        return View;
    }

    void updateStatusText()
    {
        if (!StatusText)
            return;
        const char* ModeName = Mode == EMode::Edit ? "Edit" : (Mode == EMode::Play ? "Play" : "Simulate");
        const char* ClockState = Clock.isPaused() ? "Paused" : "Running";
        const char* RenderState = Clock.isSimulateWithoutRender() ? "SimOnly" : "Render";
        const std::string Status = std::format("{}  |  {}  |  {:.2f}x  |  {}", ModeName, ClockState,
            Clock.getTimeScale(), RenderState);
        if (Status != LastStatus)
        {
            LastStatus = Status;
            StatusText->setText(Status);
        }
    }

    void tick(float RealDeltaTime)
    {
        // 1. "谁 tick 谁"矩阵: 仅 Play/Simulate 时推进运行世界(经模拟时钟); 编辑态不 tick 游戏.
        if (PlaySession)
        {
            SessionFailed = false;
            Clock.advance(RealDeltaTime, [&](float DeltaTime) {
                if (SessionFailed)
                    return;
                try
                {
                    PlaySession->tick(DeltaTime);
                }
                catch (const std::exception& Error)
                {
                    LOG_ERROR(runtime::DefaultCategory, "[EditorHost] Play session tick failed: {}", Error.what());
                    SessionFailed = true;
                }
                catch (...)
                {
                    LOG_ERROR(runtime::DefaultCategory, "[EditorHost] Play session tick failed: unknown error");
                    SessionFailed = true;
                }
            });
            // 故障隔离: 游戏异常/请求结束 -> 停止 Play 并上报, 编辑器继续运行.
            if (SessionFailed || PlaySession->wantsToEnd())
                stopPlay();
        }

        // 2. 提交活动世界到渲染器(copy-on-play: 编辑世界 vs 运行世界).
        auto& ProxyWorld = Renderer.getRenderProxyWorld();
        if (PlaySession)
            (void)Bridge.pullFrom(*PlaySession, ProxyWorld);
        else
            (void)Bridge.importSnapshot(EditorWorld, ProxyWorld);

        // 3. 相机选择(编辑器相机 vs 游戏相机) + 渲染视图 + 场景视口.
        Renderer.setRenderView(computeView());

        ui::UIRect SceneRect { 0, 0, 0, 0 };
        if (ViewportWidget)
            SceneRect = absoluteBounds(*ViewportWidget);
        Renderer.setSceneViewport(rhi::RenderArea {
            static_cast<std::int32_t>(SceneRect.X), static_cast<std::int32_t>(SceneRect.Y),
            static_cast<std::uint32_t>(std::max(0.0f, SceneRect.Width)),
            static_cast<std::uint32_t>(std::max(0.0f, SceneRect.Height)) });

        // 4. UI 绘制列表(编辑器界面画在场景之上, 中间留空).
        if (auto* UI = dynamic_cast<ui::RHIUIRenderer*>(&App.getRenderer()))
            Renderer.submitUIDrawLists(UI->getDrawLists(), UI->collectTextureUploads());

        // 5. 状态栏刷新(仅在变化时).
        updateStatusText();

        // 6. 渲染一帧 RDG(脱离渲染模拟时跳过).
        if (!Clock.isSimulateWithoutRender())
            (void)Renderer.renderFrameGraph();
    }
};

EditorApplication::EditorApplication(ui::GenericApplication& App, runtime::renderer::RendererServer& Renderer,
    std::unique_ptr<runtime::IGameInstance> GameInstance)
    : ImplPtr(std::make_unique<Impl>(App, Renderer, std::move(GameInstance)))
{
}

EditorApplication::~EditorApplication() = default;

void EditorApplication::preinitialize()
{
    LOG_INFO(runtime::DefaultCategory, "[EditorHost] preinitialize");
}

void EditorApplication::initialize()
{
    ImplPtr->buildMeshes();
    ImplPtr->buildResolver();
    ImplPtr->buildEditorWorld();
    ImplPtr->buildUI();

    // 注册按键钩子: 驱动 Play/Stop/Pause/Step/慢放/Simulate 等编辑器命令.
    ImplPtr->App.setKeyHandler([Impl = ImplPtr.get()](const runtime::platform::KeyEvent& Key) {
        return Impl->onKey(Key);
    });

    LOG_INFO(runtime::DefaultCategory, "[EditorHost] initialize (editor world with {} objects)",
        ImplPtr->EditorWorld.Objects.size());
}

void EditorApplication::postinitialize()
{
    LOG_INFO(runtime::DefaultCategory, "[EditorHost] postinitialize (first frame)");
}

void EditorApplication::tick(float RealDeltaTime)
{
    ImplPtr->tick(RealDeltaTime);
}

void EditorApplication::destroy()
{
    // 拆解顺序: 先停所有 Play 会话(先停游戏), 再释放 GameInstance; 渲染器随后由引擎
    // 在子系统逆序关闭阶段拆除 —— 保证"游戏先于渲染器"关闭。
    ImplPtr->stopPlay();
    ImplPtr->App.setKeyHandler(nullptr); // 先解除输入钩子, 避免悬空回调.
    if (ImplPtr->GameInstance)
        ImplPtr->GameInstance->shutdown();
    ImplPtr->GameInstance.reset();
    LOG_INFO(runtime::DefaultCategory, "[EditorHost] destroy");
}

} // namespace editor
