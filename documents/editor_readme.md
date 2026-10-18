# SDL3 跨平台编辑器基础

## 构建

使用 CMake 3.24+, C++23 编译器以及仓库内的 SDL3.Windows 可直接使用 VS Code 的 CMake Tools 配置, 构建和运行测试；Linux/macOS 也使用同一份 CMake 工程.SDL 的平台依赖仍需按其文档安装.

最低版本保留为 3.24, 不需要升级到 CMake 4.x.依赖全部从 Git 子模块源码构建, 配置阶段不下载代码, 不使用 FetchContent, 也不要求系统 FreeType/HarfBuzz/libpng/zlib 包.首次克隆后需执行 `git submodule update --init --recursive`；源码已就绪时可以离线配置.C++23 编译器和标准库支持仍需独立满足, CMake 版本不能替代编译器要求.

默认构建共享 SDL, Windows 自动将 SDL3 运行时 DLL 复制到可执行文件旁；也可以配置 `SDL_SHARED=OFF`, `SDL_STATIC=ON` 使用静态库.FreeType/HarfBuzz/libpng/zlib 固定静态构建, 使用同一编译器和运行时配置, 不需要 vcpkg 或额外字体和图像库 DLL.所有可执行文件输出至构建目录的配置子目录（例如 Debug）, 兼容 Ninja, Visual Studio 和 Xcode.`EDITOR_BUILD_TESTS` 与标准 `BUILD_TESTING` 均须开启才创建测试.Linux/macOS 构建目录使用 CMake 自动生成的运行时搜索路径；当前尚未提供完整安装打包规则.

渲染使用 SDL GPU RHI, 不直接依赖 Vulkan SDK.Windows/Linux 示例默认 Vulkan, 需要可用的 Vulkan 驱动；macOS 默认 Metal, 因为 SDL GPU 的原生 macOS 后端为 Metal.`--backend auto` 让 SDL 选择后端；显式选择 Vulkan 失败时报告错误, 不静默切换.后续添加着色器时, 必须为目标后端提供正确的 SPIR-V, DXIL 或 Metal 格式.

源码依赖在 [cmake/Dependencies.cmake](cmake/Dependencies.cmake) 中统一配置: 先静态构建 zlib 和 libpng，再构建 FreeType，最后构建带 FreeType 集成的 HarfBuzz，避免循环依赖。`find_package(PNG)` 与 `find_package(ZLIB)` 由本仓库查找模块解析为源码目标，不搜索系统包。FreeType 要求这两个依赖，不静默关闭 PNG 或退回内置 zlib。关闭 BZip2、Brotli、ICU 和 WOFF2。earcut 使用 v2.2.4 纯头文件。

2026-10-09 核对并检出的稳定版：libpng `v1.6.59`（`cd952f49f95bb27154ae77dbb103032d95f6e580`，来源 `pnggroup/libpng`），zlib `v1.3.2`（`da607da739fa6047df13e66a2af6b8bec7c2a498`）。依赖不跟随开发分支；提交父仓库时须一并包含这两个子模块的 gitlink 变更。已移除旧版 libpng 的 macOS `math.h` 和 CMake policy 兼容参数；保留生成配置失效检查，防止切换版本后复用旧 zlib 配置头。macOS 构建及 11 项测试通过，升级后的 Windows/Linux 尚未验证。

[.gitmodules](.gitmodules) 注册 `extern/SDL`, `extern/freetype`, `extern/harfbuzz`, `extern/zlib`, `extern/libpng`, 具体提交由父仓库 gitlink 锁定, 不自动跟随 main（`extern/earcut` 是直接提交的纯头文件, 不是子模块）.权威头文件来自 `extern/earcut`.不要提交单机绝对路径.dxc 仍为必要构建工具；macOS 生成 Metal shader 还需要 spirv-cross（可通过 Homebrew 安装）.

Shader 源码只使用 HLSL: 默认所有平台生成 SPIR-V, Windows 额外生成 DXIL, macOS 额外通过 spirv-cross 生成 MSL.可用 `EDITOR_UI_BUILD_DXIL`, `EDITOR_UI_BUILD_MSL` 显式控制；仅生成 MSL 时要求 spirv-cross.GPU 只声明已生成的格式, 显式请求缺失格式会报错.工具目前在配置阶段仍为必要依赖，包括构建测试目标时.生成资源位于构建目录 `generated-ui-shaders`, 构建后复制至各可执行文件旁的 `shaders`（兼容多配置目录）；加载优先使用可执行文件相对路径, 再回退到构建资源路径.移动程序时必须同时携带 shader 目录和动态库.

## 结构

公开头文件与实现按同一组分类镜像存放: `include/Editor/<分类>/X.hpp` 对应 `src/<分类>/X.cpp`, 分类为 `Core`, `Platform`, `App`, `Text`, `Render`, `UI`, `CommandLine`.第三方源码在 `extern/`, 构建脚本在 `cmake/`, shader 源码在 `shaders/`, 测试在 `tests/`, 可执行文件入口为仓库根的 `Main.cpp`.

- [include/Editor/Core/Events.hpp](include/Editor/Core/Events.hpp): 引擎事件与自有枚举, `std::variant` + `Event::get<T>()`.
- [include/Editor/Platform/IGenericWindow.hpp](include/Editor/Platform/IGenericWindow.hpp): 窗口虚接口, 不暴露 SDL 类型或原生句柄.
- [include/Editor/Platform/IGenericPlatform.hpp](include/Editor/Platform/IGenericPlatform.hpp): 平台虚接口及 `createPlatform()` 运行时后端工厂.
- [include/Editor/Render/IUIRenderer.hpp](include/Editor/Render/IUIRenderer.hpp): 帧提交和纹理资源的后端无关虚接口.
- [include/Editor/Render/UIRenderer.hpp](include/Editor/Render/UIRenderer.hpp): SDL GPU 三角形, 图像, 裁剪和混合实现.
- [include/Editor/Render/PaintContext.hpp](include/Editor/Render/PaintContext.hpp): Widget 绘图 API, 输出有序绘制列表.
- [include/Editor/Render/UIDrawList.hpp](include/Editor/Render/UIDrawList.hpp): 自有顶点, 批次, 资源句柄和逻辑坐标.
- [include/Editor/FontService/FontService.hpp](include/Editor/FontService/FontService.hpp): 全局单例文本会话, 字体加载, 塑形, 排版与共享字形图集缓存.
- [include/Editor/Text/TextConfig.hpp](include/Editor/Text/TextConfig.hpp), [src/Text/TextConfig.cpp](src/Text/TextConfig.cpp): UTF8, UTF16, UTF32 和自定义解码配置, 原始字节视图及兼容的 UTF-8 decodeText/encodeText 包装.
- [include/Editor/Text/Unicode.hpp](include/Editor/Text/Unicode.hpp), [src/Text/Unicode.cpp](src/Text/Unicode.cpp): 独立 Unicode 转换模块, 提供 UTF8/16/32 两两转换的六个 API、严格标量验证及按配置直接解码为内部 UTF-32/编码为外部字节的入口.
- [include/Editor/FontService/TextLayout.hpp](include/Editor/FontService/TextLayout.hpp): 实现通用 Layout 的预塑形文本快照, 保存度量, 字形, caret 和分配区域.
- [include/Editor/UI/Slots/Layout.hpp](include/Editor/UI/Slots/Layout.hpp), [src/UI/Slots/Layout.cpp](src/UI/Slots/Layout.cpp): 通用 measure/arrange 契约, 布局输入验证及锚点计算.
- [include/Editor/UI/Slots/Slot.hpp](include/Editor/UI/Slots/Slot.hpp), [src/UI/Slots/Slot.cpp](src/UI/Slots/Slot.cpp): 父容器拥有的多态 Slot, 以及 PanelSlot 锚点/自动尺寸, SingleSlot Padding/对齐策略.
- [src/Platform/Sdl/](src/Platform/Sdl/): SDL 平台, 窗口及事件转换；`SdlInternal.hpp` 是内部头文件, 只通过 `src/` 私有包含目录对本工程可见, 不进入公开包含目录.
- [src/Platform/PlatformFactory.cpp](src/Platform/PlatformFactory.cpp): 根据平台后端枚举创建具体实现.
- [include/Editor/App/GenericApplication.hpp](include/Editor/App/GenericApplication.hpp), [src/App/GenericApplication.cpp](src/App/GenericApplication.cpp): GenericApplication 单例封装平台, Renderer, 主窗口及事件循环.
- [Main.cpp](Main.cpp): 唯一入口和成品组装, 通过 `App.createTextWidget()` 创建文本, 显式创建 Root 并添加 Slot, 再交给 Window.
- [include/Editor/App/Window.hpp](include/Editor/App/Window.hpp): UI 窗口持有原生窗口和可选的内容根控件（ContentRoot）, 不持有 Renderer 或 FontService；Renderer 由 GenericApplication 创建并持有.
- [include/Editor/UI/Widget.hpp](include/Editor/UI/Widget.hpp): 实现 Layout 的 Widget 基类及 LeafWidget, ContentWidget, PanelWidget；容器通过 Slot 管理孩子, 非虚 `paint()` 管理坐标和裁剪, 虚 `onPaint()` 定义绘制内容.
- [include/Editor/UI/Widgets/TextWidget.hpp](include/Editor/UI/Widgets/TextWidget.hpp): 继承 LeafWidget, 文本构造借用全局 FontService 会话, 并支持兼容的 TextLayout 快照构造.
- [include/Editor/UI/Widgets/CanvasPanel.hpp](include/Editor/UI/Widgets/CanvasPanel.hpp), [src/UI/Widgets/CanvasPanel.cpp](src/UI/Widgets/CanvasPanel.cpp): 对标 UE `UCanvasPanel` 的容器, 提供专用 `CanvasSlot`（继承 `PanelSlot`）与 `addChildToCanvas`, 支持显式 Z 序与 debug 边框.
- [tests/PlatformTests.cpp](tests/PlatformTests.cpp): 转换单测及 dummy 视频驱动下的真实窗口/事件队列集成测试.

## 命名与格式约定

自有类型, 枚举值, 字段, 参数和局部变量使用 PascalCase（首字母大写）, 函数使用以动词开头的 lowerCamelCase, 例如 `createPlatform()`, `getSize()`, `pollEvent()`.命名空间使用小写.代码采用 4 空格缩进和 Allman 大括号风格.SDL 原生类型, 函数, 常量和字段保持原名, 不按自有命名规则改写.

## 运行时平台选择

应用通过虚接口使用平台和窗口, 不依赖具体 SDL 类: 

```cpp
auto Platform = ui::createPlatform(ui::PlatformBackend::Sdl);
auto Window = Platform->createWindow();
const auto Size = Window->getSize();
while (auto Event = Platform->pollEvent())
{
	// Consume backend-independent events.
}
```

`Auto` 当前选择 SDL.显式请求尚未实现的 `Win32`, `AppKit` 会报错, 不静默回退.平台选择发生在创建时, 不支持既有窗口原地切换后端.未来后端实现 `IGenericPlatform`, `IGenericWindow`, 在自身平台层转换原生事件, 并在工厂及 CMake 中注册即可；通用事件数据仍定义在 `Events.hpp`.

窗口平台与 GPU 后端是不同概念.当前 `UIRenderer` 仍为 SDL GPU 实现, 仅接受 SDL 窗口；其他窗口实现会在 GPU 初始化之前被明确拒绝.添加原生窗口后端时需另行提供兼容的渲染接入.窗口与平台已经移除 PImpl, 渲染器仍保留自己的 PImpl.

## 窗口内容根与安全区域

`Window(Platform, Configs, ContentRoot)` 的第三个参数为可选的 `std::unique_ptr<Widget>`, 默认 nullptr；Window 不隐式创建 PanelWidget.内容根可以是任意未挂载到父 Slot 的 Widget.`setContentRoot()` 转移独占所有权并销毁旧树, nullptr 清空；`getContentRoot()` 返回可空观察指针, `takeContentRoot()` 移出拥有树的 unique_ptr.Window 不创建, 不保存 Renderer；GenericApplication 创建并持有 `shared_ptr<UIRenderer>`, `GenericApplication::getRenderer()` 返回借用引用, 无 GPU 时抛出 `logic_error`.TextWidget 只借用全局 FontService 会话, 不保留共享 Renderer；共享 Renderer 由服务保留至 `shutdown()`.移出的树和外部快照不会延长会话或图集寿命, 必须在应用退出前释放依赖；shutdown 后快照中的纹理句柄失效, 不支持跨应用销毁继续使用.

