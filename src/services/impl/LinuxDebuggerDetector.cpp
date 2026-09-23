#include "LinuxDebuggerDetector.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ra {
namespace services {
namespace impl {

bool LinuxDebuggerDetector::IsDebuggerPresent() const
{
    // The Windows implementation also probes for a remote debugger; there is no
    // equivalent here. A non-zero TracerPid means something is ptrace-attached,
    // which covers gdb, lldb and strace alike.
    std::FILE* pFile = std::fopen("/proc/self/status", "r");
    if (pFile == nullptr)
        return false;

    bool bResult = false;
    char sLine[256];
    while (std::fgets(sLine, sizeof(sLine), pFile) != nullptr)
    {
        if (std::strncmp(sLine, "TracerPid:", 10) == 0)
        {
            bResult = (std::strtol(sLine + 10, nullptr, 10) != 0);
            break;
        }
    }

    std::fclose(pFile);
    return bResult;
}

} // namespace impl
} // namespace services
} // namespace ra
