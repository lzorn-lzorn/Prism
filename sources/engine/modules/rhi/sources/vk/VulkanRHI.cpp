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
    std::vector<const char*> Extensions;
#ifdef USE_SDL
    Uint32 ExtensionCount = 0;
    const char* const* SdlExtensions = SDL_Vulkan_GetInstanceExtensions(&ExtensionCount);
    if (!SdlExtensions)
    {
        throw std::runtime_error(std::string("Failed to query SDL Vulkan extensions: ") + SDL_GetError());
    }
    Extensions.assign(SdlExtensions, SdlExtensions + ExtensionCount);
#else
    throw std::runtime_error("No generic window backend is available for Vulkan surface creation.");
#endif
    return Extensions;
}

static bool checkInstanceExtensions(const std::vector<const char*>& Required)
{
    if (Required.empty()) return true;
    auto Available = vk::enumerateInstanceExtensionProperties();
    std::set<std::string> AvailableSet;
    for (const auto& Ext : Available)
    {
        AvailableSet.insert(Ext.extensionName);
    }
    for (const char* Ext : Required) 
    {
        if (AvailableSet.find(Ext) == AvailableSet.end()) 
        {
            std::cerr << "Missing instance extension: " << Ext << std::endl;
            return false;
        }
    }
    return true;
}

