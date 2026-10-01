#ifndef _WIN32

#include "ui/drawing/qt/OverlayImage.hh"

#include "Exports.hh"

#include "ui/drawing/qt/QtImageRepository.hh"
#include "ui/drawing/qt/QtSurface.hh"
#include "ui/drawing/qt/ScreenCapture.hh"
#include "ui/viewmodels/OverlayManager.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/context/mocks/MockUserContext.hh"
#include "tests/devkit/services/mocks/MockClock.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockFileSystem.hh"
#include "tests/devkit/services/mocks/MockHttpRequester.hh"
#include "tests/devkit/services/mocks/MockThreadPool.hh"
#include "tests/devkit/ui/mocks/MockImageRepository.hh"
#include "tests/mocks/MockAchievementRuntime.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockEmulatorContext.hh"
#include "tests/mocks/MockGameContext.hh"
#include "tests/mocks/MockOverlayTheme.hh"
#include "tests/mocks/MockSurface.hh"
#include "tests/mocks/MockWindowConfiguration.hh"
#include "tests/mocks/MockWindowManager.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>

#include <cmath>
#include <cstdlib>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace drawing {
namespace qt {
namespace tests {

using ra::ui::qt::tests::QtTestHost;
using ra::ui::viewmodels::OverlayManager;
using ra::ui::viewmodels::Popup;
using ra::ui::viewmodels::PopupLocation;

namespace {

// Wider than a QImage can be: Qt refuses more than (INT_MAX - 31) / 32 pixels a row at 32 bits a pixel (qimage.cpp,
// calculateImageParameters), before it allocates anything.
constexpr int REFUSED_WIDTH = 100000000;

// A popup message: 4 + 64 + 4 pixels high, plus the theme's 2-pixel shadow; 10 pixels in from the left and up from
// the bottom once it has slid in (PopupMessageViewModel).
constexpr int POPUP_HEIGHT = 4 + 64 + 4 + 2;

class OverlayImageHarness
{
public:
    // First, so it goes last: the factory's fonts and the popups' images must go while the application still runs.
    QtTestHost oQt;

    // what the pause overlay needs, as OverlayManager_Tests' harness has them
    ra::context::mocks::MockRcClient mockRcClient;
    ra::context::mocks::MockUserContext mockUserContext;
    ra::data::context::mocks::MockEmulatorContext mockEmulatorContext;
    ra::data::context::mocks::MockGameContext mockGameContext;
    ra::services::mocks::MockAchievementRuntime mockAchievementRuntime;
    ra::services::mocks::MockClock mockClock;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ra::services::mocks::MockHttpRequester mockHttpRequester; // RA_Init's User-Agent
    ra::services::mocks::MockThreadPool mockThreadPool;
    ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
    ra::ui::mocks::MockDesktop mockDesktop;
    ra::ui::mocks::MockImageRepository mockImageRepository;
    ra::ui::mocks::MockOverlayTheme mockTheme;
    ra::ui::viewmodels::mocks::MockWindowManager mockWindowManager;
    QtSurfaceFactory surfaceFactory;
    OverlayManager overlayManager;
    OverlayImage overlayImage; // made on this thread: the tests' "emulator thread"

    const void* pPixels = nullptr;
    int nStride = 0;

    // bAttach: what RA_Init does once the services exist (Exports.cpp's InitCommon); false leaves that to the test
    explicit OverlayImageHarness(bool bAttach = true)
        : m_oFactoryOverride(&surfaceFactory), m_oManagerOverride(&overlayManager)
    {
        mockWindowConfiguration.SetPopupLocation(Popup::Message, PopupLocation::BottomLeft);
        if (bAttach)
            overlayImage.Attach();
    }

    int Update(int nWidth, int nHeight, float fScale = 1.0f)
    {
        pPixels = &nStride; // anything but NULL, to see Update clear it
        nStride = -1;
        return overlayImage.Update(nWidth, nHeight, fScale, &pPixels, &nStride);
    }

    uint32_t PixelAt(int nX, int nY) const
    {
        return reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(pPixels) + nY * nStride)[nX];
    }

    // queues a popup, then lets it slide in (0.8 seconds) and hold
    int ShowPopup(int nWidth, int nHeight, float fScale = 1.0f)
    {
        overlayManager.QueueMessage(L"Title", L"Description");
        Update(nWidth, nHeight, fScale);
        mockClock.AdvanceTime(std::chrono::seconds(1));
        return Update(nWidth, nHeight, fScale);
    }

