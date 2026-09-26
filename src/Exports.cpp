#include "Exports.hh"

#include "RA_BuildVer.h"
#include "RA_Defs.h"
#include "util/Log.hh"
#include "RA_Resource.h"

#include "api/IServer.hh"

#include "context/IConsoleContext.hh"
#include "context/IRcClient.hh"
#include "context/UserContext.hh"
#include "context/impl/ConsoleContext.hh"
#include "context/impl/EmulatorMemoryContext.hh"

#include "data/context/EmulatorContext.hh"
#include "data/context/GameContext.hh"
#include "data/context/SessionTracker.hh"

#include "services/AchievementRuntime.hh"
#include "services/AchievementRuntimeExports.hh"
#include "services/FrameEventQueue.hh"
#include "services/GameIdentifier.hh"
#include "services/Http.hh"
#include "services/IAudioSystem.hh"
#include "services/IDebuggerDetector.hh"
#include "services/IConfiguration.hh"
#include "services/IFileSystem.hh"
#include "services/ILoginService.hh"
#include "services/Initialization.hh"
#include "services/PerformanceCounter.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/LoginService.hh"
#include "services/impl/OfflineRcClient.hh"

#ifndef _WIN32
#include "services/impl/HostThreadDispatcher.hh"
#endif

#include "ui/viewmodels/IntegrationMenuViewModel.hh"
#include "ui/viewmodels/LoginViewModel.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"
#include "ui/viewmodels/OverlayManager.hh"
#include "ui/viewmodels/WindowManager.hh"

/* Several blocks below are Win32, not merely non-test: the GDI surface, the
 * win32 Desktop and OverlayWindow, and ControlBinding's repaint handling.
 * RA_UTEST alone was enough to guard them while MSVC was the only compiler,
 * since a non-test build always had Windows.h; naming _WIN32 as well is what
 * lets the file compile for a non-test Linux build. On MSVC the two spellings
 * select exactly the same code. The blocks that are only non-test still say
 * #ifndef RA_UTEST: the per-frame view-model updates, because they are as
 * portable as the view models they drive, and _RA_UpdateHWnd, because
 * RA_Interface.h declares it for every platform - only its body is Win32, and
 * that has an #ifdef _WIN32 of its own. */
#if !defined(RA_UTEST) && defined(_WIN32)
#include "ui/drawing/gdi/GDISurface.hh"
#include "ui/win32/Desktop.hh"
#include "ui/win32/OverlayWindow.hh"
#include "ui/win32/bindings/ControlBinding.hh"
#endif

#include <RAInterface/RA_Emulators.h>

#include <rcheevos/include/rc_api_runtime.h>
#include <rcheevos/src/rapi/rc_api_common.h>
#include <rcheevos/src/rc_client_internal.h>

API const char* CCONV _RA_IntegrationVersion() { return RA_INTEGRATION_VERSION; }

API const char* CCONV _RA_HostName()
{
#if !defined(RA_UTEST) && !defined(_WIN32)
    // A loader calls this, or _RA_HostUrl, before any init: RA_Interface's
    // RA_InitCommon reads the host to choose between the online and offline
    // entry points. On Windows DllMain has registered the core services at
    // DLL_PROCESS_ATTACH by then. Nothing does that on Linux, so register
    // them here; it returns at once if they already are.
    ra::services::Initialization::RegisterCoreServices();
#endif
    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
    return pConfiguration.GetHostName().c_str();
}

API const char* CCONV _RA_HostUrl()
{
#if !defined(RA_UTEST) && !defined(_WIN32)
    // see _RA_HostName
    ra::services::Initialization::RegisterCoreServices();
#endif
    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
    return pConfiguration.GetHostUrl().c_str();
}

API int CCONV _RA_HardcoreModeIsActive()
{
    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
    return pConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore);
}

API int CCONV _RA_WarnDisableHardcore(const char* sActivity)
{
    std::string sActivityString;
    if (sActivity)
        sActivityString = sActivity;

    auto& pEmulatorContext = ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>();
    return pEmulatorContext.WarnDisableHardcoreMode(sActivityString) ? 1 : 0;
}

