#ifndef RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
#define RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
#pragma once

#include <atomic>
#include <memory>
#include <thread>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

class QtSurface;

/// <summary>
/// The Linux counterpart of Windows' OverlayWindow: OverlayManager renders into an image the emulator draws over its
/// own picture (_RA_UpdateOverlayImage), where Windows gives it a layered window over the emulator's.
/// </summary>
/// <remarks>
/// OverlayManager's render, show and hide handlers only raise flags, from whichever thread calls them. The image is
/// rendered inside Update, on the emulator's thread - the one that constructed this, in RA_Init - and only when a
/// flag says something may have changed.
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
    /// _RA_UpdateOverlayImage: RA_Interface.h has the contract. The first call on the emulator's thread installs
    /// OverlayManager's handlers (they cannot be installed earlier: OverlayManager is registered after this).
    /// </summary>
    int Update(int nWidth, int nHeight, float fScale, const void** ppPixels, int* pStride);

    /// <summary>From now on Update returns 0: the library is shutting down (StopPlatformServices).</summary>
    void Shutdown() noexcept { m_bShutdown = true; }

    /// <summary>How many times the image has been redrawn, for the tests.</summary>
    unsigned GetRenderCount() const noexcept { return m_nRenderCount; }

private:
    // Shared with OverlayManager's handlers, which may outlive this object.
    struct Flags
    {
        std::atomic<bool> bDirty{false};
        std::atomic<bool> bVisible{false};
    };

    void Attach();
    void Redraw();

    const std::thread::id m_nHostThread;
    std::shared_ptr<Flags> m_pFlags;
    std::unique_ptr<QtSurface> m_pSurface;
    int m_nDeviceWidth = 0;
    int m_nDeviceHeight = 0;
    float m_fScale = 0.0f;
    int m_nSerial = 0;
    unsigned m_nRenderCount = 0;
    bool m_bAttached = false;
    bool m_bWarnedNoSurfaces = false;
    std::atomic<bool> m_bShutdown{false};
    std::atomic<bool> m_bWarnedOffThread{false};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_OVERLAYIMAGE_HH
