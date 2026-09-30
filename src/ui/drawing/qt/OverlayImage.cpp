#include "OverlayImage.hh"

#include "QtSurface.hh"

#include "services/ServiceLocator.hh"

#include "ui/OverlayTheme.hh"
#include "ui/viewmodels/OverlayManager.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QImage>

#include <cmath>
#include <cstdlib>
#include <limits>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

namespace {

// Windows shows its overlay window at 90% opacity while the theme is transparent (OverlayWindow.cpp:
// SetLayeredWindowAttributes with LWA_ALPHA 255 * 90 / 100); the finished image gets the same.
constexpr double WINDOWS_OVERLAY_OPACITY = (255 * 90 / 100) / 255.0;

// A surface with no pixels that counts what is drawn on it: OverlayManager::Render against it, without redrawing
// everything, draws only what moved or changed - Windows' repaint - so it says whether the picture changed.
class ChangeProbe : public ISurface
{
public:
    ChangeProbe(unsigned int nWidth, unsigned int nHeight) noexcept : m_nWidth(nWidth), m_nHeight(nHeight) {}

    unsigned int GetWidth() const noexcept override { return m_nWidth; }
    unsigned int GetHeight() const noexcept override { return m_nHeight; }

    void FillRectangle(int, int, int, int, Color) noexcept override { ++m_nDraws; }
    int LoadFont(const std::string&, int, FontStyles) noexcept override { return 0; }
    ra::ui::Size MeasureText(int, const std::wstring&) const noexcept override { return {0, 0}; }
    void WriteText(int, int, int, Color, const std::wstring&) noexcept override { ++m_nDraws; }
    void DrawImage(int, int, int, int, const ImageReference&) noexcept override { ++m_nDraws; }
    void DrawImageStretched(int, int, int, int, const ImageReference&) noexcept override { ++m_nDraws; }
    void DrawSurface(int, int, const ISurface&) noexcept override { ++m_nDraws; }
    void DrawSurface(int, int, const ISurface&, int, int, int, int) noexcept override { ++m_nDraws; }
    void SetOpacity(double) noexcept override { ++m_nDraws; }
    void SetPixels(int, int, int, int, uint32_t*) noexcept override { ++m_nDraws; }

    unsigned int GetDraws() const noexcept { return m_nDraws; }

private:
    unsigned int m_nWidth;
    unsigned int m_nHeight;
    unsigned int m_nDraws = 0;
};

} // namespace

OverlayImage::OverlayImage() : m_nHostThread(std::this_thread::get_id()), m_pFlags(std::make_shared<Flags>()) {}

OverlayImage::~OverlayImage() noexcept = default;

int OverlayImage::Update(int nWidth, int nHeight, float fScale, const void** ppPixels, int* pStride)
{
    if (ppPixels != nullptr)
        *ppPixels = nullptr;
    if (pStride != nullptr)
        *pStride = 0;

    // The image is the emulator thread's: rendered and read there, so it is never shared between threads.
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

    if (!m_bAttached)
        Attach();

    if (!m_pFlags->bVisible)
        return 0;

    if (!(fScale > 0.0f) || !std::isfinite(fScale))
        fScale = 1.0f;

    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    const bool bDirty = m_pFlags->bDirty.exchange(false);
    if (m_pSurface == nullptr || nWidth != m_nDeviceWidth || nHeight != m_nDeviceHeight || fScale != m_fScale)
    {
        pFactory->SetScale(fScale);
        m_pSurface = pFactory->CreateDeviceSurface(nWidth, nHeight, fScale);
        m_nDeviceWidth = nWidth;
        m_nDeviceHeight = nHeight;
        m_fScale = fScale;
        Redraw();
    }
    else if (bDirty)
    {
        // Render keeps asking for renders while anything is shown (its bRequestRender path), not only while
        // something moves: redraw only when this pass drew something.
        ChangeProbe oProbe(m_pSurface->GetWidth(), m_pSurface->GetHeight());
        pOverlayManager.Render(oProbe, false);
        if (oProbe.GetDraws() > 0)
            Redraw();
    }

    // the last popup may have finished in that render: OverlayManager called the hide handler
    if (!m_pFlags->bVisible)
        return 0;

    *ppPixels = m_pSurface->GetImage().constBits();
    *pStride = static_cast<int>(m_pSurface->GetImage().bytesPerLine());
    return m_nSerial;
}

void OverlayImage::Redraw()
{
    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    m_pSurface->Clear();
    pOverlayManager.Render(*m_pSurface, true);

    if (ra::services::ServiceLocator::Get<ra::ui::OverlayTheme>().Transparent())
        m_pSurface->SetOpacity(WINDOWS_OVERLAY_OPACITY);

    m_nSerial = (m_nSerial == std::numeric_limits<int>::max()) ? 1 : m_nSerial + 1; // never 0: 0 is "nothing"
    ++m_nRenderCount;
}

void OverlayImage::Attach()
{
    m_bAttached = true;

    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    pOverlayManager.SetRenderRequestHandler([pFlags = m_pFlags]() { pFlags->bDirty = true; });
    pOverlayManager.SetShowRequestHandler([pFlags = m_pFlags]() noexcept {
        pFlags->bVisible = true;
        pFlags->bDirty = true;
    });
    pOverlayManager.SetHideRequestHandler([pFlags = m_pFlags]() noexcept { pFlags->bVisible = false; });
    RA_LOG_INFO("Overlay: drawn by the emulator through _RA_UpdateOverlayImage");

    // RA_OVERLAY_TEST_POPUP set to anything but empty: the headless gate's hook (checks/native-headless.sh run 4).
    // One message popup, with the value for its title, so a screenshot has something to find.
    const char* sTestPopup = std::getenv("RA_OVERLAY_TEST_POPUP");
    if (sTestPopup != nullptr && sTestPopup[0] != '\0')
    {
        RA_LOG_WARN("RA_OVERLAY_TEST_POPUP is set: showing a test popup");
        pOverlayManager.QueueMessage(ra::util::String::Widen(sTestPopup), L"RA_OVERLAY_TEST_POPUP");
    }

    // anything queued before the handlers existed never asked to be shown
    pOverlayManager.RequestRender();
}

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra
