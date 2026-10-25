#include <RHIServer.hpp>

#include <stdexcept>
#include <vk/VulkanRHI.hpp>

namespace rhi
{

RHIServer::RHIServer() = default;

RHIServer::~RHIServer()
{
    shutdown();
}

void RHIServer::initialize(ESupportedBackendAPI BackendAPI, const runtime::platform::GenericWindowPointer& Window)
{
    if (!Window)
    {
        throw std::invalid_argument("RHI initialization requires a valid generic window.");
    }

    shutdown();
    switch (BackendAPI)
    {
    case ESupportedBackendAPI::Vulkan:
        RHIInstance = std::make_unique<VulkanRHI>();
        break;
    default:
        throw std::runtime_error("The requested RHI backend is not supported.");
    }

    RHIInstance->initialize(Window);
    DeviceInstance = RHIInstance->createDevice();
    if (!DeviceInstance)
    {
        throw std::runtime_error("The selected RHI backend did not create a device.");
    }
}

void RHIServer::shutdown()
{
    DeviceInstance.reset();
    RHIInstance.reset();
}

bool RHIServer::isInitialized() const noexcept
{
    return RHIInstance && RHIInstance->isInitialized();
}

} // namespace rhi