#include "OverlayImage.hh"

#include "QtSurface.hh"

#include "services/ServiceLocator.hh"

#include "ui/OverlayTheme.hh"
#include "ui/viewmodels/OverlayManager.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QColor>
#include <QImage>
#include <QPainter>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

namespace {

// Windows shows its overlay window at 90% opacity while the theme is transparent (OverlayWindow.cpp:
// SetLayeredWindowAttributes with LWA_ALPHA 255 * 90 / 100); the image the emulator gets has the same.
constexpr int WINDOWS_OVERLAY_ALPHA = 255 * 90 / 100;

// One sequence for the whole process, not one per OverlayImage: a new RA_Init's image never repeats a serial the
// emulator had from the last session's. Never 0, which says "nothing to draw".
std::atomic<int> s_nLastSerial{0};

int NextSerial() noexcept
{
    int nSerial = s_nLastSerial.load();
    int nNext = 0;
    do
    {
        nNext = (nSerial == std::numeric_limits<int>::max()) ? 1 : nSerial + 1;
    } while (!s_nLastSerial.compare_exchange_weak(nSerial, nNext));

    return nNext;
}

// Passes every call on to the image's surface, and counts those that draw: OverlayManager::Render through it, not
// redrawing everything, is OverlayWindow's repaint, and the count says whether that changed the picture.
class DrawCounter : public ISurface
{
public:
    explicit DrawCounter(ISurface& pSurface) noexcept : m_pSurface(pSurface) {}

    unsigned int GetWidth() const override { return m_pSurface.GetWidth(); }
    unsigned int GetHeight() const override { return m_pSurface.GetHeight(); }

    void FillRectangle(int nX, int nY, int nWidth, int nHeight, Color nColor) override
    {
        ++m_nDraws;
        m_pSurface.FillRectangle(nX, nY, nWidth, nHeight, nColor);
    }

    int LoadFont(const std::string& sFont, int nFontSize, FontStyles nStyle) override
    {
        return m_pSurface.LoadFont(sFont, nFontSize, nStyle);
    }

    ra::ui::Size MeasureText(int nFont, const std::wstring& sText) const override
    {
        return m_pSurface.MeasureText(nFont, sText);
    }

    void WriteText(int nX, int nY, int nFont, Color nColor, const std::wstring& sText) override
    {
        ++m_nDraws;
        m_pSurface.WriteText(nX, nY, nFont, nColor, sText);
    }

    void DrawImage(int nX, int nY, int nWidth, int nHeight, const ImageReference& pImage) override
    {
        ++m_nDraws;
        m_pSurface.DrawImage(nX, nY, nWidth, nHeight, pImage);
    }

    void DrawImageStretched(int nX, int nY, int nWidth, int nHeight, const ImageReference& pImage) override
    {
        ++m_nDraws;
        m_pSurface.DrawImageStretched(nX, nY, nWidth, nHeight, pImage);
    }

    void DrawSurface(int nX, int nY, const ISurface& pSurface) override
    {
        ++m_nDraws;
        m_pSurface.DrawSurface(nX, nY, pSurface);
    }

    void DrawSurface(int nX, int nY, const ISurface& pSurface, int nSurfaceX, int nSurfaceY, int nWidth,
                     int nHeight) override
    {
        ++m_nDraws;
        m_pSurface.DrawSurface(nX, nY, pSurface, nSurfaceX, nSurfaceY, nWidth, nHeight);
    }

    void SetOpacity(double fAlpha) override
    {
        ++m_nDraws;
        m_pSurface.SetOpacity(fAlpha);
    }

    void SetPixels(int nX, int nY, int nWidth, int nHeight, uint32_t* pARGB) override
    {
        ++m_nDraws;
        m_pSurface.SetPixels(nX, nY, nWidth, nHeight, pARGB);
    }

    unsigned int GetDraws() const noexcept { return m_nDraws; }

private:
    ISurface& m_pSurface;
    unsigned int m_nDraws = 0;
};

} // namespace

OverlayImage::OverlayImage() : m_nHostThread(std::this_thread::get_id()), m_pFlags(std::make_shared<Flags>()) {}

OverlayImage::~OverlayImage() noexcept = default;

