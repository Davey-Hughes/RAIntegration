#ifndef _WIN32

#include "DisplayProbe.hh"

#include <cstdlib>
#include <cstring>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace ra {
namespace services {
namespace impl {

bool CanConnectUnixSocket(const std::string& sPath)
{
    sockaddr_un oAddress{};
    if (sPath.empty() || sPath.size() >= sizeof(oAddress.sun_path))
        return false;

    const int nSocket = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (nSocket < 0)
        return false;

    oAddress.sun_family = AF_UNIX;
    std::memcpy(oAddress.sun_path, sPath.c_str(), sPath.size() + 1);
    const bool bConnected = (::connect(nSocket, reinterpret_cast<const sockaddr*>(&oAddress), sizeof(oAddress)) == 0);
    ::close(nSocket);
    return bConnected;
}

static bool IsSet(const char* sValue) noexcept { return sValue != nullptr && *sValue != '\0'; }

DisplayProbeResult ProbeDisplay(const std::function<const char*(const char*)>& fGetEnv,
                                const std::function<bool(const std::string&)>& fCanConnect)
{
    DisplayProbeResult oResult;

    const char* sPlatform = fGetEnv("QT_QPA_PLATFORM");
    if (IsSet(sPlatform))
    {
        oResult.bUsable = true;
        oResult.sDetail = std::string("QT_QPA_PLATFORM=") + sPlatform;
        return oResult;
    }

    std::string sTried;

    const char* sWayland = fGetEnv("WAYLAND_DISPLAY");
    if (IsSet(sWayland))
    {
        std::string sSocket;
        if (sWayland[0] == '/')
        {
            sSocket = sWayland;
        }
        else
        {
            const char* sRuntimeDirectory = fGetEnv("XDG_RUNTIME_DIR");
            if (IsSet(sRuntimeDirectory))
                sSocket = std::string(sRuntimeDirectory) + "/" + sWayland;
        }

        if (!sSocket.empty() && fCanConnect(sSocket))
        {
            oResult.bUsable = true;
            oResult.sDetail = "Wayland socket " + sSocket;
            return oResult;
        }

        sTried = sSocket.empty() ? std::string("WAYLAND_DISPLAY=") + sWayland + " without XDG_RUNTIME_DIR"
                                 : "Wayland socket " + sSocket + " refused";
    }

    const char* sDisplay = fGetEnv("DISPLAY");
    if (IsSet(sDisplay))
    {
        // ":N[.S]" and "unix:N[.S]" are local and have a socket to try
        std::string sLocal;
        if (sDisplay[0] == ':')
        {
            sLocal = sDisplay + 1;
        }
        else if (std::strncmp(sDisplay, "unix:", 5) == 0)
        {
            sLocal = sDisplay + 5;
        }
        else
        {
            oResult.bUsable = true;
            oResult.sDetail = std::string("DISPLAY=") + sDisplay + " (remote, not probed)";
            return oResult;
        }

        const std::string sNumber = sLocal.substr(0, sLocal.find('.'));
        const std::string sSocket = "/tmp/.X11-unix/X" + sNumber;
        if (!sNumber.empty() && fCanConnect(sSocket))
        {
            oResult.bUsable = true;
            oResult.sDetail = "X11 socket " + sSocket;
            return oResult;
        }

        if (!sTried.empty())
            sTried += "; ";
        sTried += "X11 socket " + sSocket + " refused";
    }

    oResult.sDetail = sTried.empty() ? "neither WAYLAND_DISPLAY nor DISPLAY is set" : sTried;
    return oResult;
}

DisplayProbeResult ProbeDisplay()
{
    return ProbeDisplay([](const char* sName) -> const char* { return std::getenv(sName); }, &CanConnectUnixSocket);
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
