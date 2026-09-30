#ifndef _WIN32

#include "ui/drawing/qt/OverlayImage.hh"

#include "Exports.hh"

#include "ui/drawing/qt/QtSurface.hh"
#include "ui/viewmodels/OverlayManager.hh"

#include "tests/devkit/services/mocks/MockClock.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockThreadPool.hh"
#include "tests/devkit/ui/mocks/MockImageRepository.hh"
#include "tests/mocks/MockOverlayTheme.hh"
#include "tests/mocks/MockSurface.hh"
#include "tests/mocks/MockWindowConfiguration.hh"
#include "tests/ui/qt/QtTestHost.hh"

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

// A popup message: 4 + 64 + 4 pixels high, plus the theme's 2-pixel shadow; 10 pixels in from the left and up from
// the bottom once it has slid in (PopupMessageViewModel).
constexpr int POPUP_HEIGHT = 4 + 64 + 4 + 2;

class OverlayImageHarness
{
public:
    // First, so it goes last: the factory's fonts and the popups' images must go while the application still runs.
    QtTestHost oQt;

    ra::services::mocks::MockClock mockClock;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ra::services::mocks::MockThreadPool mockThreadPool;
    ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
    ra::ui::mocks::MockImageRepository mockImageRepository;
    ra::ui::mocks::MockOverlayTheme mockTheme;
    QtSurfaceFactory surfaceFactory;
    OverlayManager overlayManager;
    OverlayImage overlayImage; // made on this thread: the tests' "emulator thread"

    const void* pPixels = nullptr;
    int nStride = 0;

    OverlayImageHarness() : m_oFactoryOverride(&surfaceFactory), m_oManagerOverride(&overlayManager)
    {
        mockWindowConfiguration.SetPopupLocation(Popup::Message, PopupLocation::BottomLeft);
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

private:
    ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> m_oFactoryOverride;
    ra::services::ServiceLocator::ServiceOverride<OverlayManager> m_oManagerOverride;
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

    TEST_METHOD(TestTheOverlayIsHiddenOnceThePopupEnds)
    {
        OverlayImageHarness harness;
        Assert::IsTrue(harness.ShowPopup(320, 240) > 0);

        harness.mockClock.AdvanceTime(std::chrono::seconds(6)); // past its five seconds
        Assert::AreEqual(0, harness.Update(320, 240));
        Assert::IsNull(harness.pPixels);
        Assert::AreEqual(0, harness.nStride);
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
        overlayManager.QueueMessage(L"Title", L"Description");

        const void* pPixels = &overlayImage;
        int nStride = -1;
        Assert::AreEqual(0, overlayImage.Update(320, 240, 1.0f, &pPixels, &nStride));
        Assert::IsNull(pPixels);
        Assert::AreEqual(0U, overlayImage.GetRenderCount());
    }

    TEST_METHOD(TestTheTestHookQueuesOnePopup)
    {
        OverlayImageHarness harness;
        setenv("RA_OVERLAY_TEST_POPUP", "Hook", 1);
        const int nSerial = harness.Update(320, 240);
        unsetenv("RA_OVERLAY_TEST_POPUP");

        Assert::IsTrue(nSerial > 0, L"the hook's popup was not shown");
        const auto* pPopup = harness.overlayManager.GetMessage(1);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Hook"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"RA_OVERLAY_TEST_POPUP"), pPopup->GetDescription());
        Assert::IsNull(harness.overlayManager.GetMessage(2));
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

        Assert::IsTrue(nExported >= nSerial, L"the export did not reach the overlay image");
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
