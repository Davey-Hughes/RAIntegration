// A consumer of libRA_Integration.so that links none of its code. It reaches
// the library only through dlopen and dlsym, the way a Linux RA_Interface
// loader will, so it sees what a loader sees: the dynamic symbol table, the
// RTLD_NOW binding of every undefined symbol, and whatever the library leaves
// running when it is closed.
//
//   ./build-tests/ra_dlopen_smoke [<path to libRA_Integration.so>]
//
// With no argument it loads libRA_Integration.so from beside the executable.
// It creates no QGuiApplication, because RALibretro has none, and needs no
// desktop session and no network: init is offline and no game is loaded. The
// library resolves relative paths against the host executable, as on Windows,
// so RAPrefs_RADlopenSmoke.cfg and RACache/ are written beside this binary.
//
// The exit status is part of the result, and this program cannot report it:
// static destructors - and anything the library left behind - run after main
// returns. checks/so-smoke.sh asserts it.
//
// Result markers, as in ra_linux_smoke:
//   [ok]   asserted, and true
//   [FAIL] asserted, and false - the process exits non-zero
//   [obs]  observed and reported, but NOT asserted

// Declarations only, for decltype. Nothing linked into this program defines
// an _RA_* symbol, so calling one directly would fail to link.
#include "Exports.hh"

#include "SmokeReport.hh" // Check, Observe, Finish, CountThreads - shared with the other smoke programs

#include <dlfcn.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

// dlerror() returns NULL when there is nothing to report, and a std::string
// cannot be built from NULL.
static std::string LastDlError()
{
    const char* sError = dlerror();
    return sError ? sError : "(no dlerror)";
}

template<typename TFunction>
static TFunction Resolve(void* hLibrary, const char* sName)
{
    dlerror(); // clear any earlier error, so the one read below is dlsym's
    void* pSymbol = dlsym(hLibrary, sName);
    const std::string sLabel = std::string("dlsym ") + sName;
    Check(pSymbol != nullptr, sLabel.c_str(), pSymbol ? "resolved" : LastDlError());
    return reinterpret_cast<TFunction>(pSymbol);
}

int main(int argc, char* argv[])
{
    std::error_code oError;
    std::filesystem::path oPath;
    if (argc > 1)
        oPath = std::filesystem::absolute(argv[1], oError);
    else
        oPath = std::filesystem::read_symlink("/proc/self/exe", oError).parent_path() / "libRA_Integration.so";

    if (oError)
    {
        std::printf("ra_dlopen_smoke: cannot resolve the library path: %s\n", oError.message().c_str());
        return 1;
    }

    // Absolute, so dlopen searches nothing.
    const std::string sPath = oPath.string();
    std::printf("ra_dlopen_smoke\n  library: %s\n", sPath.c_str());
    std::fflush(stdout);

    const size_t nThreadsBeforeLoad = CountThreads();

    // CountThreads() reads /proc. If that fails it returns 0, and every thread
    // comparison below would hold trivially.
    Check(nThreadsBeforeLoad >= 1, "/proc/self/task readable",
          std::to_string(nThreadsBeforeLoad) + " thread(s) before dlopen");

    void* hLibrary = dlopen(sPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    Check(hLibrary != nullptr, "dlopen(RTLD_NOW | RTLD_LOCAL)", hLibrary ? "loaded" : LastDlError());
    if (!hLibrary)
        return Finish("ra_dlopen_smoke");

    // Resolve everything before calling anything, so a missing export stops
    // the run before the library has started a single thread.
    const auto pIntegrationVersion = Resolve<decltype(&_RA_IntegrationVersion)>(hLibrary, "_RA_IntegrationVersion");
    const auto pInitClientOffline = Resolve<decltype(&_RA_InitClientOffline)>(hLibrary, "_RA_InitClientOffline");
    const auto pShutdown = Resolve<decltype(&_RA_Shutdown)>(hLibrary, "_RA_Shutdown");
    if (!pIntegrationVersion || !pInitClientOffline || !pShutdown)
    {
        dlclose(hLibrary);
        return Finish("ra_dlopen_smoke");
    }

    const char* sVersion = pIntegrationVersion();
    Check(sVersion != nullptr && *sVersion != '\0', "_RA_IntegrationVersion()", sVersion ? sVersion : "(null)");

    const size_t nThreadsBeforeInit = CountThreads();

    // InitCommon returns 1, or 0 only when hardcore is on, a debugger is
    // attached, and the user declines to disable hardcore.
    const int nInit = pInitClientOffline(nullptr, "RADlopenSmoke", "0.1");
    Check(nInit == 1, "_RA_InitClientOffline()", "returned " + std::to_string(nInit));

    const size_t nThreadsAfterInit = CountThreads();
    Observe("threads", std::to_string(nThreadsBeforeLoad) + " before dlopen, " +
                           std::to_string(nThreadsBeforeInit) + " before init, " +
                           std::to_string(nThreadsAfterInit) + " after init");

    // Without threads to stop, the shutdown check below would pass without
    // testing anything.
    Check(nThreadsAfterInit > nThreadsBeforeInit, "init started threads",
          std::to_string(nThreadsBeforeInit) + " before init, " + std::to_string(nThreadsAfterInit) + " after init");

    // DoShutdown returns 0 on every path, so the value says nothing. What
    // matters to dlclose is that no code of the library is still running.
    pShutdown();

    const size_t nThreadsAfterShutdown = CountThreads();
    // Against the count before dlopen, not before init, so that a thread the
    // library's static initialisers started counts as well.
    Check(nThreadsAfterShutdown == nThreadsBeforeLoad, "no thread outlives _RA_Shutdown()",
          std::to_string(nThreadsBeforeLoad) + " before dlopen, " + std::to_string(nThreadsAfterShutdown) +
              " after shutdown");

    const int nClose = dlclose(hLibrary);
    Check(nClose == 0, "dlclose()", nClose == 0 ? "returned 0" : LastDlError());

    // RTLD_NOLOAD hands back the library only if it is still mapped - marked
    // NODELETE, defining a GNU-unique symbol, or referenced from elsewhere.
    // Either answer is safe once _RA_Shutdown has run, so this is reported,
    // not asserted.
    void* hStillLoaded = dlopen(sPath.c_str(), RTLD_NOW | RTLD_NOLOAD);
    if (hStillLoaded)
    {
        Observe("library unmapped by dlclose", "no - still loaded");
        dlclose(hStillLoaded);
    }
    else
    {
        Observe("library unmapped by dlclose", "yes");
    }

    return Finish("ra_dlopen_smoke");
}
