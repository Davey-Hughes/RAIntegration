#ifndef RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
#define RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
#pragma once

#include "services/IDebuggerDetector.hh"

namespace ra {
namespace services {
namespace impl {

class LinuxDebuggerDetector : public IDebuggerDetector
{
public:
    bool IsDebuggerPresent() const override;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_LINUX_DEBUGGERDETECTOR_HH