#ifndef RA_UTEST
API void CCONV _RA_UpdateHWnd([[maybe_unused]] RA_WindowHandle hMainHWND)
{
#ifdef _WIN32
    auto& pDesktop = dynamic_cast<ra::ui::win32::Desktop&>(ra::services::ServiceLocator::GetMutable<ra::ui::IDesktop>());
    if (hMainHWND != pDesktop.GetMainHWnd())
    {
        pDesktop.SetMainHWnd(hMainHWND);

        if (!IsExternalRcheevosClient())
        {
            auto& pOverlayWindow = ra::services::ServiceLocator::GetMutable<ra::ui::win32::OverlayWindow>();
            pOverlayWindow.CreateOverlayWindow(hMainHWND);
        }
    }
#else
    // The handle is reserved off Windows and ignored: the integration does not
    // parent its windows to the client's, and nothing else reads it.
#endif
}
#endif

static void InitializeOfflineMode()
{
    RA_LOG_INFO("Initializing offline mode");
    auto& pConfiguration = ra::services::ServiceLocator::GetMutable<ra::services::IConfiguration>();
    pConfiguration.SetFeatureEnabled(ra::services::Feature::Offline, true);

    ra::services::ServiceLocator::Provide<ra::context::IRcClient>(std::make_unique<ra::services::impl::OfflineRcClient>());

    // reattach hooks to new rc_client_t
    ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>().InitializeRcClient();

    auto& pUserContext = ra::services::ServiceLocator::GetMutable<ra::context::UserContext>();
    const auto& sUsername = pConfiguration.GetUsername();
    if (sUsername.empty())
    {
        pUserContext.Initialize("Player", "Player", "");
    }
    else
    {
        pUserContext.Initialize(sUsername, sUsername, "");

        auto& pSessionTracker = ra::services::ServiceLocator::GetMutable<ra::data::context::SessionTracker>();
        pSessionTracker.Initialize(sUsername);
    }

    RaiseClientExternalMenuChanged();

    auto* pClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>().GetClient();
    pClient->user.username = rc_buffer_strcpy(&pClient->state.buffer, pUserContext.GetUsername().c_str());
    pClient->user.display_name = rc_buffer_strcpy(&pClient->state.buffer, pUserContext.GetDisplayName().c_str());
    pClient->user.token = rc_buffer_strcpy(&pClient->state.buffer, pConfiguration.GetApiToken().c_str());
    pClient->state.user = RC_CLIENT_USER_STATE_LOGGED_IN;

    ra::services::ServiceLocator::GetMutable<ra::services::ILoginService>().DisableLogin();
}

static bool g_bPulseScheduled = false;
static void Pulse();

static void SchedulePulse()
{
    ra::services::ServiceLocator::GetMutable<ra::services::IThreadPool>().ScheduleAsync(std::chrono::seconds(1), []()
    {
        Pulse();
    });
}

static void Pulse()
{
    auto& vmRichPresence = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::WindowManager>().RichPresenceMonitor;
    if (vmRichPresence.IsVisible())
        vmRichPresence.UpdateDisplayString();

    auto& pRuntime = ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>();
    pRuntime.Idle();

    SchedulePulse();
}

