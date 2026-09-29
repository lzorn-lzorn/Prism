# SeedEngine RDG 图算法与显存别名设计

> **本文的定位**：Renderer 层 RDG 的**主干**是 `rdg/RDGBuilder.hpp`
> （`runtime::renderer::RDGBuilder`，由 `RendererServer::renderFrameGraph()` 驱动），
> 其权威说明是 [rdg_render_data_incremental_zh.md](rdg_render_data_incremental_zh.md)。
>
> 本文描述的是 **`renderer::rdg` 这套算法侧辅助设施**：
> `sources/engine/runtime/renderer/includes/rdg/RDG.hpp`、
> `sources/rdg/RDGGraph.cpp`、`sources/rdg/RDGPlan.cpp`，
> 以及为支持真实显存别名而扩展的 RHI 接口（transient heap / placed resource）。
> 它提供 `RDGBuilder` v1 边界内**尚未具备**的图算法（拓扑排序、Pass 裁剪、
> 生命周期、显存别名），是后续收敛进 `RDGBuilder.compile()` 的候选实现。
>
> RHI 总体契约见 [rhi_pipeline.md](rhi_pipeline.md) 与
> [modern_rhi_rendering_flow_zh.md](modern_rhi_rendering_flow_zh.md)。

---

## 1. 目标与边界

RDG 把"一帧要做什么"与"资源在哪里、什么时候同步"分开：

| 渲染代码只负责 | RDG 负责推导 |
|---|---|
| 我要读什么、写什么 | 依赖关系（RAW / WAR / WAW） |
| 用什么管线画 | 执行顺序（拓扑排序） |
| | 资源生命周期 `[FirstUse, LastUse]` |
| | 显存别名（哪些资源可以复用同一段显存） |
| | 屏障（layout / 可见性转换） |

**明确不做的事**（避免一开始就背上复杂度）：

- 自动 Pass 合并、多队列自动调度、跨队列所有权转移；
- 多线程 Secondary Recording；
- 资源状态按子资源（mip / layer）细粒度跟踪——当前按整资源跟踪。

---

## 2. 为什么不用 `core::HandleManager`

`core/functions/HandleManager.hpp` 是进程级单例注册表，内部用 `shared_mutex` 保护，
面向"跨系统长期存在"的资源（材质、网格、纹理）。RDG 的资源特征不同：

- **帧内瞬态**：生命周期严格绑定在 `RDGGraph` 实例上，不需要进程级注册；
- **单线程**：构建 / 编译 / 录制都在渲染线程，加锁纯属浪费；
- **访问模式**：需要按句柄随机访问 + 顺序遍历，这正是 `std::vector` 的强项。

因此 RDG 采用讨论稿里的**轻量值句柄**：

```cpp
struct ResourceId { uint32_t Value { InvalidId }; };
struct PassId     { uint32_t Value { InvalidId }; };
```

句柄由 `RDGGraph` 集中持有数据，不会悬垂，也不引入全局状态。
`HandleManager` 的世代号（Generation）机制在这里没有收益：
图的生存期只有一帧，资源编号不会跨帧复用。

---

## 3. 文件结构

| 文件 | 职责 |
|---|---|
| `includes/rdg/RDG.hpp` | 全部公共类型：句柄、状态、描述、`RDGPassContext`、`RDGBuilder`、`RDGGraph`、`RDGCompiledPlan`、`RDGResourcePool` |
| `sources/rdg/RDGGraph.cpp` | 图算法：依赖、拓扑、生命周期、裁剪、别名计划、屏障推导、执行 |
| `sources/rdg/RDGPlan.cpp` | 计划自洽性校验 + `RDGResourcePool`（把计划落到真实 RHI 资源） |
| `tests/rdg_cpu.cpp` | 无 GPU 的图算法验证（含伪 `RCommandList` 录制） |
| `tests/rdg_gpu.cpp` | 真实设备验证：堆 / placed resource / 别名复用 / 一帧录制提交 |

---

## 4. 两阶段构建

```
graph.addPass(...)     声明期：只记录描述，不接触设备
graph.compile()        编译期：纯 CPU 图算法 → RDGCompiledPlan
graph.execute(cmd, plan) 执行期：录屏障 + 按序跑 Pass 回调
```

`compile()` 与 `execute()` 分离带来三个好处：

1. **图算法可以完全脱离 GPU 做单元测试**（`tests/rdg_cpu.cpp` 就是这么做的）；
2. 同一张图可以**编译多次**（窗口尺寸变化）或**换设备重新实例化**；
3. `RDGCompiledPlan` 不持有设备指针，可以安全缓存与传递。

编译管线的子阶段（都在 `RDGGraph.cpp`）：

