#ifndef RA_TOOLS_SMOKE_REPORT_HH
#define RA_TOOLS_SMOKE_REPORT_HH
#pragma once

// The [ok]/[FAIL] and [obs] result lines and their counters, shared by the
// smoke programs in tools/, plus the closing summary and the thread count
// that the two library-loading ones (ra_dlopen_smoke, ra_loader_smoke) share.
// ra_linux_smoke.cpp's header comment defines what each marker means.
// Header-only and inline, so each program gets exactly one copy of the
// counters and links nothing extra: ra_dlopen_smoke and ra_loader_smoke must
// link no RA code at all.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

inline int g_nFailures = 0;
inline int g_nPassed = 0;
inline int g_nObserved = 0;

inline void Check(bool bCondition, const char* sLabel, const std::string& sDetail)
{
    if (bCondition)
    {
        ++g_nPassed;
        std::printf("  [ok]   %-34s %s\n", sLabel, sDetail.c_str());
    }
    else
    {
        ++g_nFailures;
        std::printf("  [FAIL] %-34s %s\n", sLabel, sDetail.c_str());
    }

    std::fflush(stdout);
}

// Reported, never asserted - see ra_linux_smoke.cpp's header comment.
inline void Observe(const char* sLabel, const std::string& sDetail)
{
    ++g_nObserved;
    std::printf("  [obs]  %-34s %s\n", sLabel, sDetail.c_str());
    std::fflush(stdout);
}

// Prints the summary line and verdict that checks/so-smoke.sh and
// checks/loader-smoke.sh parse, and returns the exit status: 0 only if no
// check failed.
inline int Finish(const char* sProgram)
{
    std::printf("\n%s: %d ok, %d failed, %d observed\n", sProgram, g_nPassed, g_nFailures, g_nObserved);
    std::printf("%s\n", g_nFailures == 0 ? "all checks passed" : "FAILURES");
    std::fflush(stdout);
    return g_nFailures == 0 ? 0 : 1;
}

// One entry per thread of this process, a loaded library's included. 0 if
// /proc cannot be read: callers check for that, or every comparison of two
// counts would hold trivially.
inline size_t CountThreads()
{
    std::error_code oError;
    size_t nThreads = 0;
    for (std::filesystem::directory_iterator it("/proc/self/task", oError), end; !oError && it != end;
         it.increment(oError))
    {
        ++nThreads;
    }

    return nThreads;
}

// Qt's D-Bus connection manager starts one thread, named "QDBusConnection",
// with the first GUI application, and keeps it until the process exits: a
// process-lifetime global with no API to stop it. The library pins the Qt
// libraries that thread runs in (QtApplicationHost), so it is safe to leave
// running after RA_Shutdown and dlclose. Every other thread must be gone.
struct ThreadCount
{
    size_t nOther = 0;
    size_t nQtDBus = 0;
};

inline ThreadCount CountThreadsBesideQtDBus()
{
    ThreadCount oCount;
    std::error_code oError;
    for (std::filesystem::directory_iterator it("/proc/self/task", oError), end; !oError && it != end;
         it.increment(oError))
    {
        std::ifstream oComm(it->path() / "comm");
        std::string sName;
        std::getline(oComm, sName);
        if (sName == "QDBusConnection")
            ++oCount.nQtDBus;
        else
            ++oCount.nOther;
    }

    return oCount;
}

#endif // RA_TOOLS_SMOKE_REPORT_HH