static bool checkDeviceExtensions(vk::PhysicalDevice RealGPU, const std::vector<const char*>& Required)
{
    if (Required.empty()) return true;
    auto Available = RealGPU.enumerateDeviceExtensionProperties();
    std::set<std::string> AvailableSet;
    for (const auto& Ext : Available)
    {
        AvailableSet.insert(Ext.extensionName);
    }
    for (const char* Ext : Required) 
    {
        if (AvailableSet.find(Ext) == AvailableSet.end()) 
        {
            std::cerr << "Missing device extension: " << Ext << std::endl;
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
    auto Supported = RealGPU.getFeatures();
    // 仅检查我们需要的特性
    if (Required.samplerAnisotropy && !Supported.samplerAnisotropy) return false;
    if (Required.fillModeNonSolid  && !Supported.fillModeNonSolid)  return false;
    if (Required.geometryShader    && !Supported.geometryShader)    return false;
    if (Required.tessellationShader && !Supported.tessellationShader) return false;
    return true;
}


static uint64_t getDeviceLocalMemorySize(vk::PhysicalDevice RealGPU)
{
    auto MemProps = RealGPU.getMemoryProperties();
    uint64_t MaxDeviceLocalSize = 0;
    for (uint32_t I = 0; I < MemProps.memoryHeapCount; ++I) 
    {
        if (MemProps.memoryHeaps[I].flags & vk::MemoryHeapFlagBits::eDeviceLocal) 
        {
            MaxDeviceLocalSize = std::max(MaxDeviceLocalSize, MemProps.memoryHeaps[I].size);
        }
    }
    return MaxDeviceLocalSize;
}

static int scoreDevice(vk::PhysicalDevice RealGPU)
{
    int Score = 0;
    auto Props = RealGPU.getProperties();

    // 设备类型基础分
    switch (Props.deviceType) {
    case vk::PhysicalDeviceType::eDiscreteGpu:   Score += 1000; break;
    case vk::PhysicalDeviceType::eIntegratedGpu: Score += 500;  break;
    case vk::PhysicalDeviceType::eVirtualGpu:    Score += 200;  break;
    case vk::PhysicalDeviceType::eCpu:           Score += 100;  break;
    default:                                     Score += 0;    break;
    }

    // 显存大小（每 MB 加 1 分）
    uint64_t MemSize = getDeviceLocalMemorySize(RealGPU);
    Score += static_cast<int>(MemSize / (1024 * 1024));

    // 队列能力加分
    auto QueueFamilies = RealGPU.getQueueFamilyProperties();
    bool HasGraphics = false;
    bool HasCompute = false;
    bool HasSeparateCompute = false;
    for (const auto& Qf : QueueFamilies) {
        if (Qf.queueFlags & vk::QueueFlagBits::eGraphics) HasGraphics = true;
        if (Qf.queueFlags & vk::QueueFlagBits::eCompute)  HasCompute  = true;
        if ((Qf.queueFlags & vk::QueueFlagBits::eCompute) &&
            !(Qf.queueFlags & vk::QueueFlagBits::eGraphics)) {
            HasSeparateCompute = true;
        }
    }
    if (HasGraphics && HasCompute) Score += 100;
    if (HasSeparateCompute)        Score += 50;

    return Score;
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

    const uint64_t AvailableSize = MaxSize - Offset;
    if (RequestedSize == 0)
    {
        return AvailableSize;
    }
    return std::min<uint64_t>(RequestedSize, AvailableSize);
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
    vk::ApplicationInfo AppInfo(
        "SeedEngine",
        VK_MAKE_VERSION(1, 0, 0),
        "SeedEngine",
        VK_MAKE_VERSION(1, 0, 0),
        VK_API_VERSION_1_3
    );

    std::vector<const char*> Layers;
#ifndef NDEBUG
    const char* ValidationLayer = "VK_LAYER_KHRONOS_validation";
    auto AvailableLayers = vk::enumerateInstanceLayerProperties();
    for (const auto& Layer : AvailableLayers) 
    {
        if (strcmp(Layer.layerName, ValidationLayer) == 0) 
        {
            Layers.push_back(ValidationLayer);
            break;
        }
    }
// TODO: 接入日志系统
    if (Layers.empty())
    {
        std::cerr << "Warning: Validation layer requested but not available." << std::endl;
    }
#endif

    auto RequiredInstanceExtensions = getRequiredInstanceExtensions();
    const auto AvailableInstanceExtensions = vk::enumerateInstanceExtensionProperties();
    if (hasExtension(AvailableInstanceExtensions, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME))
        RequiredInstanceExtensions.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
    if (std::ranges::any_of(AvailableInstanceExtensions, [](const vk::ExtensionProperties& Extension)
        {
            return std::strcmp(Extension.extensionName.data(), VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
        }))
    {
        RequiredInstanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    if (!checkInstanceExtensions(RequiredInstanceExtensions))
    {
        throw std::runtime_error("Required instance extensions are not supported.");
    }

    vk::InstanceCreateInfo CreateInfo(
        vk::InstanceCreateFlags(),
        &AppInfo,
        static_cast<uint32_t>(Layers.size()),
        Layers.empty() ? nullptr : Layers.data(),
        static_cast<uint32_t>(RequiredInstanceExtensions.size()),
        RequiredInstanceExtensions.empty() ? nullptr : RequiredInstanceExtensions.data()
    );

    Context->Instance = vk::createInstanceUnique(CreateInfo);
}

void VulkanRHI::createVkSurface(const ui::GenericWindowPointer& Window)
{
#ifdef USE_SDL
    auto* SdlWindow = static_cast<SDL_Window*>(Window->getNativeHandle());
    VkSurfaceKHR Surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(
        SdlWindow,
        static_cast<VkInstance>(Context->Instance.get()),
        nullptr,
        &Surface))
    {
        throw std::runtime_error(std::string("Failed to create SDL Vulkan surface: ") + SDL_GetError());
    }
    auto State = std::make_shared<VulkanSurfaceState>();
    State->Instance = Context->Instance.get();
    State->Handle = vk::SurfaceKHR(Surface);
    State->Generation = Context->NextSurfaceGeneration++;
    Context->Surface = std::move(State);
#else
    (void)Window;
    throw std::runtime_error("No generic window backend is available for Vulkan surface creation.");
#endif
}

bool VulkanRHI::recoverSurface(const ui::GenericWindowPointer& Window)
{
    if (!IsInitialized || Context->isDeviceLost()) return false;
    const auto ResolvedWindow = Window ? Window : Context->Window.lock();
    if (!ResolvedWindow || !ResolvedWindow->getNativeHandle()) return false;
    try
    {
        createVkSurface(ResolvedWindow);
        if (!Context->PhysicalDevice.getSurfaceSupportKHR(
            Context->PresentQueueFamilyIndex, Context->Surface->Handle))
            return false;
        Context->Window = ResolvedWindow;
        return true;
    }
    catch (const vk::DeviceLostError&)
    {
        Context->markDeviceLost();
        return false;
    }
    catch (const vk::SystemError& Error)
    {
        if (Error.code().value() == static_cast<int>(vk::Result::eErrorDeviceLost))
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
        

    auto PhysicalDevices = Context->Instance->enumeratePhysicalDevices();
    if (PhysicalDevices.empty())
    {
        throw std::runtime_error("No Vulkan physical devices found.");
    }

    vk::PhysicalDeviceFeatures RequiredFeatures{};

    // TODO: 需要支持的设备扩展
    std::vector<const char*> RequiredDeviceExtensions = 
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,   // 交换链
    };

    vk::PhysicalDevice BestDevice = nullptr;
    int BestScore = -1;

    for (const auto& Device : PhysicalDevices) 
    {
        if (Device.getProperties().apiVersion < VK_API_VERSION_1_3)
        {
            std::println("Device {} does not support Vulkan 1.3, skipping.",
                Device.getProperties().deviceName);
            continue;
        }

        vk::PhysicalDeviceVulkan13Features Vulkan13Features;
        vk::PhysicalDeviceFeatures2 FeatureQuery;
        FeatureQuery.pNext = &Vulkan13Features;
        Device.getFeatures2(&FeatureQuery);
        if (!Vulkan13Features.dynamicRendering || !Vulkan13Features.synchronization2)
        {
            std::println(
                "Device {} lacks dynamic rendering or synchronization2, skipping.",
                Device.getProperties().deviceName);
            continue;
        }

        // ---- 检查核心特性 ----
        if (!checkDeviceFeatures(Device, RequiredFeatures)) 
        {
            std::println("Device {} does not support required features, skipping.", 
                Device.getProperties().deviceName);
            continue;
        }

        // ---- 检查设备扩展 ----
        if (!checkDeviceExtensions(Device, RequiredDeviceExtensions)) 
        {
            std::println("Device {} does not support required extensions, skipping.", 
                Device.getProperties().deviceName);
            continue;
        }

        // ---- 检查队列族(至少需要图形和计算) ----
        auto QueueFamilies = Device.getQueueFamilyProperties();
        bool HasGraphics = false;
        bool HasPresent = false;
        for (uint32_t FamilyIndex = 0; FamilyIndex < QueueFamilies.size(); ++FamilyIndex)
        {
            const auto& Qf = QueueFamilies[FamilyIndex];
            if (Qf.queueFlags & vk::QueueFlagBits::eGraphics) HasGraphics = true;
            if (Device.getSurfaceSupportKHR(FamilyIndex, Context->Surface->Handle)) HasPresent = true;
        }
        if (!HasGraphics || !HasPresent) 
        {
            std::println("Device {} lacks graphics or presentation support, skipping.", 
                Device.getProperties().deviceName);
            continue;
        }

        int Score = scoreDevice(Device);
        if (Score > BestScore) {
            BestScore = Score;
            BestDevice = Device;
        }
    }

    if (!BestDevice)
    {
        throw std::runtime_error("No suitable Vulkan physical device found.");
    }
    Context->PhysicalDevice = BestDevice;
    std::println("Selected device: {} (score {})\n", 
                Context->PhysicalDevice.getProperties().deviceName.data(), BestScore);
}

void VulkanRHI::createLogicalDevice()
{
    if (!Context->PhysicalDevice)
    {
        throw std::runtime_error("Physical device not selected.");
    }

    auto QueueFamilies = Context->PhysicalDevice.getQueueFamilyProperties();

    // ---- 查找图形队列族 ----
    uint32_t GraphicsQueueFamily = UINT32_MAX;
    for (uint32_t I = 0; I < QueueFamilies.size(); ++I) 
    {
        if (QueueFamilies[I].queueFlags & vk::QueueFlagBits::eGraphics) 
        {
            GraphicsQueueFamily = I;
            break;
        }
    }
    if (GraphicsQueueFamily == UINT32_MAX)
    {
        throw std::runtime_error("No graphics queue family found.");
    }
    Context->GraphicsQueueFamilyIndex = GraphicsQueueFamily;

    // ---- 查找呈现队列族 ----
    uint32_t PresentQueueFamily = UINT32_MAX;
    for (uint32_t I = 0; I < QueueFamilies.size(); ++I)
    {
        if (Context->PhysicalDevice.getSurfaceSupportKHR(I, Context->Surface->Handle))
        {
            PresentQueueFamily = I;
            break;
        }
    }
    if (PresentQueueFamily == UINT32_MAX)
    {
        throw std::runtime_error("No presentation queue family found.");
    }
    Context->PresentQueueFamilyIndex = PresentQueueFamily;

    uint32_t ComputeQueueFamily = GraphicsQueueFamily;
    uint32_t CopyQueueFamily = GraphicsQueueFamily;
    for (uint32_t I = 0; I < QueueFamilies.size(); ++I)
    {
        if ((QueueFamilies[I].queueFlags & vk::QueueFlagBits::eCompute) &&
            !(QueueFamilies[I].queueFlags & vk::QueueFlagBits::eGraphics))
            ComputeQueueFamily = I;
        if ((QueueFamilies[I].queueFlags & vk::QueueFlagBits::eTransfer) &&
            !(QueueFamilies[I].queueFlags & vk::QueueFlagBits::eGraphics) &&
            !(QueueFamilies[I].queueFlags & vk::QueueFlagBits::eCompute))
            CopyQueueFamily = I;
    }
    Context->ComputeQueueFamilyIndex = ComputeQueueFamily;
    Context->CopyQueueFamilyIndex = CopyQueueFamily;

    // ---- 准备队列创建信息 ----
    std::vector<vk::DeviceQueueCreateInfo> QueueCreateInfos;
    float QueuePriority = 1.0f;
    std::set<uint32_t> UniqueQueueFamilies = {
        GraphicsQueueFamily,
        PresentQueueFamily,
        ComputeQueueFamily,
        CopyQueueFamily };
    for (uint32_t Family : UniqueQueueFamilies) 
    {
        vk::DeviceQueueCreateInfo QueueCreationInfo(
            vk::DeviceQueueCreateFlags(),
            Family,
            1,                // 每个族申请一个队列
            &QueuePriority
        );
        QueueCreateInfos.push_back(QueueCreationInfo);
    }

    const auto AvailableExtensions = Context->PhysicalDevice.enumerateDeviceExtensionProperties();
    const bool MeshShaderExtensionAvailable =
        hasExtension(AvailableExtensions, VK_EXT_MESH_SHADER_EXTENSION_NAME);
    const bool ExtendedDynamicState3ExtensionAvailable =
        hasExtension(AvailableExtensions, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    const bool VertexInputDynamicStateExtensionAvailable =
        hasExtension(AvailableExtensions, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    const bool DeferredHostOperationsAvailable =
        hasExtension(AvailableExtensions, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
    const bool AccelerationStructureExtensionAvailable =
        hasExtension(AvailableExtensions, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
    const bool RayTracingPipelineExtensionAvailable =
        hasExtension(AvailableExtensions, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
    const bool RayQueryExtensionAvailable =
        hasExtension(AvailableExtensions, VK_KHR_RAY_QUERY_EXTENSION_NAME);
    const bool HdrMetadataExtensionAvailable =
        hasExtension(AvailableExtensions, VK_EXT_HDR_METADATA_EXTENSION_NAME);

    vk::PhysicalDeviceVulkan13Features SupportedVulkan13 {};
    vk::PhysicalDeviceVulkan12Features SupportedVulkan12 {};
    vk::PhysicalDeviceMultiviewFeatures SupportedMultiview {};
    vk::PhysicalDeviceMeshShaderFeaturesEXT SupportedMeshShader {};
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT SupportedDynamicState {};
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT SupportedDynamicState2 {};
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT SupportedDynamicState3 {};
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT SupportedVertexInput {};
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR SupportedAccelerationStructure {};
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR SupportedRayTracingPipeline {};
    vk::PhysicalDeviceRayQueryFeaturesKHR SupportedRayQuery {};
    void* SupportedRayTracingHead = nullptr;
    if (AccelerationStructureExtensionAvailable)
    {
        SupportedRayTracingHead = &SupportedAccelerationStructure;
        SupportedAccelerationStructure.pNext = RayTracingPipelineExtensionAvailable
            ? static_cast<void*>(&SupportedRayTracingPipeline)
            : RayQueryExtensionAvailable ? static_cast<void*>(&SupportedRayQuery) : nullptr;
        SupportedRayTracingPipeline.pNext = RayQueryExtensionAvailable
            ? static_cast<void*>(&SupportedRayQuery) : nullptr;
    }
    SupportedVulkan13.pNext = &SupportedVulkan12;
    SupportedVulkan12.pNext = &SupportedMultiview;
    // VK_EXT_extended_dynamic_state[2] are promoted to Vulkan 1.3 but retain feature structs.
    SupportedMultiview.pNext = MeshShaderExtensionAvailable
        ? static_cast<void*>(&SupportedMeshShader)
        : static_cast<void*>(&SupportedDynamicState);
    SupportedMeshShader.pNext = &SupportedDynamicState;
    SupportedDynamicState.pNext = &SupportedDynamicState2;
    SupportedDynamicState2.pNext = ExtendedDynamicState3ExtensionAvailable
        ? static_cast<void*>(&SupportedDynamicState3)
        : VertexInputDynamicStateExtensionAvailable
            ? static_cast<void*>(&SupportedVertexInput)
            : SupportedRayTracingHead;
    SupportedDynamicState3.pNext = VertexInputDynamicStateExtensionAvailable
        ? static_cast<void*>(&SupportedVertexInput)
        : SupportedRayTracingHead;
    SupportedVertexInput.pNext = SupportedRayTracingHead;
    vk::PhysicalDeviceFeatures2 SupportedFeatures {};
    SupportedFeatures.pNext = &SupportedVulkan13;
    Context->PhysicalDevice.getFeatures2(&SupportedFeatures);
    if (!SupportedVulkan13.dynamicRendering || !SupportedVulkan13.synchronization2 ||
        !SupportedVulkan12.timelineSemaphore)
    {
        throw std::runtime_error(
            "Selected Vulkan device lacks dynamic rendering, synchronization2, or timeline semaphores.");
    }

    vk::PhysicalDeviceVulkan13Features EnabledVulkan13 {};
    EnabledVulkan13.dynamicRendering = VK_TRUE;
    EnabledVulkan13.synchronization2 = VK_TRUE;
    vk::PhysicalDeviceVulkan12Features EnabledVulkan12 {};
    EnabledVulkan12.descriptorIndexing = SupportedVulkan12.descriptorIndexing;
    EnabledVulkan12.timelineSemaphore = SupportedVulkan12.timelineSemaphore;
    // Vulkan 1.3 promotes the Vulkan 1.2 bufferDeviceAddress capability; the
    // VK_KHR_buffer_device_address extension has equivalent allocation/query rules.
    EnabledVulkan12.bufferDeviceAddress = SupportedVulkan12.bufferDeviceAddress;
    EnabledVulkan12.runtimeDescriptorArray = SupportedVulkan12.runtimeDescriptorArray;
    EnabledVulkan12.descriptorBindingPartiallyBound = SupportedVulkan12.descriptorBindingPartiallyBound;
    EnabledVulkan12.descriptorBindingVariableDescriptorCount =
        SupportedVulkan12.descriptorBindingVariableDescriptorCount;
    EnabledVulkan12.descriptorBindingUniformBufferUpdateAfterBind =
        SupportedVulkan12.descriptorBindingUniformBufferUpdateAfterBind;
    EnabledVulkan12.descriptorBindingSampledImageUpdateAfterBind =
        SupportedVulkan12.descriptorBindingSampledImageUpdateAfterBind;
    EnabledVulkan12.descriptorBindingStorageImageUpdateAfterBind =
        SupportedVulkan12.descriptorBindingStorageImageUpdateAfterBind;
    EnabledVulkan12.descriptorBindingStorageBufferUpdateAfterBind =
        SupportedVulkan12.descriptorBindingStorageBufferUpdateAfterBind;
    EnabledVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind =
        SupportedVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind;
    EnabledVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind =
        SupportedVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    EnabledVulkan12.separateDepthStencilLayouts = SupportedVulkan12.separateDepthStencilLayouts;
    vk::PhysicalDeviceMultiviewFeatures EnabledMultiview {};
    EnabledMultiview.multiview = SupportedMultiview.multiview;
    EnabledVulkan13.pNext = &EnabledVulkan12;
    EnabledVulkan12.pNext = &EnabledMultiview;
    vk::PhysicalDeviceMeshShaderFeaturesEXT EnabledMeshShader {};
    EnabledMeshShader.meshShader = MeshShaderExtensionAvailable
        ? SupportedMeshShader.meshShader
        : VK_FALSE;
    EnabledMeshShader.taskShader = MeshShaderExtensionAvailable
        ? SupportedMeshShader.taskShader
        : VK_FALSE;
    vk::PhysicalDeviceExtendedDynamicStateFeaturesEXT EnabledDynamicState {};
    EnabledDynamicState.extendedDynamicState = SupportedDynamicState.extendedDynamicState;
    vk::PhysicalDeviceExtendedDynamicState2FeaturesEXT EnabledDynamicState2 {};
    EnabledDynamicState2.extendedDynamicState2 = SupportedDynamicState2.extendedDynamicState2;
    vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT EnabledDynamicState3 {};
    if (ExtendedDynamicState3ExtensionAvailable)
    {
        // Enable the portable VK_EXT_extended_dynamic_state3 fixed-function subset represented
        // by this aggregate capability. Vendor/companion-extension states remain disabled.
        EnabledDynamicState3.extendedDynamicState3TessellationDomainOrigin =
            SupportedDynamicState3.extendedDynamicState3TessellationDomainOrigin;
        EnabledDynamicState3.extendedDynamicState3DepthClampEnable =
            SupportedDynamicState3.extendedDynamicState3DepthClampEnable;
        EnabledDynamicState3.extendedDynamicState3PolygonMode =
            SupportedDynamicState3.extendedDynamicState3PolygonMode;
        EnabledDynamicState3.extendedDynamicState3RasterizationSamples =
            SupportedDynamicState3.extendedDynamicState3RasterizationSamples;
        EnabledDynamicState3.extendedDynamicState3SampleMask =
            SupportedDynamicState3.extendedDynamicState3SampleMask;
        EnabledDynamicState3.extendedDynamicState3AlphaToCoverageEnable =
            SupportedDynamicState3.extendedDynamicState3AlphaToCoverageEnable;
        EnabledDynamicState3.extendedDynamicState3AlphaToOneEnable =
            SupportedDynamicState3.extendedDynamicState3AlphaToOneEnable;
        EnabledDynamicState3.extendedDynamicState3LogicOpEnable =
            SupportedDynamicState3.extendedDynamicState3LogicOpEnable;
        EnabledDynamicState3.extendedDynamicState3ColorBlendEnable =
            SupportedDynamicState3.extendedDynamicState3ColorBlendEnable;
        EnabledDynamicState3.extendedDynamicState3ColorBlendEquation =
            SupportedDynamicState3.extendedDynamicState3ColorBlendEquation;
        EnabledDynamicState3.extendedDynamicState3ColorWriteMask =
            SupportedDynamicState3.extendedDynamicState3ColorWriteMask;
    }
    const bool EnableDynamicState3Extension =
        EnabledDynamicState3.extendedDynamicState3TessellationDomainOrigin ||
        EnabledDynamicState3.extendedDynamicState3DepthClampEnable ||
        EnabledDynamicState3.extendedDynamicState3PolygonMode ||
        EnabledDynamicState3.extendedDynamicState3RasterizationSamples ||
        EnabledDynamicState3.extendedDynamicState3SampleMask ||
        EnabledDynamicState3.extendedDynamicState3AlphaToCoverageEnable ||
        EnabledDynamicState3.extendedDynamicState3AlphaToOneEnable ||
        EnabledDynamicState3.extendedDynamicState3LogicOpEnable ||
        EnabledDynamicState3.extendedDynamicState3ColorBlendEnable ||
        EnabledDynamicState3.extendedDynamicState3ColorBlendEquation ||
        EnabledDynamicState3.extendedDynamicState3ColorWriteMask;
    vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT EnabledVertexInput {};
    EnabledVertexInput.vertexInputDynamicState = VertexInputDynamicStateExtensionAvailable
        ? SupportedVertexInput.vertexInputDynamicState
        : VK_FALSE;
    // VK_KHR_ray_tracing_pipeline requires acceleration_structure and deferred_host_operations.
    // SPIR-V 1.4, shaderFloatControls, and bufferDeviceAddress are infra in the required Vulkan 1.3 API.
    const bool EnableAccelerationStructure = DeferredHostOperationsAvailable &&
        AccelerationStructureExtensionAvailable && EnabledVulkan12.bufferDeviceAddress &&
        SupportedAccelerationStructure.accelerationStructure;
    const bool EnableRayTracingPipeline = EnableAccelerationStructure &&
        RayTracingPipelineExtensionAvailable && SupportedRayTracingPipeline.rayTracingPipeline;
    const bool EnableRayQuery = EnableAccelerationStructure && RayQueryExtensionAvailable &&
        SupportedRayQuery.rayQuery;
    vk::PhysicalDeviceAccelerationStructureFeaturesKHR EnabledAccelerationStructure {};
    EnabledAccelerationStructure.accelerationStructure = EnableAccelerationStructure;
    vk::PhysicalDeviceRayTracingPipelineFeaturesKHR EnabledRayTracingPipeline {};
    EnabledRayTracingPipeline.rayTracingPipeline = EnableRayTracingPipeline;
    vk::PhysicalDeviceRayQueryFeaturesKHR EnabledRayQuery {};
    EnabledRayQuery.rayQuery = EnableRayQuery;
    void* EnabledRayTracingHead = EnableAccelerationStructure
        ? static_cast<void*>(&EnabledAccelerationStructure) : nullptr;
    EnabledAccelerationStructure.pNext = EnableRayTracingPipeline
        ? static_cast<void*>(&EnabledRayTracingPipeline)
        : EnableRayQuery ? static_cast<void*>(&EnabledRayQuery) : nullptr;
    EnabledRayTracingPipeline.pNext = EnableRayQuery
        ? static_cast<void*>(&EnabledRayQuery) : nullptr;

    EnabledMultiview.pNext = EnabledMeshShader.meshShader
        ? static_cast<void*>(&EnabledMeshShader)
        : static_cast<void*>(&EnabledDynamicState);
    EnabledMeshShader.pNext = &EnabledDynamicState;
    EnabledDynamicState.pNext = &EnabledDynamicState2;
    EnabledDynamicState2.pNext = EnableDynamicState3Extension
        ? static_cast<void*>(&EnabledDynamicState3)
        : EnabledVertexInput.vertexInputDynamicState
            ? static_cast<void*>(&EnabledVertexInput)
            : EnabledRayTracingHead;
    EnabledDynamicState3.pNext = EnabledVertexInput.vertexInputDynamicState
        ? static_cast<void*>(&EnabledVertexInput)
        : EnabledRayTracingHead;
    EnabledVertexInput.pNext = EnabledRayTracingHead;

    // Pipeline 后端只在对应能力实际启用后对外报告支持. 这里按硬件支持启用
    // 常用固定功能；扩展动态状态, Mesh 和 Ray Tracing 仍由后续扩展链单独管理. 
    vk::PhysicalDeviceFeatures EnabledFeatures{};
    EnabledFeatures.geometryShader = SupportedFeatures.features.geometryShader;
    EnabledFeatures.tessellationShader = SupportedFeatures.features.tessellationShader;
    EnabledFeatures.fillModeNonSolid = SupportedFeatures.features.fillModeNonSolid;
    EnabledFeatures.wideLines = SupportedFeatures.features.wideLines;
    EnabledFeatures.depthClamp = SupportedFeatures.features.depthClamp;
    EnabledFeatures.depthBounds = SupportedFeatures.features.depthBounds;
    EnabledFeatures.sampleRateShading = SupportedFeatures.features.sampleRateShading;
    EnabledFeatures.samplerAnisotropy = SupportedFeatures.features.samplerAnisotropy;
    EnabledFeatures.alphaToOne = SupportedFeatures.features.alphaToOne;
    EnabledFeatures.independentBlend = SupportedFeatures.features.independentBlend;
    EnabledFeatures.pipelineStatisticsQuery = SupportedFeatures.features.pipelineStatisticsQuery;

    // ---- 启用扩展(与之前检查对应) ----
    std::vector<const char*> EnabledExtensions;
    EnabledExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (HdrMetadataExtensionAvailable)
        EnabledExtensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    if (EnabledMeshShader.meshShader)
        EnabledExtensions.push_back(VK_EXT_MESH_SHADER_EXTENSION_NAME);
    if (EnableDynamicState3Extension)
        EnabledExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (EnabledVertexInput.vertexInputDynamicState)
        EnabledExtensions.push_back(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    if (EnableAccelerationStructure)
    {
        EnabledExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        EnabledExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
        if (EnableRayTracingPipeline)
            EnabledExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        if (EnableRayQuery)
            EnabledExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
    }

    vk::DeviceCreateInfo DeviceCreationInfo(
        vk::DeviceCreateFlags(),
        static_cast<uint32_t>(QueueCreateInfos.size()),
        QueueCreateInfos.data(),
        0,
        nullptr,
        static_cast<uint32_t>(EnabledExtensions.size()),
        EnabledExtensions.empty() ? nullptr : EnabledExtensions.data(),
        &EnabledFeatures
    );
    DeviceCreationInfo.pNext = &EnabledVulkan13;

    Context->Device = Context->PhysicalDevice.createDeviceUnique(DeviceCreationInfo);

    auto& Features = Context->EnabledFeatures;
    Features.DynamicRendering = true;
    Features.Synchronization2 = true;
    Features.TimelineSemaphore = true;
    Features.Multiview = EnabledMultiview.multiview == VK_TRUE;
    Features.SeparateDepthStencilLayouts = EnabledVulkan12.separateDepthStencilLayouts == VK_TRUE;
    Features.GeometryShader = EnabledFeatures.geometryShader == VK_TRUE;
    Features.TessellationShader = EnabledFeatures.tessellationShader == VK_TRUE;
    Features.FillModeNonSolid = EnabledFeatures.fillModeNonSolid == VK_TRUE;
    Features.WideLines = EnabledFeatures.wideLines == VK_TRUE;
    Features.DepthClamp = EnabledFeatures.depthClamp == VK_TRUE;
    Features.DepthBounds = EnabledFeatures.depthBounds == VK_TRUE;
    Features.SampleRateShading = EnabledFeatures.sampleRateShading == VK_TRUE;
    Features.SamplerAnisotropy = EnabledFeatures.samplerAnisotropy == VK_TRUE;
    Features.AlphaToOne = EnabledFeatures.alphaToOne == VK_TRUE;
    Features.IndependentBlend = EnabledFeatures.independentBlend == VK_TRUE;
    Features.ExtendedDynamicState = EnabledDynamicState.extendedDynamicState == VK_TRUE;
    Features.ExtendedDynamicState2 = EnabledDynamicState2.extendedDynamicState2 == VK_TRUE;
    // Do not overclaim EDS3: every state in the backend's aggregate subset must be enabled.
    Features.ExtendedDynamicState3 =
        EnabledDynamicState3.extendedDynamicState3TessellationDomainOrigin &&
        EnabledDynamicState3.extendedDynamicState3DepthClampEnable &&
        EnabledDynamicState3.extendedDynamicState3PolygonMode &&
        EnabledDynamicState3.extendedDynamicState3RasterizationSamples &&
        EnabledDynamicState3.extendedDynamicState3SampleMask &&
        EnabledDynamicState3.extendedDynamicState3AlphaToCoverageEnable &&
        EnabledDynamicState3.extendedDynamicState3AlphaToOneEnable &&
        EnabledDynamicState3.extendedDynamicState3LogicOpEnable &&
        EnabledDynamicState3.extendedDynamicState3ColorBlendEnable &&
        EnabledDynamicState3.extendedDynamicState3ColorBlendEquation &&
        EnabledDynamicState3.extendedDynamicState3ColorWriteMask;
    Features.DynamicVertexInput = EnabledVertexInput.vertexInputDynamicState == VK_TRUE;
    Features.DescriptorIndexing = EnabledVulkan12.descriptorIndexing == VK_TRUE;
    Features.RuntimeDescriptorArray = EnabledVulkan12.runtimeDescriptorArray == VK_TRUE;
    Features.PartiallyBoundDescriptors = EnabledVulkan12.descriptorBindingPartiallyBound == VK_TRUE;
    Features.VariableDescriptorCount = EnabledVulkan12.descriptorBindingVariableDescriptorCount == VK_TRUE;
    Features.UpdateAfterBind =
        EnabledVulkan12.descriptorBindingUniformBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingSampledImageUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageImageUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingUniformTexelBufferUpdateAfterBind &&
        EnabledVulkan12.descriptorBindingStorageTexelBufferUpdateAfterBind;
    Features.MeshShader = EnabledMeshShader.meshShader == VK_TRUE;
    Features.TaskShader = Features.MeshShader && EnabledMeshShader.taskShader == VK_TRUE;
    Features.OcclusionQueries = true;
    Features.PipelineStatisticsQueries = EnabledFeatures.pipelineStatisticsQuery == VK_TRUE;
    Features.QueryResultCopy = true;
    Features.BufferDeviceAddress = EnabledVulkan12.bufferDeviceAddress == VK_TRUE;
    Features.AccelerationStructure = EnableAccelerationStructure;
    Features.RayTracingPipeline = EnableRayTracingPipeline;
    Features.RayQuery = EnableRayQuery;
    Features.SecondaryCommandLists = true;
    Features.SwapchainStatus = true;
    Features.HDRMetadata = HdrMetadataExtensionAvailable;
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