void OverlayImage::Attach()
{
    if (m_bAttached || !ra::services::ServiceLocator::Exists<ra::ui::viewmodels::OverlayManager>())
        return;

    m_bAttached = true;

    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    pOverlayManager.SetRenderRequestHandler([pFlags = m_pFlags]() { pFlags->bDirty = true; });
    // bVisible last in both: Update looks at it first, so a change it sees there has raised the other flag already.
    // A show only asks for a picture, as OverlayWindow's only shows its window: OverlayManager also asks for one when
    // its render loop has gone idle with the overlay still showing (a tracker holding still), and a redraw of
    // everything then would erase what the sliding pause overlay has not reached yet. A hide makes the picture stale.
    pOverlayManager.SetShowRequestHandler([pFlags = m_pFlags]() noexcept {
        pFlags->bDirty = true;
        pFlags->bVisible = true;
    });
    pOverlayManager.SetHideRequestHandler([pFlags = m_pFlags]() noexcept {
        pFlags->bStale = true;
        pFlags->bVisible = false;
    });

    // anything queued before the handlers were in never asked to be shown
    pOverlayManager.RequestRender();
}

int OverlayImage::Update(int nWidth, int nHeight, float fScale, const void** ppPixels, int* pStride)
{
    if (ppPixels != nullptr)
        *ppPixels = nullptr;
    if (pStride != nullptr)
        *pStride = 0;

    // The image is the emulator thread's: drawn and read there, so it is never shared between threads.
    if (std::this_thread::get_id() != m_nHostThread)
    {
        if (!m_bWarnedOffThread.exchange(true))
            RA_LOG_WARN("_RA_UpdateOverlayImage called off the thread that called RA_Init: no overlay there");
        return 0;
    }

    if (m_bShutdown || ppPixels == nullptr || pStride == nullptr || nWidth <= 0 || nHeight <= 0)
        return 0;

    // Without a Qt application the platform gives a surface factory that draws nothing (PlatformServices_Linux.cpp).
    auto* pFactory =
        dynamic_cast<QtSurfaceFactory*>(&ra::services::ServiceLocator::GetMutable<ra::ui::drawing::ISurfaceFactory>());
    if (pFactory == nullptr)
    {
        if (!m_bWarnedNoSurfaces)
        {
            m_bWarnedNoSurfaces = true;
            RA_LOG_WARN("No Qt application: the overlay cannot be drawn");
        }
        return 0;
    }

    if (!m_bAnnounced)
    {
        m_bAnnounced = true;
        RA_LOG_INFO("Overlay: drawn by the emulator through _RA_UpdateOverlayImage");

        // RA_OVERLAY_TEST_POPUP set to anything but empty: the headless gate's hook (checks/native-headless.sh run
        // 4). One message popup, with the value for its title, so a screenshot has something to find. Read once,
        // here rather than at Attach (RA_Init): a loading game clears every popup right after RA_Init
        // (RA_ActivateGame -> OverlayManager::ClearPopups), which would destroy one queued that early before it was
        // ever drawn. RA_OVERLAY_TEST_POPUP_IMAGE, also set: the name of a badge the popup shows, so the gate can
        // find a badge drawn.
        const char* sTestPopup = std::getenv("RA_OVERLAY_TEST_POPUP");
        if (sTestPopup != nullptr && sTestPopup[0] != '\0')
        {
            RA_LOG_WARN("RA_OVERLAY_TEST_POPUP is set: showing a test popup");
            auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
            const char* sTestImage = std::getenv("RA_OVERLAY_TEST_POPUP_IMAGE");
            if (sTestImage != nullptr && sTestImage[0] != '\0')
            {
                pOverlayManager.QueueMessage(ra::util::String::Widen(sTestPopup), L"RA_OVERLAY_TEST_POPUP",
                                             ra::ui::ImageType::Badge, sTestImage);
            }
            else
            {
                pOverlayManager.QueueMessage(ra::util::String::Widen(sTestPopup), L"RA_OVERLAY_TEST_POPUP");
            }
        }
    }

    if (!m_pFlags->bVisible)
        return 0;

    if (!(fScale > 0.0f) || !std::isfinite(fScale))
        fScale = 1.0f;

    // OverlayWindow redraws everything when its window is first shown or changes size (m_bErase). Here also when the
    // scale changes, and when the overlay shows again after a hide: the image may still hold the last picture.
    bool bRedrawAll = false;
    if (m_pSurface == nullptr || nWidth != m_nDeviceWidth || nHeight != m_nDeviceHeight || fScale != m_fScale)
    {
        if (!MakeSurface(*pFactory, nWidth, nHeight, fScale))
            return 0;

        bRedrawAll = true;
    }

    // Taken only here, once the image is settled: a frame that returned above leaves them for the next. Taking the
    // render request without rendering would stop OverlayManager's render loop, which asks again only once rendered.
    const bool bStale = m_pFlags->bStale.exchange(false);
    const bool bDirty = m_pFlags->bDirty.exchange(false);
    bRedrawAll = bRedrawAll || bStale;

    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    bool bChanged = false;
    if (bRedrawAll)
    {
        m_pSurface->Clear();
        pOverlayManager.Render(*m_pSurface, true);
        bChanged = true;
    }
    else if (bDirty)
    {
        // OverlayWindow's repaint: only what moved or changed, over what the image already holds - while the overlay
        // slides in, the popups under it stay until it covers them. OverlayManager keeps asking for renders while
        // anything is shown (its bRequestRender), not only while something moves, so only a pass that drew something
        // makes a new picture.
        DrawCounter oCounter(*m_pSurface);
        pOverlayManager.Render(oCounter, false);
        bChanged = (oCounter.GetDraws() > 0);
    }

    if (bChanged && !Export())
        return 0;

    // the last popup may have finished in that render: OverlayManager called the hide handler
    if (!m_pFlags->bVisible || m_pExported == nullptr)
        return 0;

    *ppPixels = m_pExported->constBits();
    *pStride = static_cast<int>(m_pExported->bytesPerLine());
    return m_nSerial;
}

