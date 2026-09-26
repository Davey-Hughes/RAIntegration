// End-to-end check for the Linux service implementations. The unit suite runs
// against mocks; this is the only thing that proves the real ones work - and
// for the Qt services it is the only thing that can, because they need a live
// QGuiApplication event loop that the headless test binary never has.
//
//   ./build-tests/ra_linux_smoke
//
// Everything it writes goes next to the binary - RAPrefs_RASmoke.cfg beside
// it, the rest under RACache/ (IFileSystem resolves relative paths against the
// executable's directory, as it does on Windows) - so copy the binary
// somewhere scratch if you would rather it did not write into the build tree.
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

#include "Exports.hh" // _RA_InitClientOffline, _RA_DoAchievementsFrame, _RA_Shutdown

#include "services/Initialization.hh"
#include "services/ServiceLocator.hh"

#include "services/FrameEventQueue.hh"
#include "services/IAudioSystem.hh"
#include "services/IClipboard.hh"
#include "services/IConfiguration.hh"
#include "services/IDebuggerDetector.hh"
#include "services/IFileSystem.hh"
#include "services/IHttpRequester.hh"
#include "services/ILogger.hh"

#include "context/IRcClient.hh"
#include "data/context/GameContext.hh"
#include "data/context/EmulatorContext.hh"

// The reset check reads the trigger rcheevos keeps for an achievement, which
// only the internal structures expose. OfflineRcClient.cpp includes the same
// header.
#include <rcheevos/src/rc_client_external.h>
#include <rcheevos/src/rc_client_internal.h>

#include "services/Http.hh"
#include "services/HttpErrorCodes.hh"
#include "services/impl/StringTextWriter.hh"

#include "util/Strings.hh"

#include "RA_Defs.h" // RA_DIR_SEP_L, RA_DIR_BASE, RA_DIR_OVERLAY

#include "SmokeReport.hh" // Check, Observe and their counters, shared with ra_dlopen_smoke

#include <QAudioBuffer>
#include <QAudioDecoder>
#include <QAudioDevice>
#include <QAudioSink>
#include <QBuffer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMediaDevices>
#include <QTimer>
#include <QUrl>

#include <dirent.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// Check, Observe and the ok/failed/observed counters are in SmokeReport.hh.
// Skip and Note are this harness's own.
static int g_nSkipped = 0;

