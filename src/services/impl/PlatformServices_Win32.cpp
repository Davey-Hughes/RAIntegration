#ifdef _WIN32

#include "services/impl/PlatformServices.hh"

#include "services/impl/WindowsAudioSystem.hh"
#include "services/impl/WindowsClipboard.hh"
#include "services/impl/WindowsDebuggerDetector.hh"
#include "services/impl/WindowsDebuggerFileLogger.hh"
#include "services/impl/WindowsFileSystem.hh"
#include "services/impl/WindowsHttpRequester.hh"

#include "ui/drawing/gdi/GDIBitmapSurface.hh"
#include "ui/drawing/gdi/ImageRepository.hh"
#include "ui/win32/Desktop.hh"

namespace ra {
namespace services {
namespace impl {

std::unique_ptr<ra::services::IFileSystem> CreatePlatformFileSystem()
{
    return std::make_unique<WindowsFileSystem>();
}

std::unique_ptr<ra::services::ILogger> CreatePlatformLogger(const ra::services::IFileSystem& pFileSystem)
{
    return std::make_unique<WindowsDebuggerFileLogger>(pFileSystem);
}

std::unique_ptr<ra::services::IHttpRequester> CreatePlatformHttpRequester()
{
    return std::make_unique<WindowsHttpRequester>();
}

std::unique_ptr<ra::services::IClipboard> CreatePlatformClipboard()
{
    return std::make_unique<WindowsClipboard>();
}

std::unique_ptr<ra::services::IAudioSystem> CreatePlatformAudioSystem()
{
    return std::make_unique<WindowsAudioSystem>();
}

std::unique_ptr<ra::services::IDebuggerDetector> CreatePlatformDebuggerDetector()
{
    return std::make_unique<WindowsDebuggerDetector>();
}

std::unique_ptr<ra::ui::IDesktop> CreatePlatformDesktop()
{
    return std::make_unique<ra::ui::win32::Desktop>();
}

std::unique_ptr<ra::ui::drawing::ISurfaceFactory> CreatePlatformSurfaceFactory()
{
    return std::make_unique<ra::ui::drawing::gdi::GDISurfaceFactory>();
}

std::unique_ptr<ra::ui::IImageRepository> CreatePlatformImageRepository()
{
    auto pImageRepository = std::make_unique<ra::ui::drawing::gdi::ImageRepository>();
    pImageRepository->Initialize();
    return pImageRepository;
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // _WIN32
