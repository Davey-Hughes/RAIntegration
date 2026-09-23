#ifndef _WIN32

#include "services/impl/PlatformServices.hh"

#include "services/impl/FileLogger.hh"
#include "services/impl/LinuxFileSystem.hh"

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
    return std::make_unique<FileLogger>(pFileSystem); // StderrFileLogger in Task 7
}

std::unique_ptr<ra::services::IHttpRequester> CreatePlatformHttpRequester()
{
    return nullptr; // LinuxHttpRequester in Task 6
}

std::unique_ptr<ra::services::IClipboard> CreatePlatformClipboard()
{
    return nullptr; // QtClipboard in Task 8
}

std::unique_ptr<ra::services::IAudioSystem> CreatePlatformAudioSystem()
{
    return nullptr; // QtAudioSystem in Task 8
}

std::unique_ptr<ra::services::IDebuggerDetector> CreatePlatformDebuggerDetector()
{
    return nullptr; // LinuxDebuggerDetector in Task 7
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
