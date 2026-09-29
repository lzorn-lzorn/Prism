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