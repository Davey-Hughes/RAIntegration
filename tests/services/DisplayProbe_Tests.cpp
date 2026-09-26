#ifndef _WIN32

#include "services/impl/DisplayProbe.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(DisplayProbe_Tests)
{
private:
    // An environment and a set of listening sockets, both invented: the probe
    // only ever sees them through the two functions it is given.
    struct FakeEnvironment
    {
        std::map<std::string, std::string> mVariables;
        std::set<std::string> vListening;
        mutable std::vector<std::string> vTried;

        DisplayProbeResult Probe() const
        {
            return ProbeDisplay(
                [this](const char* sName) -> const char* {
                    const auto pIter = mVariables.find(sName);
                    return (pIter == mVariables.end()) ? nullptr : pIter->second.c_str();
                },
                [this](const std::string& sPath) {
                    vTried.push_back(sPath);
                    return vListening.count(sPath) != 0;
                });
        }
    };

    static bool Contains(const std::string& sText, const std::string& sPart) { return sText.find(sPart) != std::string::npos; }

public:
    TEST_METHOD(TestNothingSetIsUnusable)
    {
        FakeEnvironment oEnvironment;
        const auto oResult = oEnvironment.Probe();
        Assert::IsFalse(oResult.bUsable);
        Assert::IsTrue(Contains(oResult.sDetail, "neither WAYLAND_DISPLAY nor DISPLAY"));
    }

    TEST_METHOD(TestExplicitPlatformIsTrustedWithoutProbing)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["QT_QPA_PLATFORM"] = "offscreen";
        const auto oResult = oEnvironment.Probe();
        Assert::IsTrue(oResult.bUsable);
        Assert::IsTrue(Contains(oResult.sDetail, "QT_QPA_PLATFORM=offscreen"));
        Assert::AreEqual(size_t(0), oEnvironment.vTried.size());
    }

    TEST_METHOD(TestListeningWaylandSocket)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["WAYLAND_DISPLAY"] = "wayland-0";
        oEnvironment.mVariables["XDG_RUNTIME_DIR"] = "/run/user/1000";
        oEnvironment.vListening.insert("/run/user/1000/wayland-0");
        const auto oResult = oEnvironment.Probe();
        Assert::IsTrue(oResult.bUsable);
        Assert::IsTrue(Contains(oResult.sDetail, "Wayland socket /run/user/1000/wayland-0"));
    }

    TEST_METHOD(TestAbsoluteWaylandDisplayIsItsOwnPath)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["WAYLAND_DISPLAY"] = "/tmp/compositor.sock";
        oEnvironment.vListening.insert("/tmp/compositor.sock");
        Assert::IsTrue(oEnvironment.Probe().bUsable);
    }

    TEST_METHOD(TestRefusedWaylandFallsBackToX11)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["WAYLAND_DISPLAY"] = "wayland-0";
        oEnvironment.mVariables["XDG_RUNTIME_DIR"] = "/run/user/1000";
        oEnvironment.mVariables["DISPLAY"] = ":0";
        oEnvironment.vListening.insert("/tmp/.X11-unix/X0");
        const auto oResult = oEnvironment.Probe();
        Assert::IsTrue(oResult.bUsable);
        Assert::IsTrue(Contains(oResult.sDetail, "X11 socket /tmp/.X11-unix/X0"));
    }

    TEST_METHOD(TestDisplayScreenNumberIsIgnored)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["DISPLAY"] = ":1.0";
        oEnvironment.vListening.insert("/tmp/.X11-unix/X1");
        Assert::IsTrue(oEnvironment.Probe().bUsable);
    }

    TEST_METHOD(TestUnixPrefixedDisplayIsLocal)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["DISPLAY"] = "unix:2";
        oEnvironment.vListening.insert("/tmp/.X11-unix/X2");
        Assert::IsTrue(oEnvironment.Probe().bUsable);
    }

    TEST_METHOD(TestRemoteDisplayIsTrustedWithoutProbing)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["DISPLAY"] = "buildhost:0";
        const auto oResult = oEnvironment.Probe();
        Assert::IsTrue(oResult.bUsable);
        Assert::AreEqual(size_t(0), oEnvironment.vTried.size());
    }

    TEST_METHOD(TestBothDisplaysDeadIsUnusableAndSaysWhy)
    {
        FakeEnvironment oEnvironment;
        oEnvironment.mVariables["WAYLAND_DISPLAY"] = "wayland-0";
        oEnvironment.mVariables["XDG_RUNTIME_DIR"] = "/run/user/1000";
        oEnvironment.mVariables["DISPLAY"] = ":0";
        const auto oResult = oEnvironment.Probe();
        Assert::IsFalse(oResult.bUsable);
        Assert::IsTrue(Contains(oResult.sDetail, "/run/user/1000/wayland-0"));
        Assert::IsTrue(Contains(oResult.sDetail, "/tmp/.X11-unix/X0"));
    }

    TEST_METHOD(TestRealSocket)
    {
        const auto oPath = std::filesystem::temp_directory_path() / ("ra-probe-" + std::to_string(::getpid()));
        const std::string sPath = oPath.string();
        std::filesystem::remove(oPath);
        Assert::IsFalse(CanConnectUnixSocket(sPath), L"connected to a socket that does not exist");

        const int nListener = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        Assert::IsTrue(nListener >= 0, L"socket failed");
        sockaddr_un oAddress{};
        oAddress.sun_family = AF_UNIX;
        std::snprintf(oAddress.sun_path, sizeof(oAddress.sun_path), "%s", sPath.c_str());
        Assert::AreEqual(0, ::bind(nListener, reinterpret_cast<const sockaddr*>(&oAddress), sizeof(oAddress)));
        Assert::AreEqual(0, ::listen(nListener, 1));

        const bool bConnected = CanConnectUnixSocket(sPath);
        ::close(nListener);
        std::filesystem::remove(oPath);
        Assert::IsTrue(bConnected, L"did not connect to a listening socket");
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