    // Shows the overlay, lets it slide in, then hides it; returns the last Update's serial. The first time the overlay
    // is shown, OverlayManager itself erases everything right of it on its first frame - on Windows too: its render
    // location is still (0, 0), so UpdatePopup fills the whole width its move seems to have uncovered. From then on it
    // covers what is under it only as it slides over it.
    int ShowAndHideTheOverlay(int nWidth, int nHeight)
    {
        overlayManager.ShowOverlay();
        Update(nWidth, nHeight);
        mockClock.AdvanceTime(std::chrono::milliseconds(500));
        Update(nWidth, nHeight); // fully visible
        overlayManager.HideOverlay();
        Update(nWidth, nHeight); // fading out
        mockClock.AdvanceTime(std::chrono::milliseconds(500));
        return Update(nWidth, nHeight);
    }

private:
    ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> m_oFactoryOverride;
    ra::services::ServiceLocator::ServiceOverride<OverlayManager> m_oManagerOverride;
};

// Sets an environment variable, and unsets it however the test ends.
class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable(const char* sName, const char* sValue) : m_sName(sName) { setenv(sName, sValue, 1); }
    ~ScopedEnvironmentVariable() noexcept { unsetenv(m_sName); }
    ScopedEnvironmentVariable(const ScopedEnvironmentVariable&) noexcept = delete;
    ScopedEnvironmentVariable& operator=(const ScopedEnvironmentVariable&) noexcept = delete;
    ScopedEnvironmentVariable(ScopedEnvironmentVariable&&) noexcept = delete;
    ScopedEnvironmentVariable& operator=(ScopedEnvironmentVariable&&) noexcept = delete;

private:
    const char* m_sName;
};

// each channel within one: QPainter's blending rounds
bool Near(uint32_t nExpected, uint32_t nActual)
{
    for (int nShift = 0; nShift < 32; nShift += 8)
    {
        if (std::abs(static_cast<int>((nExpected >> nShift) & 0xFF) - static_cast<int>((nActual >> nShift) & 0xFF)) > 1)
            return false;
    }
    return true;
}

// the popup's background (MockConfiguration: softcore, so the theme's 64,80,104) at Windows' 90% (229 of 255),
// premultiplied
constexpr uint32_t POPUP_BACKGROUND = 0xE539485D;

// an opaque magenta badge at Windows' 90%, premultiplied
constexpr uint32_t MAGENTA_BADGE = 0xE5E500E5;

// a 64 x 64 opaque magenta PNG
std::string MagentaPng()
{
    QImage oImage(64, 64, QImage::Format_ARGB32);
    oImage.fill(QColor(255, 0, 255));
    QByteArray oBytes;
    QBuffer oBuffer(&oBytes);
    oBuffer.open(QIODevice::WriteOnly);
    oImage.save(&oBuffer, "PNG");
    return std::string(oBytes.constData(), static_cast<size_t>(oBytes.size()));
}

} // namespace

