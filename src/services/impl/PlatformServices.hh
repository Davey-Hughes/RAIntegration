#ifndef RA_SERVICES_PLATFORMSERVICES_HH
#define RA_SERVICES_PLATFORMSERVICES_HH
#pragma once

#include "services/IAudioSystem.hh"
#include "services/IClipboard.hh"
#include "services/IDebuggerDetector.hh"
#include "services/IFileSystem.hh"
#include "services/IHttpRequester.hh"
#include "services/ILogger.hh"
#include "ui/IDesktop.hh"
#include "ui/IImageRepository.hh"
#include "ui/drawing/ISurface.hh"

#include <memory>

namespace ra {
namespace services {
namespace impl {

// One definition per platform, in PlatformServices_<Platform>.cpp. Keeping the
// selection here rather than in Initialization.cpp leaves the composition root
// free of preprocessor conditionals and free of any platform type name.

std::unique_ptr<ra::services::IFileSystem> CreatePlatformFileSystem();
std::unique_ptr<ra::services::ILogger> CreatePlatformLogger(const ra::services::IFileSystem& pFileSystem);
std::unique_ptr<ra::services::IHttpRequester> CreatePlatformHttpRequester();
std::unique_ptr<ra::services::IClipboard> CreatePlatformClipboard();
std::unique_ptr<ra::services::IAudioSystem> CreatePlatformAudioSystem();
std::unique_ptr<ra::services::IDebuggerDetector> CreatePlatformDebuggerDetector();
std::unique_ptr<ra::ui::IDesktop> CreatePlatformDesktop();
std::unique_ptr<ra::ui::drawing::ISurfaceFactory> CreatePlatformSurfaceFactory();
std::unique_ptr<ra::ui::IImageRepository> CreatePlatformImageRepository();

// Called first in Initialization::RegisterServices, on the emulator's thread,
// and in Initialization::Shutdown once the thread pool has drained, before any
// service is deregistered. Services the others are built on start and stop
// here - on Linux, the Qt application host, whose stop hooks therefore run
// while every service is still registered.
void StartPlatformServices();
void StopPlatformServices();

#ifndef _WIN32
// Called from Initialization::RegisterServices where Windows makes its OverlayWindow: provides the
// ui::drawing::qt::OverlayImage behind _RA_UpdateOverlayImage. A function rather than an #include there, which
// would move that file's Expects() lines - and so the Windows build's __LINE__ values.
void ProvidePlatformOverlayImage();
#endif

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_PLATFORMSERVICES_HH
