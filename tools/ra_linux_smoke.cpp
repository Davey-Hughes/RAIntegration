// End-to-end check for the Linux service implementations. The unit suite runs
// against mocks; this is the only thing that proves the real ones work - and
// for the Qt services it is the only thing that can, because they need a live
// QGuiApplication event loop that the headless test binary never has.
//
//   ./build-tests/ra_linux_smoke
//
// Everything it writes goes under RACache/ next to the binary (IFileSystem
// resolves relative paths against the executable's directory, as it does on
// Windows), so copy the binary somewhere scratch if you would rather it did
// not write into the build tree.
//
// Credentials are optional. Set RA_USERNAME and RA_PASSWORD to include the
// login check; without them that one step reports skip and the rest still run.
// Neither value is ever printed - RcClient's own [redacted] handling is not
// touched, and nothing here echoes the environment.
//
// Result markers:
//   [ok]   asserted, and true
//   [FAIL] asserted, and false - the process exits non-zero
//   [skip] not run, with the reason
//   [obs]  observed and reported, but NOT asserted, because the property
//          cannot be decided from inside this process - whether a sound was
//          audible, whether the compositor accepted a clipboard offer. A
//          check that always passes would be worse than no check, so these
//          deliberately do not count towards the result.

#include "services/Initialization.hh"
#include "services/ServiceLocator.hh"

#include "services/IAudioSystem.hh"
#include "services/IClipboard.hh"
#include "services/IDebuggerDetector.hh"
#include "services/IFileSystem.hh"
#include "services/IHttpRequester.hh"
#include "services/ILogger.hh"

#include "services/Http.hh"
#include "services/HttpErrorCodes.hh"
#include "services/impl/StringTextWriter.hh"

#include "util/Strings.hh"

#include "RA_Defs.h" // RA_DIR_SEP_L, RA_DIR_BASE, RA_DIR_OVERLAY

#include "RAInterface/RA_Emulators.h"

#include <QEventLoop>
#include <QGuiApplication>
#include <QTimer>

#include <dirent.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <functional>
#include <string>
#include <thread>

static int g_nFailures = 0;
static int g_nPassed = 0;
static int g_nSkipped = 0;
static int g_nObserved = 0;

static void Check(bool bCondition, const char* sLabel, const std::string& sDetail)
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

static void Skip(const char* sLabel, const std::string& sReason)
{
    ++g_nSkipped;
    std::printf("  [skip] %-34s %s\n", sLabel, sReason.c_str());
    std::fflush(stdout);
}

// Reported, never asserted - see the header comment.
static void Observe(const char* sLabel, const std::string& sDetail)
{
    ++g_nObserved;
    std::printf("  [obs]  %-34s %s\n", sLabel, sDetail.c_str());
    std::fflush(stdout);
}

static void Section(const char* sName) { std::printf("\n%s\n", sName); }

// Every network step is already bounded by libcurl (30s to connect, 30s of
// stalled transfer), but nothing bounds a wedged event loop or a Qt call that
// never returns, and a harness that hangs reports nothing at all. This is the
// outer bound: it fails loudly instead of waiting.
static void StartWatchdog(unsigned int nSeconds)
{
    std::thread([nSeconds]() {
        std::this_thread::sleep_for(std::chrono::seconds(nSeconds));
        std::printf("  [FAIL] %-34s no result after %u seconds\n", "watchdog", nSeconds);
        std::printf("FAILURES\n");
        std::fflush(stdout);
        std::fflush(stderr);
        std::_Exit(2);
    }).detach();
}

// --- audio instrumentation --------------------------------------------------
//
// QtAudioSystem keeps its QSoundEffects in a private pool, so nothing out here
// can count them directly. What is countable is the cost the audio backend
// pays for each effect that is actually playing: on Qt 6.11 over PipeWire, one
// sound in flight costs 24 open descriptors and N simultaneous sounds cost
// 23 + N, i.e. exactly one descriptor per concurrent stream on top of a fixed
// per-burst backend cost. Measuring one sound first and subtracting turns that
// into a count of how many of a burst were really playing at once, which is
// the only external evidence for the overlap guarantee.
//
// The fixed cost is a Qt implementation detail and may differ elsewhere, so
// this is calibrated at runtime rather than hard-coded, and reports [obs]
// instead of a verdict if the calibration finds no signal to measure.