TEST_CLASS(OverlayImage_Tests)
{
public:
    TEST_METHOD(TestNothingShownReturnsZero)
    {
        OverlayImageHarness harness;

        Assert::AreEqual(0, harness.Update(320, 240));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.nStride);
    }

    TEST_METHOD(TestAQueuedPopupGivesASerialAndPixelsInItsCorner)
    {
        OverlayImageHarness harness;
        harness.overlayManager.QueueMessage(L"Title", L"Description");
        const int nFirst = harness.Update(320, 240);
        Assert::IsTrue(nFirst > 0, L"no serial for a popup");

        harness.mockClock.AdvanceTime(std::chrono::seconds(1)); // slid in
        const int nSerial = harness.Update(320, 240);

        Assert::IsTrue(nSerial != 0 && nSerial != nFirst, L"the popup's move was not redrawn");
        Assert::IsNotNull(harness.pPixels);
        Assert::AreEqual(320 * 4, harness.nStride);
        const int nTop = 240 - 10 - POPUP_HEIGHT;
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(10 + 3, nTop + 3)), L"no popup in the bottom-left corner");
        Assert::AreEqual(0U, harness.PixelAt(300, 10)); // the rest is transparent
        Assert::AreEqual(0U, harness.PixelAt(5, 120));
    }

    TEST_METHOD(TestAnUnchangedPictureKeepsItsSerialAndIsNotRedrawn)
    {
        OverlayImageHarness harness;
        const int nSerial = harness.ShowPopup(320, 240);
        const unsigned nRenders = harness.overlayImage.GetRenderCount();

        Assert::AreEqual(nSerial, harness.Update(320, 240));
        harness.mockClock.AdvanceTime(std::chrono::milliseconds(500)); // holding still
        Assert::AreEqual(nSerial, harness.Update(320, 240));

        Assert::AreEqual(nRenders, harness.overlayImage.GetRenderCount());
        Assert::IsNotNull(harness.pPixels);
    }

    TEST_METHOD(TestASizeOrScaleChangeGivesANewSerial)
    {
        OverlayImageHarness harness;
        const int nFirst = harness.ShowPopup(320, 240);

        const int nWider = harness.Update(400, 240);
        Assert::IsTrue(nWider != nFirst, L"a new size kept the serial");
        Assert::AreEqual(400 * 4, harness.nStride);

        const int nScaled = harness.Update(400, 240, 2.0f);
        Assert::IsTrue(nScaled != nWider, L"a new scale kept the serial");
        // the same popup, laid out in logical pixels (200 x 120) and painted at twice the size
        const int nTop = 2 * (120 - 10 - POPUP_HEIGHT);
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(2 * (10 + 3), nTop + 2 * 3)),
                       L"no popup in the corner at scale 2");
    }

    TEST_METHOD(TestAnInvalidScaleCountsAsOne)
    {
        OverlayImageHarness harness;
        const int nSerial = harness.ShowPopup(320, 240);

        Assert::AreEqual(nSerial, harness.Update(320, 240, 0.0f));
        Assert::AreEqual(nSerial, harness.Update(320, 240, -2.0f));
        Assert::AreEqual(nSerial, harness.Update(320, 240, std::nanf("")));
        Assert::AreEqual(nSerial, harness.Update(320, 240, INFINITY));
    }

    TEST_METHOD(TestNoSizeReturnsZero)
    {
        OverlayImageHarness harness;
        harness.ShowPopup(320, 240);

        Assert::AreEqual(0, harness.Update(0, 240));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.Update(320, -1));
    }

    TEST_METHOD(TestASizeQtRefusesReturnsZeroAndIsNotTriedAgain)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);
        const unsigned nAttempts = harness.overlayImage.GetImageAttempts();

        Assert::AreEqual(0, harness.Update(REFUSED_WIDTH, 1));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.nStride);
        Assert::AreEqual(nAttempts + 1, harness.overlayImage.GetImageAttempts());

        // the next frame asks for the same size: it is not tried again
        Assert::AreEqual(0, harness.Update(REFUSED_WIDTH, 1));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.nStride);
        Assert::AreEqual(nAttempts + 1, harness.overlayImage.GetImageAttempts());

        // a size Qt can make brings the overlay back
        Assert::IsTrue(harness.Update(320, 240) > 0);
        Assert::IsNotNull(harness.pPixels);
        Assert::AreEqual(320 * 4, harness.nStride);
        const int nTop = 240 - 10 - POPUP_HEIGHT;
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(10 + 3, nTop + 3)), L"no popup at the good size");
    }

    TEST_METHOD(TestARememberedRefusedSizeDoesNotStopTheOverlay)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);
        Assert::AreEqual(0, harness.Update(REFUSED_WIDTH, 1)); // refused: the image is dropped
        Assert::IsTrue(harness.Update(320, 240) > 0);          // made again, and OverlayManager asks for the next render
        Assert::AreEqual(0, harness.Update(REFUSED_WIDTH, 1)); // remembered, so not tried: that request must survive it

        // the overlay still follows OverlayManager: past its five seconds, the popup ends and the overlay hides
        harness.mockClock.AdvanceTime(std::chrono::seconds(6));
        Assert::AreEqual(0, harness.Update(320, 240), L"the overlay stopped: the popup never ended");
        Assert::IsNull(harness.pPixels);
    }

    TEST_METHOD(TestTheOverlayIsHiddenOnceThePopupEnds)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);

        harness.mockClock.AdvanceTime(std::chrono::seconds(6)); // past its five seconds
        Assert::AreEqual(0, harness.Update(320, 240));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.nStride);
    }

    TEST_METHOD(TestAPopupStaysUntilTheSlidingOverlayCoversIt)
    {
        // Windows repaints its overlay window rather than clearing it (OverlayWindow::Render): as the overlay slides
        // in, only the overlay is drawn, over the popups the window already shows
        OverlayImageHarness harness;
        Assert::AreEqual(0, harness.ShowAndHideTheOverlay(320, 240), L"the overlay did not hide");

        int nSerial = harness.ShowPopup(320, 240);
        // inside the popup's background, below its text, 60 pixels from the image's left edge
        const int nPopupX = 10 + 50;
        const int nPopupY = 240 - 10 - POPUP_HEIGHT + 60;
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(nPopupX, nPopupY)), L"no popup before the overlay");

        harness.overlayManager.ShowOverlay();
        for (int nFrame = 1; nFrame <= 3; ++nFrame)
        {
            // 5 ms a frame: the overlay's right edge is still left of x = 24 after the third
            harness.mockClock.AdvanceTime(std::chrono::milliseconds(5));
            const int nFrameSerial = harness.Update(320, 240);
            Assert::IsTrue(nFrameSerial != 0 && nFrameSerial != nSerial,
                           ra::util::String::Printf(L"frame %d: the overlay's move was not drawn", nFrame).c_str());
            nSerial = nFrameSerial;

            Assert::IsTrue(harness.PixelAt(2, 120) != 0,
                           ra::util::String::Printf(L"frame %d: no overlay at the left edge", nFrame).c_str());
            // the popup, beside the overlay: still there, and at 90% once - not once more for every frame
            Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(nPopupX, nPopupY)),
                           ra::util::String::Printf(L"frame %d: the popup is gone or dimmed: %08X", nFrame,
                                                    harness.PixelAt(nPopupX, nPopupY)).c_str());
        }
    }

    TEST_METHOD(TestATrackerStaysUntilTheSlidingOverlayCoversIt)
    {
        // A pause during a leaderboard attempt. Unlike a popup, a tracker holding still does not keep OverlayManager
        // asking for renders: once a repaint has drawn nothing its render loop stops, and the pause's first request
        // runs the show handler again, on an overlay that is already showing. Windows only shows its window again
        // then (OverlayWindow's show handler): nothing is erased, and the tracker stays until the overlay covers it.
        OverlayImageHarness harness;
        harness.mockWindowConfiguration.SetPopupLocation(Popup::LeaderboardTracker, PopupLocation::BottomRight);
        Assert::AreEqual(0, harness.ShowAndHideTheOverlay(320, 240), L"the overlay did not hide");

        const auto& vmTracker = harness.overlayManager.AddScoreTracker(1);
        int nSerial = harness.Update(320, 240);
        Assert::IsTrue(nSerial > 0, L"no serial for a tracker");
        Assert::AreEqual(nSerial, harness.Update(320, 240), L"the idle repaint drew something"); // the loop stops
        // inside the tracker's background, left of and above its text
        const int nTrackerX = vmTracker.GetRenderLocationX() + 1;
        const int nTrackerY = vmTracker.GetRenderLocationY() + 1;
        Assert::IsTrue(nTrackerX > 200, L"the tracker is not at the right");
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(nTrackerX, nTrackerY)), L"no tracker before the overlay");

        harness.overlayManager.ShowOverlay();
        for (int nFrame = 1; nFrame <= 3; ++nFrame)
        {
            harness.mockClock.AdvanceTime(std::chrono::milliseconds(5));
            const int nFrameSerial = harness.Update(320, 240);
            Assert::IsTrue(nFrameSerial != 0 && nFrameSerial != nSerial,
                           ra::util::String::Printf(L"frame %d: the overlay's move was not drawn", nFrame).c_str());
            nSerial = nFrameSerial;

            Assert::IsTrue(harness.PixelAt(2, 120) != 0,
                           ra::util::String::Printf(L"frame %d: no overlay at the left edge", nFrame).c_str());
            Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(nTrackerX, nTrackerY)),
                           ra::util::String::Printf(L"frame %d: the tracker is gone or dimmed: %08X", nFrame,
                                                    harness.PixelAt(nTrackerX, nTrackerY)).c_str());
        }
    }

    TEST_METHOD(TestAReshowRedrawsEverything)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);
        harness.mockClock.AdvanceTime(std::chrono::seconds(6)); // the popup ends, and the overlay hides
        Assert::AreEqual(0, harness.Update(320, 240));

        // shown again by something a repaint draws nothing for: a challenge indicator whose popups are turned off
        harness.mockWindowConfiguration.SetPopupLocation(Popup::Challenge, PopupLocation::None);
        const unsigned nRenders = harness.overlayImage.GetRenderCount();
        harness.overlayManager.AddChallengeIndicator(1, ra::ui::ImageType::Badge, "12345");

        Assert::IsTrue(harness.Update(320, 240) > 0);
        Assert::IsNotNull(harness.pPixels);
        Assert::AreEqual(nRenders + 1, harness.overlayImage.GetRenderCount(), L"the reshow was not a new picture");
    }

    TEST_METHOD(TestACallOffTheEmulatorsThreadReturnsZero)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);

        int nOffThread = -1;
        const void* pOffThreadPixels = &nOffThread;
        std::thread([&harness, &nOffThread, &pOffThreadPixels]() {
            int nStride = -1;
            nOffThread = harness.overlayImage.Update(320, 240, 1.0f, &pOffThreadPixels, &nStride);
        }).join();

        Assert::AreEqual(0, nOffThread);
        Assert::IsNull(pOffThreadPixels);
        Assert::IsTrue(harness.Update(320, 240) > 0, L"the emulator's thread lost the overlay");
    }

    TEST_METHOD(TestACallAfterShutdownReturnsZero)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);

        harness.overlayImage.Shutdown();

        Assert::AreEqual(0, harness.Update(320, 240));
        Assert::IsNull(harness.pPixels);
    }

    TEST_METHOD(TestNothingIsDrawnWithoutQtSurfaces)
    {
        // what a library without a display has: PlatformServices_Linux gives the null factory
        QtTestHost oQt;
        ra::services::mocks::MockClock mockClock;
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        ra::ui::mocks::MockImageRepository mockImageRepository;
        ra::ui::mocks::MockOverlayTheme mockTheme;
        ra::ui::drawing::mocks::MockSurfaceFactory mockSurfaceFactory;
        OverlayManager overlayManager;
        ra::services::ServiceLocator::ServiceOverride<OverlayManager> oManagerOverride(&overlayManager);
        OverlayImage overlayImage;
        overlayImage.Attach(); // as RA_Init does
        overlayManager.QueueMessage(L"Title", L"Description");

        const void* pPixels = &overlayImage;
        int nStride = -1;
        Assert::AreEqual(0, overlayImage.Update(320, 240, 1.0f, &pPixels, &nStride));
        Assert::IsNull(pPixels);
        Assert::AreEqual(0U, overlayImage.GetRenderCount());
    }

    TEST_METHOD(TestTheTestHookQueuesOnePopup)
    {
        // read once, at the first Update after RA_Init attaches the overlay (the harness's Attach), not at Attach
        // itself: see TestTheTestHookPopupSurvivesAGameLoadClearingPopups for why. Still set at every Update after.
        ScopedEnvironmentVariable oHook("RA_OVERLAY_TEST_POPUP", "Hook");
        OverlayImageHarness harness;

        const int nSerial = harness.Update(320, 240);
        Assert::IsTrue(nSerial > 0, L"the hook's popup was not shown");

        const auto* pPopup = harness.overlayManager.GetMessage(1);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Hook"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"RA_OVERLAY_TEST_POPUP"), pPopup->GetDescription());

        Assert::IsTrue(harness.Update(320, 240) > 0);
        Assert::IsNull(harness.overlayManager.GetMessage(2), L"a second test popup was queued");
    }

    TEST_METHOD(TestTheTestHookPopupCarriesItsImage)
    {
        ScopedEnvironmentVariable oHook("RA_OVERLAY_TEST_POPUP", "Hook");
        ScopedEnvironmentVariable oImage("RA_OVERLAY_TEST_POPUP_IMAGE", "o1btest");
        OverlayImageHarness harness;

        Assert::IsTrue(harness.Update(320, 240) > 0, L"the hook's popup was not shown");

        const auto* pPopup = harness.overlayManager.GetMessage(1);
        Assert::IsNotNull(pPopup);
        Assert::IsTrue(pPopup->GetImage().Type() == ra::ui::ImageType::Badge, L"the popup has no badge");
        Assert::AreEqual(std::string("o1btest"), pPopup->GetImage().Name());
    }

    TEST_METHOD(TestTheTestHookAsksForAScreenshotOfItsPopupLater)
    {
        // RA_OVERLAY_TEST_SCREENSHOT (native-headless.sh run 5): ScreenCapture takes it 2.5 seconds after the popup
        // is queued, from inside RA_DoAchievementsFrame (ScreenCapture::DoFrame), as OverlayManager takes an
        // unlock's: the screenshot job then waits in the pool
        ScopedEnvironmentVariable oHook("RA_OVERLAY_TEST_POPUP", "Hook");
        ScopedEnvironmentVariable oImage("RA_OVERLAY_TEST_POPUP_IMAGE", "o1btest");
        ScopedEnvironmentVariable oShot("RA_OVERLAY_TEST_SCREENSHOT", "/shots/test.png");
        OverlayImageHarness harness;
        ScreenCapture screenCapture;
        ra::services::ServiceLocator::ServiceOverride<ScreenCapture> oOverride(&screenCapture);

        Assert::IsTrue(harness.Update(320, 240) > 0, L"the hook's popup was not shown");
        const auto nBefore = harness.mockThreadPool.PendingTasks();

        harness.mockClock.AdvanceTime(std::chrono::milliseconds(2499));
        screenCapture.DoFrame();
        Assert::AreEqual(nBefore, harness.mockThreadPool.PendingTasks(), L"the screenshot was taken early");

        harness.mockClock.AdvanceTime(std::chrono::milliseconds(1));
        screenCapture.DoFrame();
        Assert::AreEqual(nBefore + 1, harness.mockThreadPool.PendingTasks(), L"no screenshot job once it was due");
    }

    TEST_METHOD(TestAPopupShowsItsBadge)
    {
        OverlayImageHarness harness;
        ra::services::mocks::MockFileSystem mockFileSystem;
        mockFileSystem.SetBaseDirectory(L"/base/");
        mockFileSystem.MockFile(L"/base/RACache/Badge/12345.png", MagentaPng());
        QtImageRepository imageRepository;
        ra::services::ServiceLocator::ServiceOverride<ra::ui::IImageRepository> oImagesOverride(&imageRepository);

        harness.overlayManager.QueueMessage(L"Title", L"Description", ra::ui::ImageType::Badge, "12345");
        harness.Update(320, 240);
        harness.mockClock.AdvanceTime(std::chrono::seconds(1)); // slid in
        Assert::IsTrue(harness.Update(320, 240) > 0);

        // the badge: 4 pixels into the popup, 64 x 64
        const int nLeft = 10 + 4;
        const int nTop = 240 - 10 - POPUP_HEIGHT + 4;
        for (const auto& pPoint : {std::make_pair(0, 0), std::make_pair(32, 32), std::make_pair(63, 63)})
        {
            const uint32_t nPixel = harness.PixelAt(nLeft + pPoint.first, nTop + pPoint.second);
            Assert::IsTrue(Near(MAGENTA_BADGE, nPixel),
                           ra::util::String::Printf(L"no badge at (%d,%d): %08X", pPoint.first, pPoint.second, nPixel)
                               .c_str());
        }
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(nLeft - 2, nTop + 32)), L"the badge spilled left");
    }

    TEST_METHOD(TestTheTestHookPopupSurvivesAGameLoadClearingPopups)
    {
        // RALibretro's -g loads its game right after RA_Init: RA_IdentifyHash, then RA_ActivateGame(0), which calls
        // OverlayManager::ClearPopups() (GameIdentifier.cpp) before the emulator's first frame ever calls Update.
        // Queueing the hook's popup at Attach (RA_Init) let that destroy it before it was ever drawn; queueing it at
        // the first Update instead means it is queued only once the game load's clear has already happened.
        ScopedEnvironmentVariable oHook("RA_OVERLAY_TEST_POPUP", "Hook");
        OverlayImageHarness harness;
        harness.overlayManager.ClearPopups(); // as a loading game does, before the first Update

        const int nFirst = harness.Update(320, 240);
        Assert::IsTrue(nFirst > 0, L"the hook's popup did not survive the game load");

        harness.mockClock.AdvanceTime(std::chrono::seconds(1)); // slid in
        const int nSerial = harness.Update(320, 240);
        Assert::IsTrue(nSerial != 0 && nSerial != nFirst, L"the popup's move was not redrawn");
        Assert::IsNotNull(harness.pPixels);
        const int nTop = 240 - 10 - POPUP_HEIGHT;
        Assert::IsTrue(Near(POPUP_BACKGROUND, harness.PixelAt(10 + 3, nTop + 3)),
                       L"no test popup in the bottom-left corner after a loading game cleared popups");
    }

    TEST_METHOD(TestRAInitAttachesTheOverlayBeforeAnyUpdate)
    {
        // a worker (the login reply's "Welcome back") may queue a popup, and ask for a render, as soon as RA_Init
        // returns: the handlers must be in by then. The test hook's own popup is queued only at the first Update,
        // not here - see TestTheTestHookPopupSurvivesAGameLoadClearingPopups for why.
        ScopedEnvironmentVariable oHook("RA_OVERLAY_TEST_POPUP", "Hook");
        OverlayImageHarness harness(false);
        ra::services::ServiceLocator::ServiceOverride<OverlayImage> oOverride(&harness.overlayImage);
        harness.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);

        Assert::IsFalse(harness.overlayImage.IsAttached());
        Assert::AreEqual(1, _RA_InitI(nullptr, 0, "1.0"));
        Assert::IsTrue(harness.overlayImage.IsAttached(), L"RA_Init did not attach the overlay's handlers");

        Assert::IsTrue(harness.Update(320, 240) > 0, L"the test popup was not shown");
        const auto* pPopup = harness.overlayManager.GetMessage(1);
        Assert::IsNotNull(pPopup, L"the test popup was not queued at the first Update");
        Assert::AreEqual(std::wstring(L"Hook"), pPopup->GetTitle());
    }

    TEST_METHOD(TestASecondOverlayImageNeverRepeatsASerial)
    {
        // a new RA_Init makes a new OverlayImage: a host that remembers the serial it last uploaded must not take
        // the new session's pictures for the old one's
        int vFirstSession[2] = {0, 0};
        {
            OverlayImageHarness harness;
            harness.overlayManager.QueueMessage(L"Title", L"Description");
            vFirstSession[0] = harness.Update(320, 240);
            harness.mockClock.AdvanceTime(std::chrono::seconds(1));
            vFirstSession[1] = harness.Update(320, 240);
        }

        OverlayImageHarness harness;
        harness.overlayManager.QueueMessage(L"Title", L"Description");
        const int nFirst = harness.Update(320, 240);
        harness.mockClock.AdvanceTime(std::chrono::seconds(1));
        const int nSecond = harness.Update(320, 240);

        Assert::IsTrue(vFirstSession[0] > 0 && vFirstSession[1] > 0 && nFirst > 0 && nSecond > 0);
        for (const int nOld : vFirstSession)
        {
            Assert::IsTrue(nFirst != nOld && nSecond != nOld,
                           ra::util::String::Printf(L"the second session repeated serial %d", nOld).c_str());
        }
    }

    TEST_METHOD(TestTheExportReturnsZeroBeforeInit)
    {
        // no OverlayImage registered: what a loader sees before RA_Init and after RA_Shutdown
        const void* pPixels = &pPixels;
        int nStride = -1;

        Assert::AreEqual(0, _RA_UpdateOverlayImage(64, 48, 1.0f, &pPixels, &nStride));
        Assert::IsNull(pPixels);
        Assert::AreEqual(0, nStride);
    }

    TEST_METHOD(TestTheExportReachesTheRegisteredOverlayImage)
    {
        OverlayImageHarness harness;
        ra::services::ServiceLocator::ServiceOverride<OverlayImage> oOverride(&harness.overlayImage);
        const int nSerial = harness.ShowPopup(320, 240);

        const void* pPixels = nullptr;
        int nStride = 0;
        const int nExported = _RA_UpdateOverlayImage(320, 240, 1.0f, &pPixels, &nStride);

        Assert::AreEqual(nSerial, nExported, L"the export did not reach the overlay image");
        Assert::IsTrue(pPixels == harness.pPixels, L"the export handed out other pixels");
        Assert::AreEqual(320 * 4, nStride);
    }
};

} // namespace tests
} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !_WIN32