`IGenericWindow::getSafeArea()` 返回 `Rect{X, Y, Width, Height}`, 表示窗口逻辑坐标中的安全交互客户区, 不是 framebuffer 像素.SDL 实现调用 `SDL_GetWindowSafeArea()`；桌面无额外限制时通常等于完整客户区.`SDL_EVENT_WINDOW_SAFE_AREA_CHANGED` 转为 `WindowEvent` 的 `WindowAction::SafeAreaChanged`, 收到通知后重新查询 `getSafeArea()`, 不要把事件的 `Data1`/`Data2` 当作完整矩形.

`Window::drawFrame(IUIRenderer* Renderer = nullptr)` 实际执行布局、绘制和呈现一帧, 不是能力查询: 先把非空内容根排列到当前安全区域, 再通过传入的非拥有 Renderer 指针提交帧.保留模式下（渲染器 `EUIMode::Retained`, 默认）只在失效时重排与重建绘制列表, 否则复用上一帧结果并提示渲染器复用 GPU 缓冲；立即模式（`--ui-mode immediate`）每帧全量重算.`getLastUpdate()` 返回上一帧因失效实际做了哪些工作（`UIUpdateReport{DidLayout, DidPaint, Reasons}`）.调用期间 Renderer 必须有效并适用于该窗口；nullptr 时仍按当前模式排列内容根, 但不绘制且返回 false.GenericApplication 通过 `MainWindow->drawFrame(Renderer.get())` 驱动.空内容根仍可呈现背景帧.

## 通用布局与 Slot

[include/Editor/UI/Slots/Layout.hpp](include/Editor/UI/Slots/Layout.hpp) 定义共同的 `measure(Vector2 AvailableSize) const` 和 `arrange(UIRect Bounds)` 接口, `Widget`, 多态 `Slot` 和 `TextLayout` 都实现它.`measure()` 查询期望尺寸（desired size）, 不分配 Bounds；`arrange()` 接收父级分配的实际区域（assigned bounds）.两者分离: 基础 Widget 的 `setBounds()` 同时设置手动期望尺寸和当前 Bounds, 但后续 `arrange()` 只更新分配区域, 不把拉伸后的尺寸写回期望尺寸.容器期望尺寸由 Slot 测量得出, `TextWidget` 则使用文本度量.

`Widget::measure()`, `Widget::arrange()` 是 final 验证入口；自定义控件通过 `onMeasure()`, `onArrange()` 扩展.排列先更新自身 Bounds, 再以原点 `(0,0)` 的本地内容区域调用 `onArrange()` 和排列孩子.通用布局只处理测量与区域分配元数据, 不执行 Renderer 操作, 也不负责文本塑形.窗口每帧绘制前排列内容根, Slot 在需要自动尺寸或对齐时查询孩子的期望尺寸；目前没有失效缓存.

采用 UE 风格的原则: **Slot 类型由父容器决定, 不由孩子类型决定**, 但不依赖 UE 实现.`PanelWidget` 创建 `PanelSlot`, `CanvasPanel` 创建专用的 `CanvasSlot`（继承 `PanelSlot`）, `ContentWidget` 创建 `SingleSlot`（继承 `PanelSlot`）, 三者均可接收同一类型的 `TextWidget`；文本控件不自带或选择 Slot.父容器独占拥有 Slot, Slot 再通过 `unique_ptr<Widget>` 独占拥有孩子, 孩子的 `getSlot()` 和 Slot 的 `getParent()` 只是观察指针.布局规则属于这段父子关系, 而不是 Widget 自身的持久属性.

- `PanelWidget::addSlot()` 返回可配置的 `PanelSlot&`；兼容的 `addChild()` 也创建 PanelSlot, 但返回孩子引用.PanelSlot 支持锚点和 `setAutoSize()`；没有锚点且未启用自动尺寸时保留手动 Bounds.`PanelWidget::measure()` 取各 Slot 期望尺寸的逐轴最大值, 不将孩子串行堆叠.
- `CanvasPanel::addChildToCanvas()` 返回专用 `CanvasSlot&`, 在 PanelSlot 锚点/自动尺寸之上再提供 UE 风格粒度入口（`setAnchors`/`setPosition`/`setSize`/`setAlignment`/`setOffsets`）与显式 `setZOrder()`；Z 序越大越靠前（后绘制、先命中）, 相等时按添加顺序.debug 构建下还可用 `setEnablePaintBorder`/`setEnablePaintDashedBorder` 画实线/虚线调试边框.
- `ContentWidget::addSlot()` 返回 `SingleSlot&`, 替换旧 Slot/孩子, 并启用布局, 默认双轴 `Alignment::Fill` 填满内容区域.`setPadding()` 与水平/垂直对齐 setter 也会启用布局；Padding 为有限非负值, 对齐支持 `Start`, `Center`, `End`, `Fill`, 非 Fill 轴使用不超过可用空间的期望尺寸.
- 兼容的 `ContentWidget::setChild()` 创建默认禁用布局的 SingleSlot, 保留手动 Bounds（已有锚点仍按 PanelSlot 规则处理）；传 nullptr 清空, 替换销毁旧孩子.`setLayoutEnabled(false)` 可回到此手动/Canvas 模式.启用 SingleSlot 布局时先扣除 Padding；若有锚点, 锚点优先于对齐, 并在内侧区域计算.

布尔状态查询使用 `PanelSlot::isAutoSize()` 和 `SingleSlot::isLayoutEnabled()`；对应修改入口仍为 `setAutoSize()`, `setLayoutEnabled()`.

当前范围仅包含 Canvas 与单孩子的 Padding/对齐布局, 不包含 Stack/Grid, 失效传播或测量/排列缓存.

## 锚点布局

[include/Editor/UI/Slots/Layout.hpp](include/Editor/UI/Slots/Layout.hpp) 中的 `AnchorLayout` 提供双锚点, Pivot, Position, Size 和两端 Margin, 由父级 PanelSlot（或 SingleSlot）保存和应用.布局向量复用 [include/Editor/Core/Vector.hpp](include/Editor/Core/Vector.hpp) 中的 `core::Vector<float, 2>`, 通过 `Vector2` 别名使用；`UIRect` 提供 `getPosition()`, `getSize()`.父尺寸变化后重新排列子树；未配置锚点的 PanelSlot 按手动 Bounds/自动尺寸规则处理, 启用布局的 SingleSlot 则按 Padding/对齐分配.

16 个预设为: TopLeft, TopCenter, TopRight, CenterLeft, Center, CenterRight, BottomLeft, BottomCenter, BottomRight；水平拉伸 StretchTop, StretchCenter, StretchBottom；垂直拉伸 StretchLeft, StretchMiddle, StretchRight；双轴拉伸 StretchAll.它们对应 UE 风格的 4×4 组合, 不依赖 UE 实现.

