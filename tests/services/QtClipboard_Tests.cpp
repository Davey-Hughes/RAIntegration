#ifndef _WIN32

#include "services/impl/QtApplicationHost.hh"
#include "services/impl/QtClipboard.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(QtClipboard_Tests)
{
private:
    // Qt's offscreen platform keeps the clipboard in a buffer inside this
    // process, so these tests never touch the desktop's clipboard.
    struct OffscreenApplication
    {
        int nArgc = 3;
        char sArg0[9] = "ra_tests";
        char sArg1[10] = "-platform";
        char sArg2[10] = "offscreen";
        char* vArgv[4] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication{nArgc, vArgv};
    };

public:
    TEST_METHOD(TestWorkerWriteAndReadLandOnTheQtThread)
    {
        OffscreenApplication oApp;
        QtApplicationHost oHost;
        oHost.Start();
        QtClipboard oClipboard(oHost);

        std::wstring sFromWorker = L"<never ran>";
        std::atomic<bool> bDone{false};
        std::thread oWorker([&]() {
            oClipboard.SetText(L"from a worker");
            sFromWorker = oClipboard.GetText();
            bDone = true;
        });

        // the worker's GetText waits for this thread's event loop to answer it
        const auto tDeadline = std::chrono::steady_clock::now() + 5s;
        while (!bDone && std::chrono::steady_clock::now() < tDeadline)
        {
            QEventLoop oLoop;
            QTimer::singleShot(20, &oLoop, [&oLoop]() { oLoop.quit(); });
            oLoop.exec();
        }
        oWorker.join();

        Assert::AreEqual(std::wstring(L"from a worker"), sFromWorker);
        Assert::AreEqual(std::wstring(L"from a worker"), oClipboard.GetText());
        oHost.Stop();
    }

    TEST_METHOD(TestUnavailableHostIgnoresTheClipboard)
    {
        QtApplicationHost::Options oOptions;
        oOptions.fProbe = []() { return DisplayProbeResult{false, "test: no display"}; };
        QtApplicationHost oHost(oOptions);
        oHost.Start();
        QtClipboard oClipboard(oHost);

        oClipboard.SetText(L"ignored");
        Assert::AreEqual(std::wstring(), oClipboard.GetText());
    }

    TEST_METHOD(TestReadTimesOutInsteadOfDeadlocking)
    {
        OffscreenApplication oApp;
        QtApplicationHost oHost;
        oHost.Start();
        QtClipboard oClipboard(oHost);

        // This thread owns the application and blocks in join() without turning
        // its loop, so the worker's read can never be answered.
        std::wstring sFromWorker = L"<never ran>";
        const auto tStart = std::chrono::steady_clock::now();
        std::thread oWorker([&]() { sFromWorker = oClipboard.GetText(); });
        oWorker.join();
        const auto tElapsed = std::chrono::steady_clock::now() - tStart;

        Assert::AreEqual(std::wstring(), sFromWorker);
        Assert::IsTrue(tElapsed >= 1900ms && tElapsed < 5s, L"GetText did not time out after about 2 s");
        oHost.Stop();
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
