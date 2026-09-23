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
// Neither value is ever printed, and neither reaches RACache/RALog.txt: the
// login goes out as POST data the way RcClient sends it, so the password is
// not part of any URL that a transport-level failure could log.
//
// Do not run this under QT_QPA_PLATFORM=offscreen. Qt's offscreen platform
// answers clipboard reads out of an in-process buffer, so the desktop-facing
// clipboard checks would report success without a desktop being involved;
// they report [skip] on that platform instead of a result they cannot back up.
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
//   [note] something the run is about to do to the machine it is running on.
//          Not a result at all, and counted as nothing.

#include "Exports.hh" // _RA_InitClientOffline, _RA_DoAchievementsFrame

#include "services/Initialization.hh"
#include "services/ServiceLocator.hh"

#include "services/FrameEventQueue.hh"
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
#include <iterator>
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

// Not a result - a warning about what the run is about to do to this machine.
// Counted as nothing, so it cannot dilute the tally either way.
static void Note(const char* sLabel, const std::string& sDetail)
{
    std::printf("  [note] %-34s %s\n", sLabel, sDetail.c_str());
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
// pays while sounds are actually playing: a burst of N simultaneous sounds
// holds some fixed number of descriptors plus some number more per concurrent
// stream. Recovering N from that total is the only external evidence for the
// overlap guarantee.
//
// Both halves of the cost are backend implementation details, so neither is
// hard-coded and neither is assumed. They are separated by calibrating at two
// points - one sound, then two - because a single point cannot tell them
// apart. It yields only a total, and turning a total into a count means
// assuming a marginal cost; assume one descriptor per stream, as this did
// before, and a backend that shares descriptors between streams produces a
// confident wrong answer instead of a failure. The marginal cost is the
// difference between the two calibration points, and the fixed cost is what
// is left of the one-sound measurement once the marginal is taken out.
//
// Where the measured marginal cost is not at least one descriptor - a backend
// that pools them, or a machine too noisy to measure on - there is no
// per-stream signal to count with, and the overlap step reports [obs] rather
// than a verdict it cannot support.

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

// Pumps until the descriptor count stops moving, and returns it. Sounds from
// an earlier step can still be draining when the next one starts; counting
// those into its baseline puts the baseline above the real idle level, and
// everything measured against it then comes out low - which is how a residue
// of -23 descriptors, a number that cannot describe anything the step under
// test did, passed a `<= 0` assertion while tracking something else entirely.
static int QuiesceFileDescriptors(int nMaxMs = 4000)
{
    int nLast = OpenFileDescriptors();
    int nStable = 0;

    for (int nElapsed = 0; nElapsed < nMaxMs; nElapsed += 100)
    {
        Pump(100);

        const int nNow = OpenFileDescriptors();
        if (nNow != nLast)
        {
            nLast = nNow;
            nStable = 0;
        }
        else if (++nStable == 3)
        {
            break;
        }
    }

    return nLast;
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
    oMeasurement.nBase = QuiesceFileDescriptors();

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

    Section("entry point");

    // Deliberately the emulator-facing export rather than
    // Initialization::RegisterServices: on a non-test build the registration
    // sits behind Exports.cpp's `#ifndef RA_UTEST`, and that guard is a
    // judgement call this port had to make eleven times over. Calling
    // RegisterServices directly routes around all of them, so the whole
    // harness can stay green with service registration and the per-frame UI
    // updates compiled out. Offline, because a harness should not need an
    // account to start; every network step below still goes to the real
    // server through IHttpRequester.
    //
    // If the guard were mis-set this would not reach the Check - InitCommon
    // touches the EmulatorContext immediately after registering it, and
    // ServiceLocator::GetMutable on a service that was never provided ends the
    // process. Either way the run does not come back green.
    const int nInitialised = _RA_InitClientOffline(nullptr, "RASmoke", "0.0.0.0");
    Check(nInitialised == 1 && Initialization::IsInitialized() &&
              ServiceLocator::Exists<IHttpRequester>(),
          "_RA_InitClientOffline", "returned " + std::to_string(nInitialised) + ", services registered");

    // --- filesystem -------------------------------------------------------
    Section("filesystem, logger, debugger detector");
    const auto& pFileSystem = ServiceLocator::Get<IFileSystem>();
    const std::wstring sBase = pFileSystem.BaseDirectory();
    Check(!sBase.empty() && sBase.back() == L'/', "BaseDirectory", ra::util::String::Narrow(sBase));

    // --- logger -----------------------------------------------------------
    // The logger appends, and initialization has already written a dozen lines
    // by now, so "the file is not empty" says nothing about this call - it
    // would hold just as well against an ILogger::LogMessage that did nothing
    // at all. Note where the file ends first, then look for the marker past
    // that point.
    const std::wstring sLogPath = sBase + RA_DIR_BASE L"RALog.txt";
    const int64_t nLogSizeBefore = std::max<int64_t>(pFileSystem.GetFileSize(sLogPath), 0);
    constexpr const char* sLogMarker = "smoke test log marker";
    ServiceLocator::Get<ILogger>().LogMessage(LogLevel::Info, sLogMarker);

    std::string sLogTail;
    {
        std::ifstream oLog(ra::util::String::Narrow(sLogPath), std::ios::binary);
        if (oLog)
        {
            oLog.seekg(static_cast<std::streamoff>(nLogSizeBefore));
            sLogTail.assign(std::istreambuf_iterator<char>(oLog), std::istreambuf_iterator<char>());
        }
    }

    Check(sLogTail.find(sLogMarker) != std::string::npos, "log written",
          ra::util::String::Narrow(sLogPath) + ", +" + std::to_string(sLogTail.length()) +
              " bytes past " + std::to_string(nLogSizeBefore));

    // --- debugger detector ------------------------------------------------
    // Worth being explicit about how little this proves: nothing here is being
    // traced, so a hardcoded `return false` would pass it exactly as well as
    // the real TracerPid parse. It is a no-crash check, not a behaviour check.
    // There is no positive control and no cheap way to add one -
    // LinuxDebuggerDetector reads /proc/self/status by a fixed path, so
    // staging a traced process means forking a child to PTRACE_ATTACH back to
    // this one, which Yama's default ptrace_scope refuses.
    Check(!ServiceLocator::Get<IDebuggerDetector>().IsDebuggerPresent(), "no debugger attached",
          "TracerPid == 0 (no positive control - a stubbed false passes this too)");

    // --- the per-frame export ---------------------------------------------
    // _RA_DoAchievementsFrame's call to UpdateUIForFrameChange is the other
    // #ifndef RA_UTEST that this port had to re-judge, and losing it costs
    // every per-frame view-model update silently. FrameEventQueue::DoFrame is
    // the last thing UpdateUIForFrameChange does, and a queued function is the
    // one part of it observable without a game or a window.
    bool bFrameEventRan = false;
    ServiceLocator::GetMutable<FrameEventQueue>().QueueFunction(
        [&bFrameEventRan]() { bFrameEventRan = true; });
    _RA_DoAchievementsFrame();
    Check(bFrameEventRan, "_RA_DoAchievementsFrame updates UI",
          bFrameEventRan ? "the queued frame event ran, so UpdateUIForFrameChange was called"
                         : "the queued frame event never ran - UpdateUIForFrameChange was not called");

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

        // any answer from the server proves TLS, DNS and redirects; the
        // endpoint rejects the unauthenticated call by design. The body has to
        // be looked at for this to say anything about the write callback - a
        // status code arrives whether or not a single byte reached the writer.
        Check(nStatus >= 200 && nStatus < 500 && !oWriter.GetString().empty(), "https round trip",
              "status " + std::to_string(nStatus) + " " + pHttp.GetStatusCodeText(nStatus) + ", " +
                  std::to_string(oWriter.GetString().length()) + " bytes written, " +
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
        pFileSystem.CreateDirectory(RA_DIR_BASE);
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
            // POST data, not query parameters. That is what production does -
            // RcClient hands rc_api_request_t's post_data to SetPostData, and
            // never builds a URL with a credential in it - and it is what
            // keeps the password away from everything that handles a URL as a
            // whole: LinuxHttpRequester logs one when curl fails (stripped of
            // its query string now, for exactly this reason), curl prints one
            // in a verbose trace, and a proxy or a crash dump records one.
            // Putting it in the query string here made this the only place in
            // the tree where a credential could reach any of those, and it
            // duly did.
            Http::Request oRequest("https://retroachievements.org/dorequest.php");
            std::string sPostData = "r=login2&u=";
            Http::UrlEncodeAppend(sPostData, sUser);
            sPostData += "&p=";
            Http::UrlEncodeAppend(sPostData, sPassword);
            oRequest.SetPostData(sPostData);
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

        // Qt's offscreen platform implements QPlatformClipboard as a buffer
        // inside this process. Both checks below would pass on it, and the
        // second would report that the desktop holds the text on a machine
        // with no desktop at all - a stronger result from the environment that
        // proves less, which is exactly the shape a CI runner defaults to.
        const std::string sPlatform = QGuiApplication::platformName().toStdString();
        const bool bOffscreen = (sPlatform == "offscreen");

        // this is the live desktop clipboard, not a scratch buffer - put back
        // whatever the user had in it. Only the text of it: IClipboard has no
        // way to carry an image or a file list, so a non-text selection is
        // lost either way, and this says so rather than losing it quietly.
        const std::wstring sOriginal = pClipboard.GetText();
        if (!bOffscreen)
        {
            Note("clipboard will be overwritten",
                 "the desktop selection is saved and restored as text; anything else in it "
                 "(an image, a file list, rich text) does not survive this run");
        }

        // --- round trip, including non-ASCII and astral-plane text ---------
        // also exercises Widen/Narrow, which is where the wchar_t bug lived
        const std::wstring sExpected = L"RA smoke café \U0001F30F";
        if (bOffscreen)
        {
            Skip("clipboard round trip", "platform offscreen answers from an in-process buffer, so "
                                         "this would pass without a desktop clipboard");
            Skip("clipboard offer accepted", "platform offscreen has no compositor to accept one");
        }
        else
        {
            pClipboard.SetText(sExpected);
            const std::wstring sActual = pClipboard.GetText();
            Check(sActual == sExpected, "clipboard round trip", ra::util::String::Narrow(sActual));

            // --- does the desktop actually see it? ------------------------
            // Setting the selection and reading it straight back only proves
            // Qt's own copy. Whether the compositor accepted the offer is not
            // decidable from in here, so let the event loop turn and report
            // what survives.
            Pump(400);
            const bool bSurvived = (pClipboard.GetText() == sExpected);
            std::string sDetail =
                std::string(bSurvived ? "yes, the desktop holds it" : "no, the offer was dropped") +
                " (platform " + sPlatform + ")";
            if (sPlatform == "wayland")
            {
                sDetail += "; a windowless application cannot own the Wayland selection, so this is "
                           "expected to say no until there is a window to own it";
            }

            Observe("clipboard offer accepted", sDetail);
        }

        // --- the wrong-thread branch --------------------------------------
        // No view model ever reaches the clipboard from a worker thread, but
        // nothing stops one, and QClipboard from off the GUI thread is either
        // a crash or a deadlock. QtClipboard is supposed to log and return.
        // This one does run under offscreen: it is QtClipboard's own thread
        // guard being checked, not the platform's clipboard.
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
            // Two points, because one cannot separate the backend's fixed
            // per-burst cost from its per-stream cost. See the note above
            // MeasureAudio.
            const auto oOne = MeasureAudio([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
            const int nOneSound = oOne.Delta();

            const auto oTwo = MeasureAudio([&pAudio, &sWav]() {
                pAudio.PlayAudioFile(sWav);
                pAudio.PlayAudioFile(sWav);
            });
            const int nMarginal = oTwo.Delta() - nOneSound;
            const int nFixed = nOneSound - nMarginal;

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

            const std::string sCalibration =
                "1 sound = " + std::to_string(nOneSound) + " fds, 2 = " +
                std::to_string(oTwo.Delta()) + ", so marginal = " + std::to_string(nMarginal) +
                " and fixed = " + std::to_string(nFixed) + "; burst = " +
                std::to_string(oBurst.Delta());

            if (nMarginal < 1)
            {
                // A backend that pools descriptors across streams lands here,
                // and so does a machine too noisy to calibrate on. Counting
                // anyway would have divided by an assumed marginal of one and
                // reported a number; there is nothing to count with.
                Observe("simultaneous unlocks all play",
                        "not decidable: no per-stream descriptor signal on this backend (" +
                            sCalibration + ")");
            }
            else
            {
                const int nConcurrent = (oBurst.Delta() - nFixed) / nMarginal;
                Check(nConcurrent >= nBurst, "simultaneous unlocks all play",
                      std::to_string(nConcurrent) + " of " + std::to_string(nBurst) +
                          " streams concurrent (" + sCalibration + ")");
            }

            // Zero, not "not more than zero". A negative residue is not a
            // tidier version of a clean one; it means the baseline included
            // descriptors this step never owned, and a measurement that is not
            // tracking its subject cannot report on it either way.
            Check(oBurst.Residue() == 0, "burst reaped after playing",
                  std::to_string(oBurst.Residue()) + " descriptors left over");

            // --- a realistic sequence, for unbounded growth -----------------
            const int nSequenceBase = QuiesceFileDescriptors();
            for (int nWave = 0; nWave < 4; ++nWave)
            {
                for (int i = 0; i < 3; ++i)
                    pAudio.PlayAudioFile(sWav);
                Pump(800);
            }
            Pump(1500);
            const int nSequenceEnd = QuiesceFileDescriptors();
            Check(nSequenceEnd == nSequenceBase, "12 sounds leave nothing behind",
                  std::to_string(nSequenceBase) + " -> " + std::to_string(nSequenceEnd) + " descriptors");

            // --- a path that cannot load ------------------------------------
            // Never starts playing, so the finished-playing handler never fires
            // for it; only the error handler can reap it.
            const auto oBad = MeasureAudio(
                [&pAudio]() { pAudio.PlayAudioFile(RA_DIR_BASE L"no-such-sound.wav"); }, 600, 600);
            Check(oBad.Residue() == 0, "missing file reaped, no crash",
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

    // Shutdown() clears the flag unconditionally, so what is actually being
    // asserted is that the call returned at all: no crash, no deadlock and no
    // exception with a queued QSoundEffect create still on the application
    // thread and a PlayAudioFile still on a worker. That is the property worth
    // having here; the flag is only how the statement is spelled.
    Initialization::Shutdown();
    Check(!Initialization::IsInitialized(), "shutdown with audio in flight",
          "Shutdown() returned with queued sounds outstanding - no crash, no hang");
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