在显式创建的 `std::unique_ptr<ui::PanelWidget> Root` 上添加孩子并配置 Slot, 再把 Root 交给 Window: 

```cpp
ui::AnchorLayout Placement;
Placement.setPreset(ui::AnchorPreset::Center);
auto& LabelSlot = Root->addSlot(std::move(Label));
LabelSlot.setAnchorLayout(Placement);
LabelSlot.setAutoSize(true);
Window.setContentRoot(std::move(Root));
```

每个固定轴: `PositionResult = ParentOrigin + Anchor * ParentSize + Position - Pivot * Size`.每个拉伸轴: 起点为 `ParentOrigin + AnchorMin * ParentSize + StartMargin`, 尺寸为 `max(0, (AnchorMax - AnchorMin) * ParentSize - StartMargin - EndMargin)`.Margin 正值向内, 负值允许向外；拉伸轴忽略 Position, Size 和 Pivot, 固定轴忽略 Margin.锚点和 Pivot 限于 [0,1], AnchorMin 不大于 AnchorMax, 输入必须有限且尺寸非负.

`setPreset()` 只更新锚点和 Pivot, 保留其余配置, 不执行 UE 编辑器式的保持视觉位置操作.`PanelSlot::setAutoSize(true)` 用孩子的 `measure()` 结果替代固定轴的 Size；拉伸轴仍按锚点跨度与 Margin 分配, 不因自动尺寸取消拉伸.Slot 测量时, 固定轴使用显式 Size 或自动尺寸, 拉伸轴使用孩子的期望尺寸加两端 Margin, 而不是把父级分配的锚点跨度当作期望尺寸；正向 Position 也计入所需空间.父内容区域使用逻辑坐标, 孩子 Bounds 相对父控件；排列不修改 Renderer.

`Widget::setAnchorLayout()` 保留为兼容入口: 未挂载时暂存规则, 挂载到 PanelSlot/SingleSlot 时转移至 Slot；已挂载时转发给支持锚点的父 Slot, 不支持锚点的 Slot 会拒绝设置.未挂载 Widget 的 `arrange()` 不应用暂存锚点；`getAnchorLayout()`, `clearAnchorLayout()` 同样访问或清除暂存/Slot 规则.新代码应直接配置 `addSlot()` 返回的 Slot, 避免把父级布局规则误认为孩子固有属性.

## 命令行注册

[include/Editor/CommandLine/CommandLineParser.hpp](include/Editor/CommandLine/CommandLineParser.hpp) 是独立的注册式解析器, 底层采用固定版本 CLI11 v2.5.0, 首次配置由 CMake FetchContent 获取；离线构建可通过 `FETCHCONTENT_SOURCE_DIR_CLI11` 指定本地源码.解析器不依赖窗口、SDL 或 ApplicationOptions；应用选项在 [src/CommandLine/ApplicationCommandLine.cpp](src/CommandLine/ApplicationCommandLine.cpp) 中集中注册名称、说明、类型规则与配置写入回调.Main 仅注册并调用 `parse()`, 根据结果决定启动或退出.

`registerOption<T>()` 接收类型化回调和校验/转换规则, `registerFlag()` 注册无值回调.支持 `--frames 3`、`--frames=3`、注册的短选项别名与 `--` 终止选项解析；帮助自动从注册信息生成.重复选项、未知名称、缺值、非法类型或枚举值返回非零退出码.帮助返回零退出码, 不初始化应用.回调只写入临时启动配置, 解析错误时不启动应用；不要在回调中执行资源创建等不可回滚操作.

## Widget 绘制

字体候选和选择逻辑由 `GenericApplication` 管理：`ApplicationOptions::DefaultFontPaths`、`EmojiFontPaths` 保存候选集合，`FontPath` 保存当前字体，`Text.FallbackFontPaths` 保存有序回退链。GPU 初始化时自动补全未指定的配置；非空显式回退链保持原样，headless 模式不加载字体。应用提供 `getCurrentFontPath()`、`getFallbackFontPaths()` 只读访问，不持久化到磁盘。完整技术笔记见 [文字处理与渲染全流程](docs/TextPipeline.md)。

`DiscoverSystemFonts` 默认开启，通过 macOS CoreText、Windows DirectWrite、Linux Fontconfig 查询已安装字体，不硬编码字体路径。空候选集合自动填充，显式候选不被覆盖；按 Unicode 覆盖及常规字重偏好选取最多四个文字字体、两个彩色 emoji 字体。Linux 构建需要 Fontconfig 开发包。OS API 只负责字体发现，排版仍使用 HarfBuzz/FreeType。`TextConfig::FallbackFontPaths` 按顺序指定回退字体；TextWidget 加载这些字体，FontService 按主字体缺失的塑形簇尝试回退。彩色 BGRA 位图转换为直通 RGBA，保留原始颜色，仅继承文字透明度；固定尺寸位图选择最近尺寸并缩放。`UITextChinese` 检查中文非缺字字形，`UITextEmoji` 检查真实彩色 emoji 图集和缓存。缺少对应测试字体时不启用该项；当前不是完整的跨字体 Unicode 段落排版或所有 emoji 格式支持。

彩色 PNG 字形需要 libpng: FreeType 的 `FT_CONFIG_OPTION_USE_PNG` 和 gzip 表所用的 zlib 都链接 `extern/libpng`, `extern/zlib` 静态构建的库, 配置阶段不再搜索系统包, 因此不需要 Homebrew, vcpkg, 开发包或 CMake 包路径, Windows 发布也不再需要分发 libpng/zlib 运行时 DLL.本次实际验证平台为 Windows + clang-cl + Ninja（CMake 4.3.3）：配置, 构建和全部 10 个测试（含 `UIText`, `UITextChinese`）通过.

`UIRenderer` 通过 `IUIRenderer` 提供基础 2D 图元接口；须在 `canBeginFrame()` 成功后、`endFrame()` 前调用, 位置使用逻辑坐标, 颜色使用 `Color`, 粗细为有限正数, 默认 1：

```cpp
Renderer.drawLine({10, 10}, {100, 30}, {1, 0, 0, 1}, 2);
Renderer.drawRect({10, 40, 100, 60}, {0, 1, 0, 1}, 3);
Renderer.drawTriangle({130, 40}, {180, 40}, {155, 90}, {0, 0, 1, 1}, 2);
Renderer.drawCircle({220, 70}, 25, {1, 1, 0, 1}, 4);
Renderer.drawPoint({270, 70}, {1, 0, 1, 1}, 8);
```

矩形和圆向内描边, 圆半径为外半径；粗细覆盖内部时绘制实心区域.点为以粗细作直径的实心圆.直线与三角形沿路径居中描边, 使用现有路径实现的独立线段, 不提供连接样式或抗锯齿保证.圆使用 64 段网格近似；零半径圆、起终点重合的直线不生成网格.虚线图元由 `drawDashedLine`/`strokeDashedRect`/`drawDashedCircle`/`strokeDashedPath` 提供, `DashLength`/`GapLength` 控制实线与空隙长度, 沿各边重新起算.相同接口也可通过 `PaintContext` 在帧外生成绘制列表, 以复用控件的变换、裁剪和透明度；Renderer 不依赖 Widget.

依赖方向为 `Widget -> PaintContext -> UIDrawList -> IUIRenderer`.Renderer 不保存或访问 Widget；自定义 Widget 重写 `onPaint(PaintContext&) const`, 非虚 `paint()` 负责控件树遍历, 相对坐标和裁剪.`paint(IUIRenderer&)` 将整个子树作为一个绘制列表提交；`paint(PaintContext&)` 可在帧外生成绘制数据.所有函数使用动词开头的小驼峰.Window 每帧绘制可选的内容根 Widget, GenericApplication 管理窗口生命周期和事件循环.

Widget 是不存储孩子的抽象基类.LeafWidget 固定零个孩子；ContentWidget 持有零或一个 SingleSlot, PanelWidget 持有多个 PanelSlot, CanvasPanel 持有多个专用 CanvasSlot.`getChildCount()` 统一查询非空孩子数量；添加 Slot 必须传非空孩子, 重复挂载和父子循环会被拒绝.绘制/命中按 Slot 的 `getZOrder()` 排序（PanelSlot 默认 0, 即添加顺序；CanvasSlot 可显式设置）, 相等时按添加顺序访问非空孩子.

独占所有权链为父容器 → Slot → Widget, 一个实例只属于一个父控件, 避免共享实例重复绘制及引用环.容器的 `takeChild()` 移出孩子并移除对应 Slot, 可重新挂载, 但旧锚点, 自动尺寸, Padding, 对齐等父级元数据不会随孩子转移, 孩子的父 Slot 观察指针和暂存锚点会清空.直接调用 `Slot::takeChild()` 则留下空 Slot；PanelWidget 的孩子索引跳过空 Slot, Slot 索引包含空 Slot, 可用 `getSlotCount()`/`getSlot()` 查询并用 `removeSlot()` 显式删除.

`getChild()`, `getSlot()`, `getParent()` 以及添加函数返回的指针或引用都不拥有对象, 也不是弱句柄.删除/替换 Slot 会使该 Slot 的观察指针失效；若其孩子同时销毁, 孩子观察指针也失效, 不可继续使用.`takeChild()` 转移所有权后孩子仍存活, 但旧 Slot 引用不可继续使用；直接 Slot 脱离后留下的空 Slot 除外.异步任务若需要检测存活, 应单独设计弱句柄或 ID 注册表；模型和资源可以共享, Widget/Slot 仍保持独占拥有.

成品组装位于 [Main.cpp](Main.cpp), GPU 路径创建文本后通过 Slot 居中并自动取期望尺寸: 

```cpp
auto& App = ui::GenericApplication::self();
App.initialize(Options);
auto& Window = App.getMainWindow();
const auto TextBytes = App.encodeText(U"Hello, 你好 氼, A€中😀");
auto Label = App.createTextWidget(TextBytes, 28);
auto Root = std::make_unique<ui::PanelWidget>();
ui::AnchorLayout Placement;
Placement.setPreset(ui::AnchorPreset::Center);
auto& LabelSlot = Root->addSlot(std::move(Label));
LabelSlot.setAnchorLayout(Placement);
LabelSlot.setAutoSize(true);
Window.setContentRoot(std::move(Root));
return App.run();
```

示例要求应用已初始化有效字体和 Renderer；Main 使用一份 UTF-32 源文本, 通过 `App.encodeText(std::u32string_view)` 按当前配置生成拥有存储的 UTF8/UTF16/UTF32 原始字节, 遵循 ByteOrder, 保留 NUL, 不添加 BOM.Custom 只有解码器, 不支持此编码操作；真实输入若已按配置编码, 直接传给控件, 不要重复编码.`GenericApplication::createTextWidget(Text, Size, MaxWidth, Tint)` 接受原始字节 `std::string_view` 或任意 `std::basic_string`, 使用 GenericApplication 保存的文本配置, 字体路径, Renderer 及主窗口 display scale, 返回借用全局 FontService 会话的 TextWidget.所有文本控件共享字体和图集缓存, 控件不拥有服务或 Renderer；脱离树的控件及快照不支持跨应用销毁继续使用.

子控件通过父级 Slot 独占拥有；Bounds 相对父控件, 内容裁剪在自身 Bounds 内.TextWidget 保存排版结果, 不在绘制时上传字形.GenericApplication 先销毁 MainWindow（含内容根）, 再调用 `FontService::self().shutdown()` 销毁共享图集并释放服务保留的 Renderer, 然后释放应用的 Renderer, 最后释放 Platform；Renderer 自身保留底层窗口状态.文本构造和 `setText()` 可能上传图集, 必须在活跃帧之外；控件销毁不释放共享图集, 图集统一在帧外 shutdown 时销毁.脱离树的控件、快照及引用图集的绘制列表也须在 shutdown 前释放.

```cpp
auto& Text = ui::FontService::self();
Text.initialize(Renderer);
{
	const auto Font = Text.loadFont(FontPath);
	const auto Layout = Text.createTextLayout(Font, U"Hello", 18, Window.getDisplayScale());

	ui::UIDrawList DrawList;
	ui::PaintContext Paint(DrawList);
	Paint.fillRoundedRect({16, 16, 240, 80}, 6, {0.2f, 0.3f, 0.4f, 1});
	Paint.drawTextLayout(Layout, {28, 28}, {1, 1, 1, 1});
	if (Renderer.canBeginFrame({Window.getSize(), Window.getPixelSize(), {0, 0, 0, 1}}))
	{
		Renderer.submitDrawList(DrawList);
		Renderer.endFrame();
	}
}
Text.shutdown();
```

此独立会话示例在 Layout 和 DrawList 离开作用域后才 shutdown, 且 Renderer 在整个会话期间有效.应用管理的会话由 GenericApplication 负责 shutdown, 不应在仍有内容根或其他文本依赖时提前关闭.

