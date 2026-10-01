#ifndef RA_UI_DRAWING_QT_SCREENCAPTURE_HH
#define RA_UI_DRAWING_QT_SCREENCAPTURE_HH
#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
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
/// RA_DoAchievementsFrame for an unlock. A mastery is raised by whichever call next raises rc_client's pending events:
/// RA_DoAchievementsFrame, or the emulator's own RA_OnReset, RA_OnLoadState or RA_RestoreState, on its thread too,
/// and is taken there, as on Windows. On any other thread (rc_client_set_hardcore_enabled from a worker, say) it is
/// refused rather than marshalled: a wait for the emulator's thread could deadlock.
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

    /// <summary>
    /// The headless gate's hook (checks/native-headless.sh run 5): OverlayImage, having queued the test popup because
    /// RA_OVERLAY_TEST_POPUP is set, asks here for a screenshot of it into sPath when RA_OVERLAY_TEST_SCREENSHOT is.
    /// DoFrame takes it once tDelay has passed on the registered IClock - as OverlayManager::CaptureScreenshot takes
    /// an unlock's, from inside RA_DoAchievementsFrame.
    /// </summary>
    void RequestTestScreenshot(int nPopupId, const std::wstring& sPath, std::chrono::milliseconds tDelay);

    /// <summary>_RA_DoAchievementsFrame: takes the test screenshot once it is due. Emulator's thread only.</summary>
    void DoFrame();

private:
    const std::thread::id m_nEmulatorThread;
    std::atomic<CaptureFunction> m_fpCapture{nullptr};
    std::atomic<bool> m_bShutdown{false};

    // the test screenshot: set and taken on the emulator's thread (OverlayImage::Update, DoFrame)
    int m_nTestPopupId = 0;
    std::wstring m_sTestPath;
    std::chrono::steady_clock::time_point m_tTestDue{};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_SCREENCAPTURE_HH
