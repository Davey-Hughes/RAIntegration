// A consumer of libRA_Integration.so that goes through RAInterface's POSIX
// loader - RA_Interface_posix.cpp is compiled into it - and calls nothing but
// the public RA_* API, the way RALibretro does. It is the loader's gate, and
// the acceptance test for the path neither ra_dlopen_smoke nor ra_linux_smoke
// reaches: a real consumer, online, with a game loaded at shutdown.
//
//   ra_loader_smoke <mode> [<inflight-delay-ms>]
//
//   missing   no libRA_Integration.so: every RA_* call does nothing, safely
//   offline   host.txt says OFFLINE: init, shut down, nothing left running
//
// One mode per process. The loader dlcloses the library in RA_Shutdown, and a
// second init in the same process would only start clean if dlclose had
// unmapped it, which nothing guarantees (a GCC build likely stays mapped).
//
// Run it through checks/loader-smoke.sh, which gives each run an empty
// directory: a COPY of this binary (a symlink would make /proc/self/exe, and
// so the library's base directory, resolve back to the build directory), the
// library linked in beside it, and host.txt or a prefs file. The loader's
// RA_Interface: lines go to stderr and that script judges them, along with
// the exit status, which this program cannot report: static destructors run
// after main returns. The library's own log lines are read here, from
// RACache/RALog.txt, never from stderr.
//
// No QGuiApplication, because RALibretro has none.
//
// Result markers, as in ra_linux_smoke:
//   [ok]   asserted, and true
//   [FAIL] asserted, and false - the process exits non-zero
//   [obs]  observed and reported, but NOT asserted

#include "RA_Interface.h"

#include "SmokeReport.hh" // Check, Observe, Finish, CountThreads - shared with the other smoke programs

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

namespace {

constexpr const char* CLIENT_NAME = "RALoaderSmoke";
constexpr const char* CLIENT_VERSION = "0.1";

// The directory of this binary, which is also the library's base directory:
// RACache/ and the prefs file live here.
std::filesystem::path g_oBaseDirectory;

// The library writes RALog.txt from its own threads and flushes every line.
std::string ReadLog()
{
    std::ifstream oFile(g_oBaseDirectory / "RACache" / "RALog.txt", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(oFile), std::istreambuf_iterator<char>());
}

bool LogContains(const std::string& sText)
{
    return ReadLog().find(sText) != std::string::npos;
}

std::string InLog(const std::string& sText)
{
    return LogContains(sText) ? ("RALog.txt: \"" + sText + "\"") : ("no \"" + sText + "\" in RALog.txt");
}

int RunMissing()
{
    const size_t nThreadsBefore = CountThreads();
    // CountThreads() returns 0 if /proc cannot be read, and every thread
    // comparison below would then hold trivially.
    Check(nThreadsBefore >= 1, "/proc/self/task readable", std::to_string(nThreadsBefore) + " thread(s) before init");

    // The loader finds no library, says so on stderr (the gate checks that
    // line), and leaves every entry point NULL. Each call below must then take
    // its wrapper's fallback without crashing.
    RA_InitClient(nullptr, CLIENT_NAME, CLIENT_VERSION);
    RA_AttemptLogin(1);
    RA_ActivateGame(1);
    RA_DoAchievementsFrame();

    const char* sUser = RA_UserName();
    Check(sUser != nullptr && *sUser == '\0', "RA_UserName() is empty",
          sUser ? ("\"" + std::string(sUser) + "\"") : "(null)");

    const int nHardcore = RA_HardcoreModeIsActive();
    Check(nHardcore == 0, "RA_HardcoreModeIsActive() is 0", "returned " + std::to_string(nHardcore));

    // 0 would mean "the user cancelled", and RALibretro would refuse to quit:
    // its early Linux stub wedged shutdown exactly that way.
    const int nConfirm = RA_ConfirmLoadNewRom(1);
    Check(nConfirm == 1, "RA_ConfirmLoadNewRom() allows quitting", "returned " + std::to_string(nConfirm));

    RA_Shutdown();

    const size_t nThreadsAfter = CountThreads();
    Check(nThreadsAfter == nThreadsBefore, "no thread started",
          std::to_string(nThreadsBefore) + " before, " + std::to_string(nThreadsAfter) + " after");

    // No library code ran, so nothing may have been written beside this binary.
    std::error_code oError;
    const bool bCache = std::filesystem::exists(g_oBaseDirectory / "RACache", oError);
    Check(!bCache && !oError, "no RACache/ created",
          bCache ? "RACache/ exists" : (oError ? oError.message() : "absent"));

    return Finish("ra_loader_smoke");
}

int RunOffline()
{
    const size_t nThreadsBefore = CountThreads();
    Check(nThreadsBefore >= 1, "/proc/self/task readable", std::to_string(nThreadsBefore) + " thread(s) before init");

    // host.txt says OFFLINE, so the loader must choose _RA_InitClientOffline.
    // It learns that from _RA_HostUrl(), called before any init - which on
    // Linux needs the library to register its core services on first use, as
    // DllMain does at load time on Windows.
    RA_InitClient(nullptr, CLIENT_NAME, CLIENT_VERSION);

    // Only the library starts threads, so this is also the proof that it was
    // found and loaded at all.
    const size_t nThreadsAfterInit = CountThreads();
    Check(nThreadsAfterInit > nThreadsBefore, "init started threads",
          std::to_string(nThreadsBefore) + " before init, " + std::to_string(nThreadsAfterInit) + " after");

    Check(LogContains("Initializing offline mode"), "offline entry point chosen", InLog("Initializing offline mode"));

    RA_Shutdown();

    const size_t nThreadsAfter = CountThreads();
    Check(nThreadsAfter == nThreadsBefore, "no thread outlives RA_Shutdown()",
          std::to_string(nThreadsBefore) + " before init, " + std::to_string(nThreadsAfter) + " after shutdown");

    Check(LogContains("Shutdown complete"), "library shut down", InLog("Shutdown complete"));

    return Finish("ra_loader_smoke");
}

} // namespace

int main(int argc, char* argv[])
{
    std::error_code oError;
    g_oBaseDirectory = std::filesystem::read_symlink("/proc/self/exe", oError).parent_path();
    if (oError)
    {
        std::printf("ra_loader_smoke: cannot read /proc/self/exe: %s\n", oError.message().c_str());
        return 1;
    }

    const std::string sMode = (argc > 1) ? argv[1] : "";
    std::printf("ra_loader_smoke %s\n  directory: %s\n", sMode.c_str(), g_oBaseDirectory.c_str());
    std::fflush(stdout);

    if (sMode == "missing")
        return RunMissing();
    if (sMode == "offline")
        return RunOffline();

    std::printf("usage: ra_loader_smoke missing|offline\n");
    return 2;
}
