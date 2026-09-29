#ifndef RA_SERVICES_INITIALIZATION_HH
#define RA_SERVICES_INITIALIZATION_HH
#pragma once

#include "RAInterface/RA_Emulators.h"

#include <atomic>

namespace ra {
namespace services {

class Initialization
{
public:
    static void RegisterCoreServices();

    static void RegisterServices(EmulatorID nEmulatorId, const char* sClientName);

    static void Shutdown();

    static bool IsInitialized() noexcept { return s_bIsInitialized; }

    static bool IsShuttingDown() noexcept { return s_bIsShuttingDown.load(); }

    static void StartShutdown() noexcept { s_bIsShuttingDown.store(true); }

private:
    static void InitializeNotifyTargets();

    static bool s_bIsInitialized;
    static std::atomic<bool> s_bIsShuttingDown; // polled from pool workers (QtDesktop's borrowed modal wait)
};

} // namespace services
} // namespace ra

#endif // !RA_SERVICES_INITIALIZATION_HH