```
buildDependencyGraph()  收集 RAW / WAR / WAW 边，去重
topologicalSort()       Kahn + 最小堆，保证"声明顺序即执行顺序"
analyzeLifetimes()      统计 [FirstUse, LastUse]（拓扑下标）
cullPasses()            删除对最终输出无贡献的 Pass
buildAliasPlan()        按堆做槽位划分，产出显存槽位表
generateBarriers()      按显存槽位推导状态转换
instantiateResources()  把计划落到真实 RHI 资源（可选）
```

### 4.1 依赖与拓扑

- 读 → 之前的写 = **RAW**
- 写 → 之前的写 = **WAW**
- 写 → 之前的读 = **WAR**

因为依赖边恒指向"更早声明的 Pass"，图天然无环；Kahn 算法里用最小堆
保证入度为 0 时优先跑 `PassId` 更小的 Pass。

### 4.2 Pass 裁剪规则

一个 Pass 必须执行，当且仅当满足任一条：

1. **没有访问声明**——纯副作用 Pass（调试标记、时间戳），无法用依赖表达；
2. **写了被导出（`keepAlive`）或导入的资源**——它是最终输出的一部分；
3. **是只读 Pass**——只读意味着它是消费者（后处理 / 表现），删掉就没有输出了。

其余"只写不读、且不写导出资源"的 Pass 都是中间产物生产者的候选；
若它写的资源没有任何存活消费者，整条链会被裁掉。
判定用"从消费者出发沿依赖反向传播"的闭包——由于 `SortedPasses` 已是拓扑序，
一次反向扫描即可收敛。

---

## 5. 显存别名

### 5.1 模型

别名的基本单位是**显存槽位（memory slot）**，不是资源：

- 一个槽位是堆内一段连续显存；
- 槽位内可以放进多个资源，前提是**任意两个资源的 `[FirstUse, LastUse]` 互不重叠**；
- 槽位内部按时间先后**线性摆放**每个占用，因此槽位的跨度等于
  Σ(占用大小 + 对齐填充)，而"同时存活量"是它的下界。

这样做的理由：复用同一段显存的两个资源对驱动来说**是同一个对象**，
布局状态必须连续，因此每个占用都拿到一段独立字节区间，
而不是把空闲区间直接塞给下一个资源。

堆的分类（`EPoolKind`）：`RenderTarget` / `DepthStencil` / `Buffer` / `UAV`。
不同堆是独立显存空间，不共享别名。

### 5.2 一个例子

| 资源 | 生命周期 | 槽位 | 槽位内偏移 |
|---|---|---|---|
| `Small` (RT, 16KB) | `[0,0]` | 0 | 0 |
| `Medium` (RT, 128KB) | `[1,3]` | 1 | 0 |
| `Overlapping` (RT, 128KB) | `[3,3]` | 1 | 131072 |

`Small` 与 `Medium` 生命周期不重叠 → 同槽位 0，`Small` 排在前面、
`Medium` 排在 64KB 边界之后。`Overlapping` 与 `Medium` 在同一个 Pass 内被使用
→ 生命周期重叠 → 不能放进槽位 1，另起槽位。

### 5.3 不变量（有测试保护）

```cpp
// 同一槽位内任意两个资源的生命周期不得重叠
RDGCompiledPlan::validateAliasing(Resources);
// 同一堆内任意两个资源的 [AliasOffset, +AliasSize) 不得相交
RDGCompiledPlan::validateNoOverlap(Resources);
```

两条合起来才能保证"复用同一段显存"是安全的。

### 5.4 计划里的偏移 vs 真实偏移

`RDGGraph` 产出的 `AliasOffset` 是**计划用**的偏移（`DefaultResourceAlignment = 256` 对齐），
它的作用是让 `validateNoOverlap` 与显存预算可复核。
真正切显存时必须用**驱动给出的对齐**（`vkGetImageMemoryRequirements` 的 `alignment`
可能远大于 256），这一步由 `RDGResourcePool::beginFrame()` /
`VulkanTransientHeap::suballocate()` 完成。

---

## 6. 屏障推导

### 6.1 状态挂在"显存槽位"上

这是整个屏障逻辑的关键点。被别名复用的两个资源共享同一段显存，
状态是**连续的**；若按资源记录状态，复用点就会漏掉一次
`Undefined -> Target` 的转换（Vulkan 明确要求别名复用点必须做整段布局重置）。

```cpp
struct SlotState { ResourceId LastUser; EResourceState State; bool Valid; };
std::vector<SlotState> SlotStates(MemorySlotScratch.size());
```

三种情况都要插屏障：

1. **首次使用**（`!State.Valid`）：`Undefined -> Target`；
2. **布局变化**（`Before != Target`）；
3. **槽位换人**（`AliasedReuse`）：`Undefined -> Target`，并标记 `AliasedMemory = true`。

