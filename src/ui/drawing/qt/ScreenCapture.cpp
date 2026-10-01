#include "ScreenCapture.hh"

#include "QtSurface.hh"

#include "services/IClock.hh"
#include "services/ServiceLocator.hh"

#include "ui/viewmodels/OverlayManager.hh"

#include "util/Log.hh"

#include <QImage>

#include <cstdint>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

ScreenCapture::ScreenCapture() noexcept : m_nEmulatorThread(std::this_thread::get_id()) {}

ScreenCapture::Result ScreenCapture::Capture(std::unique_ptr<QtSurface>& pSurface) const
{
    pSurface.reset();

    if (m_bShutdown)
        return Result::ShutDown;

    // the emulator draws its picture with its own GL context, current on its own thread
    if (std::this_thread::get_id() != m_nEmulatorThread)
        return Result::NotOnEmulatorThread;

    const auto fpCapture = m_fpCapture.load();
    if (fpCapture == nullptr)
        return Result::NoFunction;

    // OverlayManager renders the screenshot with the registered factory: without a Qt one there is nothing to
    // render with, so the emulator is not asked
    const auto* pFactory =
        dynamic_cast<const QtSurfaceFactory*>(&ra::services::ServiceLocator::Get<ISurfaceFactory>());
    if (pFactory == nullptr)
        return Result::NoSurfaces;

    int nWidth = 0;
    int nHeight = 0;
    int nStride = 0;
    const void* pPixels = nullptr;
    if (fpCapture(&nWidth, &nHeight, &pPixels, &nStride) == 0)
        return Result::NoPicture;

    // nWidth <= MaxSize, so nWidth * 4 cannot overflow; whole rows of 32-bit pixels only
    if (pPixels == nullptr || nWidth <= 0 || nHeight <= 0 || nWidth > MaxSize || nHeight > MaxSize ||
        nStride < nWidth * 4 || nStride % 4 != 0)
    {
        return Result::BadPicture;
    }

    auto pCaptured = pFactory->CreateDeviceSurface(nWidth, nHeight, pFactory->GetScale());
    QImage& oImage = pCaptured->GetMutableImage();
    if (oImage.width() != nWidth || oImage.height() != nHeight)
        return Result::NoSurfaces; // Qt could not make the image

    const auto* pRow = static_cast<const uint8_t*>(pPixels);
    for (int nY = 0; nY < nHeight; ++nY, pRow += nStride)
    {
        const auto* pFrom = reinterpret_cast<const uint32_t*>(pRow);
        auto* pTo = reinterpret_cast<uint32_t*>(oImage.scanLine(nY));
        for (int nX = 0; nX < nWidth; ++nX)
            pTo[nX] = pFrom[nX] | 0xFF000000U; // the top byte is ignored: the picture is opaque (RA_Interface.h)
    }

    pSurface = std::move(pCaptured);
    return Result::Captured;
}

const char* ScreenCapture::Describe(Result nResult) noexcept
{
    switch (nResult)
    {
        case Result::Captured:
            return "captured";
        case Result::ShutDown:
            return "the library is shutting down";
        case Result::NotOnEmulatorThread:
            return "not on the emulator's thread";
        case Result::NoFunction:
            return "the emulator installed no screen capture (RA_InstallScreenCapture)";
        case Result::NoSurfaces:
            return "no Qt surface to copy the picture into";
        case Result::NoPicture:
            return "the emulator had no picture";
        case Result::BadPicture:
            return "the emulator's picture broke RA_InstallScreenCapture's contract";
    }

    return "unknown";
}

void ScreenCapture::RequestTestScreenshot(int nPopupId, const std::wstring& sPath, std::chrono::milliseconds tDelay)
{
    m_nTestPopupId = nPopupId;
    m_sTestPath = sPath;
    m_tTestDue = ra::services::ServiceLocator::Get<ra::services::IClock>().UpTime() + tDelay;
}

void ScreenCapture::DoFrame()
{
    if (m_nTestPopupId == 0 || std::this_thread::get_id() != m_nEmulatorThread)
        return;

    if (ra::services::ServiceLocator::Get<ra::services::IClock>().UpTime() < m_tTestDue)
        return;

    const int nPopupId = m_nTestPopupId;
    m_nTestPopupId = 0;

    RA_LOG_INFO("Taking the test screenshot %s", m_sTestPath);
    ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>().CaptureScreenshot(nPopupId,
                                                                                                    m_sTestPath);
}

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra
