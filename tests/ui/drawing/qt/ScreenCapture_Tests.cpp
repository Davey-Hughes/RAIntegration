#ifndef _WIN32

#include "ui/drawing/qt/ScreenCapture.hh"

#include "Exports.hh"

#include "services/ServiceLocator.hh"

#include "ui/drawing/null/NullSurface.hh"
#include "ui/drawing/qt/QtSurface.hh"

#include "util/Strings.hh"

#include <QImage>

#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace drawing {
namespace qt {
namespace tests {

namespace {

// What the fake emulator answers: RA_InstallScreenCapture's function has no context, so it reads this.
struct FakePicture
{
    int nWidth = 0;
    int nHeight = 0;
    int nStride = 0;
    std::vector<uint32_t> vPixels;
    bool bNullPixels = false;
    int nAnswer = 1;
    std::atomic<int> nCalls{0};
};

FakePicture* g_pFake = nullptr;
std::atomic<int> g_nOtherCalls{0};

int FakeCapture(int* pWidth, int* pHeight, const void** ppPixels, int* pStride)
{
    ++g_pFake->nCalls;
    *pWidth = g_pFake->nWidth;
    *pHeight = g_pFake->nHeight;
    *pStride = g_pFake->nStride;
    *ppPixels = g_pFake->bNullPixels ? nullptr : g_pFake->vPixels.data();
    return g_pFake->nAnswer;
}

int OtherCapture(int*, int*, const void**, int*)
{
    ++g_nOtherCalls;
    return 0;
}

// the fake picture's pixel (x, y), with an alpha of 0 the capture must make opaque
constexpr uint32_t Pattern(int nX, int nY) noexcept
{
    return (static_cast<uint32_t>(nY) << 16) | (static_cast<uint32_t>(nX) << 4) | 0x08U;
}

uint32_t PixelAt(const QImage& oImage, int nX, int nY)
{
    return reinterpret_cast<const uint32_t*>(oImage.constScanLine(nY))[nX];
}

// A QtSurfaceFactory and a ScreenCapture made on this thread - the tests' emulator thread - with the fake installed.
class CaptureHarness
{
public:
    QtSurfaceFactory surfaceFactory;
    ScreenCapture screenCapture;
    FakePicture fake;

    CaptureHarness() : m_oFactoryOverride(&surfaceFactory)
    {
        g_pFake = &fake;
        screenCapture.SetCaptureFunction(FakeCapture);
    }

    ~CaptureHarness() noexcept { g_pFake = nullptr; }

    CaptureHarness(const CaptureHarness&) noexcept = delete;
    CaptureHarness& operator=(const CaptureHarness&) noexcept = delete;
    CaptureHarness(CaptureHarness&&) noexcept = delete;
    CaptureHarness& operator=(CaptureHarness&&) noexcept = delete;

    // nStride in bytes, 0 for none to spare; the padding holds 0xDEADBEEF
    void SetPicture(int nWidth, int nHeight, int nStride = 0)
    {
        fake.nWidth = nWidth;
        fake.nHeight = nHeight;
        fake.nStride = nStride != 0 ? nStride : nWidth * 4;
        const int nRowPixels = fake.nStride / 4;
        fake.vPixels.assign(static_cast<size_t>(nRowPixels) * static_cast<size_t>(nHeight), 0xDEADBEEFU);
        for (int nY = 0; nY < nHeight; ++nY)
        {
            for (int nX = 0; nX < nWidth; ++nX)
                fake.vPixels.at(static_cast<size_t>(nY) * nRowPixels + nX) = Pattern(nX, nY);
        }
    }

    ScreenCapture::Result Capture(std::unique_ptr<QtSurface>& pSurface) const
    {
        return screenCapture.Capture(pSurface);
    }

private:
    ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> m_oFactoryOverride;
};

} // namespace

TEST_CLASS(ScreenCapture_Tests)
{
public:
    TEST_METHOD(TestCopiesThePictureOpaqueWithItsRowsInOrder)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::Captured);

