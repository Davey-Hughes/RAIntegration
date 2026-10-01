#ifndef RA_UI_DRAWING_QT_SCREENCAPTURE_HH
#define RA_UI_DRAWING_QT_SCREENCAPTURE_HH
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
/// The emulator's picture for achievement screenshots: what Windows' Desktop::CaptureClientArea BitBlts from the
/// emulator's window, the Linux emulator hands over through the function it installed with RA_InstallScreenCapture
/// (RA_Interface.h has the contract). QtDesktop::CaptureClientArea asks here.
/// </summary>
/// <remarks>
/// The emulator's function is called only on the emulator's thread - the one that constructed this, in RA_Init - and
/// only while nothing has begun shutting the library down. OverlayManager::CaptureScreenshot asks from inside
/// RA_DoAchievementsFrame for an unlock; a mastery can also arrive elsewhere (rc_client raises it from whichever call
/// next raises pending events), and is then refused rather than marshalled: a wait for the emulator's thread could
/// deadlock.
/// </remarks>
class ScreenCapture
{
public:
    /// <summary>RA_InstallScreenCapture's function.</summary>
    using CaptureFunction = int (*)(int* pWidth, int* pHeight, const void** ppPixels, int* pStride);

    /// <summary>What Capture did.</summary>
    enum class Result
    {
        Captured,
        ShutDown,            // RA_Shutdown has begun
        NotOnEmulatorThread, // only the thread that called RA_Init may call the emulator
        NoFunction,          // the emulator installed none
        NoSurfaces,          // no QtSurfaceFactory registered (no Qt application), or no image could be made
        NoPicture,           // the emulator's function answered 0
        BadPicture,          // its answer broke the contract
    };

    /// <summary>The largest width or height accepted.</summary>
    static constexpr int MaxSize = 16384;

    /// <summary>Records the calling thread as the emulator's.</summary>
    ScreenCapture() noexcept;
    ~ScreenCapture() noexcept = default;
    ScreenCapture(const ScreenCapture&) noexcept = delete;
    ScreenCapture& operator=(const ScreenCapture&) noexcept = delete;
    ScreenCapture(ScreenCapture&&) noexcept = delete;
    ScreenCapture& operator=(ScreenCapture&&) noexcept = delete;

    /// <summary>_RA_InstallScreenCapture: the emulator's function, or nullptr for none. Any thread.</summary>
    void SetCaptureFunction(CaptureFunction fpCapture) noexcept { m_fpCapture.store(fpCapture); }

    /// <summary>
    /// Asks the emulator for its picture and copies it, opaque, into a surface of exactly that many device pixels at
    /// the registered QtSurfaceFactory's scale. pSurface is set only when this returns Captured.
    /// </summary>
    Result Capture(std::unique_ptr<QtSurface>& pSurface) const;

    /// <summary>From now on Capture refuses without calling the emulator (StopPlatformServices).</summary>
    void Shutdown() noexcept { m_bShutdown = true; }

    /// <summary>Why a capture did not happen, for a log line.</summary>
    static const char* Describe(Result nResult) noexcept;

private:
    const std::thread::id m_nEmulatorThread;
    std::atomic<CaptureFunction> m_fpCapture{nullptr};
    std::atomic<bool> m_bShutdown{false};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_SCREENCAPTURE_HH