外加一种**同 Pass 内读写**：`read(PixelShaderResource)` + `write(RenderTarget)`
合并成一个访问状态 `RenderTarget`，此时布局可能没变（`RT -> RT`），
但读必须先于写完成，因此仍然要插一次执行依赖屏障。

### 6.2 冗余屏障抑制

连续两个只读 Pass 采样同一资源时状态不变，第二个 Pass 不产生屏障（有测试）。

---

## 7. RHI 扩展：placed resource 与瞬态堆

真正的显存别名需要"在指定堆偏移处创建资源"。为此 RHI 增加了下列接口
（全部带默认实现，未实现的后端会自动退化）：

```cpp
// RHI.hpp
class RTransientHeap                     // 一段可被多个资源复用的设备显存
{
    DeviceSizeType getSize() const;
    std::optional<DeviceSizeType> suballocate(Size, Alignment);
    void releaseSuballocation(Offset, Size);
};

class RDevice
{
    std::shared_ptr<RTransientHeap> createTransientHeap(const MemoryHeapDescriptor&);
    std::shared_ptr<RBuffer> createPlacedBuffer(Desc, Heap, Offset);
    std::shared_ptr<RImage>  createPlacedImage (Desc, Heap, Offset);
    std::optional<MemoryRequirements> getBufferMemoryRequirements(const BufferRequirementsRequest&);
    std::optional<MemoryRequirements> getImageMemoryRequirements (const ImageRequirementsRequest&);
};
```

Vulkan 侧实现（`VulkanMemoryHeap.hpp/.cpp`、`VulkanImage/Buffer/Device`）：

- 堆选择内存类型时要求"调用方给的类型位 ∩ 属性要求"非空；
- `createPlacedBuffer/Image` 先 `createUnbound`（只建 `VkBuffer`/`VkImage`），
  再用 `bindBufferMemory/bindImageMemory` 绑定到 `(Heap, Offset)`；
- 非法偏移（未对齐、越界、类型位不匹配）返回 `nullptr`，由调用方回退，
  **不会产生一个坏资源**；
- 图像复用显存必须带 `VK_IMAGE_CREATE_ALIAS_BIT`，`createPlacedImage` 走
  `VulkanImage::createUnboundAliasing()` 路径；
- `getImageMemoryRequirements` 也走带别名标志的路径，因为该标志会影响驱动
  给出的需求估算。

### 退化路径

三层防线，任何一层缺失都不会导致功能不可用：

1. `RDevice` 的默认实现返回空 → `RDGResourcePool::beginFrame()` 创建堆失败 →
   `supportsAliasing() == false` → RDG 逐资源独立创建；
2. 堆申请成功但某个 `createPlacedXxx` 返回空 → 该资源回退到独立创建；
3. `RDGResourcePool::setAliasingEnabled(false)` 显式关闭，用于 A/B 对照。

只有**真的**把资源放到了堆上时，`RDGCompiledPlan::isAliasingEnabled()` 才为 `true`；
从测试到生产代码都以这个标志为准，而不是"计划里说可以别名"。

### 内存类型位的来龙去脉

池需要提前知道"这个资源能用哪些内存类型"才能创建合适的堆，
因此编译期会为每个瞬态资源调用 `IRDGResourcePool::prepareResource()`，
池向设备查询需求并按堆取**类型位交集**写入 `RDGHeapLayout::MemoryTypeBits`。

---

## 8. 使用示例

```cpp
using namespace renderer::rdg;

RDGGraph Graph;
RDGResourcePool Pool(*Device);          // 每帧在飞一份，可复用

ResourceId Backbuffer, Shadow, GBuffer, SceneColor;

Graph.addPass("Import", [&](RDGBuilder& B) {
    Backbuffer = B.importTexture("Backbuffer", SwapchainImage);
});

Graph.addPass("Shadow", [&](RDGBuilder& B) {
    Shadow = B.createTexture("ShadowDepth", {
        .Format = rhi::EFormat::D32_Float,
        .Width = 2048, .Height = 2048,
        .Usage  = rhi::EImageUsage(rhi::EImageUsage_t::DepthStencil)
                | rhi::EImageUsage_t::Sampled,
    });
    B.write(Shadow, EResourceState::DepthWrite);
    B.setExecute([Shadow](RDGPassContext& Ctx) {
        auto& Depth = Ctx.texture(Shadow);
        auto& Cmd   = Ctx.getCommandList();
        // 设置 RT、绑定管线、Draw...
    });
});

Graph.addPass("GBuffer", [&](RDGBuilder& B) {
    GBuffer = B.createTexture("GBufferA", {
        .Format = rhi::EFormat::RGBA8_UNorm,
        .Width = 1920, .Height = 1080,
        .Usage  = rhi::EImageUsage(rhi::EImageUsage_t::Target)
                | rhi::EImageUsage_t::Sampled,
    });
    B.read (Shadow,  EResourceState::PixelShaderResource);
    B.write(GBuffer, EResourceState::RenderTarget);
    B.setExecute([](RDGPassContext&) { /* ... */ });
});

Graph.addPass("Lighting", [&](RDGBuilder& B) {
    SceneColor = B.createTexture("SceneColor", {
        .Format = rhi::EFormat::RGBA16_Float,
        .Width = 1920, .Height = 1080,
        .Usage  = rhi::EImageUsage(rhi::EImageUsage_t::Target)
                | rhi::EImageUsage_t::Sampled,
    });
    B.keepAlive(SceneColor);            // 导出到图外 -> 不参与别名
    B.read (GBuffer,    EResourceState::PixelShaderResource);
    B.write(SceneColor, EResourceState::RenderTarget);
    B.setExecute([](RDGPassContext&) { /* ... */ });
});

const RDGCompiledPlan Plan = Graph.compile(&Pool);

auto Cmd = Device->createCommandList();
Cmd->begin();
Graph.execute(*Cmd, Plan);
Cmd->end();
GraphicsQueue->submit({ .CommandLists = std::span(&Cmd, 1) });
```

