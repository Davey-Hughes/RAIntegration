#ifndef _WIN32

#include "services/impl/LinuxDebuggerDetector.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <signal.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

namespace {

// Bits of the forked child's exit status - the only way it reports. It must
// never assert: a failed assertion throws, and the exception would unwind into
// the child's copy of the runner, which would go on to run the rest of the
// suite a second time.
constexpr int CHILD_UNTRACED_RESULT = 0x01;
constexpr int CHILD_TRACED_RESULT = 0x02;
constexpr int CHILD_NO_STATUS_FILE = 0x40;
constexpr int CHILD_TRACEME_REFUSED = 0x80; // errno in the low seven bits

struct TracedProbeResult
{
    bool bSkipped = false;
    bool bUntraced = false; // the probe's answer before PTRACE_TRACEME
    bool bTraced = false;   // its answer after, with this process as the tracer
};

// The runner has no "skipped" status. Printed in the runner's own FAIL layout
// so that it is at least visible; the test then returns, and counts as passed.
void ReportSkip(const char* sTestName, const std::string& sReason)
{
    std::printf("SKIP LinuxDebuggerDetector_Tests::%s\n     %s\n", sTestName, sReason.c_str());
    std::fflush(stdout);
}

// Forks a child that runs pProbe, makes this process its tracer with
// PTRACE_TRACEME, and runs pProbe again.
//
// PTRACE_TRACEME rather than a PTRACE_ATTACH from here: nothing has to be
// synchronised and no SIGSTOP has to be consumed. Both make the parent the
// tracer, which Yama's default ptrace_scope of 1 permits either way. The child
// exits while still traced - a tracee cannot detach itself, and exiting ends
// the trace - so there is no detach step.
TracedProbeResult ProbeInTracedChild(bool (*pProbe)(), const char* sTestName)
{
    TracedProbeResult pResult;

    std::fflush(stdout);
    const pid_t nPid = ::fork();
    if (nPid < 0)
    {
        const int nError = errno;
        Assert::Fail(ra::util::String::Widen(std::string("fork failed: ") + std::strerror(nError)).c_str());
    }

    if (nPid == 0)
    {
        // Checked separately so that an unreadable file skips rather than
        // failing as "TracerPid read as zero while traced".
        std::FILE* pFile = std::fopen("/proc/self/status", "r");
        if (pFile == nullptr)
            ::_exit(CHILD_NO_STATUS_FILE);
        std::fclose(pFile);

        int nStatus = 0;
        if (pProbe())
            nStatus |= CHILD_UNTRACED_RESULT;

        if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0)
            ::_exit(CHILD_TRACEME_REFUSED | (errno & 0x7F));

        if (pProbe())
            nStatus |= CHILD_TRACED_RESULT;

        ::_exit(nStatus);
    }

    // Poll rather than block, so that a child wedged on anything cannot hang
    // the suite. It has one fopen to do; ten seconds is generous under load.
    const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    int nStatus = 0;
    for (;;)
    {
        const pid_t nWaited = ::waitpid(nPid, &nStatus, WNOHANG);
        if (nWaited == nPid)
        {
            if (!WIFSTOPPED(nStatus))
                break;

            // A signal reached the traced child (a terminal resize sends
            // SIGWINCH to the whole foreground process group) and stopped it
            // for its tracer, which is this process. Deliver it and carry on.
            ::ptrace(PTRACE_CONT, nPid, nullptr,
                     reinterpret_cast<void*>(static_cast<uintptr_t>(WSTOPSIG(nStatus))));
            continue;
        }

        if (nWaited < 0 && errno != EINTR)
        {
            const int nError = errno;
            ::kill(nPid, SIGKILL);
            Assert::Fail(ra::util::String::Widen(std::string("waitpid failed: ") + std::strerror(nError)).c_str());
        }

        if (std::chrono::steady_clock::now() > tDeadline)
        {
            // SIGKILL ends a traced child even in a ptrace stop. Reap it, so
            // that a timeout does not also leave a zombie behind.
            ::kill(nPid, SIGKILL);
            while (::waitpid(nPid, &nStatus, 0) == nPid && WIFSTOPPED(nStatus))
                continue;

            Assert::Fail(L"probe child did not exit within 10 seconds");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    if (WIFSIGNALED(nStatus))
        Assert::Fail((L"probe child killed by signal " + std::to_wstring(WTERMSIG(nStatus))).c_str());

    const int nExitCode = WEXITSTATUS(nStatus);
    if (nExitCode & CHILD_TRACEME_REFUSED)
    {
        const int nError = nExitCode & 0x7F;
        ReportSkip(sTestName, std::string("PTRACE_TRACEME refused: ") + std::strerror(nError) +
                                  " - check kernel.yama.ptrace_scope, a seccomp filter, or an outer tracer");
        pResult.bSkipped = true;
    }
    else if (nExitCode & CHILD_NO_STATUS_FILE)
    {
        ReportSkip(sTestName, "/proc/self/status cannot be opened - no procfs?");
        pResult.bSkipped = true;
    }
    else
    {
        pResult.bUntraced = (nExitCode & CHILD_UNTRACED_RESULT) != 0;
        pResult.bTraced = (nExitCode & CHILD_TRACED_RESULT) != 0;
    }

    return pResult;
}

bool DetectorReportsDebugger()
{
    const LinuxDebuggerDetector oDetector;
    return oDetector.IsDebuggerPresent();
}

} // namespace

TEST_CLASS(LinuxDebuggerDetector_Tests)
{
public:
    TEST_METHOD(TestNoDebuggerWhenRunningNormally)
    {
        // Asks the probe, not the detector: the detector only consults it when
        // NDEBUG is defined, and the test build does not define it. ctest does
        // not attach a debugger; under gdb this test is expected to fail.
        Assert::IsFalse(detail::IsTracerAttached());
    }

    TEST_METHOD(TestProbeSeesPtraceTracer)
    {
        const auto pResult = ProbeInTracedChild(&detail::IsTracerAttached, "TestProbeSeesPtraceTracer");
        if (pResult.bSkipped)
            return;

        Assert::IsFalse(pResult.bUntraced, L"TracerPid read as non-zero before PTRACE_TRACEME");
        Assert::IsTrue(pResult.bTraced, L"TracerPid read as zero while traced");
    }

    TEST_METHOD(TestDetectorReportsTracerOnlyWhenNDEBUG)
    {
        // Same rule as WindowsDebuggerDetector: a DEBUG build keeps working
        // under a debugger, so a developer does not lose hardcore by attaching one.
        const auto pResult = ProbeInTracedChild(&DetectorReportsDebugger, "TestDetectorReportsTracerOnlyWhenNDEBUG");
        if (pResult.bSkipped)
            return;

        Assert::IsFalse(pResult.bUntraced, L"debugger reported before PTRACE_TRACEME");
#ifdef NDEBUG
        Assert::IsTrue(pResult.bTraced, L"NDEBUG build did not report its tracer");
#else
        Assert::IsFalse(pResult.bTraced, L"build without NDEBUG reported its tracer");
#endif
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
