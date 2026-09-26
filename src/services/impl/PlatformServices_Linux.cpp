#ifndef _WIN32

#include "services/impl/PlatformServices.hh"

#include "services/IQtApplicationHost.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/HostThreadDispatcher.hh"
#include "services/impl/LinuxDebuggerDetector.hh"
#include "services/impl/LinuxFileSystem.hh"
#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/QtApplicationHost.hh"
#include "services/impl/QtAudioSystem.hh"
#include "services/impl/QtClipboard.hh"
#include "services/impl/StderrFileLogger.hh"

#include "ui/drawing/null/NullSurface.hh"
#include "ui/null/LinuxDesktop.hh"
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
    return std::make_unique<ra::ui::null::LinuxDesktop>();
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
    // An _RA_Init that finds the dispatcher and the host still registered - a
    // second one without _RA_Shutdown, or one after a shutdown that left them
    // in place - reuses them rather than replacing them. Replacing either would
    // destroy it under code still using it: the old QtAudioSystem and
    // QtClipboard hold the host by reference until RegisterServices replaces
    // them, and a pool worker (the login chime from HandleLoginResponse) can be
    // inside one of them, or inside the dispatcher's Invoke, right now.
    //
    // RegisterServices runs on the emulator's thread (from _RA_Init*), which is
    // the thread the dispatcher records as the host's - Reset() records it
    // again, since this _RA_Init may come from another thread than the last.
    if (ra::services::ServiceLocator::Exists<HostThreadDispatcher>())
        ra::services::ServiceLocator::GetMutable<HostThreadDispatcher>().Reset();
    else
        ra::services::ServiceLocator::Provide<HostThreadDispatcher>(std::make_unique<HostThreadDispatcher>());

    // The host restarts in place: Stop() first, then Start() on the same
    // object. The order matters: Start() looks the running Qt application up
    // through QCoreApplication::instance(), so a host started while this one
    // still owned its application would BORROW that application - which this
    // host's Stop() then destroys. Only this function registers a host, always
    // a QtApplicationHost; the cast only keeps a stranger (a test's) out.
    if (ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>())
    {
        auto& pRegistered = ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>();
        pRegistered.Stop();

        auto* pHost = dynamic_cast<QtApplicationHost*>(&pRegistered);
        if (pHost != nullptr)
        {
            pHost->Start();
            return;
        }
    }

    auto pHost = std::make_unique<QtApplicationHost>();
    pHost->Start();
    ra::services::ServiceLocator::Provide<ra::services::IQtApplicationHost>(std::move(pHost));
}

void StopPlatformServices()
{
    if (ra::services::ServiceLocator::Exists<HostThreadDispatcher>())
        ra::services::ServiceLocator::GetMutable<HostThreadDispatcher>().Shutdown();

    if (ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>())
        ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>().Stop();
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
