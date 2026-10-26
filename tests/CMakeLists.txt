add_executable(SeedRHIContractTests todo.cpp)
find_package(Vulkan REQUIRED)
target_link_libraries(SeedRHIContractTests PRIVATE Module::RHI)
target_include_directories(SeedRHIContractTests PRIVATE
	${CMAKE_SOURCE_DIR}/sources/engine/modules/rhi/sources)
if(TARGET Vulkan::Vulkan)
	target_link_libraries(SeedRHIContractTests PRIVATE Vulkan::Vulkan)
endif()
add_test(NAME SeedRHI.Contract COMMAND SeedRHIContractTests)

# RDG 图算法 / 屏障推导 / 别名计划的 CPU 侧验证; 不需要 GPU.
add_executable(SeedRDGTests rdg_cpu.cpp)
target_link_libraries(SeedRDGTests PRIVATE Runtime::Renderer)
add_test(NAME SeedRDG.Cpu COMMAND SeedRDGTests)

# 渲染命令队列(SPSC 无锁)的 CPU 侧验证; 不需要 GPU.
add_executable(SeedRenderCommandTests render_command.cpp)
target_link_libraries(SeedRenderCommandTests PRIVATE Runtime::Renderer)
add_test(NAME SeedRenderCommand.Cpu COMMAND SeedRenderCommandTests)

# 渲染线程(命令消费 + 场景快照)的 CPU 侧验证; 不需要 GPU.
add_executable(SeedRenderThreadTests render_thread.cpp)
target_link_libraries(SeedRenderThreadTests PRIVATE Runtime::Renderer)
add_test(NAME SeedRenderThread.Cpu COMMAND SeedRenderThreadTests)

# 资源加载层(pbrt 场景 + PLY 网格)的 CPU 侧验证; 不需要 GPU.
add_executable(SeedPbrtLoadTests pbrt_load.cpp)
target_link_libraries(SeedPbrtLoadTests PRIVATE Runtime::AssetPbrt)
add_test(NAME SeedAsset.PbrtLoad COMMAND SeedPbrtLoadTests)

# RDG 在真实设备上的验证: 瞬态堆 / placed resource / 别名复用 / 一帧录制提交.
# 没有 GPU 或无法创建窗口时测试自身会打印 SKIP 并以 0 退出.
add_executable(SeedRDGGpuTests rdg_gpu.cpp)
target_link_libraries(SeedRDGGpuTests PRIVATE Runtime::Renderer Module::RHI SDL3::SDL3)
target_include_directories(SeedRDGGpuTests PRIVATE
	${CMAKE_SOURCE_DIR}/sources/engine/modules/rhi/sources)
if(TARGET Vulkan::Vulkan)
	target_link_libraries(SeedRDGGpuTests PRIVATE Vulkan::Vulkan)
endif()
add_test(NAME SeedRDG.Gpu COMMAND SeedRDGGpuTests)

# UI 纯布局验证: 不依赖字体/GPU, 验证窗口放大后子控件按 flex 规则跟随增长.
add_executable(SeedUILayoutTests ui_layout.cpp)
target_link_libraries(SeedUILayoutTests PRIVATE UI::UI)
add_test(NAME SeedUI.Layout COMMAND SeedUILayoutTests)

# 端到端: 最大化窗口后 Window::drawFrame 是否按新安全区重新布局内容根.
add_executable(SeedWindowResizeTests window_resize.cpp)
target_link_libraries(SeedWindowResizeTests PRIVATE Editor::Editor SDL3::SDL3)
add_test(NAME SeedWindow.Resize COMMAND SeedWindowResizeTests)

# 交换链 resize 回归: 窗口缩放后交换链必须跟随窗口像素尺寸(Windows+Vulkan 上
# acquireNextImage 缩放后仍返回 Success 的回归). 无 GPU 时打印 SKIP 并以 0 退出.
add_executable(SeedSwapchainResizeTests swapchain_resize.cpp)
target_link_libraries(SeedSwapchainResizeTests PRIVATE Module::RHI SDL3::SDL3)
target_include_directories(SeedSwapchainResizeTests PRIVATE
	${CMAKE_SOURCE_DIR}/sources/engine/modules/rhi/sources)
if(TARGET Vulkan::Vulkan)
	target_link_libraries(SeedSwapchainResizeTests PRIVATE Vulkan::Vulkan)
endif()
add_test(NAME SeedSwapchain.Resize COMMAND SeedSwapchainResizeTests)