static int OpenFileDescriptors() noexcept
{
    DIR* pDirectory = opendir("/proc/self/fd");
    if (pDirectory == nullptr)
        return -1;

    int nCount = 0;
    while (readdir(pDirectory) != nullptr)
        ++nCount;

    closedir(pDirectory);
    return nCount - 2; // "." and ".."
}

// Runs the event loop for a while without returning to the caller's stack.
// The Qt services only do their work on the application thread, so the checks
// have to let that thread run between the call and the result.
static void Pump(int nMilliseconds)
{
    QEventLoop oLoop;
    QTimer::singleShot(nMilliseconds, &oLoop, [&oLoop]() { oLoop.quit(); });
    oLoop.exec();
}

struct AudioMeasurement
{
    int nBase = 0;
    int nPeak = 0;
    int nFinal = 0;

    int Delta() const noexcept { return nPeak - nBase; }
    int Residue() const noexcept { return nFinal - nBase; }
};

static AudioMeasurement MeasureAudio(const std::function<void()>& fFire, int nPlayMs = 1400,
                                     int nSettleMs = 1000)
{
    AudioMeasurement oMeasurement;
    oMeasurement.nBase = OpenFileDescriptors();

    fFire();

    oMeasurement.nPeak = oMeasurement.nBase;
    for (int nElapsed = 0; nElapsed < nPlayMs; nElapsed += 50)
    {
        Pump(50);
        oMeasurement.nPeak = std::max(oMeasurement.nPeak, OpenFileDescriptors());
    }

    Pump(nSettleMs);
    oMeasurement.nFinal = OpenFileDescriptors();
    return oMeasurement;
}

// A short, quiet tone. Overlay/*.wav ships in the Windows release zip and is
// almost never present next to a Linux build, but the overlap and reap checks
// need something real to play, so generate one.
static bool WriteTestWav(const std::string& sPath, double fSeconds)
{
    constexpr uint32_t nSampleRate = 44100;
    const auto nSamples = static_cast<uint32_t>(nSampleRate * fSeconds);
    const uint32_t nDataBytes = nSamples * 2;

    std::ofstream oFile(sPath, std::ios::binary | std::ios::trunc);
    if (!oFile)
        return false;

    const auto Put32 = [&oFile](uint32_t n) {
        const char c[4]{gsl::narrow_cast<char>(n & 0xFF), gsl::narrow_cast<char>((n >> 8) & 0xFF),
                        gsl::narrow_cast<char>((n >> 16) & 0xFF), gsl::narrow_cast<char>((n >> 24) & 0xFF)};
        oFile.write(c, 4);
    };
    const auto Put16 = [&oFile](uint16_t n) {
        const char c[2]{gsl::narrow_cast<char>(n & 0xFF), gsl::narrow_cast<char>((n >> 8) & 0xFF)};
        oFile.write(c, 2);
    };

    oFile.write("RIFF", 4);
    Put32(36 + nDataBytes);
    oFile.write("WAVEfmt ", 8);
    Put32(16);                 // fmt chunk size
    Put16(1);                  // PCM
    Put16(1);                  // mono
    Put32(nSampleRate);
    Put32(nSampleRate * 2);    // byte rate
    Put16(2);                  // block align
    Put16(16);                 // bits per sample
    oFile.write("data", 4);
    Put32(nDataBytes);

    for (uint32_t i = 0; i < nSamples; ++i)
    {
        const double fFade = 1.0 - (static_cast<double>(i) / nSamples);
        const double fValue = std::sin(2.0 * 3.14159265358979 * 440.0 * i / nSampleRate) * fFade * 2600.0;
        Put16(static_cast<uint16_t>(static_cast<int16_t>(fValue)));
    }

    return oFile.good();
}

