#ifndef _WIN32

#include "services/impl/QtApplicationHost.hh"
#include "services/impl/QtAudioSystem.hh"

#include "tests/RA_UnitTestHelpers.h"
#include "tests/devkit/services/mocks/MockFileSystem.hh"

#include <QAudioDecoder>
#include <QAudioSink>
#include <QEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QTimer>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <thread>
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
    // destroyed before they are delivered - so seeing one reach a decoder
    // means that decoder was still alive once control got back to the event
    // loop. Needs no moc: overriding eventFilter is not a new slot.
    //
    // It also records whether the object still had anything connected to its
    // signals then - the library's lambdas, which the reap must have dropped
    // already, since in borrowed mode this event can outlive the library. A
    // wildcard disconnect() says so by returning true, and costs nothing: the
    // object is about to be deleted.
    //
    // Sinks are watched the same way, but only counted: a sink is reaped
    // inside its own signal only when an audio device let it start, and CI
    // has none.
    class DeferredDeleteWatcher : public QObject
    {
    public:
        bool eventFilter(QObject* pWatched, QEvent* pEvent) override
        {
            if (pEvent->type() == QEvent::DeferredDelete)
            {
                const bool bDecoder = qobject_cast<QAudioDecoder*>(pWatched) != nullptr;
                const bool bSink = qobject_cast<QAudioSink*>(pWatched) != nullptr;
                if (bDecoder)
                    ++m_nDecoderDeferredDeletes;
                if (bSink)
                    ++m_nSinkDeferredDeletes;
                if ((bDecoder || bSink) && QObject::disconnect(pWatched, nullptr, nullptr, nullptr))
                    ++m_nStillConnected;
            }

            return false;
        }

        int m_nDecoderDeferredDeletes = 0;
        int m_nSinkDeferredDeletes = 0;
        int m_nStillConnected = 0;
    };

    // A fresh directory under the system temp directory, removed with
    // everything in it when this goes out of scope.
    class TempDirectory
    {
    public:
        TempDirectory()
        {
            std::string sTemplate = (std::filesystem::temp_directory_path() / "ra-audio-XXXXXX").string();
            std::vector<char> vBuffer(sTemplate.begin(), sTemplate.end());
            vBuffer.push_back('\0');
            Assert::IsNotNull(mkdtemp(vBuffer.data()), L"mkdtemp failed");
            m_sPath = vBuffer.data();
        }

        ~TempDirectory() noexcept
        {
            std::error_code oError;
            std::filesystem::remove_all(m_sPath, oError);
        }

        TempDirectory(const TempDirectory&) = delete;
        TempDirectory& operator=(const TempDirectory&) = delete;
        TempDirectory(TempDirectory&&) = delete;
        TempDirectory& operator=(TempDirectory&&) = delete;

        const std::string& Path() const noexcept { return m_sPath; }

    private:
        std::string m_sPath;
    };

    // 0.2 s of silence, 16-bit little-endian PCM, 22050 Hz mono: a valid file
    // any decoder opens, and nothing audible where a device plays it.
    static void WriteSilentWav(const std::string& sPath)
    {
        constexpr uint32_t nSampleRate = 22050;
        constexpr uint32_t nSamples = nSampleRate / 5;
        constexpr uint32_t nDataBytes = nSamples * 2;

        std::ofstream oFile(sPath, std::ios::binary | std::ios::trunc);
        const auto Put32 = [&oFile](uint32_t n) {
            const char c[4]{static_cast<char>(n & 0xFF), static_cast<char>((n >> 8) & 0xFF),
                            static_cast<char>((n >> 16) & 0xFF), static_cast<char>((n >> 24) & 0xFF)};
            oFile.write(c, 4);
        };
        const auto Put16 = [&oFile](uint16_t n) {
            const char c[2]{static_cast<char>(n & 0xFF), static_cast<char>((n >> 8) & 0xFF)};
            oFile.write(c, 2);
        };

        oFile.write("RIFF", 4);
        Put32(36 + nDataBytes);
        oFile.write("WAVEfmt ", 8);
        Put32(16);              // fmt chunk size
        Put16(1);               // PCM
        Put16(1);               // mono
        Put32(nSampleRate);
        Put32(nSampleRate * 2); // byte rate
        Put16(2);               // block align
        Put16(16);              // bits per sample
        oFile.write("data", 4);
        Put32(nDataBytes);
        const std::vector<char> vSilence(nDataBytes, '\0');
        oFile.write(vSilence.data(), vSilence.size());

        Assert::IsTrue(oFile.good(), L"could not write the test WAV");
    }

    // Turns the event loop until fDone() holds or 10 s have passed.
    static void PumpUntil(const std::function<bool()>& fDone)
    {
        const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!fDone() && std::chrono::steady_clock::now() < tDeadline)
        {
            QEventLoop oLoop;
            QTimer::singleShot(20, &oLoop, [&oLoop]() { oLoop.quit(); });
            oLoop.exec();
        }
    }

    // Turns the event loop once, for about 100 ms.
    static void Pump()
    {
        QEventLoop oLoop;
        QTimer::singleShot(100, &oLoop, [&oLoop]() { oLoop.quit(); });
        oLoop.exec();
    }

    // Points this process's file descriptor 2 at a file until Release(), which
    // puts it back and returns what arrived. FFmpeg's log reaches the terminal
    // through that descriptor directly (av_log's default callback, stdio's
    // unbuffered stderr), below anything a Qt message handler sees, so the
    // descriptor is the only place to look for it. Anything else the process
    // writes to stderr meanwhile lands in the file too.
    class StderrCapture
    {
    public:
        explicit StderrCapture(std::string sPath) : m_sPath(std::move(sPath))
        {
            std::fflush(stderr);
            const int nFile = ::open(m_sPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            Assert::IsTrue(nFile >= 0, L"could not open the stderr capture file");
            m_nSaved = ::dup(STDERR_FILENO);
            ::dup2(nFile, STDERR_FILENO);
            ::close(nFile);
        }

        ~StderrCapture() noexcept { Restore(); }

        StderrCapture(const StderrCapture&) = delete;
        StderrCapture& operator=(const StderrCapture&) = delete;
        StderrCapture(StderrCapture&&) = delete;
        StderrCapture& operator=(StderrCapture&&) = delete;

        std::string Release()
        {
            Restore();
            std::ifstream oFile(m_sPath, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(oFile), std::istreambuf_iterator<char>());
        }

    private:
        void Restore() noexcept
        {
            if (m_nSaved < 0)
                return;

            std::fflush(stderr);
            ::dup2(m_nSaved, STDERR_FILENO);
            ::close(m_nSaved);
            m_nSaved = -1;
        }

        std::string m_sPath;
        int m_nSaved = -1;
    };

    // The first line of sText that starts an FFmpeg media dump, or empty.
    static std::string FindFFmpegDump(const std::string& sText)
    {
        const auto nStart = sText.find("Input #0");
        if (nStart == std::string::npos)
            return {};
        return sText.substr(nStart, sText.find('\n', nStart) - nStart);
    }

public:
    TEST_METHOD(TestDecoderIsDeletedLaterNotInsideItsOwnSignal)
    {
        // QAudioDecoder on Qt 6.11's FFmpeg backend reports a file it cannot
        // open at all - missing, empty, not audio, no decoder for it - inside
        // start(), before PlayAudioFile has pooled it; PlayAudioFile then
        // destroys it itself (TestFailedDecodeIsNotRetried). A file that opens
        // reports finished() later, from the event loop, after the decoder
        // was pooled - so the reap runs inside that emission. Needs no audio
        // device: without one the decoder keeps the file's own format, and
        // the sink that follows fails to start and is destroyed where it was
        // created. Were finished() ever reported inside start() instead,
        // nothing would be deleted later, and this would fail rather than pass
        // without having reached the reap.
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

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
                oAudioSystem.PlayAudioFile(L"silence.wav");

                // Until the decoder's deferred delete has come up and the sink
                // (if a device let one start) has drained and gone as well.
                PumpUntil([&oWatcher, &oAudioSystem]() {
                    return oWatcher.m_nDecoderDeferredDeletes > 0 && oAudioSystem.PooledEffectCount() == 0;
                });
                Pump(); // one more pass, for the sink's own deferred delete

                oHost.Stop();
            }

            oApplication.removeEventFilter(&oWatcher);
        }

        Assert::AreEqual(1, oWatcher.m_nDecoderDeferredDeletes,
                         L"the reaped decoder never reached the event loop alive - destroyed inside its own signal?");
        Assert::AreEqual(0, oWatcher.m_nStillConnected,
                         L"a reaped decoder or sink still held the library's connections when its deferred delete came up");
    }

    TEST_METHOD(TestStopHookDestroysEffectsStillInThePool)
    {
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

        int nArgc = 3;
        char sArg0[] = "ra_tests";
        char sArg1[] = "-platform";
        char sArg2[] = "offscreen";
        char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication(nArgc, vArgv);

        QtApplicationHost oHost;
        oHost.Start();
        QtAudioSystem oAudioSystem(oHost);

        // On the Qt thread - this one - Invoke runs inline, so the decoder is
        // in the pool as soon as PlayAudioFile returns; it reports finished()
        // later, from the event loop, which this test never turns.
        oAudioSystem.PlayAudioFile(L"silence.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.PooledEffectCount(), L"the decoder was not pooled");

        oHost.Stop();
        Assert::AreEqual(size_t(0), oAudioSystem.PooledEffectCount(), L"Stop() left objects in the pool");
    }

    TEST_METHOD(TestStopMidDecodeThenStartDecodesAndPlaysAfresh)
    {
        // Stop() cuts a decode short: its decoder goes, and so do the plays
        // waiting on it. Nothing of that decode may reach the next Start() -
        // not a pending count that swallows the next play, and not a cached
        // result or a "failed" mark from the decoder's stop() - so the next
        // play of the path decodes it afresh and plays it once. PlaybackCount()
        // counts sinks as they are created, whether or not a device then lets
        // them start, so none of this depends on an audio device.
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

        int nArgc = 3;
        char sArg0[] = "ra_tests";
        char sArg1[] = "-platform";
        char sArg2[] = "offscreen";
        char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication(nArgc, vArgv);

        QtApplicationHost oHost;
        oHost.Start(); // borrows oApplication
        QtAudioSystem oAudioSystem(oHost);

        // Inline on this thread, so the decoder is pooled when this returns,
        // and it cannot finish before the event loop turns.
        oAudioSystem.PlayAudioFile(L"silence.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.PooledEffectCount(), L"the decoder was not pooled");

        oHost.Stop();
        Assert::AreEqual(size_t(0), oAudioSystem.PooledEffectCount(), L"Stop() left the decoder in the pool");

        oHost.Start();
        oAudioSystem.PlayAudioFile(L"silence.wav");
        PumpUntil([&oAudioSystem]() { return oAudioSystem.PooledEffectCount() == 0; });
        Assert::AreEqual(size_t(2), oAudioSystem.DecodeCount(),
                         L"the play after Start() did not decode the file afresh");
        Assert::AreEqual(size_t(1), oAudioSystem.PlaybackCount(),
                         L"the play after Start() did not play exactly once");

        oHost.Stop();
    }

    TEST_METHOD(TestDecodesEachFileOnce)
    {
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

        int nArgc = 3;
        char sArg0[] = "ra_tests";
        char sArg1[] = "-platform";
        char sArg2[] = "offscreen";
        char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication(nArgc, vArgv);

        QtApplicationHost oHost;
        oHost.Start();
        QtAudioSystem oAudioSystem(oHost);

        oAudioSystem.PlayAudioFile(L"silence.wav");
        PumpUntil([&oAudioSystem]() { return oAudioSystem.PooledEffectCount() == 0; });
        Assert::AreEqual(size_t(1), oAudioSystem.DecodeCount(), L"the first play did not decode the file");
        Assert::AreEqual(size_t(1), oAudioSystem.PlaybackCount(), L"the decoded sound was not played");

        // Inline again: by the time this returns, the second play has either
        // started a decoder or gone straight to a sink.
        oAudioSystem.PlayAudioFile(L"silence.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.DecodeCount(), L"the second play decoded the file again");
        Assert::AreEqual(size_t(2), oAudioSystem.PlaybackCount(), L"the second play did not play the decoded sound");

        oHost.Stop();
    }

    TEST_METHOD(TestFailedDecodeIsNotRetried)
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

        // The decoder cannot open the file and says so inside start() (see
        // TestDecoderIsDeletedLaterNotInsideItsOwnSignal), so it is destroyed
        // before PlayAudioFile returns and never pooled.
        oAudioSystem.PlayAudioFile(L"missing.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.DecodeCount(), L"the first play did not try to decode the file");
        Assert::AreEqual(size_t(0), oAudioSystem.PooledEffectCount(), L"the failed decoder was left in the pool");

        Pump();

        oAudioSystem.PlayAudioFile(L"missing.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.DecodeCount(), L"a failed decode was tried again");
        Assert::AreEqual(size_t(0), oAudioSystem.PlaybackCount(), L"a sound that failed to decode was played");

        oHost.Stop();
    }

    TEST_METHOD(TestPlaysRequestedWhileDecodingAllPlay)
    {
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

        int nArgc = 3;
        char sArg0[] = "ra_tests";
        char sArg1[] = "-platform";
        char sArg2[] = "offscreen";
        char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
        QGuiApplication oApplication(nArgc, vArgv);

        QtApplicationHost oHost;
        oHost.Start();
        QtAudioSystem oAudioSystem(oHost);

        // A burst of identical unlocks: all three arrive before the event loop
        // turns, so before the first play's decode can finish.
        oAudioSystem.PlayAudioFile(L"silence.wav");
        oAudioSystem.PlayAudioFile(L"silence.wav");
        oAudioSystem.PlayAudioFile(L"silence.wav");
        Assert::AreEqual(size_t(1), oAudioSystem.DecodeCount(), L"the burst decoded the file more than once");
        Assert::AreEqual(size_t(0), oAudioSystem.PlaybackCount(), L"something played before the decode finished");

        // Sinks are counted as they are created, device or not.
        PumpUntil([&oAudioSystem]() { return oAudioSystem.PooledEffectCount() == 0; });
        Assert::AreEqual(size_t(3), oAudioSystem.PlaybackCount(), L"not every play requested while decoding was played");

        oHost.Stop();
    }

    TEST_METHOD(TestOwnedDecodePrintsNoFFmpegDump)
    {
        // Qt 6.11's FFmpeg backend has FFmpeg print a media dump ("Input #0,
        // wav, from ...") to stderr for every file it opens, and the library
        // owns no stderr: a chime must not write to the emulator's terminal
        // (QtApplicationHost::StartOwned). A borrowed host's logging rules are
        // its own, so the dump still appears there - which makes a borrowed
        // decode this check's control. If the control prints no dump, neither
        // this Qt nor its logging rules would print one when owned either, and
        // the owned half could not fail; so that is a failure here, not a pass.
        //
        // Needs no display (offscreen) and no audio device: the decode happens
        // either way, and PlaybackCount() counts a sink as it is created, after
        // the decode has finished, whether or not a device lets it start.
        TempDirectory oDirectory;
        WriteSilentWav(oDirectory.Path() + "/silence.wav");

        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(ra::util::String::Widen(oDirectory.Path() + "/"));

        std::string sBorrowedStderr;
        size_t nBorrowedPlays = 0;
        {
            StderrCapture oCapture(oDirectory.Path() + "/borrowed.err");
            {
                int nArgc = 3;
                char sArg0[] = "ra_tests";
                char sArg1[] = "-platform";
                char sArg2[] = "offscreen";
                char* vArgv[] = {sArg0, sArg1, sArg2, nullptr};
                QGuiApplication oApplication(nArgc, vArgv);

                QtApplicationHost oHost;
                oHost.Start(); // borrows oApplication
                QtAudioSystem oAudioSystem(oHost);
                oAudioSystem.PlayAudioFile(L"silence.wav");
                PumpUntil([&oAudioSystem]() { return oAudioSystem.PlaybackCount() > 0; });
                nBorrowedPlays = oAudioSystem.PlaybackCount();
                oHost.Stop();
            }
            sBorrowedStderr = oCapture.Release();
        }

        Assert::AreEqual(size_t(1), nBorrowedPlays, L"control: the borrowed decode did not finish");
        Assert::IsFalse(FindFFmpegDump(sBorrowedStderr).empty(),
                        L"control: a borrowed decode printed no FFmpeg dump, so the owned check cannot fail here");

        std::string sOwnedStderr;
        size_t nOwnedPlays = 0;
        {
            StderrCapture oCapture(oDirectory.Path() + "/owned.err");
            {
                QtApplicationHost::Options oOptions;
                oOptions.fProbe = []() { return DisplayProbeResult{true, "test: offscreen"}; };
                oOptions.vArguments = {"-platform", "offscreen"};
                QtApplicationHost oHost(oOptions);
                oHost.Start();
                Assert::IsTrue(oHost.GetMode() == QtApplicationHost::Mode::Owned, L"the host did not start owned");

                // Invoke queues the play on the host's own thread; nothing here
                // turns an event loop.
                QtAudioSystem oAudioSystem(oHost);
                oAudioSystem.PlayAudioFile(L"silence.wav");
                const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                while (oAudioSystem.PlaybackCount() == 0 && std::chrono::steady_clock::now() < tDeadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                nOwnedPlays = oAudioSystem.PlaybackCount();
                oHost.Stop();
            }
            sOwnedStderr = oCapture.Release();
        }

        Assert::AreEqual(size_t(1), nOwnedPlays, L"the owned decode did not finish, so there was nothing to check");
        Assert::AreEqual(std::string(), FindFFmpegDump(sOwnedStderr),
                         L"an owned decode printed FFmpeg's media dump to stderr");
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
