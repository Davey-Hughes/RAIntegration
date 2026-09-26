#ifndef _WIN32

#include "services/impl/StderrFileLogger.hh"

#include "tests/devkit/services/mocks/MockClock.hh"
#include "tests/devkit/services/mocks/MockFileSystem.hh"
#include "tests/RA_UnitTestHelpers.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

using ra::services::mocks::MockClock;
using ra::services::mocks::MockFileSystem;

namespace ra {
namespace services {
namespace impl {
namespace tests {

namespace {

// Sets or clears RA_LOG_STDERR for one test and puts back whatever the
// environment had on scope exit - including when an Assert::* throws - so a
// developer shell that exports it cannot decide the default-off tests, and no
// test leaks its value into the next.
class ScopedLogStderrVariable
{
public:
    explicit ScopedLogStderrVariable(const char* sValue)
    {
        const char* sPrevious = std::getenv(s_sName);
        m_bHadPrevious = (sPrevious != nullptr);
        if (m_bHadPrevious)
            m_sPrevious = sPrevious;

        Set(sValue);
    }

    ~ScopedLogStderrVariable() { Set(m_bHadPrevious ? m_sPrevious.c_str() : nullptr); }

    ScopedLogStderrVariable(const ScopedLogStderrVariable&) = delete;
    ScopedLogStderrVariable& operator=(const ScopedLogStderrVariable&) = delete;
    ScopedLogStderrVariable(ScopedLogStderrVariable&&) = delete;
    ScopedLogStderrVariable& operator=(ScopedLogStderrVariable&&) = delete;

    // nullptr clears the variable.
    static void Set(const char* sValue)
    {
        if (sValue != nullptr)
            ::setenv(s_sName, sValue, 1);
        else
            ::unsetenv(s_sName);
    }

private:
    static constexpr const char* s_sName = "RA_LOG_STDERR";

    std::string m_sPrevious;
    bool m_bHadPrevious = false;
};

// Points file descriptor 2 at an anonymous temporary file, so a test can read
// back exactly what reached stderr. Release() restores the real stderr and
// returns what was written; the destructor restores it too, so an Assert::*
// that throws first cannot swallow the rest of the run's stderr.
class StderrCapture
{
public:
    StderrCapture()
    {
        std::fflush(stderr);

        m_pFile = std::tmpfile();
        Assert::IsNotNull(m_pFile, L"tmpfile failed");

        m_nSavedDescriptor = ::dup(STDERR_FILENO);
        if (m_nSavedDescriptor < 0)
        {
            std::fclose(m_pFile);
            Assert::Fail(L"dup(2) failed");
        }

        if (::dup2(::fileno(m_pFile), STDERR_FILENO) < 0)
        {
            ::close(m_nSavedDescriptor);
            std::fclose(m_pFile);
            Assert::Fail(L"dup2 onto stderr failed");
        }
    }

    ~StderrCapture()
    {
        Restore();
        std::fclose(m_pFile);
    }

    StderrCapture(const StderrCapture&) = delete;
    StderrCapture& operator=(const StderrCapture&) = delete;
    StderrCapture(StderrCapture&&) = delete;
    StderrCapture& operator=(StderrCapture&&) = delete;

    std::string Release()
    {
        Restore();

        std::string sCaptured;
        std::rewind(m_pFile);
        char sBuffer[256];
        size_t nRead = 0;
        while ((nRead = std::fread(sBuffer, 1, sizeof(sBuffer), m_pFile)) > 0)
            sCaptured.append(sBuffer, nRead);

        return sCaptured;
    }

private:
    void Restore() noexcept
    {
        if (m_nSavedDescriptor < 0)
            return;

        std::fflush(stderr);
        ::dup2(m_nSavedDescriptor, STDERR_FILENO);
        ::close(m_nSavedDescriptor);
        m_nSavedDescriptor = -1;
    }

    std::FILE* m_pFile = nullptr;
    int m_nSavedDescriptor = -1;
};

} // namespace

TEST_CLASS(StderrFileLogger_Tests)
{
private:
    const std::wstring mockLogFileName = L".\\RACache\\RALog.txt";

    // Logs one line with RA_LOG_STDERR as given (nullptr: unset) at
    // construction, and returns everything that reached stderr. A control line
    // is written straight to stderr inside the capture, so an empty result
    // can never come from a capture that was not in place.
    std::string CaptureOneLine(const char* sVariable)
    {
        MockClock mockClock;
        MockFileSystem mockFileSystem;
        ScopedLogStderrVariable oVariable(sVariable);
        StderrFileLogger logger(mockFileSystem);

        StderrCapture oCapture;
        logger.LogMessage(LogLevel::Info, "This is a message.");
        std::fputs("control\n", stderr);
        std::string sCaptured = oCapture.Release();

        // the file log gets the line whether or not stderr does
        Assert::IsTrue(mockFileSystem.GetFileContents(mockLogFileName).find("|INFO| This is a message.\n") !=
                           std::string::npos,
                       L"RALog.txt did not get the line");

        return sCaptured;
    }

public:
    TEST_METHOD(TestNoMirrorByDefault)
    {
        Assert::AreEqual(std::string("control\n"), CaptureOneLine(nullptr));
    }

    TEST_METHOD(TestNoMirrorWhenEmpty)
    {
        Assert::AreEqual(std::string("control\n"), CaptureOneLine(""));
    }

    TEST_METHOD(TestNoMirrorWhenZero)
    {
        Assert::AreEqual(std::string("control\n"), CaptureOneLine("0"));
    }

    TEST_METHOD(TestMirrorWhenOptedIn)
    {
        Assert::AreEqual(std::string("This is a message.\ncontrol\n"), CaptureOneLine("1"));
    }

    TEST_METHOD(TestVariableIsReadOnceAtConstruction)
    {
        MockClock mockClock;
        MockFileSystem mockFileSystem;
        ScopedLogStderrVariable oVariable(nullptr);
        StderrFileLogger oLoggerMadeUnset(mockFileSystem);
        ScopedLogStderrVariable::Set("1");
        StderrFileLogger oLoggerMadeSet(mockFileSystem);

        // Changing the variable afterwards, in either direction, changes
        // neither logger: a per-line read would flip one of them each time.
        StderrCapture oCapture;
        ScopedLogStderrVariable::Set(nullptr);
        oLoggerMadeUnset.LogMessage(LogLevel::Info, "unset, logged while unset");
        oLoggerMadeSet.LogMessage(LogLevel::Info, "set, logged while unset");
        ScopedLogStderrVariable::Set("1");
        oLoggerMadeUnset.LogMessage(LogLevel::Info, "unset, logged while set");
        oLoggerMadeSet.LogMessage(LogLevel::Info, "set, logged while set");

        Assert::AreEqual(std::string("set, logged while unset\nset, logged while set\n"), oCapture.Release());
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
