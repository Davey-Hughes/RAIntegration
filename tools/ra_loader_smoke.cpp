// A consumer of libRA_Integration.so that goes through RAInterface's POSIX
// loader - RA_Interface_posix.cpp is compiled into it - and calls nothing but
// the public RA_* API, the way RALibretro does. It is the loader's gate, and
// the acceptance test for the path neither ra_dlopen_smoke nor ra_linux_smoke
// reaches: a real consumer, online, with a game loaded at shutdown.
//
//   ra_loader_smoke <mode> [<inflight-delay-ms>]
//
//   missing          no libRA_Integration.so: every RA_* call does nothing, safely
//   offline          host.txt says OFFLINE: init, shut down, nothing left running
//   online-loaded    log in with a staged token, load a real game, shut down
//                    with it still loaded
//   online-inflight  as online-loaded, but shut down while the load is in flight
//
// <inflight-delay-ms> exists only for the negative control of
// online-inflight's precondition.
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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>

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

// Super Mario Bros. (NES): game 1446 on retroachievements.org, with 77
// published achievements. Checked 2026-09-24 with r=gameid and r=patch.
constexpr const char* KNOWN_HASH = "8e3630186e35d477231bf8fd50e54cdd";
constexpr unsigned int KNOWN_GAME_ID = 1446;
constexpr unsigned int CONSOLE_NES = 7;

// A 2 KiB bank, the size of NES work RAM, that reads as zeros and ignores
// writes.
unsigned char ReadZero(unsigned int) { return 0; }
void IgnoreWrite(unsigned int, unsigned char) {}

