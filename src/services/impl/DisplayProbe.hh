#ifndef RA_SERVICES_DISPLAYPROBE_HH
#define RA_SERVICES_DISPLAYPROBE_HH
#pragma once

#ifndef _WIN32

#include <functional>
#include <string>

namespace ra {
namespace services {
namespace impl {

struct DisplayProbeResult
{
    bool bUsable = false;
    std::string sDetail; // what was found or tried, for the log
};

// Decides whether a Qt GUI application can be created here. Qt answers "no" by
// calling qFatal, which aborts the whole process - and a library must never
// abort the emulator that loaded it - so the question is asked first.
//
// An explicit QT_QPA_PLATFORM is trusted as given, "offscreen" included.
// Otherwise a Wayland or local X11 display must accept a connection on its
// socket; a remote X11 display (host:N) is trusted, as it cannot be probed
// without speaking X. fGetEnv returns nullptr for an unset variable, and
// fCanConnect reports whether a unix-domain socket accepts a connection: both
// are parameters so the decision can be tested.
DisplayProbeResult ProbeDisplay(const std::function<const char*(const char*)>& fGetEnv,
                                const std::function<bool(const std::string&)>& fCanConnect);

// The same, against the process environment and the real sockets.
DisplayProbeResult ProbeDisplay();

bool CanConnectUnixSocket(const std::string& sPath);

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_DISPLAYPROBE_HH
