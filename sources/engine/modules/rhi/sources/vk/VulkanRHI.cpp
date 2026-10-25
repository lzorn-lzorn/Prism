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
        // 1.3 是 sync2(vkCmdPipelineBarrier2/vkQueueSubmit2)与动态渲染的最低核心版本;
        // 特性仍按"核心版本或扩展"逐项协商, 实例版本 ≥1.3 才保证这些核心命令可用.
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

    // 除特性协商(由 VulkanFeatureSet 负责)外, 设备还需具备交换链扩展.
    std::vector<const char*> RequiredDeviceExtensions =
    {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };

    vk::PhysicalDevice BestDevice = nullptr;
    int BestScore = -1;

    for (const auto& Device : PhysicalDevices) 
    {
        // ---- 检查设备扩展 ----
        if (!checkDeviceExtensions(Device, RequiredDeviceExtensions)) 
        {
            std::println("Device {} does not support required extensions, skipping.", 
                Device.getProperties().deviceName);
            continue;
        }

        // ---- 检查队列族(至少需要图形和呈现) ----
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
        // 更高核心版本意味着更少依赖扩展, 给予稳定加分.
        const uint32_t DeviceApiVersion = Device.getProperties().apiVersion;
        if (DeviceApiVersion >= VK_API_VERSION_1_3) Score += 300;
        else if (DeviceApiVersion >= VK_API_VERSION_1_2) Score += 150;
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

    // ---- 特性协商: 以 1.0 为基线, 按"核心版本或扩展"探测并启用; 增强特性缺失时回退 1.0 基线 ----
    Context->FeatureSet.detect(Context->PhysicalDevice);

    const std::vector<const char*> EnabledExtensions = Context->FeatureSet.buildEnabledExtensions();
    const vk::PhysicalDeviceFeatures EnabledCoreFeatures = Context->FeatureSet.buildCoreFeatures();
    const void* FeatureChainHead = Context->FeatureSet.buildFeatureChainHead();

    vk::DeviceCreateInfo DeviceCreationInfo(
        vk::DeviceCreateFlags(),
        static_cast<uint32_t>(QueueCreateInfos.size()),
        QueueCreateInfos.data(),
        0,
        nullptr,
        static_cast<uint32_t>(EnabledExtensions.size()),
        EnabledExtensions.empty() ? nullptr : EnabledExtensions.data(),
        &EnabledCoreFeatures
    );
    DeviceCreationInfo.pNext = const_cast<void*>(FeatureChainHead);

    Context->Device = Context->PhysicalDevice.createDeviceUnique(DeviceCreationInfo);
    Context->FeatureSet.writeBackTo(Context->EnabledFeatures);
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