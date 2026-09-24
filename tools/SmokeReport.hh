#ifndef RA_TOOLS_SMOKE_REPORT_HH
#define RA_TOOLS_SMOKE_REPORT_HH
#pragma once

// The [ok]/[FAIL] and [obs] result lines, and their counters, shared by the
// smoke programs in tools/. ra_linux_smoke.cpp's header comment defines what
// each marker means. Header-only and inline, so each program gets exactly one
// copy of the counters and links nothing extra: ra_dlopen_smoke must link no
// RA code at all.

#include <cstdio>
#include <string>

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

#endif // RA_TOOLS_SMOKE_REPORT_HH