`canBeginFrame()` 不是纯查询: 成功时实际开始帧, 随后必须调用 `endFrame()`.在窗口最小化或像素尺寸为零时返回 false, 此时不调用提交和结束.提交会复制数据, 允许随后清空原列表；多个列表和批次保持顺序.矩形裁剪按实际交换链尺寸换算, 逻辑坐标与像素尺寸独立.`canClear()` 同样不是纯查询, 而是清除并呈现空绘制帧, 返回是否成功, 不能在活跃帧内调用.`Window::drawFrame()` 也实际排列, 绘制并呈现, 内部配对开始/结束帧；不要将这几个 bool 操作当成无副作用的检查.

纹理为紧密排列的 straight-alpha RGBA8；`createTexture()`, `updateTexture()`, `destroyTexture()` 必须在帧外调用.资源句柄不暴露 SDL, 跨 Renderer, 已销毁和零句柄的资源操作会被拒绝.绘制批次的零纹理表示白色, 可用于纯色几何.列表拥有顶点和索引, 但不拥有纹理, 提交直到结束期间资源必须有效.

绘图支持矩形/圆角填充与边框, 简单凹多边形填充, 线段描边, 图像, 文本, 嵌套矩形裁剪, 仿射变换和逐图元透明度.路径目前不支持孔洞, 自交, 曲线及专业描边连接；相邻描边线段可能重叠或留隙.旋转/剪切后的裁剪被明确拒绝.透明度不是离屏分组透明度；暂未实现路径遮罩, 阴影, 自定义 shader 或抗锯齿矢量边缘.

文本由 FontService 使用 FreeType 栅格化, HarfBuzz 塑形, 缓存至固定大小图集；支持换行, 基于空白的 cluster 安全软换行, 单向 RTL run.[include/Editor/FontService/TextLayout.hpp](include/Editor/FontService/TextLayout.hpp) 定义预塑形快照, 其测量/排列实现在 [src/FontService/FontService.cpp](src/FontService/FontService.cpp): `measure()` 验证 AvailableSize 后返回已有 Metrics 的 Width/Height, 不按新可用宽度重排, 截断或重新塑形；`arrange()` 只记录分配区域, 字形与 caret 坐标保持文本本地坐标.TextWidget 的 `onMeasure()` 委托给该快照, `onArrange()` 记录本地分配区域, 因此父级 Fill/拉伸不会改变文本期望尺寸或换行结果.

`FontService::createTextLayout()` 的文本参数只接受 `std::u32string_view`, 不接受 UTF-8 或原始字节.`TextLayout::Text` 拥有 `std::u32string`.`getMetrics()` 返回逻辑坐标度量, `TextGlyph::Cluster`、`TextCaret::Offset`、`hitTest()` 和 `getCaretRect()` 使用内部 UTF-32 文本的标量索引（码点索引）, 光标只位于塑形簇边界, 不伪造逐码点或连字内部 caret；偏移不再是 UTF-8 字节偏移, 也不是原始输入的字节或 UTF-16 码元索引.

`FontService::self()` 返回唯一服务, 通过 `initialize(IUIRenderer&)` 借用 Renderer, 或通过 `initialize(std::shared_ptr<IUIRenderer>)` 由服务保留共享 Renderer 至 `shutdown()`；共享指针不能为空.借用方式要求调用方保证原始 Renderer 从初始化到 shutdown 完成始终有效, 共享方式则由服务保证这一寿命.一个会话只绑定一个 Renderer, 同一 Renderer 可复用；shutdown 前尝试绑定不同 Renderer 抛出 `logic_error`, 必须先释放旧会话依赖并 shutdown, 才能初始化新绑定.字体按传入路径字符串精确匹配缓存, 同一路径返回同一字体句柄, 不规范化路径或合并别名；字形和图集缓存由所有控件共享, 字体句柄及图集纹理只在当前会话内有效.

文本控件为 `TextWidget`(纯构造, 不做任何排版), 运行时通过 `setText/setFont(路径)/setColor/setFontSize/setPixelScale/setMaxWidth/setEnableTextOuter` 写入描述信息.排版/测量/绘制委托给内部 `TextLayoutEngine`(由 `FontService::createTextLayoutEngine()` 用全局默认配置/字体/回退创建); 自定义可通过继承覆盖受保护钩子 `setupTextLayoutEngine(TextLayoutEngine&)` 直接设置该引擎.`TextLayout` 是内部实现, 不对外暴露(无 `getLayout/setLayout/快照构造`).`setEnableTextOuter` 为 false 时字体超出控件边界被裁剪, 为 true 时渲染出自身边界.文本编码默认用全局(经 FontService 的默认 config, App 在 initialize 时写入), 引擎内部 `setConfig` 可适配不同编码.

新的换行约束须通过 `FontService::createTextLayout()` 重建快照并 `setLayout()`, 或重新创建控件；通用 Layout 测量不替代塑形.DPI 变化不会自动重新塑形, `setText()` 也沿用原 PixelScale；调用方需按新 scale 重建布局或控件.尚未实现完整双向段落或文本编辑器选区语义.快照不拥有图集纹理, 脱离控件也仍依赖全局会话；shutdown 销毁图集后旧快照失效, 重新 initialize 不会恢复旧快照, 不支持快照跨应用销毁存活并继续绘制.创建文本布局可能上传纹理, 应在帧外完成.

## 可见性、命中测试与增量渲染

控件有三个交互状态: `EVisibility{Visible, Hidden}`, `EnableOuterRender`, `AcceptEvents`.访问器为 `setVisibility(EVisibility)`/`getVisibility()`/`isVisible()`, `setEnableOuterRender(bool)`/`isEnableOuterRender()`, `setAcceptEvents(bool)`/`isAcceptEvents()`.默认 `Visible`, `EnableOuterRender=false`, `AcceptEvents=false`（按需开启）.

- `Hidden` 仍占布局空间, 但只影响渲染（不绘制）, 不影响命中测试；`Visible` 恢复正常绘制.
- `EnableOuterRender=true` 时该控件不被祖先区域裁剪, 只被自身矩形裁剪, 可绘制到父区域之外；默认仍与所有祖先裁剪求交.
- `AcceptEvents=true` 时该控件自身可成为命中目标并接收事件；为 false 时事件穿透给子控件, 若没有子控件接住则视为未处理（向外传递, 未来抛给 3D 空间）.命中顺序与绘制一致（后添加的孩子优先）.