        Assert::IsNotNull(pSurface.get());
        Assert::AreEqual(3U, pSurface->GetWidth());
        Assert::AreEqual(2U, pSurface->GetHeight());
        const QImage& oImage = pSurface->GetImage();
        Assert::AreEqual(3, oImage.width());
        Assert::AreEqual(2, oImage.height());
        for (int nY = 0; nY < 2; ++nY)
        {
            for (int nX = 0; nX < 3; ++nX)
            {
                Assert::AreEqual(0xFF000000U | Pattern(nX, nY), PixelAt(oImage, nX, nY),
                                 ra::util::String::Printf(L"pixel (%d,%d)", nX, nY).c_str());
            }
        }
        Assert::AreEqual(1, harness.fake.nCalls.load());
    }

    TEST_METHOD(TestSkipsThePaddingAtTheEndOfEachRow)
    {
        CaptureHarness harness;
        harness.SetPicture(2, 3, 16); // 2 pixels a row, 4 pixels apart

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::Captured);

        const QImage& oImage = pSurface->GetImage();
        Assert::AreEqual(2, oImage.width());
        for (int nY = 0; nY < 3; ++nY)
        {
            Assert::AreEqual(0xFF000000U | Pattern(0, nY), PixelAt(oImage, 0, nY));
            Assert::AreEqual(0xFF000000U | Pattern(1, nY), PixelAt(oImage, 1, nY));
        }
    }

    TEST_METHOD(TestAtScaleTwoTheLogicalSizeIsHalvedAndRoundedDown)
    {
        CaptureHarness harness;
        harness.surfaceFactory.SetScale(2.0);
        harness.SetPicture(9, 4);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::Captured);

        Assert::AreEqual(4U, pSurface->GetWidth());
        Assert::AreEqual(2U, pSurface->GetHeight());
        Assert::AreEqual(9, pSurface->GetImage().width(), L"the device pixels are not all kept");
        Assert::AreEqual(2.0, pSurface->GetImage().devicePixelRatio());
    }

    TEST_METHOD(TestAtAFractionalScaleTheLogicalSizeRoundsDown)
    {
        CaptureHarness harness;
        harness.surfaceFactory.SetScale(1.25);
        harness.SetPicture(10, 5);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::Captured);

        Assert::AreEqual(8U, pSurface->GetWidth());
        Assert::AreEqual(4U, pSurface->GetHeight());
        Assert::AreEqual(10, pSurface->GetImage().width());
    }

    TEST_METHOD(TestRefusesOffTheEmulatorsThreadWithoutCallingIt)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);

        ScreenCapture::Result nResult = ScreenCapture::Result::Captured;
        bool bSurface = true;
        std::thread oOther([&harness, &nResult, &bSurface]() {
            std::unique_ptr<QtSurface> pSurface;
            nResult = harness.Capture(pSurface);
            bSurface = (pSurface != nullptr);
        });
        oOther.join();

        Assert::IsTrue(nResult == ScreenCapture::Result::NotOnEmulatorThread);
        Assert::IsFalse(bSurface);
        Assert::AreEqual(0, harness.fake.nCalls.load(), L"the emulator was called off its thread");
    }

    TEST_METHOD(TestRefusesWithoutAFunction)
    {
        CaptureHarness harness;
        harness.screenCapture.SetCaptureFunction(nullptr);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::NoFunction);
        Assert::IsNull(pSurface.get());
    }

    TEST_METHOD(TestRefusesAfterShutdownWithoutCallingTheEmulator)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);
        harness.screenCapture.Shutdown();

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::ShutDown);
        Assert::IsNull(pSurface.get());
        Assert::AreEqual(0, harness.fake.nCalls.load());
    }

    TEST_METHOD(TestRefusesWithoutAQtSurfaceFactoryWithoutCallingTheEmulator)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);
        ra::ui::drawing::null::NullSurfaceFactory oNullFactory;
        ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> oOverride(&oNullFactory);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::NoSurfaces);
        Assert::AreEqual(0, harness.fake.nCalls.load());
    }

    TEST_METHOD(TestAZeroAnswerIsNoPicture)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);
        harness.fake.nAnswer = 0;

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::NoPicture);
        Assert::IsNull(pSurface.get());
    }

    TEST_METHOD(TestEveryAnswerThatBreaksTheContractIsABadPicture)
    {
        struct Case
        {
            const wchar_t* sName;
            int nWidth;
            int nHeight;
            int nStride;
            bool bNullPixels;
        };
        const Case vCases[] = {
            {L"no width", 0, 2, 16, false},
            {L"negative height", 4, -1, 16, false},
            {L"wider than the limit", ScreenCapture::MaxSize + 1, 1, (ScreenCapture::MaxSize + 1) * 4, false},
            {L"taller than the limit", 1, ScreenCapture::MaxSize + 1, 4, false},
            {L"no pixels", 4, 2, 16, true},
            {L"rows shorter than the width", 4, 2, 12, false},
            {L"rows not whole pixels apart", 4, 2, 18, false},
        };

        for (const auto& oCase : vCases)
        {
            CaptureHarness harness;
            harness.SetPicture(4, 2, 20); // pixels enough for every case that reads them
            harness.fake.nWidth = oCase.nWidth;
            harness.fake.nHeight = oCase.nHeight;
            harness.fake.nStride = oCase.nStride;
            harness.fake.bNullPixels = oCase.bNullPixels;

            std::unique_ptr<QtSurface> pSurface;
            Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::BadPicture, oCase.sName);
            Assert::IsNull(pSurface.get(), oCase.sName);
        }
    }

    TEST_METHOD(TestTheLastFunctionInstalledIsTheOneCalled)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);
        g_nOtherCalls = 0;
        harness.screenCapture.SetCaptureFunction(OtherCapture);

        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::NoPicture);
        Assert::AreEqual(1, g_nOtherCalls.load());
        Assert::AreEqual(0, harness.fake.nCalls.load());
    }

    TEST_METHOD(TestDescribeNamesEveryResultDifferently)
    {
        std::set<std::string> vSeen;
        for (const auto nResult :
             {ScreenCapture::Result::Captured, ScreenCapture::Result::ShutDown, ScreenCapture::Result::NotOnEmulatorThread,
              ScreenCapture::Result::NoFunction, ScreenCapture::Result::NoSurfaces, ScreenCapture::Result::NoPicture,
              ScreenCapture::Result::BadPicture})
        {
            const std::string sText = ScreenCapture::Describe(nResult);
            Assert::IsFalse(sText.empty());
            Assert::IsTrue(vSeen.insert(sText).second, L"two results share a description");
        }
    }

    TEST_METHOD(TestTheExportInstallsTheFunctionInTheRegisteredScreenCapture)
    {
        CaptureHarness harness;
        harness.SetPicture(3, 2);
        harness.screenCapture.SetCaptureFunction(nullptr);
        ra::services::ServiceLocator::ServiceOverride<ScreenCapture> oOverride(&harness.screenCapture);

        _RA_InstallScreenCapture(FakeCapture);
        std::unique_ptr<QtSurface> pSurface;
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::Captured);

        _RA_InstallScreenCapture(nullptr);
        Assert::IsTrue(harness.Capture(pSurface) == ScreenCapture::Result::NoFunction);
    }

    TEST_METHOD(TestTheExportDoesNothingBeforeInit)
    {
        // no ScreenCapture registered: what a loader sees before RA_Init and after RA_Shutdown. The test is that this
        // returns at all: ServiceLocator::Get on a service nobody registered aborts the run (control o).
        _RA_InstallScreenCapture(FakeCapture);
        _RA_InstallScreenCapture(nullptr);
    }
};

} // namespace tests
} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !_WIN32
