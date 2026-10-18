#include "VulkanRHI.hpp"
#include "VulkanDevice.hpp"
#include "vulkan/vulkan.hpp"

#include <iostream>
#include <vector>
#include <set>
#include <print>
#include <algorithm>
#include <stdexcept>
#include <cstring>
#include <string_view>

#ifdef USE_SDL
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#endif

namespace 
{
static std::vector<const char*> getRequiredInstanceExtensions()
{
    std::vector<const char*> extensions;
#ifdef USE_SDL
    Uint32 extension_count = 0;
    const char* const* sdl_extensions = SDL_Vulkan_GetInstanceExtensions(&extension_count);
    if (!sdl_extensions)
    {
        throw std::runtime_error(std::string("Failed to query SDL Vulkan extensions: ") + SDL_GetError());
    }
    extensions.assign(sdl_extensions, sdl_extensions + extension_count);
#else
    throw std::runtime_error("No generic window backend is available for Vulkan surface creation.");
#endif
    return extensions;
}

static bool checkInstanceExtensions(const std::vector<const char*>& Required)
{
    if (Required.empty()) return true;
    auto available = vk::enumerateInstanceExtensionProperties();
    std::set<std::string> available_set;
    for (const auto& ext : available)
    {
		available_set.insert(ext.extensionName);
	}
    for (const char* ext : Required) 
	{
        if (available_set.find(ext) == available_set.end()) 
		{
            std::cerr << "Missing instance extension: " << ext << std::endl;
            return false;
        }
    }
    return true;
}

static bool checkDeviceExtensions(vk::PhysicalDevice RealGPU, const std::vector<const char*>& Required)
{
    if (Required.empty()) return true;
    auto available = RealGPU.enumerateDeviceExtensionProperties();
    std::set<std::string> available_set;
    for (const auto& ext : available)
    {
		available_set.insert(ext.extensionName);
	}
    for (const char* ext : Required) 
	{
        if (available_set.find(ext) == available_set.end()) 
		{
            std::cerr << "Missing device extension: " << ext << std::endl;
            return false;
        }
    }
    return true;
}

static bool hasExtension(
    const std::vector<vk::ExtensionProperties>& Available,
    std::string_view Name)
{
    return std::ranges::any_of(Available, [Name](const vk::ExtensionProperties& Extension)
    {
        return Name == Extension.extensionName.data();
    });
}

static bool checkDeviceFeatures(vk::PhysicalDevice RealGPU, const vk::PhysicalDeviceFeatures& Required)
{
    auto supported = RealGPU.getFeatures();
    // 仅检查我们需要的特性
    if (Required.samplerAnisotropy && !supported.samplerAnisotropy) return false;
    if (Required.fillModeNonSolid  && !supported.fillModeNonSolid)  return false;
    if (Required.geometryShader    && !supported.geometryShader)    return false;
    if (Required.tessellationShader && !supported.tessellationShader) return false;
    return true;
}


static uint64_t getDeviceLocalMemorySize(vk::PhysicalDevice RealGPU)
{
    auto mem_props = RealGPU.getMemoryProperties();
    uint64_t max_device_local_size = 0;
    for (uint32_t i = 0; i < mem_props.memoryHeapCount; ++i) 
	{
        if (mem_props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) 
		{
            max_device_local_size = std::max(max_device_local_size, mem_props.memoryHeaps[i].size);
        }
    }
    return max_device_local_size;
}

static int scoreDevice(vk::PhysicalDevice RealGPU)
{
    int score = 0;
    auto props = RealGPU.getProperties();

    // 设备类型基础分
    switch (props.deviceType) {
    case vk::PhysicalDeviceType::eDiscreteGpu:   score += 1000; break;
    case vk::PhysicalDeviceType::eIntegratedGpu: score += 500;  break;
    case vk::PhysicalDeviceType::eVirtualGpu:    score += 200;  break;
    case vk::PhysicalDeviceType::eCpu:           score += 100;  break;
    default:                                     score += 0;    break;
    }

    // 显存大小（每 MB 加 1 分）
    uint64_t mem_size = getDeviceLocalMemorySize(RealGPU);
    score += static_cast<int>(mem_size / (1024 * 1024));

    // 队列能力加分
    auto queue_families = RealGPU.getQueueFamilyProperties();
    bool has_graphics = false;
    bool has_compute = false;
    bool has_separate_compute = false;
    for (const auto& qf : queue_families) {
        if (qf.queueFlags & vk::QueueFlagBits::eGraphics) has_graphics = true;
        if (qf.queueFlags & vk::QueueFlagBits::eCompute)  has_compute  = true;
        if ((qf.queueFlags & vk::QueueFlagBits::eCompute) &&
            !(qf.queueFlags & vk::QueueFlagBits::eGraphics)) {
            has_separate_compute = true;
        }
    }
    if (has_graphics && has_compute) score += 100;
    if (has_separate_compute)        score += 50;

    return score;
}


}
namespace rhi
{

uint64_t clampCopySize(uint64_t Offset, uint64_t RequestedSize, uint64_t MaxSize)
{
	if (Offset >= MaxSize)
	{
		return 0;
	}

	const uint64_t available_size = MaxSize - Offset;
	if (RequestedSize == 0)
	{
		return available_size;
	}
	return std::min<uint64_t>(RequestedSize, available_size);
}

VulkanRHI::~VulkanRHI()
{
    IsInitialized = false;
	Context.reset();
}

void VulkanRHI::initialize(const ui::GenericWindowPointer& Window)
{
    if (!Window || !Window->getNativeHandle())
    {
        throw std::invalid_argument("Vulkan initialization requires a valid generic window.");
    }
    if (IsInitialized)
    {
        return;
    }

    createVkInstance();
	Context->Window = Window;
    createVkSurface(Window);
    pickPhysicalDevice();
    createLogicalDevice();
    IsInitialized = true;
}



void VulkanRHI::createVkInstance()
{
    vk::ApplicationInfo app_info(
        "SeedEngine",
        VK_MAKE_VERSION(1, 0, 0),
        "SeedEngine",
        VK_MAKE_VERSION(1, 0, 0),
        VK_API_VERSION_1_3
    );

    std::vector<const char*> layers;
#ifndef NDEBUG
    const char* validation_layer = "VK_LAYER_KHRONOS_validation";
    auto available_layers = vk::enumerateInstanceLayerProperties();
    for (const auto& layer : available_layers) 
	{
        if (strcmp(layer.layerName, validation_layer) == 0) 
		{
            layers.push_back(validation_layer);
            break;
        }
    }
// TODO: 接入日志系统
    if (layers.empty())
	{
		std::cerr << "Warning: Validation layer requested but not available." << std::endl;
	}
#endif

    auto required_instance_extensions = getRequiredInstanceExtensions();
    const auto available_instance_extensions = vk::enumerateInstanceExtensionProperties();
    if (hasExtension(available_instance_extensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        required_instance_extensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (std::ranges::any_of(available_instance_extensions, [](const vk::ExtensionProperties& extension)
        {
            return std::strcmp(extension.extensionName.data(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        }))
    {
        required_instance_extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (!checkInstanceExtensions(required_instance_extensions))
	{
        throw std::runtime_error("Required instance extensions are not supported.");
    }

    vk::InstanceCreateInfo create_info(
        vk::InstanceCreateFlags(),
        &app_info,
        static_cast<uint32_t>(layers.size()),
        layers.empty() ? nullptr : layers.data(),
        static_cast<uint32_t>(required_instance_extensions.size()),
        required_instance_extensions.empty() ? nullptr : required_instance_extensions.data()
    );

    Context->Instance = vk::createInstanceUnique(create_info);
}

void VulkanRHI::createVkSurface(const ui::GenericWindowPointer& Window)
{
#ifdef USE_SDL
    auto* sdl_window = static_cast<SDL_Window*>(Window->getNativeHandle());
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(
        sdl_window,
        static_cast<VkInstance>(Context->Instance.get()),
        nullptr,
        &surface))
    {
        throw std::runtime_error(std::string("Failed to create SDL Vulkan surface: ") + SDL_GetError());
    }
    auto state = std::make_shared<VulkanSurfaceState>();
    state->Instance = Context->Instance.get();
    state->Handle = vk::SurfaceKHR(surface);
    state->Generation = Context->NextSurfaceGeneration++;
    Context->Surface = std::move(state);
#else
    (void)Window;
    throw std::runtime_error("No generic window backend is available for Vulkan surface creation.");
#endif
}

bool VulkanRHI::recoverSurface(const ui::GenericWindowPointer& Window)
{
    if (!IsInitialized || Context->isDeviceLost()) return false;
    const auto window = Window ? Window : Context->Window.lock();
    if (!window || !window->getNativeHandle()) return false;
    try
    {
        createVkSurface(window);
        if (!Context->PhysicalDevice.getSurfaceSupportKHR(
            Context->PresentQueueFamilyIndex, Context->Surface->Handle))
            return false;
        Context->Window = window;
        return true;
    }
    catch (const vk::DeviceLostError&)
    {
        Context->markDeviceLost();
        return false;
    }
    catch (const vk::SystemError& error)
    {
        if (error.code().value() == static_cast<int>(vk::Result::eErrorDeviceLost))
            Context->markDeviceLost();
        return false;
    }
    catch (...)
    {
        return false;
    }
}

void VulkanRHI::pickPhysicalDevice()
{
    if (!Context->Instance)
	{
		throw std::runtime_error("Vulkan instance not created before picking physical device.");
	}
        

    auto physical_devices = Context->Instance->enumeratePhysicalDevices();
    if (physical_devices.empty())
    {
		throw std::runtime_error("No Vulkan physical devices found.");
	}

    vk::PhysicalDeviceFeatures required_features{};

    // TODO: 需要支持的设备扩展
    std::vector<const char*> requiredDeviceExtensions = 
	{
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,   // 交换链
    };

    vk::PhysicalDevice best_device = nullptr;
    int best_score = -1;

    for (const auto& device : physical_devices) 
	{
        if (device.getProperties().apiVersion < VK_API_VERSION_1_3)
        {
            std::println("Device {} does not support Vulkan 1.3, skipping.",
                device.getProperties().deviceName);
            continue;
        }

        vk::PhysicalDeviceVulkan13Features vulkan13_features;
        vk::PhysicalDeviceFeatures2 feature_query;
        feature_query.pNext = &vulkan13_features;
        device.getFeatures2(&feature_query);
        if (!vulkan13_features.dynamicRendering || !vulkan13_features.synchronization2)
        {
            std::println(
                "Device {} lacks dynamic rendering or synchronization2, skipping.",
                device.getProperties().deviceName);
            continue;
        }

        // ---- 检查核心特性 ----
        if (!checkDeviceFeatures(device, required_features)) 
		{
			std::println("Device {} does not support required features, skipping.", 
				device.getProperties().deviceName);
            continue;
        }

        // ---- 检查设备扩展 ----
        if (!checkDeviceExtensions(device, requiredDeviceExtensions)) 
		{
            std::println("Device {} does not support required extensions, skipping.", 
				device.getProperties().deviceName);
            continue;
        }

        // ---- 检查队列族(至少需要图形和计算) ----
        auto queue_families = device.getQueueFamilyProperties();
        bool has_graphics = false;
        bool has_present = false;
        for (uint32_t family_index = 0; family_index < queue_families.size(); ++family_index)
		{
            const auto& qf = queue_families[family_index];
            if (qf.queueFlags & vk::QueueFlagBits::eGraphics) has_graphics = true;
            if (device.getSurfaceSupportKHR(family_index, Context->Surface->Handle)) has_present = true;
        }
        if (!has_graphics || !has_present) 
		{
            std::println("Device {} lacks graphics or presentation support, skipping.", 
				device.getProperties().deviceName);
            continue;
        }

        int score = scoreDevice(device);
        if (score > best_score) {
            best_score = score;
            best_device = device;
        }
    }

    if (!best_device)
    {
		throw std::runtime_error("No suitable Vulkan physical device found.");
	}
    Context->PhysicalDevice = best_device;
    std::println("Selected device: {} (score {})\n", 
				Context->PhysicalDevice.getProperties().deviceName.data(), best_score);
}

void VulkanRHI::createLogicalDevice()
{
    if (!Context->PhysicalDevice)
    {
		throw std::runtime_error("Physical device not selected.");
	}

    auto queue_families = Context->PhysicalDevice.getQueueFamilyProperties();

    // ---- 查找图形队列族 ----
    uint32_t graphics_queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < queue_families.size(); ++i) 
	{
        if (queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) 
		{
            graphics_queue_family = i;
            break;
        }
    }
    if (graphics_queue_family == UINT32_MAX)
    {
		throw std::runtime_error("No graphics queue family found.");
	}
    Context->GraphicsQueueFamilyIndex = graphics_queue_family;

    // ---- 查找呈现队列族 ----
    uint32_t present_queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < queue_families.size(); ++i)
	{
        if (Context->PhysicalDevice.getSurfaceSupportKHR(i, Context->Surface->Handle))
        {
            present_queue_family = i;
            break;
		}
    }
    if (present_queue_family == UINT32_MAX)
    {
        throw std::runtime_error("No presentation queue family found.");
    }
    Context->PresentQueueFamilyIndex = present_queue_family;

    uint32_t compute_queue_family = graphics_queue_family;
    uint32_t copy_queue_family = graphics_queue_family;
    for (uint32_t i = 0; i < queue_families.size(); ++i)
    {
        if ((queue_families[i].queueFlags & vk::QueueFlagBits::eCompute) &&
            !(queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics))
            compute_queue_family = i;
        if ((queue_families[i].queueFlags & vk::QueueFlagBits::eTransfer) &&
            !(queue_families[i].queueFlags & vk::QueueFlagBits::eGraphics) &&
            !(queue_families[i].queueFlags & vk::QueueFlagBits::eCompute))
            copy_queue_family = i;
    }
    Context->ComputeQueueFamilyIndex = compute_queue_family;
    Context->CopyQueueFamilyIndex = copy_queue_family;

    // ---- 准备队列创建信息 ----
    std::vector<vk::DeviceQueueCreateInfo> queue_createInfos;
    float queue_priority = 1.0f;
    std::set<uint32_t> unique_queue_families = {
        graphics_queue_family,
        present_queue_family,
        compute_queue_family,
        copy_queue_family };
    for (uint32_t family : unique_queue_families) 
	{
        vk::DeviceQueueCreateInfo queue_creation_info(
            vk::DeviceQueueCreateFlags(),
            family,
            1,                // 每个族申请一个队列
            &queue_priority
        );
        queue_createInfos.push_back(queue_creation_info);
    }

    const auto available_extensions = Context->PhysicalDevice.enumerateDeviceExtensionProperties();
    const bool mesh_shader_extension_available =
        hasExtension(available_extensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const bool extended_dynamic_state3_extension_available =
        hasExtension(available_extensions, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool vertex_input_dynamic_state_extension_available =
        hasExtension(available_extensions, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const bool deferred_host_operations_available =
        hasExtension(available_extensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool acceleration_structure_extension_available =
        hasExtension(available_extensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool ray_tracing_pipeline_extension_available =
        hasExtension(available_extensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool ray_query_extension_available =
        hasExtension(available_extensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
	const bool hdr_metadata_extension_available =
		hasExtension(available_extensions, VK_EXT_HDR_METADATA_EXTENSION_NAME);

    vk::PhysicalDeviceVulkan13Features supported_vulkan13 {};
    vk::PhysicalDeviceVulkan12Features supported_vulkan12 {};
    vk::PhysicalDeviceMultiviewFeatures supported_multiview {};
    vk::PhysicalDeviceMeshShaderFeaturesEXT supported_mesh_shader {};
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT supported_dynamic_state {};
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT supported_dynamic_state2 {};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT supported_dynamic_state3 {};
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT supported_vertex_input {};
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR supported_acceleration_structure {};
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR supported_ray_tracing_pipeline {};
    vk::PhysicalDeviceRayQueryFeaturesKHR supported_ray_query {};
    void* supported_ray_tracing_head = nullptr;
    if (acceleration_structure_extension_available)
    {
        supported_ray_tracing_head = &supported_acceleration_structure;
        supported_acceleration_structure.pNext = ray_tracing_pipeline_extension_available
            ? static_cast<void*>(&supported_ray_tracing_pipeline)
            : ray_query_extension_available ? static_cast<void*>(&supported_ray_query) : nullptr;
        supported_ray_tracing_pipeline.pNext = ray_query_extension_available
            ? static_cast<void*>(&supported_ray_query) : nullptr;
    }
    supported_vulkan13.pNext = &supported_vulkan12;
    supported_vulkan12.pNext = &supported_multiview;
    // VK_EXT_extended_dynamic_state[2] are promoted to Vulkan 1.3 but retain feature structs.
    supported_multiview.pNext = mesh_shader_extension_available
        ? static_cast<void*>(&supported_mesh_shader)
        : static_cast<void*>(&supported_dynamic_state);
    supported_mesh_shader.pNext = &supported_dynamic_state;
    supported_dynamic_state.pNext = &supported_dynamic_state2;
    supported_dynamic_state2.pNext = extended_dynamic_state3_extension_available
        ? static_cast<void*>(&supported_dynamic_state3)
        : vertex_input_dynamic_state_extension_available
            ? static_cast<void*>(&supported_vertex_input)
            : supported_ray_tracing_head;
    supported_dynamic_state3.pNext = vertex_input_dynamic_state_extension_available
        ? static_cast<void*>(&supported_vertex_input)
        : supported_ray_tracing_head;
	supported_vertex_input.pNext = supported_ray_tracing_head;
    vk::PhysicalDeviceFeatures2 supported_features {};
    supported_features.pNext = &supported_vulkan13;
    Context->PhysicalDevice.getFeatures2(&supported_features);
    if (!supported_vulkan13.dynamicRendering || !supported_vulkan13.synchronization2 ||
		!supported_vulkan12.timelineSemaphore)
    {
        throw std::runtime_error(
            "Selected Vulkan device lacks dynamic rendering, synchronization2, or timeline semaphores.");
    }

    vk::PhysicalDeviceVulkan13Features enabled_vulkan13 {};
    enabled_vulkan13.dynamicRendering = VK_TRUE;
    enabled_vulkan13.synchronization2 = VK_TRUE;
    vk::PhysicalDeviceVulkan12Features enabled_vulkan12 {};
    enabled_vulkan12.descriptorIndexing = supported_vulkan12.descriptorIndexing;
	enabled_vulkan12.timelineSemaphore = supported_vulkan12.timelineSemaphore;
    // Vulkan 1.3 promotes the Vulkan 1.2 bufferDeviceAddress capability; the
    // VK_KHR_buffer_device_address extension has equivalent allocation/query rules.
    enabled_vulkan12.bufferDeviceAddress = supported_vulkan12.bufferDeviceAddress;
    enabled_vulkan12.runtimeDescriptorArray = supported_vulkan12.runtimeDescriptorArray;
    enabled_vulkan12.descriptorBindingPartiallyBound = supported_vulkan12.descriptorBindingPartiallyBound;
    enabled_vulkan12.descriptorBindingVariableDescriptorCount =
        supported_vulkan12.descriptorBindingVariableDescriptorCount;
    enabled_vulkan12.descriptorBindingUniformBufferUpdateAfterBind =
        supported_vulkan12.descriptorBindingUniformBufferUpdateAfterBind;
    enabled_vulkan12.descriptorBindingSampledImageUpdateAfterBind =
        supported_vulkan12.descriptorBindingSampledImageUpdateAfterBind;
    enabled_vulkan12.descriptorBindingStorageImageUpdateAfterBind =
        supported_vulkan12.descriptorBindingStorageImageUpdateAfterBind;
    enabled_vulkan12.descriptorBindingStorageBufferUpdateAfterBind =
        supported_vulkan12.descriptorBindingStorageBufferUpdateAfterBind;
    enabled_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind =
        supported_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind;
    enabled_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind =
        supported_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    enabled_vulkan12.separateDepthStencilLayouts = supported_vulkan12.separateDepthStencilLayouts;
    vk::PhysicalDeviceMultiviewFeatures enabled_multiview {};
    enabled_multiview.multiview = supported_multiview.multiview;
    enabled_vulkan13.pNext = &enabled_vulkan12;
    enabled_vulkan12.pNext = &enabled_multiview;
    vk::PhysicalDeviceMeshShaderFeaturesEXT enabled_mesh_shader {};
    enabled_mesh_shader.meshShader = mesh_shader_extension_available
        ? supported_mesh_shader.meshShader
        : VK_FALSE;
    enabled_mesh_shader.taskShader = mesh_shader_extension_available
        ? supported_mesh_shader.taskShader
        : VK_FALSE;
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT enabled_dynamic_state {};
    enabled_dynamic_state.extendedDynamicState = supported_dynamic_state.extendedDynamicState;
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT enabled_dynamic_state2 {};
    enabled_dynamic_state2.extendedDynamicState2 = supported_dynamic_state2.extendedDynamicState2;
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT enabled_dynamic_state3 {};
    if (extended_dynamic_state3_extension_available)
    {
        // Enable the portable VK_EXT_extended_dynamic_state3 fixed-function subset represented
        // by this aggregate capability. Vendor/companion-extension states remain disabled.
        enabled_dynamic_state3.extendedDynamicState3TessellationDomainOrigin =
            supported_dynamic_state3.extendedDynamicState3TessellationDomainOrigin;
        enabled_dynamic_state3.extendedDynamicState3DepthClampEnable =
            supported_dynamic_state3.extendedDynamicState3DepthClampEnable;
        enabled_dynamic_state3.extendedDynamicState3PolygonMode =
            supported_dynamic_state3.extendedDynamicState3PolygonMode;
        enabled_dynamic_state3.extendedDynamicState3RasterizationSamples =
            supported_dynamic_state3.extendedDynamicState3RasterizationSamples;
        enabled_dynamic_state3.extendedDynamicState3SampleMask =
            supported_dynamic_state3.extendedDynamicState3SampleMask;
        enabled_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable =
            supported_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable;
        enabled_dynamic_state3.extendedDynamicState3AlphaToOneEnable =
            supported_dynamic_state3.extendedDynamicState3AlphaToOneEnable;
        enabled_dynamic_state3.extendedDynamicState3LogicOpEnable =
            supported_dynamic_state3.extendedDynamicState3LogicOpEnable;
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEnable =
            supported_dynamic_state3.extendedDynamicState3ColorBlendEnable;
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEquation =
            supported_dynamic_state3.extendedDynamicState3ColorBlendEquation;
        enabled_dynamic_state3.extendedDynamicState3ColorWriteMask =
            supported_dynamic_state3.extendedDynamicState3ColorWriteMask;
    }
    const bool enable_dynamic_state3_extension =
        enabled_dynamic_state3.extendedDynamicState3TessellationDomainOrigin ||
        enabled_dynamic_state3.extendedDynamicState3DepthClampEnable ||
        enabled_dynamic_state3.extendedDynamicState3PolygonMode ||
        enabled_dynamic_state3.extendedDynamicState3RasterizationSamples ||
        enabled_dynamic_state3.extendedDynamicState3SampleMask ||
        enabled_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable ||
        enabled_dynamic_state3.extendedDynamicState3AlphaToOneEnable ||
        enabled_dynamic_state3.extendedDynamicState3LogicOpEnable ||
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEnable ||
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEquation ||
        enabled_dynamic_state3.extendedDynamicState3ColorWriteMask;
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT enabled_vertex_input {};
    enabled_vertex_input.vertexInputDynamicState = vertex_input_dynamic_state_extension_available
        ? supported_vertex_input.vertexInputDynamicState
        : VK_FALSE;
    // VK_KHR_ray_tracing_pipeline requires acceleration_structure and deferred_host_operations.
    // SPIR-V 1.4, shaderFloatControls, and bufferDeviceAddress are infra in the required Vulkan 1.3 API.
    const bool enable_acceleration_structure = deferred_host_operations_available &&
        acceleration_structure_extension_available && enabled_vulkan12.bufferDeviceAddress &&
        supported_acceleration_structure.accelerationStructure;
    const bool enable_ray_tracing_pipeline = enable_acceleration_structure &&
        ray_tracing_pipeline_extension_available && supported_ray_tracing_pipeline.rayTracingPipeline;
    const bool enable_ray_query = enable_acceleration_structure && ray_query_extension_available &&
        supported_ray_query.rayQuery;
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR enabled_acceleration_structure {};
    enabled_acceleration_structure.accelerationStructure = enable_acceleration_structure;
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR enabled_ray_tracing_pipeline {};
    enabled_ray_tracing_pipeline.rayTracingPipeline = enable_ray_tracing_pipeline;
    vk::PhysicalDeviceRayQueryFeaturesKHR enabled_ray_query {};
    enabled_ray_query.rayQuery = enable_ray_query;
    void* enabled_ray_tracing_head = enable_acceleration_structure
        ? static_cast<void*>(&enabled_acceleration_structure) : nullptr;
    enabled_acceleration_structure.pNext = enable_ray_tracing_pipeline
        ? static_cast<void*>(&enabled_ray_tracing_pipeline)
        : enable_ray_query ? static_cast<void*>(&enabled_ray_query) : nullptr;
    enabled_ray_tracing_pipeline.pNext = enable_ray_query
        ? static_cast<void*>(&enabled_ray_query) : nullptr;

    enabled_multiview.pNext = enabled_mesh_shader.meshShader
        ? static_cast<void*>(&enabled_mesh_shader)
        : static_cast<void*>(&enabled_dynamic_state);
    enabled_mesh_shader.pNext = &enabled_dynamic_state;
    enabled_dynamic_state.pNext = &enabled_dynamic_state2;
    enabled_dynamic_state2.pNext = enable_dynamic_state3_extension
        ? static_cast<void*>(&enabled_dynamic_state3)
        : enabled_vertex_input.vertexInputDynamicState
            ? static_cast<void*>(&enabled_vertex_input)
            : enabled_ray_tracing_head;
    enabled_dynamic_state3.pNext = enabled_vertex_input.vertexInputDynamicState
        ? static_cast<void*>(&enabled_vertex_input)
        : enabled_ray_tracing_head;
	enabled_vertex_input.pNext = enabled_ray_tracing_head;

    // Pipeline 后端只在对应能力实际启用后对外报告支持. 这里按硬件支持启用
    // 常用固定功能；扩展动态状态, Mesh 和 Ray Tracing 仍由后续扩展链单独管理. 
    vk::PhysicalDeviceFeatures enabled_features{};
    enabled_features.geometryShader = supported_features.features.geometryShader;
    enabled_features.tessellationShader = supported_features.features.tessellationShader;
    enabled_features.fillModeNonSolid = supported_features.features.fillModeNonSolid;
    enabled_features.wideLines = supported_features.features.wideLines;
    enabled_features.depthClamp = supported_features.features.depthClamp;
    enabled_features.depthBounds = supported_features.features.depthBounds;
    enabled_features.sampleRateShading = supported_features.features.sampleRateShading;
    enabled_features.samplerAnisotropy = supported_features.features.samplerAnisotropy;
    enabled_features.alphaToOne = supported_features.features.alphaToOne;
    enabled_features.independentBlend = supported_features.features.independentBlend;
	enabled_features.pipelineStatisticsQuery = supported_features.features.pipelineStatisticsQuery;

    // ---- 启用扩展(与之前检查对应) ----
    std::vector<const char*> enabledExtensions;
    enabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (hdr_metadata_extension_available)
        enabledExtensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (enabled_mesh_shader.meshShader)
        enabledExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (enable_dynamic_state3_extension)
        enabledExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (enabled_vertex_input.vertexInputDynamicState)
        enabledExtensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    if (enable_acceleration_structure)
    {
        enabledExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        enabledExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        if (enable_ray_tracing_pipeline)
            enabledExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (enable_ray_query)
            enabledExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }

    vk::DeviceCreateInfo device_creation_info(
        vk::DeviceCreateFlags(),
        static_cast<uint32_t>(queue_createInfos.size()),
        queue_createInfos.data(),
		0,
		nullptr,
        static_cast<uint32_t>(enabledExtensions.size()),
        enabledExtensions.empty() ? nullptr : enabledExtensions.data(),
        &enabled_features
    );
	device_creation_info.pNext = &enabled_vulkan13;

    Context->Device = Context->PhysicalDevice.createDeviceUnique(device_creation_info);

    auto& features = Context->EnabledFeatures;
    features.DynamicRendering = true;
    features.Synchronization2 = true;
    features.TimelineSemaphore = true;
    features.Multiview = enabled_multiview.multiview == VK_TRUE;
    features.SeparateDepthStencilLayouts = enabled_vulkan12.separateDepthStencilLayouts == VK_TRUE;
    features.GeometryShader = enabled_features.geometryShader == VK_TRUE;
    features.TessellationShader = enabled_features.tessellationShader == VK_TRUE;
    features.FillModeNonSolid = enabled_features.fillModeNonSolid == VK_TRUE;
    features.WideLines = enabled_features.wideLines == VK_TRUE;
    features.DepthClamp = enabled_features.depthClamp == VK_TRUE;
    features.DepthBounds = enabled_features.depthBounds == VK_TRUE;
    features.SampleRateShading = enabled_features.sampleRateShading == VK_TRUE;
    features.SamplerAnisotropy = enabled_features.samplerAnisotropy == VK_TRUE;
    features.AlphaToOne = enabled_features.alphaToOne == VK_TRUE;
    features.IndependentBlend = enabled_features.independentBlend == VK_TRUE;
    features.ExtendedDynamicState = enabled_dynamic_state.extendedDynamicState == VK_TRUE;
    features.ExtendedDynamicState2 = enabled_dynamic_state2.extendedDynamicState2 == VK_TRUE;
    // Do not overclaim EDS3: every state in the backend's aggregate subset must be enabled.
    features.ExtendedDynamicState3 =
        enabled_dynamic_state3.extendedDynamicState3TessellationDomainOrigin &&
        enabled_dynamic_state3.extendedDynamicState3DepthClampEnable &&
        enabled_dynamic_state3.extendedDynamicState3PolygonMode &&
        enabled_dynamic_state3.extendedDynamicState3RasterizationSamples &&
        enabled_dynamic_state3.extendedDynamicState3SampleMask &&
        enabled_dynamic_state3.extendedDynamicState3AlphaToCoverageEnable &&
        enabled_dynamic_state3.extendedDynamicState3AlphaToOneEnable &&
        enabled_dynamic_state3.extendedDynamicState3LogicOpEnable &&
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEnable &&
        enabled_dynamic_state3.extendedDynamicState3ColorBlendEquation &&
        enabled_dynamic_state3.extendedDynamicState3ColorWriteMask;
    features.DynamicVertexInput = enabled_vertex_input.vertexInputDynamicState == VK_TRUE;
    features.DescriptorIndexing = enabled_vulkan12.descriptorIndexing == VK_TRUE;
    features.RuntimeDescriptorArray = enabled_vulkan12.runtimeDescriptorArray == VK_TRUE;
    features.PartiallyBoundDescriptors = enabled_vulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    features.VariableDescriptorCount = enabled_vulkan12.descriptorBindingVariableDescriptorCount == VK_TRUE;
    features.UpdateAfterBind =
        enabled_vulkan12.descriptorBindingUniformBufferUpdateAfterBind &&
        enabled_vulkan12.descriptorBindingSampledImageUpdateAfterBind &&
        enabled_vulkan12.descriptorBindingStorageImageUpdateAfterBind &&
        enabled_vulkan12.descriptorBindingStorageBufferUpdateAfterBind &&
        enabled_vulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
        enabled_vulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    features.MeshShader = enabled_mesh_shader.meshShader == VK_TRUE;
    features.TaskShader = features.MeshShader && enabled_mesh_shader.taskShader == VK_TRUE;
    features.OcclusionQueries = true;
    features.PipelineStatisticsQueries = enabled_features.pipelineStatisticsQuery == VK_TRUE;
    features.QueryResultCopy = true;
	features.BufferDeviceAddress = enabled_vulkan12.bufferDeviceAddress == VK_TRUE;
    features.AccelerationStructure = enable_acceleration_structure;
    features.RayTracingPipeline = enable_ray_tracing_pipeline;
    features.RayQuery = enable_ray_query;
    features.SecondaryCommandLists = true;
    features.SwapchainStatus = true;
    features.HDRMetadata = hdr_metadata_extension_available;
}

std::shared_ptr<RDevice> VulkanRHI::createDevice()
{
    if (!Context->Device)
	{
		throw std::runtime_error("Logical device not created.");
	}
    return std::make_shared<VulkanDevice>(Context);
}
} // namespace rhi