也会有一个更紧凑的门面写法：

```cpp
{
    auto P = beginPass(Graph, "PostProcess");   // 析构时提交
    const ResourceId Output = P->createTexture("Output", Desc);
    P->read(SceneColor, EResourceState::PixelShaderResource);
    P->setRenderTargets(std::span<const ResourceId>(&Output, 1));
    P->setExecute([](RDGPassContext&) { /* ... */ });
}
```

### 与材质系统对接

材质系统产出的 `RBindGroup` 天然适合由 Pass 使用：在 `setExecute` 里从外部捕获
材质实例，向 `RDGPassContext::getCommandList()` 记录
`bindBindGroups` / `pushConstants` 即可。RDG 只关心资源生命周期，
不侵入材质 ABI。

---

## 9. 帧循环中的位置

```
Acquire
  ↓
构建 RDG（声明 Pass，每帧重建）
  ↓
compile(&Pool)        ← 池按计划申请/复用堆，实例化瞬态资源
  ↓
Cmd->begin() → execute() → Cmd->end()
  ↓
Submit → Present
  ↓
下一帧：Pool.beginFrame() 把上一帧区间归还给堆后重新切分
```

多帧在飞需要**每帧一个 `RDGGraph` 与一个 `RDGResourcePool`**：
一帧的显存在被 GPU 使用期间不能被下一帧复用。
`RDGResourcePool::beginFrame()` 负责把上一帧切出去的区间归还，
因此调用它之前必须保证上一帧的 GPU 工作已完成（timeline semaphore）。

---

## 10. 验证

| 测试 | 覆盖内容 | 是否需要 GPU |
|---|---|---|
| `SeedRDGTests`（`tests/rdg_cpu.cpp`，136 项断言） | 依赖构建、拓扑排序、声明顺序无关性、菱形依赖、Pass 裁剪、生命周期、别名计划、区间不重叠、屏障推导、别名复用 `Undefined` 重置、冗余屏障抑制、读写合并、池交互（别名 / 退化 / placed 被拒）、上下文类型解析、非法声明拒绝、尺寸估算、图复用 | 否 |
| `SeedRDGGpuTests`（`tests/rdg_gpu.cpp`，43 项断言） | 真实堆申请 / 子分配、placed Buffer/Image 绑定、非法偏移拒绝、带别名的整帧编译 + 录制 + 提交、关闭别名时的退化路径、连续多帧复用堆 | 是（无设备时打印 SKIP 并以 0 退出） |
| `SeedRHIContractTests` | RHI 公共契约（原有） | 否 |

关键技术点用**伪 `RCommandList`** 把执行期的 `barriers()` 调用与 Pass 顺序录下来做断言，
因此"屏障计划"和"屏障录制"两段都有回归保护，而不是只验证前者。

运行：

```bash
ctest --test-dir build --output-on-failure
```

---

## 11. 已知限制与后续方向

- **单队列**：屏障只表达同队列内的执行/内存依赖，没有队列族所有权转移
  （`SourceQueue/DestinationQueue` 字段在 RHI 已存在，RDG 尚未使用）；
- **整资源粒度**：不跟踪 mip / layer 子资源状态；
- **堆不会自动缩小**：池保留上一帧的堆，只在 `releaseHeaps()` 时释放；
  后续可按帧间显存预算做收缩策略；
- **别名跨度是保守上界**：槽位之间线性摆放，未做槽位级时间重叠的进一步压缩
  （`RDGHeapLayout::PeakSize` 给出了同时存活量的下界，可用于后续优化）；
- **跨帧资源池化**：目前导出资源（`keepAlive`）不参与别名，
  持久化 RTT 的复用需要显式管理。
