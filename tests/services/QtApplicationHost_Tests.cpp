#ifndef _WIN32

#include "services/impl/QtApplicationHost.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QGuiApplication>
#include <QObject>
#include <QTimer>

#include <glib.h>
#include <pthread.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(QtApplicationHost_Tests)
{
private:
    // Every owned application in these tests is headless: the probe is told a
    // display exists, and the application is told to use Qt's offscreen platform.
    static QtApplicationHost::Options OffscreenOptions()
    {
        QtApplicationHost::Options oOptions;
        oOptions.fProbe = []() { return DisplayProbeResult{true, "test: offscreen"}; };
        oOptions.vArguments = {"-platform", "offscreen"};
        return oOptions;
    }

    // Turns this thread's event loop until fDone() or the timeout.
    template<typename TPredicate>
    static bool PumpUntil(TPredicate fDone, std::chrono::milliseconds tTimeout)
    {
        const auto tDeadline = std::chrono::steady_clock::now() + tTimeout;
        while (!fDone() && std::chrono::steady_clock::now() < tDeadline)
        {
            QEventLoop oLoop;
            QTimer::singleShot(20, &oLoop, [&oLoop]() { oLoop.quit(); });
            oLoop.exec();
        }
        return fDone();
    }

    // argv for an application the test itself creates (the borrowed cases)
    struct OffscreenArguments
    {
        char sArg0[9] = "ra_tests";
        char sArg1[10] = "-platform";
        char sArg2[10] = "offscreen";
        char* vArgv[4] = {sArg0, sArg1, sArg2, nullptr};
        int nArgc = 3;
    };

    struct GlibTicks
    {
        std::thread::id nTestThread;
        std::atomic<int> nOnTestThread{0};
        std::atomic<int> nElsewhere{0};
    };

    static gboolean CountTick(gpointer pData)
    {
        auto* pTicks = static_cast<GlibTicks*>(pData);
        if (std::this_thread::get_id() == pTicks->nTestThread)
            ++pTicks->nOnTestThread;
        else
            ++pTicks->nElsewhere;
        return G_SOURCE_CONTINUE;
    }

    static gboolean QuitLoop(gpointer pData)
    {
        g_main_loop_quit(static_cast<GMainLoop*>(pData));
        return G_SOURCE_REMOVE;
    }

    // Runs a glib main loop on glib's DEFAULT context - as a GTK or other
    // glib-based emulator does on its main thread - for 600 ms beside an owned
    // Qt application, and reports which threads ran the loop's own timer.
    static void RunGlibHostBeside(bool bUseNonGlibDispatcher, GlibTicks& oTicks)
    {
        auto oOptions = OffscreenOptions();
        oOptions.bUseNonGlibDispatcher = bUseNonGlibDispatcher;
        QtApplicationHost oHost(oOptions);
        oHost.Start();

        // keep the Qt thread iterating its event loop, as a live application does
        oHost.InvokeAndWait(
            []() {
                auto* pTimer = new QTimer(QCoreApplication::instance());
                QObject::connect(pTimer, &QTimer::timeout, []() {});
                pTimer->start(5);
            },
            5s);

        oTicks.nTestThread = std::this_thread::get_id();
        GMainLoop* pLoop = g_main_loop_new(nullptr, FALSE);
        const guint nTick = g_timeout_add(50, &CountTick, &oTicks);
        g_timeout_add(600, &QuitLoop, pLoop);
        g_main_loop_run(pLoop);
        g_main_loop_unref(pLoop);

        oHost.Stop();
        g_source_remove(nTick);
    }

    static void SentinelHandler(QtMsgType, const QMessageLogContext&, const QString&) {}

public:
    TEST_METHOD(TestUnavailableWithoutADisplay)
    {
        QtApplicationHost::Options oOptions;
        oOptions.fProbe = []() { return DisplayProbeResult{false, "test: no display"}; };
        QtApplicationHost oHost(oOptions);
        oHost.Start();

        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Unavailable);
        Assert::IsFalse(oHost.IsAvailable());
        Assert::IsFalse(oHost.IsOnQtThread());
        Assert::IsTrue(oHost.GetUnavailableReason().find("test: no display") != std::string::npos);

        bool bRan = false;
        oHost.Invoke([&bRan]() { bRan = true; });
        Assert::IsFalse(oHost.InvokeAndWait([&bRan]() { bRan = true; }, 100ms));
        Assert::IsFalse(bRan, L"work ran with no Qt application");
    }

    TEST_METHOD(TestOwnedRunsWorkOnItsOwnThread)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Owned);
        Assert::IsTrue(oHost.IsAvailable());
        Assert::IsTrue(oHost.HasWidgets());
        Assert::IsFalse(oHost.IsOnQtThread(), L"the test thread is not the Qt thread");

        std::thread::id nFirst, nSecond;
        Assert::IsTrue(oHost.InvokeAndWait([&nFirst]() { nFirst = std::this_thread::get_id(); }, 5s));
        Assert::IsTrue(oHost.InvokeAndWait([&nSecond]() { nSecond = std::this_thread::get_id(); }, 5s));
        Assert::IsTrue(nFirst != std::this_thread::get_id(), L"work ran on the calling thread");
        Assert::IsTrue(nFirst == nSecond, L"work ran on two different threads");

        std::atomic<bool> bAsyncRan{false};
        std::thread::id nAsync;
        oHost.Invoke([&nAsync, &bAsyncRan]() {
            nAsync = std::this_thread::get_id();
            bAsyncRan = true;
        });
        const auto tDeadline = std::chrono::steady_clock::now() + 5s;
        while (!bAsyncRan && std::chrono::steady_clock::now() < tDeadline)
            std::this_thread::sleep_for(5ms);
        Assert::IsTrue(bAsyncRan.load(), L"queued work never ran");
        Assert::IsTrue(nAsync == nFirst);

        oHost.Stop();
        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Stopped);
        Assert::IsFalse(oHost.IsAvailable());
        Assert::IsNull(QCoreApplication::instance(), L"the owned application outlived Stop()");
    }

    TEST_METHOD(TestInvokeAndWaitIsInlineOnTheQtThread)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        bool bInnerReturned = false;
        bool bInnerRan = false;
        const bool bOuter = oHost.InvokeAndWait(
            [&oHost, &bInnerReturned, &bInnerRan]() {
                bInnerReturned = oHost.InvokeAndWait([&bInnerRan]() { bInnerRan = true; }, 1s);
            },
            5s);
        oHost.Stop();

        Assert::IsTrue(bOuter);
        Assert::IsTrue(bInnerReturned && bInnerRan, L"a wait from the Qt thread did not run inline");
    }

    TEST_METHOD(TestInvokeAndWaitThatTimesOutBeforeItStartsNeverRuns)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        // hold the Qt thread, so the waited call cannot start in time
        std::mutex oMutex;
        std::condition_variable cvRelease;
        bool bRelease = false;
        oHost.Invoke([&]() {
            std::unique_lock<std::mutex> oLock(oMutex);
            cvRelease.wait(oLock, [&bRelease]() { return bRelease; });
        });

        std::atomic<bool> bRan{false};
        const bool bResult = oHost.InvokeAndWait([&bRan]() { bRan = true; }, 50ms);

        {
            std::lock_guard<std::mutex> oLock(oMutex);
            bRelease = true;
        }
        cvRelease.notify_all();

        // queued behind the timed-out call: once it has run, that call has
        // had its turn
        const bool bSentinelRan = oHost.InvokeAndWait([]() {}, 5s);
        oHost.Stop();

        Assert::IsFalse(bResult, L"a call that never started was reported as run");
        Assert::IsTrue(bSentinelRan);
        Assert::IsFalse(bRan.load(), L"a call that timed out before it started ran later");
    }

    TEST_METHOD(TestInvokeAndWaitThatHasStartedIsWaitedFor)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        // the limit covers starting only: a call already running may use the
        // caller's locals, so it is waited for past the limit
        std::atomic<bool> bFinished{false};
        const bool bResult = oHost.InvokeAndWait(
            [&bFinished]() {
                std::this_thread::sleep_for(200ms);
                bFinished = true;
            },
            50ms);
        const bool bFinishedOnReturn = bFinished.load();
        oHost.Stop();

        Assert::IsTrue(bResult, L"a call that had started was reported as not run");
        Assert::IsTrue(bFinishedOnReturn, L"InvokeAndWait returned while its call was still running");
    }

    TEST_METHOD(TestStopRefusesLaterWork)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        oHost.Stop();

        bool bRan = false;
        oHost.Invoke([&bRan]() { bRan = true; });
        Assert::IsFalse(oHost.InvokeAndWait([&bRan]() { bRan = true; }, 100ms));
        Assert::IsFalse(bRan);
    }

    TEST_METHOD(TestQueuedWorkThatHasNotRunIsSkippedOnceStopBegins)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        // The first call holds the Qt thread until released; the second waits
        // behind it. Stop() begins while both are queued-or-running.
        std::mutex oMutex;
        std::condition_variable cvRelease;
        bool bRelease = false;
        std::atomic<bool> bSecondRan{false};
        oHost.Invoke([&]() {
            std::unique_lock<std::mutex> oLock(oMutex);
            cvRelease.wait(oLock, [&bRelease]() { return bRelease; });
        });
        oHost.Invoke([&bSecondRan]() { bSecondRan = true; });

        // released only once Stop() has closed the gate, so the second call
        // finds it closed whenever it runs
        std::thread oReleaser([&]() {
            while (oHost.IsAvailable())
                std::this_thread::sleep_for(1ms);
            {
                std::lock_guard<std::mutex> oLock(oMutex);
                bRelease = true;
            }
            cvRelease.notify_all();
        });

        oHost.Stop();
        oReleaser.join();

        Assert::IsFalse(bSecondRan.load(), L"work queued before Stop() ran after it began");
    }

    TEST_METHOD(TestStopHooksRunOnTheQtThreadWhileTheApplicationExists)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        std::thread::id nQtThread;
        oHost.InvokeAndWait([&nQtThread]() { nQtThread = std::this_thread::get_id(); }, 5s);

        std::thread::id nHookThread;
        bool bApplicationAlive = false;
        int nHookRuns = 0;
        oHost.AddStopHook([&]() {
            nHookThread = std::this_thread::get_id();
            bApplicationAlive = (QCoreApplication::instance() != nullptr);
            ++nHookRuns;
        });
        oHost.Stop();

        Assert::IsTrue(nHookThread == nQtThread, L"the stop hook ran off the Qt thread");
        Assert::IsTrue(bApplicationAlive, L"the stop hook ran after the application was gone");

        // hooks are cleared by Stop(): a second cycle must not run it again
        oHost.Start();
        oHost.Stop();
        Assert::AreEqual(1, nHookRuns);
    }

    TEST_METHOD(TestDeferredDeletesAreFlushedBeforeTheApplicationGoes)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();

        std::atomic<bool> bDestroyed{false};
        oHost.AddStopHook([&bDestroyed]() {
            auto* pObject = new QObject();
            QObject::connect(pObject, &QObject::destroyed, [&bDestroyed]() { bDestroyed = true; });
            pObject->deleteLater();
        });
        oHost.Stop();

        Assert::IsTrue(bDestroyed.load(), L"an object handed to deleteLater() was never deleted");
    }

    TEST_METHOD(TestThreeStartStopCycles)
    {
        QtApplicationHost oHost(OffscreenOptions());
        for (int nCycle = 0; nCycle < 3; ++nCycle)
        {
            oHost.Start();
            Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Owned);
            bool bRan = false;
            Assert::IsTrue(oHost.InvokeAndWait([&bRan]() { bRan = true; }, 5s));
            Assert::IsTrue(bRan);
            oHost.Stop();
        }
    }

    TEST_METHOD(TestBorrowsTheHostsGuiApplication)
    {
        OffscreenArguments oArguments;
        QGuiApplication oApplication(oArguments.nArgc, oArguments.vArgv);

        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Borrowed);
        Assert::IsFalse(oHost.HasWidgets(), L"a QGuiApplication cannot host widgets");
        Assert::IsTrue(oHost.IsOnQtThread(), L"the borrowed application lives on this thread");

        // a worker's call is queued here, and runs when this thread's loop turns
        std::atomic<bool> bRan{false};
        std::thread::id nRanOn;
        std::thread oWorker([&]() {
            oHost.Invoke([&]() {
                nRanOn = std::this_thread::get_id();
                bRan = true;
            });
        });
        oWorker.join();
        Assert::IsFalse(bRan.load(), L"queued work ran before this thread's loop turned");
        Assert::IsTrue(PumpUntil([&bRan]() { return bRan.load(); }, 5s));
        Assert::IsTrue(nRanOn == std::this_thread::get_id());

        // Stop() runs the hooks inline and leaves the host's application alone
        bool bHookRan = false;
        oHost.AddStopHook([&bHookRan]() { bHookRan = true; });
        oHost.Stop();
        Assert::IsTrue(bHookRan);
        Assert::IsTrue(QCoreApplication::instance() == &oApplication, L"Stop() touched the host's application");
    }

    TEST_METHOD(TestBorrowedStopTakesQueuedWorkOutOfTheHostsLoop)
    {
        OffscreenArguments oArguments;
        QGuiApplication oApplication(oArguments.nArgc, oArguments.vArgv);

        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Borrowed);

        // A worker's call waits in this thread's queue - the host's - until the
        // loop turns. Only that queued call holds pToken.
        auto pToken = std::make_shared<int>(0);
        const std::weak_ptr<int> pQueued = pToken;
        std::thread oWorker([&oHost, pToken = std::move(pToken)]() { oHost.Invoke([pToken]() {}); });
        oWorker.join();
        Assert::IsFalse(pQueued.expired(), L"the worker's call is not queued");

        // Stop() on the Qt thread must leave none of this library's code in the
        // host's queue: once the library is unloaded, the host's loop would run
        // or destroy it there. This loop never turns, so only Stop() can have
        // destroyed the call.
        oHost.Stop();
        Assert::IsTrue(pQueued.expired(), L"work queued before Stop() is still in the host's event queue");
    }

    TEST_METHOD(TestNonGuiHostApplicationIsUnavailable)
    {
        OffscreenArguments oArguments;
        QCoreApplication oApplication(oArguments.nArgc, oArguments.vArgv);

        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Unavailable);
        Assert::IsTrue(oHost.GetUnavailableReason().find("not a GUI application") != std::string::npos);
    }

    TEST_METHOD(TestGlibHostKeepsItsDefaultContext)
    {
        // Negative control first. Without QT_NO_GLIB, Qt's glib dispatcher takes
        // glib's default context from the application's thread, and the host's
        // own callbacks run on the Qt thread. If that does not happen here, this
        // rig cannot tell the guard from its absence.
        ::unsetenv("QT_NO_GLIB");
        GlibTicks oHijacked;
        RunGlibHostBeside(false, oHijacked);
        Assert::IsTrue(oHijacked.nElsewhere.load() > 0,
                       L"negative control: no host glib callback ran on the Qt thread - the rig cannot see a hijack");

        GlibTicks oGuarded;
        RunGlibHostBeside(true, oGuarded);
        Assert::AreEqual(0, oGuarded.nElsewhere.load(), L"a host glib callback ran off the host's thread");
        Assert::IsTrue(oGuarded.nOnTestThread.load() > 5, L"the host's glib loop barely ran");
    }

    TEST_METHOD(TestOwnedThreadIsNamed)
    {
        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        std::string sName;
        oHost.InvokeAndWait(
            [&sName]() {
                char sBuffer[16] = {};
                pthread_getname_np(pthread_self(), sBuffer, sizeof(sBuffer));
                sName = sBuffer;
            },
            5s);
        oHost.Stop();
        Assert::AreEqual(std::string("RA-Qt"), sName);
    }

    TEST_METHOD(TestMessageHandlerForwardedWhileOwnedAndRestoredAfter)
    {
        const QtMessageHandler fOriginal = qInstallMessageHandler(&SentinelHandler);

        QtApplicationHost oHost(OffscreenOptions());
        oHost.Start();
        const QtMessageHandler fWhileOwned = qInstallMessageHandler(nullptr);
        qInstallMessageHandler(fWhileOwned);
        oHost.Stop();
        const QtMessageHandler fAfter = qInstallMessageHandler(fOriginal);

        Assert::IsTrue(fWhileOwned != &SentinelHandler, L"no handler of the library's was installed while owned");
        Assert::IsTrue(fAfter == &SentinelHandler, L"Stop() did not restore the previous handler");
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
