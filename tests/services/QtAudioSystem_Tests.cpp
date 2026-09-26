#ifndef _WIN32

#include "services/impl/QtApplicationHost.hh"
#include "services/impl/QtAudioSystem.hh"

#include "tests/RA_UnitTestHelpers.h"
#include "tests/devkit/services/mocks/MockFileSystem.hh"

#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QSoundEffect>
#include <QTimer>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(QtAudioSystem_Tests)
{
private:
    // An application-wide event filter sees every event delivered to an object
    // on the application thread. A DeferredDelete is only ever posted by
    // deleteLater(), and ~QObject discards the posted events of an object
    // destroyed before they are delivered - so seeing one reach a QSoundEffect
    // means that effect was still alive once control got back to the event
    // loop. Needs no moc: overriding eventFilter is not a new slot.
    class DeferredDeleteWatcher : public QObject
    {
    public:
        bool eventFilter(QObject* pWatched, QEvent* pEvent) override
        {
            if (pEvent->type() == QEvent::DeferredDelete && qobject_cast<QSoundEffect*>(pWatched) != nullptr)
                ++m_nDeferredDeletes;

            return false;
        }

        int m_nDeferredDeletes = 0;
    };

public:
    TEST_METHOD(TestFailedEffectIsDeletedLaterNotInsideItsOwnSignal)
    {
        // A source that cannot load reaches statusChanged(Error) from the event
        // loop on Qt 6.11, after the queued create has put the effect in the
        // pool - so the reap runs inside that emission, and needs neither an
        // audio device nor a desktop session. Were the Error ever reported
        // inside setSource() instead, PlayAudioFile would destroy the effect
        // itself, nothing would be deleted later, and this would fail rather
        // than pass without having reached the reap.
        std::string sTemplate = (std::filesystem::temp_directory_path() / "ra-audio-XXXXXX").string();
        std::vector<char> vBuffer(sTemplate.begin(), sTemplate.end());
        vBuffer.push_back('\0');
        Assert::IsNotNull(mkdtemp(vBuffer.data()), L"mkdtemp failed");
        const std::string sDirectory(vBuffer.data());

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(sDirectory + "/"));

        // Declared before the application so it is destroyed after it, and is
        // never left installed as a filter on a live application.
        DeferredDeleteWatcher oWatcher;
        {
            int nArgc = 3;
            char sArg0[] = "ra_tests";
            char sArg1[] = "-platform";
            char sArg2[] = "offscreen";
            char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
            QGuiApplication oApplication(nArgc, vArgv);
            oApplication.installEventFilter(&oWatcher);

            {
                QtApplicationHost oHost;
                oHost.Start(); // borrows oApplication
                QtAudioSystem oAudioSystem(oHost);
                oAudioSystem.PlayAudioFile(L"missing.wav");

                const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (oWatcher.m_nDeferredDeletes == 0 && std::chrono::steady_clock::now() < tDeadline)
                {
                    QEventLoop oLoop;
                    QTimer::singleShot(20, &oLoop, [&oLoop]() { oLoop.quit(); });
                    oLoop.exec();
                }

                oHost.Stop();
            }

            oApplication.removeEventFilter(&oWatcher);
        }

        std::error_code oError;
        std::filesystem::remove(sDirectory, oError);

        Assert::AreEqual(1, oWatcher.m_nDeferredDeletes,
                         L"the reaped effect never reached the event loop alive - destroyed inside its own signal?");
    }

    TEST_METHOD(TestStopHookDestroysEffectsStillInThePool)
    {
        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(L"/nonexistent/");

        int nArgc = 3;
        char sArg0[] = "ra_tests";
        char sArg1[] = "-platform";
        char sArg2[] = "offscreen";
        char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication(nArgc, vArgv);

        QtApplicationHost oHost;
        oHost.Start();
        QtAudioSystem oAudioSystem(oHost);

        // On the Qt thread - this one - Invoke runs inline, so the effect is in
        // the pool as soon as PlayAudioFile returns; its load failure arrives
        // later, from the event loop, which this test never turns.
        oAudioSystem.PlayAudioFile(L"missing.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.PooledEffectCount(), L"the effect was not pooled");

        oHost.Stop();
        Assert::AreEqual(size_t(0), oAudioSystem.PooledEffectCount(), L"Stop() left effects in the pool");
    }

    TEST_METHOD(TestUnavailableHostPlaysNothing)
    {
        // QtAudioSystem reports an unavailable host once (a WARN on the first
        // call only, guarded by m_bReportedUnavailable), but this cannot be
        // observed here because the test build defines RA_UTEST, which
        // compiles every RA_LOG_* call to nothing. This test checks only that
        // nothing is pooled.
        QtApplicationHost::Options oOptions;
        oOptions.fProbe = []() { return DisplayProbeResult{false, "test: no display"}; };
        QtApplicationHost oHost(oOptions);
        oHost.Start();

        QtAudioSystem oAudioSystem(oHost);
        oAudioSystem.PlayAudioFile(L"missing.wav");
        Assert::AreEqual(size_t(0), oAudioSystem.PooledEffectCount());
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
