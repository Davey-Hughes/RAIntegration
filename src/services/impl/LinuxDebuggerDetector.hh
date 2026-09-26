#ifndef RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
#define RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
#pragma once

#ifndef _WIN32

#include "services/IDebuggerDetector.hh"

namespace ra {
namespace services {
namespace impl {

namespace detail {

/// <summary>
/// Reads the TracerPid line of /proc/self/status.
/// </summary>
/// <returns><c>true</c> if something is ptrace-attached to this process.
/// <c>false</c> if nothing is - and also if the probe could not run, which is
/// logged as a warning the first time it happens.</returns>
/// <remarks>Exposed only so the probe can be covered in every build type.
/// LinuxDebuggerDetector::IsDebuggerPresent only calls it when NDEBUG is
/// defined, as the Windows detector does, and the test build does not define
/// NDEBUG.</remarks>
bool IsTracerAttached();

} // namespace detail

class LinuxDebuggerDetector : public IDebuggerDetector
{
public:
    bool IsDebuggerPresent() const override;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