static int InitCommon([[maybe_unused]] RA_WindowHandle hMainHWND, [[maybe_unused]] int nEmulatorID,
    [[maybe_unused]] const char* sClientName, const char* sClientVer, bool bOffline)
{
#ifndef RA_UTEST
    ra::services::Initialization::RegisterServices(ra::itoe<EmulatorID>(nEmulatorID), sClientName);
#endif

#if !defined(RA_UTEST) && defined(_WIN32)
    _RA_UpdateHWnd(hMainHWND);

    // When using SDL, the Windows message queue is never empty (there's a flood of WM_PAINT messages for the
    // SDL window). InvalidateRect only generates a WM_PAINT when the message queue is empty, so we have to
    // explicitly generate (and dispatch) a WM_PAINT message by calling UpdateWindow.
    switch (ra::itoe<EmulatorID>(nEmulatorID))
    {
        case RA_Libretro:
        case RA_Oricutron:
            ra::ui::win32::bindings::ControlBinding::SetNeedsUpdateWindow(true);
            break;

        default:
            ra::ui::win32::bindings::ControlBinding::SetNeedsUpdateWindow(false);
            break;
    }
#endif

    // Set the client version and User-Agent string
    ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>().SetClientVersion(sClientVer);

    if (bOffline)
    {
        InitializeOfflineMode();
    }
    else
    {
        // validate version (async call)
        ra::services::ServiceLocator::GetMutable<ra::services::IThreadPool>().RunAsync([]
        {
            if (!ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>().ValidateClientVersion())
                ra::services::ServiceLocator::GetMutable<ra::services::ILoginService>().Logout();
        });
    }

    if (_RA_HardcoreModeIsActive())
    {
        const auto& pDebuggerDetector = ra::services::ServiceLocator::GetMutable<ra::services::IDebuggerDetector>();
        if (pDebuggerDetector.IsDebuggerPresent())
        {
            if (ra::ui::viewmodels::MessageBoxViewModel::ShowWarningMessage(L"Disable Hardcore mode?",
                L"A debugger or similar tool has been detected. If you do not disable hardcore mode, RetroAchievements functionality will be disabled.",
                ra::ui::viewmodels::MessageBoxViewModel::Buttons::YesNo) == ra::ui::DialogResult::No)
            {
                return 0;
            }

            RA_LOG_INFO("Hardcore disabled by external tool");
            ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>().DisableHardcoreMode();
        }
    }

    if (!g_bPulseScheduled)
    {
        g_bPulseScheduled = true;
        SchedulePulse();
    }

    return 1;
}

API int CCONV _RA_InitOffline(RA_WindowHandle hMainHWND, /*enum EmulatorID*/int nEmulatorID, const char* sClientVer)
{
    return InitCommon(hMainHWND, nEmulatorID, nullptr, sClientVer, true);
}

API int CCONV _RA_InitClientOffline(RA_WindowHandle hMainHWND, const char* sClientName, const char* sClientVer)
{
    return InitCommon(hMainHWND, EmulatorID::UnknownEmulator, sClientName, sClientVer, true);
}

API int CCONV _RA_InitI(RA_WindowHandle hMainHWND, /*enum EmulatorID*/int nEmulatorID, const char* sClientVer)
{
    return InitCommon(hMainHWND, nEmulatorID, nullptr, sClientVer, false);
}

API int CCONV _RA_InitClient(RA_WindowHandle hMainHWND, const char* sClientName, const char* sClientVer)
{
    return InitCommon(hMainHWND, EmulatorID::UnknownEmulator, sClientName, sClientVer, false);
}

API void CCONV _RA_InstallSharedFunctions(bool(*)(void), void(*fpCauseUnpause)(void), void(*fpRebuildMenu)(void),
                                          void(*fpEstimateTitle)(char*), void(*fpResetEmulation)(void), void(*fpLoadROM)(const char*))
{
    _RA_InstallSharedFunctionsExt(nullptr, fpCauseUnpause, nullptr, fpRebuildMenu, fpEstimateTitle, fpResetEmulation, fpLoadROM);
}

API void CCONV _RA_InstallSharedFunctionsExt(bool(*)(void), void(*fpCauseUnpause)(void), void(*fpCausePause)(void), void(*fpRebuildMenu)(void),
                                             void(*fpEstimateTitle)(char*), void(*fpResetEmulation)(void), [[maybe_unused]] void(*fpLoadROM)(const char*))
{
    auto& pEmulatorContext = ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>();
    pEmulatorContext.SetResetFunction(fpResetEmulation);
    pEmulatorContext.SetPauseFunction(fpCausePause);
    pEmulatorContext.SetUnpauseFunction(fpCauseUnpause);
    pEmulatorContext.SetGetGameTitleFunction(fpEstimateTitle);
    pEmulatorContext.SetRebuildMenuFunction(fpRebuildMenu);
}