static void RunChecks()
{
    using namespace ra::services;

    Initialization::RegisterServices(RA_Libretro, "RASmoke");

    // --- filesystem -------------------------------------------------------
    Section("filesystem, logger, debugger detector");
    const auto& pFileSystem = ServiceLocator::Get<IFileSystem>();
    const std::wstring sBase = pFileSystem.BaseDirectory();
    Check(!sBase.empty() && sBase.back() == L'/', "BaseDirectory", ra::util::String::Narrow(sBase));

    // --- logger -----------------------------------------------------------
    ServiceLocator::Get<ILogger>().LogMessage(LogLevel::Info, "smoke test");
    const std::wstring sLogPath = sBase + RA_DIR_BASE L"RALog.txt";
    Check(pFileSystem.GetFileSize(sLogPath) > 0, "log written", ra::util::String::Narrow(sLogPath));

    // --- debugger detector ------------------------------------------------
    Check(!ServiceLocator::Get<IDebuggerDetector>().IsDebuggerPresent(), "no debugger attached",
          "TracerPid == 0");

    Section("http");

    // --- http: unauthenticated GET ---------------------------------------
    {
        const auto& pHttp = ServiceLocator::Get<IHttpRequester>();
        Http::Request oRequest("https://retroachievements.org/API/API_GetAchievementCount.php");
        impl::StringTextWriter oWriter;

        const auto tStart = std::chrono::steady_clock::now();
        const unsigned int nStatus = pHttp.Request(oRequest, oWriter);
        const auto nElapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tStart)
                .count();

        // any answer from the server proves TLS, DNS, redirects and the write
        // callback; the endpoint rejects the unauthenticated call by design
        Check(nStatus >= 200 && nStatus < 500, "https round trip",
              "status " + std::to_string(nStatus) + " " + pHttp.GetStatusCodeText(nStatus) + ", " +
                  std::to_string(nElapsed) + "ms");
    }

    // --- http: negative control ------------------------------------------
    {
        const auto& pHttp = ServiceLocator::Get<IHttpRequester>();
        Http::Request oRequest("http://127.0.0.1:1/");
        impl::StringTextWriter oWriter;
        const unsigned int nStatus = pHttp.Request(oRequest, oWriter);

        Check(nStatus == RA_HTTP_ERROR_CANNOT_CONNECT && pHttp.IsRetryable(nStatus),
              "refused port is retryable", "status " + std::to_string(nStatus));
    }

    // --- http: download to the cache -------------------------------------
    {
        Http::Request oRequest("https://media.retroachievements.org/Badge/00000.png");
        const std::wstring sTarget = RA_DIR_BADGE L"smoke.png";
        pFileSystem.CreateDirectory(L"RACache");
        pFileSystem.CreateDirectory(RA_DIR_BASE L"Badge");

        pFileSystem.DeleteFile(sTarget);
        const auto oResponse = oRequest.Download(sTarget);
        const int64_t nSize = pFileSystem.GetFileSize(sTarget);

        Check(nSize > 0, "badge downloaded",
              std::to_string(nSize) + " bytes, status " +
                  std::to_string(static_cast<unsigned int>(oResponse.StatusCode())));
    }

    // --- login ------------------------------------------------------------
    {
        const char* sUser = std::getenv("RA_USERNAME");
        const char* sPassword = std::getenv("RA_PASSWORD");
        if (sUser == nullptr || sPassword == nullptr)
        {
            Skip("login", "set RA_USERNAME and RA_PASSWORD to include this");
        }
        else
        {
            Http::Request oRequest("https://retroachievements.org/dorequest.php");
            oRequest.AddQueryParm("r", "login2");
            oRequest.AddQueryParm("u", sUser);
            oRequest.AddQueryParm("p", sPassword);
            impl::StringTextWriter oWriter;

            const unsigned int nStatus = ServiceLocator::Get<IHttpRequester>().Request(oRequest, oWriter);
            const bool bSucceeded =
                (nStatus == 200) && oWriter.GetString().find("\"Success\":true") != std::string::npos;

            // deliberately only the status code - the response body carries the
            // account's API token and the request carried the password
            Check(bSucceeded, "login", "status " + std::to_string(nStatus));
        }
    }

    Section("clipboard");
    {
        const auto& pClipboard = ServiceLocator::Get<IClipboard>();

        // this is the live desktop clipboard, not a scratch buffer - put back
        // whatever the user had in it
        const std::wstring sOriginal = pClipboard.GetText();

        // --- round trip, including non-ASCII and astral-plane text ---------
        // also exercises Widen/Narrow, which is where the wchar_t bug lived
        const std::wstring sExpected = L"RA smoke café \U0001F30F";
        pClipboard.SetText(sExpected);
        const std::wstring sActual = pClipboard.GetText();
        Check(sActual == sExpected, "clipboard round trip", ra::util::String::Narrow(sActual));

        // --- does the desktop actually see it? ----------------------------
        // Setting the selection and reading it straight back only proves Qt's
        // own copy. Whether the compositor accepted the offer is not decidable
        // from in here, so let the event loop turn and report what survives.
        Pump(400);
        const bool bSurvived = (pClipboard.GetText() == sExpected);
        Observe("clipboard offer accepted",
                std::string(bSurvived ? "yes, the desktop holds it" : "no, the offer was dropped") +
                    " (platform " + QGuiApplication::platformName().toStdString() +
                    "); a windowless application cannot own the Wayland selection, so this is "
                    "expected to say no until there is a window to own it");

        // --- the wrong-thread branch --------------------------------------
        // No view model ever reaches the clipboard from a worker thread, but
        // nothing stops one, and QClipboard from off the GUI thread is either
        // a crash or a deadlock. QtClipboard is supposed to log and return.
        const std::wstring sGuard = L"RA smoke guard";
        pClipboard.SetText(sGuard);

        std::wstring sFromWorker = L"<never ran>";
        bool bWorkerReturned = false;
        std::thread oWorker([&pClipboard, &sFromWorker, &bWorkerReturned]() {
            pClipboard.SetText(L"RA smoke WRONG THREAD");
            sFromWorker = pClipboard.GetText();
            bWorkerReturned = true;
        });
        oWorker.join();

        Check(bWorkerReturned && sFromWorker.empty(), "clipboard off-thread read refused",
              bWorkerReturned ? "returned empty without touching QClipboard" : "never returned");
        Check(pClipboard.GetText() == sGuard, "clipboard off-thread write refused",
              "clipboard still holds the guard value");

        pClipboard.SetText(sOriginal);
    }

    Section("audio");
    {
        const auto& pAudio = ServiceLocator::Get<IAudioSystem>();

        // --- the asset the product actually plays -------------------------
        const std::wstring sUnlock = RA_DIR_OVERLAY L"unlock.wav";
        if (pFileSystem.GetFileSize(sUnlock) < 0)
        {
            Skip("overlay unlock.wav",
                 ra::util::String::Narrow(sBase + sUnlock) + " not present (ships in the Windows zip)");
        }
        else
        {
            pAudio.PlayAudioFile(sUnlock);
            Pump(1500);
            Observe("overlay unlock.wav", "dispatched; whether it was audible is not decidable here");
        }

        // --- something real to play ---------------------------------------
        const std::wstring sWav = RA_DIR_BASE L"smoke-tone.wav";
        const std::string sWavPath = ra::util::String::Narrow(sBase + sWav);
        const bool bWavWritten = WriteTestWav(sWavPath, 0.70);
        Check(bWavWritten && pFileSystem.GetFileSize(sWav) > 44, "test tone generated", sWavPath);

        if (!bWavWritten)
        {
            Skip("audio behaviour", "no tone to play");
        }
        else
        {
            // First burst also warms the backend up - the very first sound in
            // a process costs more descriptors than any after it.
            MeasureAudio([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });

            // --- calibration ----------------------------------------------
            const auto oOne = MeasureAudio([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
            const int nOneSound = oOne.Delta();

            // --- PlayAudioFile from a worker thread ------------------------
            // AchievementRuntime plays unlock sounds from the frame thread, so
            // this is the real call path, not a contrived one. A QSoundEffect
            // constructed off the application thread would not play at all.
            const auto oWorker = MeasureAudio([&pAudio, &sWav]() {
                std::thread oThread([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
                oThread.join();
            });
            if (nOneSound <= 0)
            {
                Observe("PlayAudioFile off the GUI thread",
                        "no descriptor signal to measure against; dispatched without crashing");
            }
            else
            {
                Check(oWorker.Delta() >= nOneSound / 2, "PlayAudioFile off the GUI thread",
                      "backend engaged (" + std::to_string(oWorker.Delta()) + " vs " +
                          std::to_string(nOneSound) + " for a GUI-thread call)");
            }

            // --- the overlap guarantee -------------------------------------
            // Simultaneous unlocks queue several PlayAudioFile calls into one
            // pass of the event loop. A reap that cannot tell "still loading"
            // from "finished" destroys each sound as the next arrives and only
            // the last one is ever heard.
            constexpr int nBurst = 5;
            const auto oBurst = MeasureAudio([&pAudio, &sWav]() {
                for (int i = 0; i < nBurst; ++i)
                    pAudio.PlayAudioFile(sWav);
            });

            if (nOneSound <= 0)
            {
                Observe("simultaneous unlocks all play",
                        "not decidable: no per-stream descriptor signal on this backend");
            }
            else
            {
                const int nConcurrent = oBurst.Delta() - nOneSound + 1;
                Check(nConcurrent >= nBurst, "simultaneous unlocks all play",
                      std::to_string(nConcurrent) + " of " + std::to_string(nBurst) +
                          " streams concurrent (one sound = " + std::to_string(nOneSound) +
                          " fds, burst = " + std::to_string(oBurst.Delta()) + ")");
            }

            Check(oBurst.Residue() <= 0, "burst reaped after playing",
                  std::to_string(oBurst.Residue()) + " descriptors left over");

            // --- a realistic sequence, for unbounded growth -----------------
            const int nSequenceBase = OpenFileDescriptors();
            for (int nWave = 0; nWave < 4; ++nWave)
            {
                for (int i = 0; i < 3; ++i)
                    pAudio.PlayAudioFile(sWav);
                Pump(800);
            }
            Pump(1500);
            const int nSequenceEnd = OpenFileDescriptors();
            Check(nSequenceEnd <= nSequenceBase, "12 sounds leave nothing behind",
                  std::to_string(nSequenceBase) + " -> " + std::to_string(nSequenceEnd) + " descriptors");

            // --- a path that cannot load ------------------------------------
            // Never starts playing, so the finished-playing handler never fires
            // for it; only the error handler can reap it.
            const auto oBad = MeasureAudio(
                [&pAudio]() { pAudio.PlayAudioFile(RA_DIR_BASE L"no-such-sound.wav"); }, 600, 600);
            Check(oBad.Residue() <= 0, "missing file reaped, no crash",
                  std::to_string(oBad.Residue()) + " descriptors left over");

            // --- teardown with sound in flight ------------------------------
            // A queued create on the application thread and a call still inside
            // PlayAudioFile on a worker, both racing Shutdown().
            std::thread oLate([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
            pAudio.PlayAudioFile(sWav);
            pAudio.PlayAudioFile(sWav);
            oLate.join();
        }
    }

    Section("shutdown");
    Initialization::Shutdown();
    Check(!Initialization::IsInitialized(), "shutdown with audio in flight",
          "teardown completed with queued sounds outstanding");
}

int main(int argc, char* argv[])
{
    QGuiApplication oApplication(argc, argv);

    std::printf("ra_linux_smoke\n");
    std::fflush(stdout);

    StartWatchdog(240);

    // run once the event loop is live, so the clipboard and audio paths see a
    // fully constructed application
    QTimer::singleShot(0, &oApplication, []() {
        RunChecks();

        // let queued sound effects run past the teardown before exiting
        QTimer::singleShot(1500, qApp, []() {
            std::printf("\nra_linux_smoke: %d ok, %d failed, %d skipped, %d observed\n", g_nPassed,
                        g_nFailures, g_nSkipped, g_nObserved);
            std::printf("%s\n", g_nFailures == 0 ? "all checks passed" : "FAILURES");
            std::fflush(stdout);
            qApp->exit(g_nFailures == 0 ? 0 : 1);
        });
    });

    return oApplication.exec();
}
