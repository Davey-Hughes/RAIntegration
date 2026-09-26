#ifndef _WIN32

#include "services/impl/PlatformServices.hh"

#include "services/IQtApplicationHost.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/LinuxDebuggerDetector.hh"
#include "services/impl/LinuxFileSystem.hh"
#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/QtApplicationHost.hh"
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
    return std::make_unique<QtClipboard>(ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>());
}

std::unique_ptr<ra::services::IAudioSystem> CreatePlatformAudioSystem()
{
    return std::make_unique<QtAudioSystem>(ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>());
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

void StartPlatformServices()
{
    // A second _RA_Init without an intervening shutdown replaces the host
    // while the old one is still registered and running. It must be Stop()ped
    // BEFORE the new host is constructed: Start() looks the running Qt
    // application up through QCoreApplication::instance(), so if the old
    // host's application were still alive when the new host starts, the new
    // host would BORROW it - and then Provide() below would destroy the old
    // host, running its Stop() (from its destructor) and tearing that
    // application down out from under the new host that just borrowed it. It
    // is fine to leave the old host registered in the meantime; Provide()
    // replaces it once the new one has started.
    if (ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>())
        ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>().Stop();

    auto pHost = std::make_unique<QtApplicationHost>();
    pHost->Start();
    ra::services::ServiceLocator::Provide<ra::services::IQtApplicationHost>(std::move(pHost));
}

void StopPlatformServices()
{
    if (ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>())
        ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>().Stop();
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