API void CCONV _RA_InstallHostDispatcher([[maybe_unused]] void (*fpPost)(void (*fpWork)(void*), void* pContext))
{
#ifndef _WIN32
    if (ra::services::ServiceLocator::Exists<ra::services::impl::HostThreadDispatcher>())
        ra::services::ServiceLocator::GetMutable<ra::services::impl::HostThreadDispatcher>().SetPostFunction(fpPost);
#endif
    // On Windows the toolkit marshals the emulator's callbacks through its own
    // dispatching window (WindowBinding::InvokeOnUIThread); nothing to install.
}

#ifdef _WIN32
API HMENU CCONV _RA_CreatePopupMenu()
{
    HMENU hMenu = CreatePopupMenu();

    ra::ui::viewmodels::LookupItemViewModelCollection vmMenuItems;
    ra::ui::viewmodels::IntegrationMenuViewModel::BuildMenu(vmMenuItems);

    for (const auto& pItem : vmMenuItems)
    {
        const int nId = pItem.GetId();
        if (nId == 0)
            AppendMenu(hMenu, MF_SEPARATOR, 0U, nullptr);
        else if (pItem.IsSelected())
            AppendMenu(hMenu, MF_CHECKED, nId, pItem.GetLabel().c_str());
        else
            AppendMenu(hMenu, MF_STRING, nId, pItem.GetLabel().c_str());
    }

    return hMenu;
}
#endif /* _WIN32 */

API int CCONV _RA_GetPopupMenuItems(RA_MenuItem *pItems)
{
    // have to keep a static variable here to hold label references
    static ra::ui::viewmodels::LookupItemViewModelCollection vmMenuItems;

    vmMenuItems.Clear();
    ra::ui::viewmodels::IntegrationMenuViewModel::BuildMenu(vmMenuItems);

    RA_MenuItem* pItem = pItems;
    for (const auto& vmItem : vmMenuItems)
    {
        const int nId = vmItem.GetId();
        pItem->nID = nId;

        if (nId == 0)
        {
            pItem->sLabel = nullptr;
            pItem->bChecked = 0;
        }
        else
        {
            pItem->sLabel = vmItem.GetLabel().c_str();
            pItem->bChecked = vmItem.IsSelected() ? 1 : 0;
        }

        ++pItem;
    }

    return gsl::narrow_cast<int>(vmMenuItems.Count());
}

API void CCONV _RA_InvokeDialog(RA_MenuItemId nID)
{
    ra::ui::viewmodels::IntegrationMenuViewModel::ActivateMenuItem(gsl::narrow_cast<int>(nID));
}

GSL_SUPPRESS_CON3
static void HandleLoginResponse(int nResult, const char* sErrorMessage, rc_client_t* pClient, void*)
{
    if (nResult == RC_OK)
    {
        // play the login sound
        ra::services::ServiceLocator::Get<ra::services::IAudioSystem>().PlayAudioFile(L"Overlay" RA_DIR_SEP_L L"login.wav");

        const auto& pSessionTracker = ra::services::ServiceLocator::Get<ra::data::context::SessionTracker>();
        const auto* pUser = rc_client_get_user_info(pClient);

        // start fetching the avatar image
        ra::services::ServiceLocator::GetMutable<ra::ui::IImageRepository>().FetchImage(
            ra::ui::ImageType::UserPic, pUser->username, pUser->avatar_url, pUser->avatar_last_updated);

        // show the welcome message
        std::unique_ptr<ra::ui::viewmodels::PopupMessageViewModel> vmMessage(
            new ra::ui::viewmodels::PopupMessageViewModel);
        vmMessage->SetTitle(ra::util::String::Printf(L"Welcome %s%s", pSessionTracker.HasSessionData() ? L"back " : L"",
                                             pUser->display_name));

        const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
        const auto bHardcore = pConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore);
        if (bHardcore)
            vmMessage->SetDescription(ra::util::String::Printf(L"%u points", pUser->score));
        else
            vmMessage->SetDescription(ra::util::String::Printf(L"%u points (softcore)", pUser->score_softcore));

        vmMessage->SetDetail((pUser->num_unread_messages == 1)
            ? L"You have 1 new message"
            : ra::util::String::Printf(L"You have %u new messages", pUser->num_unread_messages));

        vmMessage->SetImage(ra::ui::ImageType::UserPic, pUser->username);
        ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>().QueueMessage(vmMessage);
    }
    else
    {
        if (sErrorMessage && *sErrorMessage)
            ra::ui::viewmodels::MessageBoxViewModel::ShowErrorMessage(L"Login Failed", ra::util::String::Widen(sErrorMessage));
        else
            ra::ui::viewmodels::MessageBoxViewModel::ShowErrorMessage(L"Login Failed", L"Please login again.");

        // show the login dialog box
        ra::ui::viewmodels::LoginViewModel vmLogin;
        vmLogin.ShowModal();
    }
}

API void CCONV _RA_AttemptLogin(int bBlocking)
{
    auto& pLoginContext = ra::services::ServiceLocator::GetMutable<ra::services::ILoginService>();
    if (pLoginContext.IsLoginDisabled())
        return;

    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
    if (pConfiguration.GetApiToken().empty() || pConfiguration.GetUsername().empty())
    {
        // show the login dialog box
        ra::ui::viewmodels::LoginViewModel vmLogin;
        vmLogin.ShowModal();
    }
    else
    {
        auto& pRuntime = ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>();

        if (bBlocking)
        {
            ra::services::AchievementRuntime::Synchronizer pSynchronizer;

            pRuntime.BeginLoginWithToken(pConfiguration.GetUsername(), pConfiguration.GetApiToken(),
                [](int nResult, const char* sErrorMessage, rc_client_t*, void* pUserdata) {
                    auto* pSynchronizer = static_cast<ra::services::AchievementRuntime::Synchronizer*>(pUserdata);
                    Expects(pSynchronizer != nullptr);

                    pSynchronizer->CaptureResult(nResult, sErrorMessage);
                    pSynchronizer->Notify();
                },
                &pSynchronizer);

            pSynchronizer.Wait();

            auto* pClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>().GetClient();
            HandleLoginResponse(pSynchronizer.GetResult(), pSynchronizer.GetErrorMessage().c_str(),
                                pClient, nullptr);
        }
        else
        {
            pRuntime.BeginLoginWithToken(pConfiguration.GetUsername(), pConfiguration.GetApiToken(),
                                         HandleLoginResponse, static_cast<void*>(nullptr));
        }
    }
}

API const char* CCONV _RA_UserName()
{
    auto& pUserContext = ra::services::ServiceLocator::Get<ra::context::UserContext>();
    return pUserContext.GetDisplayName().c_str();
}

API void CCONV _RA_SetConsoleID(unsigned int nConsoleId)
{
    auto pContext = std::make_unique<ra::context::impl::ConsoleContext>(ra::itoe<ConsoleID>(nConsoleId));
    RA_LOG_INFO("Console set to %u (%s)", pContext->Id(), pContext->Name());
    ra::services::ServiceLocator::Provide<ra::context::IConsoleContext>(std::move(pContext));

    if (IsExternalRcheevosClient())
        ResetEmulatorMemoryRegionsForRcheevosClient();
}

API void CCONV _RA_SetUserAgentDetail(const char* sDetail)
{
    auto& pEmulatorContext = ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>();
    pEmulatorContext.SetClientUserAgentDetail(sDetail);
}

API void CCONV _RA_InstallMemoryBank(int nBankID, void* pReader, void* pWriter, int nBankSize)
{
    auto* pEmulatorMemoryContext = dynamic_cast<ra::context::impl::EmulatorMemoryContext*>(&ra::services::ServiceLocator::GetMutable<ra::context::IEmulatorMemoryContext>());
    if (pEmulatorMemoryContext)
    {
        pEmulatorMemoryContext->AddMemoryBlock(nBankID, nBankSize,
            reinterpret_cast<ra::context::impl::EmulatorMemoryContext::MemoryReadFunction*>(pReader),
            reinterpret_cast<ra::context::impl::EmulatorMemoryContext::MemoryWriteFunction*>(pWriter));
    }
}

API void CCONV _RA_InstallMemoryBankBlockReader(int nBankID, void* pReader)
{
    auto* pEmulatorMemoryContext = dynamic_cast<ra::context::impl::EmulatorMemoryContext*>(&ra::services::ServiceLocator::GetMutable<ra::context::IEmulatorMemoryContext>());
    if (pEmulatorMemoryContext)
    {
        pEmulatorMemoryContext->AddMemoryBlockReader(
            nBankID, reinterpret_cast<ra::context::impl::EmulatorMemoryContext::MemoryReadBlockFunction*>(pReader));
    }
}

API void CCONV _RA_ClearMemoryBanks()
{
    auto* pEmulatorMemoryContext = dynamic_cast<ra::context::impl::EmulatorMemoryContext*>(&ra::services::ServiceLocator::GetMutable<ra::context::IEmulatorMemoryContext>());
    if (pEmulatorMemoryContext)
        pEmulatorMemoryContext->ClearMemoryBlocks();;
}

API unsigned int CCONV _RA_IdentifyRom(const unsigned char* pROM, unsigned int nROMSize)
{
    return ra::services::ServiceLocator::GetMutable<ra::services::GameIdentifier>().IdentifyGame(pROM, nROMSize);
}

API unsigned int CCONV _RA_IdentifyHash(const char* sHash)
{
    return ra::services::ServiceLocator::GetMutable<ra::services::GameIdentifier>().IdentifyHash(sHash);
}

API void CCONV _RA_ActivateGame(unsigned int nGameId)
{
    _RA_SuspendRepaint();

    if (nGameId == 0)
    {
        auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
        pOverlayManager.ClearPopups();
        pOverlayManager.HideOverlayImmediately();

        auto& pRuntime = ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>();
        pRuntime.UnloadGame();
    }

    ra::services::ServiceLocator::GetMutable<ra::services::GameIdentifier>().ActivateGame(nGameId);
    _RA_ResumeRepaint();
}

API int CCONV _RA_OnLoadNewRom(const unsigned char* pROM, unsigned int nROMSize)
{
    ra::services::ServiceLocator::GetMutable<ra::services::GameIdentifier>().IdentifyAndActivateGame(pROM, nROMSize);
    return 0;
}

API void CCONV _RA_UpdateAppTitle(const char* sMessage)
{
    auto& vmEmulator = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::WindowManager>().Emulator;
    vmEmulator.SetAppTitleMessage(sMessage);
}

API int _RA_IsOverlayFullyVisible()
{
    return ra::services::ServiceLocator::Get<ra::ui::viewmodels::OverlayManager>().IsOverlayFullyVisible() ? 1 : 0;
}

_Use_decl_annotations_
API void _RA_NavigateOverlay(const ControllerInput* pInput)
{
    static const ControllerInput pNoInput{};
    if (pInput == nullptr)
        pInput = &pNoInput;

    ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>().Update(*pInput);
}

API void CCONV _RA_SetPaused(int bIsPaused)
{
    if (bIsPaused)
    {
        auto* pClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>().GetClient();
        if (!rc_client_can_pause(pClient, nullptr))
        {
            ra::services::ServiceLocator::Get<ra::data::context::EmulatorContext>().Unpause();
            return;
        }
    }

    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    if (bIsPaused)
        pOverlayManager.ShowOverlay();
    else
        pOverlayManager.HideOverlay();
}

#ifndef RA_UTEST

static void UpdateUIForFrameChange()
{
    TALLY_PERFORMANCE(PerformanceCheckpoint::OverlayManagerAdvanceFrame);
    auto& pOverlayManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>();
    pOverlayManager.AdvanceFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::MemoryBookmarksDoFrame);
    auto& pWindowManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::WindowManager>();
    pWindowManager.MemoryBookmarks.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::MemoryInspectorDoFrame);
    pWindowManager.MemoryInspector.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::AssetListDoFrame);
    auto& pGameContext = ra::services::ServiceLocator::GetMutable<ra::data::context::GameContext>();
    pGameContext.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::AssetEditorDoFrame);
    pWindowManager.AssetEditor.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::PointerFinderDoFrame);
    pWindowManager.PointerFinder.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::PointerInspectorDoFrame);
    pWindowManager.PointerInspector.DoFrame();

    TALLY_PERFORMANCE(PerformanceCheckpoint::FrameEvents);
    auto& pFrameEventQueue = ra::services::ServiceLocator::GetMutable<ra::services::FrameEventQueue>();
    pFrameEventQueue.DoFrame();
}

#endif

API void CCONV _RA_DoAchievementsFrame()
{
#ifndef _WIN32
    // The emulator's callbacks queued from other threads while it had no
    // dispatcher installed (see HostThreadDispatcher). Only on its own thread.
    if (ra::services::ServiceLocator::Exists<ra::services::impl::HostThreadDispatcher>())
        ra::services::ServiceLocator::GetMutable<ra::services::impl::HostThreadDispatcher>().DrainIfOnHostThread();
#endif

#if !defined(RA_UTEST) && defined(_WIN32)
    ra::ui::win32::bindings::ControlBinding::RepaintGuard guard;
#endif

    // make sure we process the achievements _before_ updating the UI.
    // the frozen bookmarks may modify the memory
    TALLY_PERFORMANCE(PerformanceCheckpoint::RuntimeProcess);
    auto& pRcheevosClient = ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>();
    pRcheevosClient.DoFrame();

#ifndef RA_UTEST
    UpdateUIForFrameChange();
#endif

    CHECK_PERFORMANCE();
}

API void CCONV _RA_SetForceRepaint([[maybe_unused]] int bEnable)
{
#if !defined(RA_UTEST) && defined(_WIN32)
    ra::ui::win32::bindings::ControlBinding::SetNeedsUpdateWindow(bEnable != 0);
#endif
}

API void CCONV _RA_SuspendRepaint()
{
#if !defined(RA_UTEST) && defined(_WIN32)
    ra::ui::win32::bindings::ControlBinding::SuspendRepaint();
#endif
}

API void CCONV _RA_ResumeRepaint()
{
#if !defined(RA_UTEST) && defined(_WIN32)
    ra::ui::win32::bindings::ControlBinding::ResumeRepaint();
#endif
}

API void CCONV _RA_OnSaveState(const char* sFilename)
{
    ra::services::ServiceLocator::Get<ra::services::AchievementRuntime>().SaveProgressToFile(sFilename);
}

API int CCONV _RA_CaptureState(char* pBuffer, int nBufferSize)
{
    GSL_SUPPRESS_TYPE1
    return ra::services::ServiceLocator::Get<ra::services::AchievementRuntime>().
        SaveProgressToBuffer(reinterpret_cast<uint8_t*>(pBuffer), nBufferSize);
}

static bool CanRestoreState()
{
    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();

    if (!ra::services::ServiceLocator::Get<ra::services::ILoginService>().IsLoggedIn())
    {
        if (!pConfiguration.IsFeatureEnabled(ra::services::Feature::Offline))
            return false;
    }

    if (pConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore))
    {
        // save state is being allowed by app (user should have been warned!)
        ra::ui::viewmodels::MessageBoxViewModel::ShowWarningMessage(L"Disabling Hardcore mode.", L"Loading save states is not allowed in Hardcore mode.");

        RA_LOG_INFO("Hardcore disabled by loading state");
        ra::services::ServiceLocator::GetMutable<ra::data::context::EmulatorContext>().DisableHardcoreMode();
    }

    return true;
}

void OnStateRestored()
{
    auto& pAssets = ra::services::ServiceLocator::GetMutable<ra::data::context::GameContext>().Assets();
    pAssets.BeginUpdate();
    for (auto& pAsset : pAssets)
    {
        switch (pAsset.GetType())
        {
            case ra::data::models::AssetType::Achievement:
            case ra::data::models::AssetType::Leaderboard:
                // synchronize the state
                pAsset.DoFrame();
                break;
        }
    }
    pAssets.EndUpdate();

#ifndef RA_UTEST
    UpdateUIForFrameChange();
#endif
}

API void CCONV _RA_OnLoadState(const char* sFilename)
{
    if (CanRestoreState())
    {
        ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>().LoadProgressFromFile(sFilename);
        OnStateRestored();
    }
}

API void CCONV _RA_RestoreState(const char* pBuffer)
{
    if (CanRestoreState())
    {
        GSL_SUPPRESS_TYPE1
        ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>().
            LoadProgressFromBuffer(reinterpret_cast<const uint8_t*>(pBuffer));
        OnStateRestored();
    }
}
