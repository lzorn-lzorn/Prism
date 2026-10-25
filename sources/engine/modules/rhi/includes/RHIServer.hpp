#pragma once 

#include <RHI.hpp>
#include <memory>
namespace rhi
{

/** @brief Process-wide owner of the selected RHI backend and its primary logical device. */
class RHIServer
{
public:
    /** @brief Returns the process-wide RHI service. @return Stable singleton reference. */
    static RHIServer& self() 
    {
        static RHIServer Instance;
        return Instance;
    }
    RHIServer(const RHIServer&) = delete;
    RHIServer& operator=(const RHIServer&) = delete;
    RHIServer(RHIServer&&) = delete;
    RHIServer& operator=(RHIServer&&) = delete;

    /** @brief Creates and initializes the selected backend and primary device. @param BackendAPI Backend implementation to instantiate. @param Window Window used to create presentation surface state. */
    void initialize(ESupportedBackendAPI BackendAPI, const runtime::platform::GenericWindowPointer& Window);
    /** @brief Waits for the device, releases device state, and destroys the backend instance. */
    void shutdown();
    /** @brief Reports whether both backend and logical device are available. @return True after successful initialize() and before shutdown(). */
    [[nodiscard]] bool isInitialized() const noexcept;
    /** @brief Returns the active backend instance without transferring ownership. @return Backend pointer, or nullptr while uninitialized. */
    [[nodiscard]] IRHI* getRHI() const noexcept { return RHIInstance.get(); }
    /** @brief Returns shared ownership of the primary logical device. @return Current device; empty while uninitialized. */
    [[nodiscard]] const std::shared_ptr<RDevice>& getDevice() const noexcept { return DeviceInstance; }

private:
    RHIServer();
    ~RHIServer();

    std::unique_ptr<IRHI> RHIInstance;
    std::shared_ptr<RDevice> DeviceInstance;

};

using RenderServer = RHIServer;

}