命中与输入派发: `Widget::hitTest(UIPoint)` 接受父坐标系点（根控件即窗口逻辑坐标）, 返回最深命中的可交互控件；`Window::hitTest(UIPoint)` 用窗口逻辑坐标查询整棵树.`Widget::localizePoint(UIPoint)` 把窗口/父坐标点沿祖先链换算成自身局部坐标.`Window` 维护悬停/按下/焦点状态, 由应用事件循环通过 `dispatchMouseMove/dispatchMouseButton/dispatchMouseWheel/dispatchKey/dispatchTextInput/dispatchTextEditing` 派发.

控件输入钩子（默认返回 `EventReply::Unhandled`, `LocalPoint` 为自身局部坐标）: `onMouseButtonDown`, `onMouseButtonUp`, `onMouseButtonClicked`（按下与释放都命中同一控件时触发一次完整点击）, `onMouseMoved`, `onMouseEnter`/`onMouseLeave`（悬停进出）, `onMouseWheel`, `onKeyDown`/`onKeyUp`（派发给聚焦控件, 按住重复时 `onKeyDown` 随 `Repeat` 再次触发）.钩子返回 `EventReply{Handled, Unhandled}` 表示是否消费该事件: `Handled` 会阻止应用的默认处理（例如全局快捷键 Esc/F11/Ctrl+V）；`Window` 的 `dispatch*` 方法同样返回该结果.文本输入与 IME 组合（`TextInputEvent`/`TextEditingEvent`）不在 `Widget` 基类上监听, 由具体的文本输入控件自行处理.键盘焦点由 `setFocusable(bool)`/`isFocusable()` 控制: 点击可聚焦控件会自动 `Window::setFocus`, 键盘事件只派发给聚焦控件, `clearFocus()` 清除焦点.尚未实现按下捕获、事件冒泡或剪贴板式手势.

失效与增量渲染（参照 UE 的保留模式）: `Widget::invalidate(EInvalidationReason)` 标记需要重算的结果, 并沿父链冒泡到根; `isInvalidated(Reason)`/`getInvalidationReason()` 供查询.`EInvalidationReason` 为位标志: `Layout`（隐含 `Paint`）, `Paint`, `Visibility`（隐含 `Paint|HitTest`）, `ChildOrder`（隐含 `Paint|HitTest`）, `HitTest`, `Clipping`（隐含 `Paint`）.所有改变布局/绘制/可见性/命中的 setter 与增删子控件都会自动失效.

渲染模式由渲染器决定: `UIRenderer::setUIMode(EUIMode)`（默认 `EUIMode::Retained`）, `--ui-mode retained|immediate` 通过 `ApplicationOptions.UIMode` 传入.保留模式下 `Window::drawFrame` 只在 `Layout` 失效时重排、只在绘制相关失效时重建绘制列表, 否则复用缓存并让渲染器复用上次的 GPU 顶点/索引缓冲（`submitDrawList` 的 `ReuseBuffers` 提示）；立即模式每帧全量重算.`Window::getLastUpdate()` 返回 `UIUpdateReport{DidLayout, DidPaint, Reasons}` 便于观察与测试.

## 文本编码配置

`ApplicationOptions::Text` 是 `TextConfig`, `Encoding` 默认为 `TextEncoding::UTF8`, `ByteOrder` 默认为 `TextByteOrder::Native`；输入统一为原始内存字节, 不按字符串字符类型推断或转换编码.

- `UTF8` 按字节读取, 内容必须是严格有效的 UTF-8；宽字符串的 NUL 字节也作为输入保留, 不自动转码.
- `UTF16` 每两字节读取一个码元, 支持 supplementary surrogate pair；孤立高/低代理, 错配代理和奇数字节长度会报错.
- `UTF32` 每四字节读取一个 Unicode 标量值, 拒绝不足四字节的末尾单元, surrogate 和越界标量.
- `UTF16`/`UTF32` 的 `ByteOrder` 可设置为 `TextByteOrder::Native`, `LittleEndian` 或 `BigEndian`.默认 Native 使用宿主字节序, 因此本机 `std::u16string`/`std::u32string` 的原始内存可直接读取；序列化 LE/BE 字节须显式选择对应字节序, 不猜测字节序, 不自动剥离 BOM.
- `Custom` 要求 `CustomDecoder`, 签名为 `std::function<std::string(std::string_view)>`；接收完整原始字节视图, 可以包含 NUL, 不要求输入为 UTF-8.解码器可按约定解释任意字节编码, 然后必须返回严格有效的 UTF-8, 返回值立即经严格验证转换为 UTF-32, 不作为内部存储.缺少解码器会报错, 解码器自身异常原样传播.CLI 不提供 Custom 选项, 应在代码中配置.

Widget 边界仍接收原始字节, 按 TextConfig 调用 `decodeTextToUTF32(std::string_view, Config)` 直接解码为内部 UTF-32, 再交给 FontService, 不经过兼容的 UTF-8 输出包装.`getTextBytes()` 的长度为 `size() * sizeof(Character)`, 不包含额外终止符, 但保留内容中的 NUL；视图不拥有数据, 源字符串须在调用期间有效且不可使视图失效.TextWidget 构造与 `setText()` 同样接受字节视图或任意 basic_string；模板仅通过 `getTextBytes()` 暴露字节, 不转换码点.输入类型不必与 Encoding 一致, 例如 u32string 在 UTF16 模式下每个四字节元素会按两个两字节码元读取, 能否成功取决于字节内容和配置的字节序.

独立 [Unicode.hpp](include/Editor/Text/Unicode.hpp)/[Unicode.cpp](src/Text/Unicode.cpp) 提供六个转换 API: `utf8ToUTF32()`、`utf16ToUTF32()`、`utf32ToUTF8()`、`utf32ToUTF16()`、`utf8ToUTF16()`、`utf16ToUTF8()`.转换保留嵌入 NUL、已有 BOM 和标量顺序, 不做 NFC/NFD 规范化；UTF-16/UTF-32 转换参数是码元视图, 不涉及字节序.`validateUTF32()` 校验所有码元均为 Unicode 标量, 允许 NUL 和非字符.非法序列、截断码元、孤立或错配代理、代理标量、超过 U+10FFFF 的值及非最短 UTF-8 抛出 `invalid_argument`, 不会静默替换为 U+FFFD.

`encodeText(std::u32string_view, Config)` 将内部标量编码为配置指定的原始字节, UTF16/UTF32 遵循 ByteOrder；应用的 `App.encodeText(std::u32string_view)` 重载使用当前配置.现有 `decodeText()`（返回 UTF-8）及 `encodeText(std::string_view UTF8, Config)`/`App.encodeText(std::string_view UTF8)` 仅保留兼容用途, 分别包装 UTF-32 解码后转 UTF-8、UTF-8 转 UTF-32 后编码；它们不决定内部存储或布局偏移.

有效 UTF-32 的每个码元对应一个标量, 提供 O(1) 码点随机访问, 但不是 O(1) 字素簇访问, 也不自动解决光标导航；每个标量占四字节, 对 ASCII 和多数常用文本比 UTF-8 使用更多内存.UTF-8 仍适合文件、SDL 文本事件、剪贴板和网络等外部边界, 不仅用于网络.SDL 文本输入事件仍是 UTF-8, 不随 GenericApplication 的文本构造配置改变；进入内部布局前应显式转换或按实际输入编码解码.

## 事件与扩展约定

已支持关闭, 窗口尺寸/像素尺寸/安全区域/位置/焦点/状态, 键盘按下/释放/重复, UTF-8 文本, IME 组合与候选, 鼠标移动/按键/精确滚轮, 文件/文本拖放, 显示器变化, 剪贴板通知, 键鼠热插拔和键盘布局变化.

`Event::Window` 为窗口 ID（0 表示全局或无关联窗口）；多窗口应用应统一轮询, 再按 ID 路由.关闭事件是请求, 不自动销毁窗口.`Event::TimestampNs` 使用 SDL 的单调纳秒时钟, `Event::Payload` 保存强类型事件载荷.鼠标坐标为窗口逻辑坐标, 渲染目标使用像素尺寸.物理键采用 USB HID usage, 自有枚举包含 PC 常用键；逻辑字符仅表示按键的布局映射, 实际文本必须消费 `TextInputEvent`, 不能从按键拼接.

所有文本, 候选和拖放字符串在轮询时深拷贝, 可以安全保存事件.SDL3 的这些事件指针由 SDL 管理, 不按 SDL2 的方式释放.组合文本起点/长度按字符计, 不是 UTF-8 字节偏移.

新增内建事件时扩展 `EventPayload` 和各平台内部转换函数；插件/编辑器命令使用 `CustomEvent{Name, Payload}` 与 `IGenericPlatform::postEvent()`, 其中 `CustomEvent::Name` 为事件名称, `CustomEvent::Payload` 为 `std::any` 载荷, 无需修改 variant.载荷应拥有数据, 不要传临时对象指针.该自定义队列只允许主线程调用, 先于原生队列消费, 因此不承诺两队列之间的全局 FIFO.

未封装的 SDL 事件生成 `UnhandledEvent`, 仅保留后端类型编号和可安全获得的元数据, 不复制含悬空指针风险的原生 union.需要手柄, 触控, 笔, 音频等时应新增强类型事件及转换, 而不是让应用依赖 SDL 数值.此版本针对桌面, 不实现移动平台生命周期回调.

## 生命周期

平台, 窗口, 事件轮询, GPU 操作及销毁均必须在主线程.当前 SDL 后端一个进程仅允许一个封装运行时；不应在外部另外调用 SDL 初始化/退出.运行时引用由窗口持有, GPU 内部也持有窗口状态, 因此先销毁外部窗口对象或平台对象不会产生悬空 SDL 句柄.通常仍按 GPU → 窗口 → 平台顺序销毁；窗口对象销毁时若 GPU 仍在使用, 底层窗口延迟至 GPU 释放后销毁.每个窗口只允许一个 GPU renderer.

GPU 清屏处理最小化时的空交换链纹理；获得交换链纹理后只提交, 不取消命令缓冲.退出时等待 GPU idle, 再释放交换链窗口与设备.

## 验证与交互

默认测试包含 `PlatformEvents`, `EditorHeadlessSmoke`, `UIPaint`, `UIWidget`, `UILayout`, `UIText`.前两个使用 dummy 驱动, 其余验证 CPU 绘图, 控件/Slot 拥有关系, 测量与排列及假 Renderer 上的字体图集.没有系统字体时设置 `EDITOR_TEST_FONT` 启用文本测试.`EDITOR_BUILD_GPU_TESTS=ON` 增加真实窗口/GPU 的 `UIGpu`, 不能在 dummy 驱动下运行.

运行 `Editor` 显示主窗口内容根下居中, 自动尺寸的 `Hello, Editor!` 文本, 并在控制台输出事件: Esc 退出, F11 全屏, Ctrl+V 读取剪贴板；输入文本会修改标题；鼠标移动显示坐标, 可测试滚轮, 拖放和窗口缩放.Widget 树目前提供 Slot 拥有关系, 通用测量/排列, 锚点/自动尺寸, 单孩子 Padding/对齐, 局部坐标, 裁剪和绘制；不包含 Stack/Grid, 失效缓存或输入命中分发.

`--frames N` 在指定帧数后退出；`--backend vulkan|auto|metal|direct3d12` 选择 GPU 后端；`--font PATH` 指定字体；`--text-encoding utf8|utf16|utf32` 设置文本编码；`--help` 显示参数.窗口平台不属于 ApplicationOptions, 也不提供 `--platform` 覆盖: GenericApplication 初始化时调用 `createPlatform()` 自动选择当前可用实现, 目前为 SDL.

正式 `EditorApplication` 固定创建 GPU 渲染器，不提供运行时禁用选项。仅测试库 `EditorTestApplication` 私有定义 `EDITOR_TEST_NO_GPU=1`，编译掉字体初始化与 Renderer 创建；`UIApplication` 和独立 `EditorHeadlessSmoke` 链接该库并配合 `SDL_VIDEODRIVER=dummy`。测试仍共享平台/渲染依赖，不是免 GPU 工具链的独立构建。

通过 `GenericApplication::self()` 取得唯一实例, 再调用 `initialize(ApplicationOptions)` 启动.构造, 析构均为私有, 禁止复制和移动；初始化前访问资源以及重复初始化均抛出 `logic_error`.初始化失败不提交资源状态, 可以重试.初始化, 事件循环和 UI 操作必须在主线程进行.单例退出时先销毁 MainWindow（含内容根）, 再 shutdown 全局 FontService 以销毁图集并释放服务保留的 Renderer, 然后释放应用的 Renderer 和 Platform；外部文本依赖须在此之前释放, 不能跨应用销毁继续使用.产品入口与可执行文件仍命名为 Editor.