bool WaitForLog(const std::string& sText, int nTimeoutSeconds)
{
    const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(nTimeoutSeconds);
    while (!LogContains(sText))
    {
        if (std::chrono::steady_clock::now() >= tDeadline)
            return false;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    return true;
}

// online-loaded shuts down with a game loaded: the path that runs
// SaveKnownHashes, EndSession and the session history, which no other test
// reaches. online-inflight calls RA_Shutdown straight after RA_ActivateGame,
// so rc_client is destroyed while the load's requests are still in flight.
//
// Neither calls RA_DoAchievementsFrame. The account is real, and zeroed
// memory could satisfy a real achievement's trigger.
int RunOnline(bool bInFlight, int nDelayMs)
{
    const size_t nThreadsBefore = CountThreads();
    Check(nThreadsBefore >= 1, "/proc/self/task readable", std::to_string(nThreadsBefore) + " thread(s) before init");

    RA_InitClient(nullptr, CLIENT_NAME, CLIENT_VERSION);

    const size_t nThreadsAfterInit = CountThreads();
    Check(nThreadsAfterInit > nThreadsBefore, "init started threads",
          std::to_string(nThreadsBefore) + " before init, " + std::to_string(nThreadsAfterInit) + " after");

    // Blocking: it returns once the server has answered. RA_UserName() is the
    // account's DISPLAY name (Exports.cpp), which need not match the username
    // the prefs were staged with, so only "someone is logged in" is asserted.
    RA_AttemptLogin(1);
    const std::string sLoggedIn = RA_UserName();
    Check(!sLoggedIn.empty(), "logged in with the staged token", "RA_UserName() is \"" + sLoggedIn + "\"");

    // The login handler plays login.wav. With no QGuiApplication the Qt audio
    // service must refuse, say so, and leave the process running.
    Check(LogContains("PlayAudioFile ignored: no QGuiApplication"), "Qt service without QGuiApplication",
          InLog("PlayAudioFile ignored: no QGuiApplication"));

    RA_SetConsoleID(CONSOLE_NES);
    RA_InstallMemoryBank(0, ReadZero, IgnoreWrite, 0x800);

    const unsigned int nGameId = RA_IdentifyHash(KNOWN_HASH);
    Check(nGameId == KNOWN_GAME_ID, "RA_IdentifyHash()",
          "returned " + std::to_string(nGameId) + ", expected " + std::to_string(KNOWN_GAME_ID));
    if (nGameId != KNOWN_GAME_ID)
    {
        RA_Shutdown();
        return Finish("ra_loader_smoke");
    }

    const auto tActivated = std::chrono::steady_clock::now();
    RA_ActivateGame(nGameId);

    // rc_client's own completion line, "Game %u loaded, hardcore %s%s".
    // "Starting new session" cannot serve: it is logged as the load begins.
    const std::string sLoaded = "Game " + std::to_string(KNOWN_GAME_ID) + " loaded";
    if (!bInFlight)
    {
        // "disabled" is also the proof that the session is softcore.
        const bool bLoaded = WaitForLog(sLoaded + ", hardcore disabled", 30);
        const auto nMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - tActivated).count();
        Check(bLoaded, "game loaded online, softcore",
              bLoaded ? ("after " + std::to_string(nMs) + " ms")
                      : (LogContains(sLoaded) ? "loaded, but NOT with \"hardcore disabled\""
                                              : "no \"" + sLoaded + "\" within 30 s"));
    }
    else
    {
        // Only the negative control passes a delay. It lets the load finish,
        // so the check below must then fail.
        if (nDelayMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(nDelayMs));

        // Without this, the mode would prove nothing about in-flight teardown.
        // "Loading game" is logged synchronously inside RA_ActivateGame, just
        // before the load is handed to rc_client, in the same call; its
        // absence would mean the load never began, which "not finished"
        // alone cannot tell apart.
        const bool bBegun = LogContains("Loading game " + std::to_string(KNOWN_GAME_ID));
        const bool bLoaded = LogContains(sLoaded);
        Check(bBegun && !bLoaded, "shutdown raced the load",
              !bBegun   ? "no \"Loading game " + std::to_string(KNOWN_GAME_ID) + "\" - the load never began"
              : bLoaded ? "the load had already finished - nothing was in flight"
                        : "load began and was still in flight at RA_Shutdown()");
    }

    RA_Shutdown();

    const size_t nThreadsAfter = CountThreads();
    Check(nThreadsAfter == nThreadsBefore, "no thread outlives RA_Shutdown()",
          std::to_string(nThreadsBefore) + " before init, " + std::to_string(nThreadsAfter) + " after shutdown");

    Check(LogContains("Shutdown complete"), "library shut down", InLog("Shutdown complete"));

    if (!bInFlight)
    {
        const std::string sEnded = "Ending session for game " + std::to_string(KNOWN_GAME_ID);
        Check(LogContains(sEnded), "session ended at shutdown", InLog(sEnded));

        // Named after the username as the server spells it
        // (AchievementRuntime.cpp), which may differ in case from the staged
        // one. RACache/ was empty before this run, so any history file counts.
        std::string sHistory;
        std::error_code oError;
        for (std::filesystem::directory_iterator it(g_oBaseDirectory / "RACache", oError), end; !oError && it != end;
             it.increment(oError))
        {
            const std::string sName = it->path().filename().string();
            if (sName.size() > 12 && sName.compare(sName.size() - 12, 12, "-history.txt") == 0)
                sHistory = sName;
        }
        Check(!sHistory.empty(), "session history written",
              sHistory.empty() ? "no RACache/*-history.txt" : "RACache/" + sHistory);

        std::ifstream oFile(g_oBaseDirectory / "RACache" / "Data" / "Hashes.txt", std::ios::binary);
        const std::string sHashes((std::istreambuf_iterator<char>(oFile)), std::istreambuf_iterator<char>());
        const std::string sEntry = std::string(KNOWN_HASH) + "=" + std::to_string(KNOWN_GAME_ID);
        const bool bSaved = sHashes.find(sEntry) != std::string::npos;
        Check(bSaved, "known hash saved at shutdown",
              "RACache/Data/Hashes.txt " + std::string(bSaved ? "has " : "lacks ") + sEntry);
    }

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

    const int nDelayMs = (argc > 2) ? std::atoi(argv[2]) : 0;
    if (sMode == "online-loaded")
        return RunOnline(false, 0);
    if (sMode == "online-inflight")
        return RunOnline(true, nDelayMs);

    std::printf("usage: ra_loader_smoke missing|offline|online-loaded|online-inflight [<inflight-delay-ms>]\n");
    return 2;
}