bool OverlayImage::MakeSurface(QtSurfaceFactory& pFactory, int nWidth, int nHeight, float fScale)
{
    // Qt could not make the image at this size last time: it is not tried again at every frame
    if (nWidth == m_nFailedWidth && nHeight == m_nFailedHeight && fScale == m_fFailedScale)
        return false;

    // the old picture goes first, so it never adds to what the new one needs
    m_pExported = nullptr;
    m_oCopy = QImage();
    m_pSurface.reset();

    ++m_nImageAttempts;
    m_nDeviceWidth = nWidth;
    m_nDeviceHeight = nHeight;
    m_fScale = fScale;
    pFactory.SetScale(fScale);
    m_pSurface = pFactory.CreateDeviceSurface(nWidth, nHeight, fScale);
    if (m_pSurface->GetImage().isNull())
    {
        Fail();
        return false;
    }

    return true;
}

bool OverlayImage::Export()
{
    const QImage& oPicture = m_pSurface->GetImage();
    if (ra::services::ServiceLocator::Get<ra::ui::OverlayTheme>().Transparent())
    {
        if (m_oCopy.size() != oPicture.size() || m_oCopy.format() != oPicture.format())
        {
            m_oCopy = QImage(oPicture.size(), oPicture.format());
            if (m_oCopy.isNull())
            {
                Fail();
                return false;
            }
        }

        // a copy, scaled: the surface keeps its full opacity, so no pixel is ever scaled twice
        std::memcpy(m_oCopy.bits(), oPicture.constBits(), static_cast<size_t>(oPicture.sizeInBytes()));
        QPainter oPainter(&m_oCopy);
        oPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
        oPainter.fillRect(m_oCopy.rect(), QColor(0, 0, 0, WINDOWS_OVERLAY_ALPHA));
        m_pExported = &m_oCopy;
    }
    else
    {
        m_oCopy = QImage();
        m_pExported = &oPicture;
    }

    m_nSerial = NextSerial();
    ++m_nRenderCount;
    return true;
}

void OverlayImage::Fail()
{
    // Qt refused the size, or the memory was not there: no overlay at this size, said once, since the size is not
    // tried again until the emulator asks for another
    RA_LOG_WARN("Overlay: no %dx%d image could be made (scale %d%%): no overlay at that size", m_nDeviceWidth,
                m_nDeviceHeight, static_cast<int>(std::lround(m_fScale * 100.0f)));
    m_nFailedWidth = m_nDeviceWidth;
    m_nFailedHeight = m_nDeviceHeight;
    m_fFailedScale = m_fScale;

    m_pExported = nullptr;
    m_oCopy = QImage();
    m_pSurface.reset();
}

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra
