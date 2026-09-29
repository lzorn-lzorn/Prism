# SeedEngine

SeedEngine 当前可用图形后端为 Vulkan 1.3. 现代 RHI 已形成 Acquire → Record → Submit →
Synchronize → Present → Retire 闭环，并实现显式 Barrier, Dynamic Rendering, BindGroup, 
现代内存子分配, BDA, 扩展动态状态, Secondary inheritance, Query, 可选 Ray Tracing 和
WSI/HDR 状态分类；所有可选路径必须以 `DeviceFeatures` 为准. 

Renderer 层已建立多 Pass 帧录制主干：`RendererServer::renderFrameGraph()` + `RDGBuilder`
（RDG 编排、自动 barrier 与 rendering scope）+ `SceneRenderer`（Shadow/Depth/Opaque/
Transparent/Post/UI 子渲染器）+ `RenderData`（`DrawPacket` / `UIQuad` 交换格式与
`EChangeFlags` 增量意图）。RHI 侧补齐 transient heap 与 placed resource
（`createPlacedBuffer` / `createPlacedImage`），显存别名在真实设备上已验证；
`renderer::rdg` 提供拓扑排序、Pass 裁剪、生命周期与别名的算法实现（辅助设施，
待收敛进 `RDGBuilder`）。

文档入口：

- [现代 RHI 与 2D/3D 渲染全流程（实现状态唯一来源）](documents/modern_rhi_rendering_flow_zh.md)
- [RenderGraph 与渲染交换数据格式（Renderer 层权威说明）](documents/rdg_render_data_incremental_zh.md)
- [RDG 图算法与显存别名设计](documents/rdg_design_zh.md)
- [Pipeline, CommandList 与 Dynamic Rendering 契约](documents/rhi_pipeline.md)
- [BindGroup, SPIR-V 反射与语义绑定](documents/rhi_bind_groups.md)