#ifndef _WIN32

#include "services/impl/PlatformServices.hh"

#include "services/impl/LinuxDebuggerDetector.hh"
#include "services/impl/LinuxFileSystem.hh"
#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/QtAudioSystem.hh"
#include "services/impl/QtClipboard.hh"
#include "services/impl/StderrFileLogger.hh"

#include "ui/drawing/null/NullSurface.hh"
#include "ui/null/NullDesktop.hh"
#include "ui/null/NullImageRepository.hh"

namespace ra {
namespace services {
namespace impl {

std::unique_ptr<ra::services::IFileSystem> CreatePlatformFileSystem()
{
    return std::make_unique<LinuxFileSystem>();
}

std::unique_ptr<ra::services::ILogger> CreatePlatformLogger(const ra::services::IFileSystem& pFileSystem)
{
    return std::make_unique<StderrFileLogger>(pFileSystem);
}

std::unique_ptr<ra::services::IHttpRequester> CreatePlatformHttpRequester()
{
    return std::make_unique<LinuxHttpRequester>();
}

std::unique_ptr<ra::services::IClipboard> CreatePlatformClipboard()
{
    return std::make_unique<QtClipboard>();
}

std::unique_ptr<ra::services::IAudioSystem> CreatePlatformAudioSystem()
{
    return std::make_unique<QtAudioSystem>();
}

std::unique_ptr<ra::services::IDebuggerDetector> CreatePlatformDebuggerDetector()
{
    return std::make_unique<LinuxDebuggerDetector>();
}

std::unique_ptr<ra::ui::IDesktop> CreatePlatformDesktop()
{
    return std::make_unique<ra::ui::null::NullDesktop>();
}

std::unique_ptr<ra::ui::drawing::ISurfaceFactory> CreatePlatformSurfaceFactory()
{
    return std::make_unique<ra::ui::drawing::null::NullSurfaceFactory>();
}

std::unique_ptr<ra::ui::IImageRepository> CreatePlatformImageRepository()
{
    return std::make_unique<ra::ui::null::NullImageRepository>();
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
