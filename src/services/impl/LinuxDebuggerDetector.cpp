#include "LinuxDebuggerDetector.hh"

#include "util/Log.hh"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ra {
namespace services {
namespace impl {

namespace detail {

// A probe that cannot run fails open: it answers "no debugger", which is what
// the detector reported before it logged anything. The warning is the only way
// to tell the two apart in a player's log. Only the first one is written -
// EmulatorMemoryContext::IsMemoryInsecure asks again every 10 seconds, and a
// probe that could not open /proc/self/status is not going to start being able to.
static void LogProbeFailure(const char* sReason, int nError)
{
    static std::atomic<bool> s_bLogged{false};
    if (s_bLogged.exchange(true))
        return;

    if (nError != 0)
    {
        RA_LOG_WARN("Debugger check unavailable, treating as no debugger: %s: %s (%d)", sReason,
                    std::strerror(nError), nError);
    }
    else
    {
        RA_LOG_WARN("Debugger check unavailable, treating as no debugger: %s", sReason);
    }
}

bool IsTracerAttached()
{
    // The Windows implementation also probes for a remote debugger; there is no
    // equivalent here. A non-zero TracerPid means something is ptrace-attached,
    // which covers gdb, lldb and strace alike.
    std::FILE* pFile = std::fopen("/proc/self/status", "r");
    if (pFile == nullptr)
    {
        // captured before anything else can overwrite it (see LinuxFileSystem::GetFileSize)
        const int nError = errno;
        LogProbeFailure("cannot open /proc/self/status", nError);
        return false;
    }

    bool bFound = false;
    bool bResult = false;
    char sLine[256];
    while (std::fgets(sLine, sizeof(sLine), pFile) != nullptr)
    {
        if (std::strncmp(sLine, "TracerPid:", 10) == 0)
        {
            bFound = true;
            bResult = (std::strtol(sLine + 10, nullptr, 10) != 0);
            break;
        }
    }

    // fgets returns nullptr for a read error as well as at the end of the file
    const bool bReadError = (std::ferror(pFile) != 0);
    const int nError = bReadError ? errno : 0;
    std::fclose(pFile);

    if (!bFound)
        LogProbeFailure(bReadError ? "cannot read /proc/self/status" : "no TracerPid in /proc/self/status", nError);

    return bResult;
}

} // namespace detail

bool LinuxDebuggerDetector::IsDebuggerPresent() const
{
#ifdef NDEBUG // allow debugger-limited functionality when using a DEBUG build
    if (detail::IsTracerAttached())
    {
        RA_LOG_WARN("Debugger detected");
        return true;
    }
#endif

    // The Windows implementation also looks for Cheat Engine by process name,
    // outside the NDEBUG check; there is no equivalent here.
    return false;
}

} // namespace impl
} // namespace services
} // namespace ra
