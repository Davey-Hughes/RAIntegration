#ifndef RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
#define RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
#pragma once

#include <QImage>

#include <atomic>
#include <memory>
#include <thread>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

class QtSurface;
class QtSurfaceFactory;

/// <summary>
/// The Linux counterpart of Windows' OverlayWindow: OverlayManager renders into an image the emulator draws over its
/// own picture (_RA_UpdateOverlayImage), where Windows gives it a layered window over the emulator's.
/// </summary>
/// <remarks>
/// OverlayManager's render, show and hide handlers only raise flags, from whichever thread calls them. The image is
/// drawn inside Update, on the emulator's thread - the one that constructed this, in RA_Init - and only when a flag
/// says something may have changed. It is repainted as OverlayWindow repaints its window: everything only when it is
/// first shown, shown again after a hide, or changes size or scale; otherwise only what moved or changed, over what
/// is already there. The emulator gets a copy of it at Windows' 90% opacity, made once for each picture that changed.
/// </remarks>
class OverlayImage
{
public:
    /// <summary>Records the calling thread as the emulator's.</summary>
    OverlayImage();
    ~OverlayImage() noexcept; // in the .cpp, where QtSurface is complete
    OverlayImage(const OverlayImage&) noexcept = delete;
    OverlayImage& operator=(const OverlayImage&) noexcept = delete;
    OverlayImage(OverlayImage&&) noexcept = delete;
    OverlayImage& operator=(OverlayImage&&) noexcept = delete;

    /// <summary>
    /// Installs OverlayManager's handlers, and queues the test hook's popup (RA_OVERLAY_TEST_POPUP). RA_Init calls it
    /// (Exports.cpp), on its own thread, once OverlayManager is registered and before any other thread can queue a
    /// popup. Once per object: a second call does nothing.
    /// </summary>
    void Attach();

    /// <summary>_RA_UpdateOverlayImage: RA_Interface.h has the contract.</summary>
    int Update(int nWidth, int nHeight, float fScale, const void** ppPixels, int* pStride);

    /// <summary>From now on Update returns 0: the library is shutting down (StopPlatformServices).</summary>
    void Shutdown() noexcept { m_bShutdown = true; }

    /// <summary>
    /// How many pictures have been made - a full redraw, or a repaint that drew something - for the tests.
    /// </summary>
    unsigned GetRenderCount() const noexcept { return m_nRenderCount; }

    /// <summary>
    /// How many times an image has been made for a new size or scale, those Qt could not make included, for the
    /// tests.
    /// </summary>
    unsigned GetImageAttempts() const noexcept { return m_nImageAttempts; }

private:
    // Shared with OverlayManager's handlers, which may outlive this object.
    struct Flags
    {
        std::atomic<bool> bDirty{false};
        std::atomic<bool> bShown{false}; // shown since Update last looked: everything is redrawn
        std::atomic<bool> bVisible{false};
    };

    bool MakeSurface(QtSurfaceFactory& pFactory, int nWidth, int nHeight, float fScale);
    bool Export();
    void Fail();

    const std::thread::id m_nHostThread;
    std::shared_ptr<Flags> m_pFlags;

    std::unique_ptr<QtSurface> m_pSurface; // the picture, at full opacity, repainted in place
    QImage m_oCopy;                        // the picture at 90%, while the theme is transparent
    const QImage* m_pExported = nullptr;   // what the emulator gets: m_oCopy, or the surface's own image
    int m_nDeviceWidth = 0;
    int m_nDeviceHeight = 0;
    float m_fScale = 0.0f;
    int m_nSerial = 0;

    // the last size Qt could not make an image for: not tried again until the emulator asks for another
    int m_nFailedWidth = 0;
    int m_nFailedHeight = 0;
    float m_fFailedScale = 0.0f;

    unsigned m_nRenderCount = 0;
    unsigned m_nImageAttempts = 0;
    bool m_bAttached = false;
    bool m_bAnnounced = false;
    bool m_bWarnedNoSurfaces = false;
    std::atomic<bool> m_bShutdown{false};
    std::atomic<bool> m_bWarnedOffThread{false};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
