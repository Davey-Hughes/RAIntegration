#ifndef _WIN32

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace compat {
namespace tests {

namespace {

// The forked child reports only through how it ends. It must never assert: a
// failed assertion throws, and the exception would unwind into the child's copy
// of the runner, which would go on to run the rest of the suite a second time.
constexpr int CHILD_RETURNED = 0x7F;

struct ChildOutcome
{
    int nStatus = 0;
    std::string sStderr;
};

std::wstring ToWide(const std::string& sText) { return std::wstring(sText.begin(), sText.end()); }

// Runs pAction in a forked child with its stderr captured, and waits up to ten
// seconds for the child to end.
ChildOutcome RunInChild(void (*pAction)())
{
    int aPipe[2];
    if (::pipe(aPipe) != 0)
    {
        const int nError = errno;
        Assert::Fail(ToWide(std::string("pipe failed: ") + std::strerror(nError)).c_str());
    }

    std::fflush(stdout);
    std::fflush(stderr);
    const pid_t nPid = ::fork();
    if (nPid < 0)
    {
        const int nError = errno;
        Assert::Fail(ToWide(std::string("fork failed: ") + std::strerror(nError)).c_str());
    }

    if (nPid == 0)
    {
        ::close(aPipe[0]);
        ::dup2(aPipe[1], STDERR_FILENO);
        ::close(aPipe[1]);

        // The abort is the expected outcome; don't leave a core dump for it.
        ::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);

        pAction();
        ::_exit(CHILD_RETURNED);
    }

    ::close(aPipe[1]);
    ::fcntl(aPipe[0], F_SETFL, O_NONBLOCK);

    ChildOutcome pOutcome;
    const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    for (;;)
    {
        char sBuffer[512];
        const ssize_t nRead = ::read(aPipe[0], sBuffer, sizeof(sBuffer));
        if (nRead > 0)
        {
            pOutcome.sStderr.append(sBuffer, static_cast<size_t>(nRead));
            continue;
        }
        if (nRead == 0) // every copy of the write end is closed: the child has ended
            break;
        if (errno != EAGAIN && errno != EINTR)
            break;

        if (std::chrono::steady_clock::now() > tDeadline)
        {
            ::kill(nPid, SIGKILL);
            ::waitpid(nPid, &pOutcome.nStatus, 0);
            ::close(aPipe[0]);
            Assert::Fail(L"child did not end within 10 seconds");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::close(aPipe[0]);

    while (::waitpid(nPid, &pOutcome.nStatus, 0) < 0 && errno == EINTR)
        continue;

    return pOutcome;
}

void CallWassert() { _wassert(L"MsvcCompat_Tests sentinel", L"sentinel_file.cpp", 4242); }

} // namespace

TEST_CLASS(MsvcCompat_Tests)
{
public:
    TEST_METHOD(TestWassertAbortsWithItsMessage)
    {
        // Upstream calls _wassert directly, not through assert(), as a hard stop:
        // a GSL contract violation (GSL.cpp, RA_Core.cpp) or a missing service
        // (ServiceLocator.hh). MSVC's _wassert stops the process in every build
        // type, so the shim must abort whether or not NDEBUG has switched
        // assert() off. checks/release-build.sh runs this class from an NDEBUG
        // build as well.
        const auto pOutcome = RunInChild(&CallWassert);
        const std::wstring sCaptured = L" - captured stderr: [" + ToWide(pOutcome.sStderr) + L"]";

        Assert::IsFalse(WIFEXITED(pOutcome.nStatus) && WEXITSTATUS(pOutcome.nStatus) == CHILD_RETURNED,
                        (L"_wassert returned" + sCaptured).c_str());
        Assert::IsTrue(WIFSIGNALED(pOutcome.nStatus), (L"the child did not die by a signal" + sCaptured).c_str());
        Assert::AreEqual(static_cast<int>(SIGABRT), static_cast<int>(WTERMSIG(pOutcome.nStatus)),
                         (L"the child died by the wrong signal" + sCaptured).c_str());
        Assert::IsTrue(pOutcome.sStderr.find("MsvcCompat_Tests sentinel") != std::string::npos,
                       (L"the message is not on stderr" + sCaptured).c_str());
        Assert::IsTrue(pOutcome.sStderr.find("sentinel_file.cpp:4242") != std::string::npos,
                       (L"file:line is not on stderr" + sCaptured).c_str());
    }
};

} // namespace tests
} // namespace compat
} // namespace ra

#endif // !_WIN32
