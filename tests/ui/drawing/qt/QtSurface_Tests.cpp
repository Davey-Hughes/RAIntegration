#ifndef _WIN32

#include "ui/drawing/qt/QtSurface.hh"

#include "services/IQtApplicationHost.hh"
#include "services/impl/PlatformServices.hh"

#include "ui/drawing/null/NullSurface.hh"

#include "tests/devkit/ui/mocks/MockImageRepository.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QImage>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace drawing {
namespace qt {
namespace tests {

using ra::ui::qt::tests::QtTestHost;

namespace {

// the pixel at device (nX, nY): premultiplied ARGB, 0xAARRGGBB
uint32_t PixelAt(const QtSurface& pSurface, int nX, int nY)
{
    return reinterpret_cast<const uint32_t*>(pSurface.GetImage().constScanLine(nY))[nX];
}

std::wstring Describe(uint32_t nPixel)
{
    char sBuffer[16];
    std::snprintf(sBuffer, sizeof(sBuffer), "0x%08X", nPixel);
    return std::wstring(sBuffer, sBuffer + std::strlen(sBuffer));
}

void AssertPixel(uint32_t nExpected, const QtSurface& pSurface, int nX, int nY)
{
    const uint32_t nActual = PixelAt(pSurface, nX, nY);
    if (nActual != nExpected)
    {
        const std::wstring sMessage = L"pixel (" + std::to_wstring(nX) + L"," + std::to_wstring(nY) + L") is " +
                                      Describe(nActual) + L", expected " + Describe(nExpected);
        Assert::Fail(sMessage.c_str());
    }
}

// every channel within one of the expected one: QPainter's blending rounds
void AssertPixelNear(uint32_t nExpected, const QtSurface& pSurface, int nX, int nY)
{
    const uint32_t nActual = PixelAt(pSurface, nX, nY);
    for (int nShift = 0; nShift < 32; nShift += 8)
    {
        const int nWant = static_cast<int>((nExpected >> nShift) & 0xFF);
        const int nGot = static_cast<int>((nActual >> nShift) & 0xFF);
        if (std::abs(nWant - nGot) > 1)
        {
            const std::wstring sMessage = L"pixel (" + std::to_wstring(nX) + L"," + std::to_wstring(nY) + L") is " +
                                          Describe(nActual) + L", expected about " + Describe(nExpected);
            Assert::Fail(sMessage.c_str());
        }
    }
}

int CountPainted(const QtSurface& pSurface, int nLeft, int nTop, int nRight, int nBottom, bool bInside)
{
    int nCount = 0;
    const QImage& oImage = pSurface.GetImage();
    for (int nY = 0; nY < oImage.height(); ++nY)
    {
        for (int nX = 0; nX < oImage.width(); ++nX)
        {
            const bool bIn = nX >= nLeft && nX < nRight && nY >= nTop && nY < nBottom;
            if (bIn == bInside && (PixelAt(pSurface, nX, nY) >> 24) != 0)
                ++nCount;
        }
    }
    return nCount;
}

std::unique_ptr<QtSurface> MakeSurface(const QtSurfaceFactory& oFactory, int nWidth, int nHeight)
{
    auto pSurface = oFactory.CreateSurface(nWidth, nHeight);
    return std::unique_ptr<QtSurface>(dynamic_cast<QtSurface*>(pSurface.release()));
}

// Only availability matters to CreatePlatformSurfaceFactory.
class FakeQtApplicationHost : public ra::services::IQtApplicationHost
{
public:
    explicit FakeQtApplicationHost(bool bAvailable) noexcept : m_bAvailable(bAvailable), m_Override(this) {}

    bool IsAvailable() const override { return m_bAvailable; }
    bool HasWidgets() const override { return false; }
    bool IsOnQtThread() const override { return false; }
    bool IsBorrowed() const noexcept override { return false; }
    void Invoke(std::function<void()>) const override {}
    bool InvokeAndWait(std::function<void()>, std::chrono::milliseconds) const override { return false; }
    void AddStopHook(std::function<void()>) override {}
    void Stop() override {}

private:
    bool m_bAvailable;
    ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> m_Override;
};

} // namespace

TEST_CLASS(QtSurface_Tests)
{
public:
    TEST_METHOD(TestFillRectangleWritesExactPremultipliedValues)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 8, 2);

        pSurface->FillRectangle(0, 0, 8, 2, Color(0xFF404040));
        pSurface->FillRectangle(0, 0, 2, 2, Color(0x80FF0000));
        pSurface->FillRectangle(4, 0, 2, 2, Color::Transparent); // replaces, as GDIBitmapSurface's does

        AssertPixel(0x80800000, *pSurface, 0, 0);
        AssertPixel(0x80800000, *pSurface, 1, 1);
        AssertPixel(0xFF404040, *pSurface, 2, 0);
        AssertPixel(0x00000000, *pSurface, 4, 0);
        AssertPixel(0x00000000, *pSurface, 5, 1);
        AssertPixel(0xFF404040, *pSurface, 7, 1);
    }

    TEST_METHOD(TestSetPixelsWritesExactPremultipliedValues)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 4, 4);
        pSurface->FillRectangle(0, 0, 4, 4, Color(0xFF00FF00));

        uint32_t pARGB[4] = {0x80FF0000, 0x00000000, 0xFF0000FF, 0x40FFFFFF}; // straight ARGB, rows top-down
        pSurface->SetPixels(1, 1, 2, 2, pARGB);

        AssertPixel(0x80800000, *pSurface, 1, 1);
        AssertPixel(0x00000000, *pSurface, 2, 1); // replaces what was under it
        AssertPixel(0xFF0000FF, *pSurface, 1, 2);
        AssertPixel(0x40404040, *pSurface, 2, 2);
        AssertPixel(0xFF00FF00, *pSurface, 0, 0);
        AssertPixel(0xFF00FF00, *pSurface, 3, 3);
    }

    TEST_METHOD(TestWriteTextLandsInsideMeasureTextsBoxAndNowhereElse)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 300, 60);
        const int nFont = pSurface->LoadFont("Tahoma", 22, FontStyles::Normal);
        Assert::AreNotEqual(0, nFont);

        // descenders too: the whole cell, top to bottom, is the font's
        const auto szText = pSurface->MeasureText(nFont, L"Welcome back, gjpqy");
        pSurface->WriteText(10, 5, nFont, Color(0xFFFFFFFF), L"Welcome back, gjpqy");

        // anti-aliasing may reach one pixel past the rounded advance (measured); top to bottom it is exact
        const int nRight = 10 + szText.Width + 1;
        Assert::IsTrue(CountPainted(*pSurface, 10, 5, nRight, 5 + szText.Height, true) > 100,
                       L"too little text inside the box");
        Assert::AreEqual(0, CountPainted(*pSurface, 10, 5, nRight, 5 + szText.Height, false));
    }

    TEST_METHOD(TestMeasureTextGrowsWithLengthAndSize)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 1, 1);
        const int nSmall = pSurface->LoadFont("Tahoma", 16, FontStyles::Normal);
        const int nLarge = pSurface->LoadFont("Tahoma", 26, FontStyles::Normal);

        const auto szShort = pSurface->MeasureText(nSmall, L"Welcome");
        const auto szLong = pSurface->MeasureText(nSmall, L"Welcome back");
        const auto szLarge = pSurface->MeasureText(nLarge, L"Welcome back");

        Assert::IsTrue(szShort.Width > 0);
        Assert::IsTrue(szLong.Width > szShort.Width);
        Assert::IsTrue(szLarge.Width > szLong.Width);
        // the cell height asked for, as GDI measures a font made with a positive height
        Assert::AreEqual(16, szLong.Height);
        Assert::AreEqual(26, szLarge.Height);
    }

    TEST_METHOD(TestAFontIdFromOneSurfaceDrawsOnAnother)
    {
        // PopupMessageViewModel measures on a 1x1 surface and writes with the same ids on the real one
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pMeasure = MakeSurface(oFactory, 1, 1);
        const int nFont = pMeasure->LoadFont("Tahoma", 18, FontStyles::Normal);
        auto pSurface = MakeSurface(oFactory, 100, 30);

        pSurface->WriteText(0, 0, nFont, Color(0xFFFFFFFF), L"Popup");

        Assert::IsTrue(CountPainted(*pSurface, 0, 0, 100, 30, true) > 20, L"nothing drawn with the other surface's font");
    }

    TEST_METHOD(TestDrawSurfaceBlendsHalfAlphaOverOpaque)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pTarget = MakeSurface(oFactory, 4, 1);
        pTarget->FillRectangle(0, 0, 4, 1, Color(0xFF00FF00));
        auto pSource = MakeSurface(oFactory, 2, 1);
        pSource->FillRectangle(0, 0, 1, 1, Color(0x800000FF));
        pSource->FillRectangle(1, 0, 1, 1, Color::Transparent);

        pTarget->DrawSurface(1, 0, *pSource);

        AssertPixel(0xFF00FF00, *pTarget, 0, 0);
        AssertPixelNear(0xFF007F80, *pTarget, 1, 0); // 50% blue over green
        AssertPixel(0xFF00FF00, *pTarget, 2, 0);     // transparent: what was under it
    }

    TEST_METHOD(TestDrawSurfacePartDrawsOnlyThatPart)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pTarget = MakeSurface(oFactory, 8, 1);
        auto pSource = MakeSurface(oFactory, 3, 1);
        pSource->FillRectangle(0, 0, 1, 1, Color(0xFFFF0000));
        pSource->FillRectangle(1, 0, 1, 1, Color(0xFF00FF00));
        pSource->FillRectangle(2, 0, 1, 1, Color(0xFF0000FF));

        pTarget->DrawSurface(5, 0, *pSource, 1, 0, 1, 1);

        AssertPixel(0xFF00FF00, *pTarget, 5, 0);
        AssertPixel(0x00000000, *pTarget, 4, 0);
        AssertPixel(0x00000000, *pTarget, 6, 0);
    }

    TEST_METHOD(TestSetOpacityScalesAlpha)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 3, 1);
        pSurface->FillRectangle(0, 0, 1, 1, Color(0xFFFFFFFF));
        pSurface->FillRectangle(1, 0, 1, 1, Color(0x80FF0000));

        pSurface->SetOpacity(229 / 255.0);

        AssertPixelNear(0xE5E5E5E5, *pSurface, 0, 0);
        AssertPixelNear(0x73730000, *pSurface, 1, 0);
        AssertPixel(0x00000000, *pSurface, 2, 0);
    }

    TEST_METHOD(TestDrawImageIsAHarmlessNoOp)
    {
        QtTestHost oQt;
        ra::ui::mocks::MockImageRepository mockImageRepository;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 8, 8);
        pSurface->FillRectangle(0, 0, 8, 8, Color(0xFF123456));
        const QImage oBefore = pSurface->GetImage().copy();

        const ImageReference pImage(ImageType::Badge, "12345");
        pSurface->DrawImage(0, 0, 8, 8, pImage);
        pSurface->DrawImageStretched(0, 0, 8, 8, pImage);

        Assert::IsTrue(pSurface->GetImage() == oBefore, L"the image changed");
    }

    TEST_METHOD(TestAtScaleTwoEveryDrawIsScaled)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        oFactory.SetScale(2.0);
        auto pSurface = MakeSurface(oFactory, 10, 5);

        Assert::AreEqual(10U, pSurface->GetWidth());
        Assert::AreEqual(5U, pSurface->GetHeight());
        Assert::AreEqual(20, pSurface->GetImage().width());
        Assert::AreEqual(10, pSurface->GetImage().height());

        pSurface->FillRectangle(0, 0, 2, 2, Color(0xFFFF0000));
        AssertPixel(0xFFFF0000, *pSurface, 3, 3);
        AssertPixel(0x00000000, *pSurface, 4, 4);

        // text too: drawn at twice the size, from the same logical layout
        const int nFont = pSurface->LoadFont("Tahoma", 4, FontStyles::Normal);
        Assert::AreEqual(4, pSurface->MeasureText(nFont, L"W").Height);
    }

    TEST_METHOD(TestADeviceSurfaceHasExactlyTheDeviceSize)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;

        const auto pHalf = oFactory.CreateDeviceSurface(1281, 721, 2.0);
        Assert::AreEqual(1281, pHalf->GetImage().width());
        Assert::AreEqual(721, pHalf->GetImage().height());
        Assert::AreEqual(640U, pHalf->GetWidth());
        Assert::AreEqual(360U, pHalf->GetHeight());

        const auto pNoScale = oFactory.CreateDeviceSurface(64, 48, 0.0); // treated as 1
        Assert::AreEqual(64U, pNoScale->GetWidth());
        Assert::AreEqual(48U, pNoScale->GetHeight());
    }

    TEST_METHOD(TestDrawSurfaceOfASurfaceWithoutPixelsDoesNothing)
    {
        // OverlayManager::RenderScreenshot draws NullDesktop::CaptureClientArea's NullSurface
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 4, 4);
        pSurface->FillRectangle(0, 0, 4, 4, Color(0xFF123456));
        const ra::ui::drawing::null::NullSurface oNull(4, 4);

        pSurface->DrawSurface(0, 0, oNull);
        pSurface->DrawSurface(0, 0, oNull, 0, 0, 2, 2);

        AssertPixel(0xFF123456, *pSurface, 1, 1);
    }

    TEST_METHOD(TestAnEmptySurfaceIgnoresEveryDraw)
    {
        QtTestHost oQt;
        QtSurfaceFactory oFactory;
        auto pEmpty = MakeSurface(oFactory, 0, 0);
        auto pOther = MakeSurface(oFactory, 2, 2);
        uint32_t pARGB[1] = {0xFFFFFFFF};

        pEmpty->FillRectangle(0, 0, 1, 1, Color(0xFFFFFFFF));
        pEmpty->WriteText(0, 0, pEmpty->LoadFont("Tahoma", 12, FontStyles::Normal), Color(0xFFFFFFFF), L"x");
        pEmpty->DrawSurface(0, 0, *pOther);
        pEmpty->SetOpacity(0.5);
        pEmpty->SetPixels(0, 0, 1, 1, pARGB);

        Assert::IsTrue(pEmpty->GetImage().isNull());
        Assert::AreEqual(0U, pEmpty->GetWidth());
    }

    TEST_METHOD(TestNoFontsWithoutAQtApplication)
    {
        // no QtTestHost: Qt would abort on a font
        QtSurfaceFactory oFactory;
        auto pSurface = MakeSurface(oFactory, 20, 20);

        const int nFont = pSurface->LoadFont("Tahoma", 16, FontStyles::Normal);
        pSurface->WriteText(0, 0, nFont, Color(0xFFFFFFFF), L"x");

        Assert::AreEqual(0, nFont);
        Assert::AreEqual(0, pSurface->MeasureText(nFont, L"x").Width);
        Assert::AreEqual(0, CountPainted(*pSurface, 0, 0, 20, 20, true));
    }

    TEST_METHOD(TestThePlatformFactoryIsQtOnlyWithAQtApplication)
    {
        {
            FakeQtApplicationHost oHost(true);
            const auto pFactory = ra::services::impl::CreatePlatformSurfaceFactory();
            Assert::IsNotNull(dynamic_cast<const QtSurfaceFactory*>(pFactory.get()));
        }
        {
            FakeQtApplicationHost oHost(false);
            const auto pFactory = ra::services::impl::CreatePlatformSurfaceFactory();
            Assert::IsNotNull(dynamic_cast<const ra::ui::drawing::null::NullSurfaceFactory*>(pFactory.get()));
        }
        {
            // no host registered at all
            const auto pFactory = ra::services::impl::CreatePlatformSurfaceFactory();
            Assert::IsNotNull(dynamic_cast<const ra::ui::drawing::null::NullSurfaceFactory*>(pFactory.get()));
        }
    }
};

} // namespace tests
} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !_WIN32