static void Skip(const char* sLabel, const std::string& sReason)
{
    ++g_nSkipped;
    std::printf("  [skip] %-34s %s\n", sLabel, sReason.c_str());
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
// QtAudioSystem keeps its sinks in a private pool, so nothing out here can
// count them directly. What is countable is the cost the audio backend
// pays while sounds are actually playing: a burst of N simultaneous sounds
// holds some fixed number of descriptors plus some number more per concurrent
// stream. Recovering N from that total is the only external evidence that a
// burst opened a stream for every sound in it.
//
// That is all it shows. N is read at the burst's peak, and a destroyed
// QAudioSink keeps its descriptors (about 22) for a further 200-250 ms, so a
// sound cut off early - ended as the next one starts - still counts at the
// peak as if it were playing: a library that does that to every sound of a
// burst of five still scores "5 of 5, burst = 110". What the count does catch
// is a play that never produces a sink: a library that drops a play arriving
// while another sound plays scores "1 of 5, burst = 22". The cut-off case is a
// known gap, and deliberately left uncovered: a check for it would have to be
// scored on timing - the level after that 200-250 ms tail but before the tone
// ends - on a desktop whose load moves both.
//
// Both halves of the cost are backend implementation details, so neither is
// hard-coded and neither is assumed. They are separated by calibrating at two
// points - one stream, then two - because a single point cannot tell them
// apart: the difference between the two points is the marginal cost, and what
// is left of the one-stream measurement once the marginal is taken out is the
// fixed cost.
//
// The two calibration points are QAudioSinks - the class QtAudioSystem plays
// through - that this harness creates and holds itself, over PCM it decoded
// itself the way QtAudioSystem does, played for longer than the measuring
// window, never handed to QtAudioSystem and never reaped by anything - so "two
// held" really are two concurrent streams, regardless of what the reap logic
// under test does with the burst it is scored against. They were QSoundEffects
// while the library played through those; calibrating on a class the library
// no longer uses measures nothing it does (a QSoundEffect added 1 descriptor
// per stream, a QAudioSink adds about 22), and a QSoundEffect's stream carries
// media.role=Notification, which checks/chime-role.sh counts against this
// process. Calibrating through QtAudioSystem instead would make the
// measurement depend on the correctness of the thing it exists to check: a
// library that drops a play arriving while another sound plays - the failure
// the burst check exists to catch - would turn a two-sound calibration into
// one stream (its burst of five measures 22 descriptors, one stream's worth),
// the measured marginal would come out 0, and the check below would conclude
// the backend gives no signal instead of concluding the burst it just
// measured was wrong.
//
// Where the independently measured marginal cost is not at least one
// descriptor - a backend that pools them, or a machine too noisy to measure
// on - there is genuinely no per-stream signal to count the burst with, and
// the overlap step reports [obs] rather than a verdict it cannot support.

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

// The calibration's PCM: sWavPath decoded as QtAudioSystem decodes a sound -
// QAudioDecoder, straight to the default output's preferred format, or the
// file's own format when there is no device - but by this harness, so that
// nothing in QtAudioSystem is involved. oPcm is empty if the decode failed.
struct CalibrationSound
{
    QAudioFormat oFormat;
    QByteArray oPcm;
};

static CalibrationSound DecodeForCalibration(const std::string& sWavPath)
{
    CalibrationSound oSound;
    bool bDone = false;
    bool bFailed = false;

    QAudioDecoder oDecoder;
    QObject::connect(&oDecoder, &QAudioDecoder::bufferReady, &oDecoder, [&oDecoder, &oSound]() {
        const QAudioBuffer oBuffer = oDecoder.read();
        if (oBuffer.isValid())
        {
            oSound.oFormat = oBuffer.format();
            oSound.oPcm.append(oBuffer.constData<char>(), oBuffer.byteCount());
        }
    });
    QObject::connect(&oDecoder, &QAudioDecoder::finished, &oDecoder, [&bDone]() { bDone = true; });
    QObject::connect(&oDecoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &oDecoder,
                     [&bDone, &bFailed]() { bDone = bFailed = true; });

    oDecoder.setSource(QUrl::fromLocalFile(QString::fromStdString(sWavPath)));
    const QAudioFormat oPreferred = QMediaDevices::defaultAudioOutput().preferredFormat();
    if (oPreferred.isValid())
        oDecoder.setAudioFormat(oPreferred);
    oDecoder.start();

    for (int nElapsed = 0; !bDone && nElapsed < 5000; nElapsed += 20)
        Pump(20);

    if (!bDone || bFailed || !oSound.oFormat.isValid())
        oSound.oPcm.clear();

    return oSound;
}

// Independent calibration point: nCount QAudioSinks this function creates,
// starts and holds itself over oSound - not through IAudioSystem, so
// QtAudioSystem's cache, pool and reap logic never see them and cannot end one
// early. Each plays the sound repeated to last a second past the window, so
// every one of them is still playing, not drained and freed, at every sample
// point in it; nCount held sinks are therefore really nCount concurrent
// streams, which is the property the burst check needs a reference for.
static AudioMeasurement MeasureConcurrentSinks(const CalibrationSound& oSound, int nCount, int nPlayMs = 1400,
                                               int nSettleMs = 1000)
{
    QByteArray oHeld;
    const qint64 nHeldBytes = oSound.oFormat.bytesForDuration((nPlayMs + 1000) * qint64(1000));
    while (!oSound.oPcm.isEmpty() && oHeld.size() < nHeldBytes)
        oHeld.append(oSound.oPcm);

    AudioMeasurement oMeasurement;
    oMeasurement.nBase = QuiesceFileDescriptors();

    std::vector<std::unique_ptr<QAudioSink>> vSinks;
    for (int i = 0; i < nCount; ++i)
    {
        auto pSink = std::make_unique<QAudioSink>(QMediaDevices::defaultAudioOutput(), oSound.oFormat);
        auto* pBuffer = new QBuffer(pSink.get());
        pBuffer->setData(oHeld);
        pBuffer->open(QIODevice::ReadOnly);
        pSink->start(pBuffer);
        vSinks.push_back(std::move(pSink));
    }

    oMeasurement.nPeak = oMeasurement.nBase;
    for (int nElapsed = 0; nElapsed < nPlayMs; nElapsed += 50)
    {
        Pump(50);
        oMeasurement.nPeak = std::max(oMeasurement.nPeak, OpenFileDescriptors());
    }

    // Stop and destroy - this harness reaps its own reference instances
    // directly, deliberately not by the mechanism under test.
    vSinks.clear();

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

// Everything in the file past nOffset. The logger appends, so a check on its
// output has to look only at what arrived after that check's own "before".
static std::string ReadFileFrom(const std::wstring& sPath, int64_t nOffset)
{
    std::string sContents;
    std::ifstream oFile(ra::util::String::Narrow(sPath), std::ios::binary);
    if (oFile)
    {
        oFile.seekg(static_cast<std::streamoff>(nOffset));
        sContents.assign(std::istreambuf_iterator<char>(oFile), std::istreambuf_iterator<char>());
    }

    return sContents;
}

// Replaces the file's contents. The offline game fixture is written fresh on
// every run and removed afterwards.
static bool WriteTextFile(const std::wstring& sPath, const std::string& sContents)
{
    std::ofstream oFile(ra::util::String::Narrow(sPath), std::ios::binary | std::ios::trunc);
    oFile << sContents;
    return oFile.good();
}

// No header declares this. rcheevos' rc_client_raintegration.c looks it up by
// name in the loaded library, so it is declared here the way
// AchievementRuntimeExports.cpp defines it.
extern "C" int _Rcheevos_GetExternalClient(rc_client_external_t* pClientExternal, int nVersion);

// The offline game the reset section loads. The id is under
// IGameContext::IsVirtualGameId's threshold, so it loads like a real game.
static constexpr unsigned int nResetGameId = 999001;
static constexpr uint32_t nResetAchievementId = 999002;
static constexpr const char* sResetGameHash = "a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2a2";

// An achievementsets response, shaped after rcheevos' own fixture
// (test_rc_client.c, patchdata_2ach_0lbd). The one achievement needs byte 0 to
// be 1 for 100 frames, so the 3 frames the section runs build hits without
// ever unlocking it. ConsoleId 7 (NES) matches the section's _RA_SetConsoleID.
static constexpr const char* sResetGameJson =
    "{\"Success\":true,\"GameId\":999001,\"Title\":\"RASmoke reset fixture\",\"ConsoleId\":7,"
    "\"ImageIconUrl\":\"http://server/Images/112233.png\","
    "\"RichPresenceGameId\":999001,\"RichPresencePatch\":\"\",\"Sets\":[{"
    "\"AchievementSetId\":999003,\"GameId\":999001,\"Title\":null,\"Type\":\"core\","
    "\"ImageIconUrl\":\"http://server/Images/112233.png\","
    "\"Achievements\":[{\"ID\":999002,\"Title\":\"Reset fixture\","
    "\"Description\":\"Byte 0 is 1 for 100 frames\",\"Flags\":3,\"Points\":5,"
    "\"MemAddr\":\"0xH0000=1.100.\",\"Author\":\"RASmoke\",\"BadgeName\":\"00234\","
    "\"Created\":1367266583,\"Modified\":1376929305}],"
    "\"Leaderboards\":[]}]}";

// The emulator memory that achievement reads. Only byte 0 matters.
static uint8_t g_pResetMemory[16] = {};

static uint8_t ReadResetMemory(uint32_t nAddress)
{
    return nAddress < sizeof(g_pResetMemory) ? g_pResetMemory[nAddress] : 0;
}

static void WriteResetMemory(uint32_t nAddress, uint8_t nValue)
{
    if (nAddress < sizeof(g_pResetMemory))
        g_pResetMemory[nAddress] = nValue;
}

// What a reset changes: the trigger's state and its hit count. nState is -1
// when the achievement, its trigger or its first condition does not exist.
struct TriggerSnapshot
{
    int nState = -1;
    uint32_t nHits = 0;

    std::string Describe() const
    {
        if (nState < 0)
            return "not found";

        const char* sState = (nState == RC_TRIGGER_STATE_WAITING) ? "WAITING"
                           : (nState == RC_TRIGGER_STATE_ACTIVE)  ? "ACTIVE"
                                                                  : nullptr;
        return (sState ? std::string(sState) : "state " + std::to_string(nState)) + " with " +
               std::to_string(nHits) + " hits";
    }
};

static TriggerSnapshot SnapshotTrigger(uint32_t nAchievementId)
{
    TriggerSnapshot oSnapshot;

    const auto* pClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>().GetClient();
    if (!pClient || !pClient->game)
        return oSnapshot;

    for (const auto* pSubset = pClient->game->subsets; pSubset; pSubset = pSubset->next)
    {
        for (uint32_t i = 0; i < pSubset->public_.num_achievements; ++i)
        {
            const auto& pInfo = pSubset->achievements[i];
            if (pInfo.public_.id != nAchievementId)
                continue;

            const auto* pTrigger = pInfo.trigger;
            if (pTrigger && pTrigger->requirement && pTrigger->requirement->conditions)
            {
                oSnapshot.nState = pTrigger->state;
                oSnapshot.nHits = pTrigger->requirement->conditions->current_hits;
            }
            return oSnapshot;
        }
    }

    return oSnapshot;
}

static void RunChecks()
{
    using namespace ra::services;

    // RAPrefs_<name>.cfg is named after this, so the shutdown section needs it
    // as well as the entry point.
    constexpr const char* sClientName = "RASmoke";

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
    const int nInitialised = _RA_InitClientOffline(nullptr, sClientName, "0.0.0.0");
    Check(nInitialised == 1 && Initialization::IsInitialized() &&
              ServiceLocator::Exists<IHttpRequester>(),
          "_RA_InitClientOffline", "returned " + std::to_string(nInitialised) + ", services registered");

    // RA_Interface.h declares _RA_UpdateHWnd for every platform, but only its
    // Windows body does anything: off Windows the handle is reserved and
    // ignored. What protects it is the call, not the Check. Calling it here
    // turns a missing definition into a link error in this target, instead of
    // a NULL that a dlsym loader skips without a word - which is how the
    // function went missing on Linux the first time. The Check is no-crash
    // only: it can fail only by the process dying, i.e. by something
    // dereferencing the handle, and the bogus non-null handle is there so that
    // doing so crashes rather than reads.
    _RA_UpdateHWnd(nullptr);
    _RA_UpdateHWnd(reinterpret_cast<RA_WindowHandle>(uintptr_t{1}));
    Check(true, "_RA_UpdateHWnd ignores the handle",
          "returned for NULL and a bogus handle (no-crash only; the call is the link check)");

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

    const std::string sLogTail = ReadFileFrom(sLogPath, nLogSizeBefore);

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

        // --- from a worker thread -----------------------------------------
        // No view model reaches the clipboard from a worker today, but nothing
        // stops one. QtClipboard queues the write onto the application's thread
        // and waits there for the read's answer: this thread, whose event loop
        // is turned below while the worker waits, as an emulator's would be.
        // This runs under offscreen too: it checks QtClipboard's marshalling,
        // not the platform's clipboard - whether the desktop still holds the
        // text afterwards is the same question the round trip above asks, and
        // gets the same answer: observed, not asserted, below.
        const std::wstring sFromWorkerExpected = L"RA smoke from a worker";
        std::wstring sFromWorker = L"<never ran>";
        std::atomic<bool> bWorkerReturned{false};
        std::thread oWorker([&pClipboard, &sFromWorker, &bWorkerReturned, &sFromWorkerExpected]() {
            pClipboard.SetText(sFromWorkerExpected);
            sFromWorker = pClipboard.GetText();
            bWorkerReturned = true;
        });
        const auto tWorkerDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!bWorkerReturned && std::chrono::steady_clock::now() < tWorkerDeadline)
            Pump(20);
        oWorker.join();

        // The worker's SetText is queued onto the Qt thread and its GetText is
        // InvokeAndWait'd on that same thread; reading back its own write, on
        // that same round trip, proves the write reached the application's
        // clipboard - QtClipboard's cross-thread marshalling, which is what
        // this checks, not the desktop's.
        Check(bWorkerReturned && sFromWorker == sFromWorkerExpected, "clipboard off-thread read answers",
              bWorkerReturned ? ra::util::String::Narrow(sFromWorker) : "never returned");

        // Whether the desktop still holds it once this thread's own loop has
        // turned (above, while the worker waited) is not decidable from in
        // here any more than the round trip's own "does the desktop actually
        // see it?" check is, and for the same reason.
        const bool bStillHeld = (pClipboard.GetText() == sFromWorkerExpected);
        std::string sStillHeldDetail = std::string(bStillHeld ? "yes" : "no") + " (platform " + sPlatform + ")";
        if (sPlatform == "wayland")
        {
            sStillHeldDetail += "; a windowless application cannot own the Wayland selection, so the "
                                 "desktop may take it back once the loop turns";
        }
        Observe("clipboard still holds the worker's text", sStillHeldDetail);

        pClipboard.SetText(sOriginal);
    }

    Section("reset and confirm-load, offline game");
    {
        // _RA_OnReset returns at once when no game is loaded, so without one a
        // working reset and a no-op look the same. Offline mode serves a game
        // from two files, with no network and no account: Hashes.txt maps the
        // hash to an id (GameIdentifier::IdentifyHash), and <id>.json answers
        // the achievementsets request (OfflineRcClient).
        //
        // This section must stay before the audio section. That section ends
        // by leaving sound work queued so that shutdown runs with sound in
        // flight, and the Pump() calls here would run that work early.
        const std::wstring sHashesPath = sBase + RA_DIR_DATA L"Hashes.txt";
        const std::wstring sGamePath = sBase + RA_DIR_DATA + std::to_wstring(nResetGameId) + L".json";
        const bool bStaged =
            WriteTextFile(sHashesPath, std::string(sResetGameHash) + "=" + std::to_string(nResetGameId) + "\n") &&
            WriteTextFile(sGamePath, sResetGameJson);

        _RA_SetConsoleID(7); // NES, matching the fixture's ConsoleId
        g_pResetMemory[0] = 0;
        _RA_InstallMemoryBank(0, reinterpret_cast<void*>(&ReadResetMemory),
                              reinterpret_cast<void*>(&WriteResetMemory), sizeof(g_pResetMemory));

        // rcheevos builds no request without an API token, offline ones
        // included (rc_api_url_build_dorequest), and offline mode gives
        // rc_client whatever token the prefs hold (InitializeOfflineMode). A
        // real offline user has one from an earlier online login. This harness
        // never logs in, so it lends rc_client a placeholder for the length of
        // this section. OfflineRcClient answers every request itself, so the
        // value never leaves the process.
        auto* pRcClient = ServiceLocator::Get<ra::context::IRcClient>().GetClient();
        const char* sSavedToken = pRcClient->user.token;
        pRcClient->user.token = "ra-smoke-offline-token";

        const unsigned int nIdentified = _RA_IdentifyHash(sResetGameHash);
        _RA_ActivateGame(nIdentified);

        // The load finishes on the thread pool. GameId() is set before the
        // load starts, so on its own it proves nothing. The achievement only
        // appears in rc_client once the load has completed.
        auto& pGameContext = ServiceLocator::GetMutable<ra::data::context::GameContext>();
        int nWaitedMs = 0;
        for (; pGameContext.IsGameLoading() && nWaitedMs < 5000; nWaitedMs += 20)
            Pump(20);

        const auto oLoaded = SnapshotTrigger(nResetAchievementId);
        const bool bLoaded = bStaged && nIdentified == nResetGameId && !pGameContext.IsGameLoading() &&
                             pGameContext.GameId() == nResetGameId && oLoaded.nState >= 0;
        Check(bLoaded, "offline game loaded",
              std::string(bStaged ? "" : "fixture NOT staged, ") + "identified as " +
                  std::to_string(nIdentified) + ", GameId " + std::to_string(pGameContext.GameId()) + ", " +
                  (pGameContext.IsGameLoading() ? "still loading after " : "settled after ") +
                  std::to_string(nWaitedMs) + " ms, achievement " + oLoaded.Describe());

        if (!bLoaded)
        {
            Skip("reset resets the runtime", "the offline game did not load");
            Skip("confirm-load with no edits", "the offline game did not load");
            Skip("confirm-load with unsaved edits", "the offline game did not load");
        }
        else
        {
            // Any frame on which the trigger does not fire moves it from
            // WAITING to ACTIVE (rcheevos trigger.c). The first frame runs
            // with the condition false so that it adds no hit, and the three
            // after it, with the condition true, leave exactly 3 of its 100
            // hits.
            _RA_DoAchievementsFrame();
            g_pResetMemory[0] = 1;
            for (int i = 0; i < 3; ++i)
                _RA_DoAchievementsFrame();
            const auto oBefore = SnapshotTrigger(nResetAchievementId);

            // Through the callback that rc_client_reset() forwards to, not
            // _RA_OnReset() directly, because AchievementRuntimeExports::reset()
            // is where the guard lives. 7 is the highest version that
            // _Rcheevos_GetExternalClient implements.
            rc_client_external_t oExternal{};
            _Rcheevos_GetExternalClient(&oExternal, 7);

            const int64_t nLogSizeBeforeReset = std::max<int64_t>(pFileSystem.GetFileSize(sLogPath), 0);
            if (oExternal.reset)
                oExternal.reset();
            const auto oAfter = SnapshotTrigger(nResetAchievementId);
            const bool bResetLogged =
                ReadFileFrom(sLogPath, nLogSizeBeforeReset).find("Resetting runtime") != std::string::npos;

            Check(oBefore.nState == RC_TRIGGER_STATE_ACTIVE && oBefore.nHits == 3 &&
                      oAfter.nState == RC_TRIGGER_STATE_WAITING && oAfter.nHits == 0 && bResetLogged,
                  "reset resets the runtime",
                  oBefore.Describe() + " -> " + oAfter.Describe() + (bResetLogged ? ", logged" : ", NOT logged") +
                      (oExternal.reset ? "" : ", no reset callback"));

            // Nothing on Linux can edit an asset yet, so this is the answer a
            // real Linux user gets.
            const int nConfirmClean = _RA_ConfirmLoadNewRom(1);
            Check(nConfirmClean == 1, "confirm-load with no edits", "returned " + std::to_string(nConfirmClean));

            // An unsaved edit makes it ask, and NullDesktop answers No. No
            // keeps the edit, which is what Windows does when the user clicks
            // No. Undoing the edit has to bring the answer back to 1, which
            // shows the 0 came from the edit and from nothing else.
            auto* pAchievement = pGameContext.Assets().FindAchievement(nResetAchievementId);
            if (!pAchievement)
            {
                Check(false, "confirm-load with unsaved edits", "no achievement model for the fixture");
            }
            else
            {
                const std::wstring sName = pAchievement->GetName();
                pAchievement->SetName(sName + L" (edited)");
                const bool bModified = pAchievement->IsModified();

                const int64_t nLogSizeBeforeConfirm = std::max<int64_t>(pFileSystem.GetFileSize(sLogPath), 0);
                const int nConfirmEdited = _RA_ConfirmLoadNewRom(1);
                const bool bPromptLogged = ReadFileFrom(sLogPath, nLogSizeBeforeConfirm)
                                               .find("No view layer to show dialog") != std::string::npos;

                pAchievement->SetName(sName);
                const int nConfirmUndone = _RA_ConfirmLoadNewRom(1);

                Check(bModified && nConfirmEdited == 0 && bPromptLogged && nConfirmUndone == 1,
                      "confirm-load with unsaved edits",
                      std::string(bModified ? "edited" : "edit NOT registered") + ", returned " +
                          std::to_string(nConfirmEdited) + (bPromptLogged ? ", prompt logged" : ", prompt NOT logged") +
                          ", " + std::to_string(nConfirmUndone) + " after the edit was undone");
            }

            // What a real consumer's teardown does: clear the external-client
            // flag that _Rcheevos_GetExternalClient set. destroy() leaves the
            // pause and reset hooks that the same call installed on
            // EmulatorContext (HookupCallbackEvents), so they are emptied here,
            // back to how this harness runs everywhere else - it never
            // installs any.
            if (oExternal.destroy)
                oExternal.destroy();

            auto& pEmulatorContext = ServiceLocator::GetMutable<ra::data::context::EmulatorContext>();
            pEmulatorContext.SetPauseFunction(nullptr);
            pEmulatorContext.SetResetFunction(nullptr);
        }

        // Leave nothing behind for the sections after this one: no game, no
        // memory banks, the console a fresh init starts with, rc_client's own
        // token, and none of the files this section caused to be written.
        // Shutdown then still runs offline with no game, and the load's
        // info.wav has finished before the audio section takes its baselines.
        _RA_ActivateGame(0);
        _RA_ClearMemoryBanks();
        _RA_SetConsoleID(0); // ConsoleID::UnknownConsoleID, as Initialization registers it
        pRcClient->user.token = sSavedToken;
        std::remove(ra::util::String::Narrow(sHashesPath).c_str());
        std::remove(ra::util::String::Narrow(sGamePath).c_str());
        // Unloading the game ends its session, and SessionTracker writes the
        // stats under the username - empty here, hence the bare name.
        std::remove(ra::util::String::Narrow(sBase + RA_DIR_BASE L"-history.txt").c_str());
        QuiesceFileDescriptors();
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
            // One point through PlayAudioFile, for the worker-thread check
            // below. See the note above MeasureAudio.
            const auto oOne = MeasureAudio([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
            const int nOneSound = oOne.Delta();

            // Two more points, independent of PlayAudioFile entirely - see the
            // note above MeasureConcurrentSinks - for the burst check further
            // down, which is the one a reap bug can corrupt.
            const auto oCalibrationSound = DecodeForCalibration(sWavPath);
            const auto oRefOne = MeasureConcurrentSinks(oCalibrationSound, 1);
            const auto oRefTwo = MeasureConcurrentSinks(oCalibrationSound, 2);
            const int nRefMarginal = oRefTwo.Delta() - oRefOne.Delta();
            const int nRefFixed = oRefOne.Delta() - nRefMarginal;

            // --- PlayAudioFile from a worker thread ------------------------
            // AchievementRuntime plays unlock sounds from the frame thread, so
            // this is the real call path, not a contrived one. QtAudioSystem
            // has to hand the call to the application thread. Run on the
            // worker instead, it still plays: the tone is decoded already, so
            // a sink is created there and starts inside start(), and the
            // descriptor delta reads a whole stream, the same as a GUI-thread
            // call. But that sink belongs to a thread with no event loop, so
            // its drained state change is never delivered, it is never
            // reaped, and its stream stays open. Hence the residue as well:
            // MeasureAudio settles for 1000 ms after its 1400 ms window, well
            // past the 0.7 s tone and the 200-250 ms a destroyed sink keeps
            // its descriptors, so a dispatched call leaves 0 behind, and a
            // call run on the worker leaves a stream's worth.
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
                Check(oWorker.Delta() >= nOneSound / 2 && oWorker.Residue() == 0,
                      "PlayAudioFile off the GUI thread",
                      "backend engaged (" + std::to_string(oWorker.Delta()) + " vs " +
                          std::to_string(nOneSound) + " for a GUI-thread call), " +
                          std::to_string(oWorker.Residue()) + " descriptors left over");
            }

            // --- the overlap guarantee -------------------------------------
            // Simultaneous unlocks queue several PlayAudioFile calls into one
            // pass of the event loop, and every one of them must play. This
            // counts the streams the burst opened, at its peak, so it fails a
            // library that never creates a sink for some of them - dropping a
            // play that arrives while another sound plays scores "1 of 5". It
            // cannot see a sound cut off by the next one: the cut sink's
            // descriptors outlive it by 200-250 ms and still count at the
            // peak. That gap is known and deliberately not covered; see the
            // audio instrumentation note near the top of this file.
            constexpr int nBurst = 5;
            const auto oBurst = MeasureAudio([&pAudio, &sWav]() {
                for (int i = 0; i < nBurst; ++i)
                    pAudio.PlayAudioFile(sWav);
            });

            const std::string sCalibration =
                "1 held stream = " + std::to_string(oRefOne.Delta()) + " fds, 2 held = " +
                std::to_string(oRefTwo.Delta()) + ", so marginal = " + std::to_string(nRefMarginal) +
                " and fixed = " + std::to_string(nRefFixed) + "; burst = " +
                std::to_string(oBurst.Delta());

            if (oCalibrationSound.oPcm.isEmpty())
            {
                Observe("simultaneous unlocks all play",
                        "not decidable: the harness could not decode the tone to calibrate with (" +
                            sCalibration + ")");
            }
            else if (nRefMarginal < 1)
            {
                // A backend that pools descriptors across streams lands here,
                // and so does a machine too noisy to calibrate on. Counting
                // anyway would have divided by an assumed marginal of one and
                // reported a number; there is nothing to count with. This is
                // measured against the harness's own held sinks, never against
                // PlayAudioFile, so a reap bug in QtAudioSystem cannot
                // manufacture this outcome - see the note above
                // MeasureConcurrentSinks.
                Observe("simultaneous unlocks all play",
                        "not decidable: no per-stream descriptor signal on this backend (" +
                            sCalibration + ")");
            }
            else
            {
                const int nConcurrent = (oBurst.Delta() - nRefFixed) / nRefMarginal;
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
            // One call from a worker is queued on the application's thread
            // (joined below); the two from here run inline, since this IS the
            // application's thread. The queued one would only run during
            // main()'s exit timer, after _RA_Shutdown() - where the host's
            // closed gate makes it skip itself (QtApplicationHost::Post)
            // rather than create a sink after the stop hook emptied the pool.
            std::thread oLate([&pAudio, &sWav]() { pAudio.PlayAudioFile(sWav); });
            pAudio.PlayAudioFile(sWav);
            pAudio.PlayAudioFile(sWav);
            oLate.join();
        }
    }

    Section("shutdown");

    // Through the emulator-facing export, not Initialization::Shutdown(): no
    // real consumer can reach the latter, and only _RA_Shutdown() also saves
    // the preferences, ends the session and marks the process as shutting
    // down. Every check below takes its "before" first, so none of them can
    // pass on a _RA_Shutdown() that returned without doing anything.
    //
    // The preferences file is deleted rather than inspected: nothing earlier
    // in the run is required to have written it, so "it exists afterwards"
    // only says something about this call if it provably did not before.
    const std::wstring sPrefsPath = sBase + L"RAPrefs_" + ra::util::String::Widen(sClientName) + L".cfg";
    const std::string sPrefsPathNarrow = ra::util::String::Narrow(sPrefsPath);
    std::remove(sPrefsPathNarrow.c_str());
    const bool bPrefsAbsentBefore = !std::ifstream(sPrefsPathNarrow).good();
    const int64_t nLogSizeBeforeShutdown = std::max<int64_t>(pFileSystem.GetFileSize(sLogPath), 0);
    const bool bInitialisedBefore = Initialization::IsInitialized();
    const bool bShuttingDownBefore = ServiceLocator::IsShuttingDown();
    const bool bConfigurationRegisteredBefore = ServiceLocator::Exists<IConfiguration>();

    // Nothing runs concurrently with this call: the audio section joined its
    // worker, and the one sound call it left queued on this thread cannot run
    // until RunChecks() returns - by which time the Qt host has stopped and
    // the call skips itself. _RA_Shutdown() returns 0 on every path, so its
    // return value only shows that it came back; the flag is what shows it did
    // the work.
    const int nShutdown = _RA_Shutdown();
    const bool bInitialisedAfter = Initialization::IsInitialized();
    Check(bInitialisedBefore && nShutdown == 0 && !bInitialisedAfter, "_RA_Shutdown() deinitialises",
          "returned " + std::to_string(nShutdown) + ", IsInitialized " +
              (bInitialisedBefore ? "true" : "false") + " -> " + (bInitialisedAfter ? "true" : "false"));

    int64_t nPrefsSize = -1;
    {
        std::ifstream oPrefs(sPrefsPathNarrow, std::ios::binary | std::ios::ate);
        if (oPrefs)
            nPrefsSize = static_cast<int64_t>(oPrefs.tellg());
    }
    Check(bPrefsAbsentBefore && nPrefsSize > 0, "preferences saved",
          sPrefsPathNarrow + (bPrefsAbsentBefore ? ", " : " (still present before the call), ") +
              std::to_string(nPrefsSize) + " bytes");

    const bool bShuttingDown = ServiceLocator::IsShuttingDown();
    const bool bConfigurationRegistered = ServiceLocator::Exists<IConfiguration>();
    Check(!bShuttingDownBefore && bShuttingDown && bConfigurationRegisteredBefore && !bConfigurationRegistered,
          "services torn down",
          std::string("IsShuttingDown ") + (bShuttingDownBefore ? "true" : "false") + " -> " +
              (bShuttingDown ? "true" : "false") + ", IConfiguration " +
              (bConfigurationRegisteredBefore ? "registered" : "absent") + " -> " +
              (bConfigurationRegistered ? "registered" : "gone"));

    const std::string sShutdownTail = ReadFileFrom(sLogPath, nLogSizeBeforeShutdown);
    Check(sShutdownTail.find("Shutdown complete") != std::string::npos, "shutdown logged",
          "+" + std::to_string(sShutdownTail.length()) + " bytes past " +
              std::to_string(nLogSizeBeforeShutdown));

    Skip("known hashes, session, game unload",
         "offline, no game loaded: SaveKnownHashes is skipped offline; EndSession, UnloadGame and "
         "OnActiveGameChanged have nothing to act on");

    // Windows really does this: the emulator's RA_Shutdown(), then DllMain's
    // detach-time call. The second call has to be a no-op, and nothing else
    // can write to the log at this point - the thread pool is joined and the
    // queued sound work waits for the event loop - so any byte it adds is a
    // failure. A second pass through the sequence would log, and so would one
    // that hit a missing service, which a release build's catch in
    // _RA_Shutdown() would otherwise hide. A crash takes the whole process
    // down. The offset comes from the bytes already read rather than from
    // IFileSystem, which this check should not lean on after teardown.
    const int64_t nLogSizeBeforeSecond =
        nLogSizeBeforeShutdown + static_cast<int64_t>(sShutdownTail.length());
    const int nSecondShutdown = _RA_Shutdown();
    const std::string sSecondTail = ReadFileFrom(sLogPath, nLogSizeBeforeSecond);
    Check(nSecondShutdown == 0 && sSecondTail.empty(), "second _RA_Shutdown() is a no-op",
          "returned " + std::to_string(nSecondShutdown) + ", +" + std::to_string(sSecondTail.length()) +
              " log bytes" + (sSecondTail.empty() ? "" : ", first: " + sSecondTail.substr(0, sSecondTail.find('\n'))));
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
