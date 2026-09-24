#include "Exports.hh"

#include "context/IRcClient.hh"

#include "util/Log.hh"

#include "data/context/SessionTracker.hh"

#include "services/AchievementRuntime.hh"
#include "services/GameIdentifier.hh"
#include "services/IConfiguration.hh"
#include "services/IThreadPool.hh"
#include "services/Initialization.hh"
#include "services/ServiceLocator.hh"

#include "ui/IDesktop.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"
#include "ui/viewmodels/OverlayManager.hh"
#include "ui/viewmodels/WindowManager.hh"

// Everything from here to the matching #endif is Win32: the module and window
// handles the win32 views share, DllMain, and the GSL contract handler that
// src/pch.h routes Expects() to. Only the MSVC projects force-include pch.h, so
// off Windows GSL's own default applies and nothing calls the handler. The
// shutdown, reset and load-confirmation exports after the #endif are portable.
//
// DllMain's DLL_PROCESS_DETACH calls _RA_Shutdown() as a safety net for an
// emulator that never called RA_Shutdown(). There is deliberately no Linux
// counterpart such as __attribute__((destructor)). Loaded the way RA_Interface
// loads it - dlopen() - and never dlclose()d, which is exactly the case of an
// emulator that skipped RA_Shutdown(), such a function runs after this
// library's static destructors, with every ServiceLocator slot already gone,
// so it would have nothing left to shut down. That emulator gets the
// static-destruction teardown instead, and that can abort: ~ThreadPool logs
// through an ILogger slot that may already have been destroyed. Emulators
// must call RA_Shutdown() themselves.
#ifdef _WIN32
#include "RA_Core.h"

HMODULE g_hThisDLLInst = nullptr;
HWND g_RAMainWnd = nullptr;

BOOL APIENTRY DllMain(HMODULE hModule, DWORD dwReason, _UNUSED LPVOID)
{
    switch (dwReason)
    {
        case DLL_PROCESS_ATTACH:
            g_hThisDLLInst = hModule;
            ra::services::Initialization::RegisterCoreServices();
            break;

        case DLL_PROCESS_DETACH:
            _RA_Shutdown();
            IsolationAwareCleanup();
            break;
    }

    return TRUE;
}

static inline constexpr const char* __gsl_filename(const char* const str)
{
    if (str == nullptr)
        return str;

    if (str[0] == 's' && str[1] == 'r' && str[2] == 'c' && str[3] == '\\')
        return str;

    const char* scan = str;
    if (scan == nullptr)
        return str;

    while (*scan != '\\')
    {
        if (!*scan)
            return str;
        scan++;
    }

    return __gsl_filename(scan + 1);
}

#ifdef NDEBUG
void __gsl_contract_handler(const char* const file, unsigned int line)
{
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "Assertion failure at %s: %u", __gsl_filename(file), line);

    if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        RA_LOG_ERR(buffer);
    }

    if (ra::services::ServiceLocator::Exists<ra::ui::IDesktop>())
    {
        const auto sBuffer = ra::util::String::Widen(buffer);
        ra::ui::viewmodels::MessageBoxViewModel::ShowErrorMessage(L"Unexpected error", sBuffer);
    }

    gsl::details::throw_exception(gsl::fail_fast(buffer));
}
#else
void __gsl_contract_handler(const char* const file, unsigned int line, const char* const error)
{
    const char* const filename = __gsl_filename(file);
    const auto sError = ra::util::String::Printf("Assertion failure at %s: %d: %s", filename, line, error);

    if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        RA_LOG_ERR("%s", sError.c_str());
    }

    _wassert(ra::util::String::Widen(error).c_str(), ra::util::String::Widen(filename).c_str(), line);
}
#endif

#endif /* _WIN32 */

static int DoShutdown()
{
    // if the services haven't been registered, there's nothing to shut down
    if (!ra::services::Initialization::IsInitialized())
        return 0;

    ra::services::Initialization::StartShutdown();

    // detach any client-registered functions
    _RA_InstallSharedFunctionsExt(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);

    // notify the background threads as soon as possible so they start to wind down
    ra::services::ServiceLocator::GetMutable<ra::services::IThreadPool>().Shutdown(false);

    if (!ra::services::ServiceLocator::Get<ra::services::IConfiguration>()
        .IsFeatureEnabled(ra::services::Feature::Offline))
    {
        ra::services::ServiceLocator::Get<ra::services::GameIdentifier>().SaveKnownHashes();
    }

    ra::services::ServiceLocator::Get<ra::services::IConfiguration>().Save();

    ra::services::ServiceLocator::GetMutable<ra::data::context::SessionTracker>().EndSession();

    ra::services::ServiceLocator::GetMutable<ra::data::context::GameContext>().LoadGame(0U, "");

    if (ra::services::ServiceLocator::Exists<ra::ui::viewmodels::WindowManager>())
    {
        auto& pWindowManager = ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::WindowManager>();
        auto& pDesktop = ra::services::ServiceLocator::Get<ra::ui::IDesktop>();

        if (pWindowManager.RichPresenceMonitor.IsVisible())
            pDesktop.CloseWindow(pWindowManager.RichPresenceMonitor);

        if (pWindowManager.MemoryInspector.IsVisible())
            pDesktop.CloseWindow(pWindowManager.MemoryInspector);

        if (pWindowManager.MemoryBookmarks.IsVisible())
            pDesktop.CloseWindow(pWindowManager.MemoryBookmarks);

        if (pWindowManager.AssetEditor.IsVisible())
            pDesktop.CloseWindow(pWindowManager.AssetEditor);

        if (pWindowManager.AssetList.IsVisible())
            pDesktop.CloseWindow(pWindowManager.AssetList);

        if (pWindowManager.MemoryNotes.IsVisible())
            pDesktop.CloseWindow(pWindowManager.MemoryNotes);

        if (pWindowManager.PointerFinder.IsVisible())
            pDesktop.CloseWindow(pWindowManager.PointerFinder);
    }

    ra::services::ServiceLocator::GetMutable<ra::context::IRcClient>().Shutdown();

    ra::services::Initialization::Shutdown();

    RA_LOG_INFO("Shutdown complete");

    return 0;
}

API int CCONV _RA_Shutdown()
{
    try
    {
        return DoShutdown();
    }
    catch (std::runtime_error&)
    {

    }

    return 0;
}

API int CCONV _RA_ConfirmLoadNewRom(int bQuittingApp)
{
    //	Returns true if we can go ahead and load the new rom.
    std::wstring sModifiedSet;

    bool bPromotedModified = false;
    bool bUnpromotedModified = false;
    bool bLocalModified = false;

    const auto& pGameContext = ra::services::ServiceLocator::Get<ra::data::context::GameContext>();
    for (const auto& pAsset : pGameContext.Assets())
    {
        if (pAsset.IsModified() && pAsset.IsShownInList())
        {
            switch (pAsset.GetCategory())
            {
                case ra::data::models::AssetCategory::Local:
                    bLocalModified = true;
                    break;
                case ra::data::models::AssetCategory::Promoted:
                    bPromotedModified = true;
                    break;
                case ra::data::models::AssetCategory::Unpromoted:
                    bUnpromotedModified = true;
                    break;
            }
        }
    }

    if (bPromotedModified)
        sModifiedSet = L"Promoted";
    else if (bUnpromotedModified)
        sModifiedSet = L"Unpromoted";
    else if (bLocalModified)
        sModifiedSet = L"Local";
    else
        return true;

    ra::ui::viewmodels::MessageBoxViewModel vmMessageBox;
    vmMessageBox.SetHeader(bQuittingApp ? L"Are you sure that you want to exit?" : L"Continue load?");
    vmMessageBox.SetMessage(ra::util::String::Printf(L"You have unsaved changes in the %s achievements set. If you %s, you will lose these changes.",
        sModifiedSet, bQuittingApp ? L"quit now" : L"load a new ROM"));
    vmMessageBox.SetButtons(ra::ui::viewmodels::MessageBoxViewModel::Buttons::YesNo);
    vmMessageBox.SetIcon(ra::ui::viewmodels::MessageBoxViewModel::Icon::Warning);
    return (vmMessageBox.ShowModal() == ra::ui::DialogResult::Yes);
}

API void CCONV _RA_OnReset()
{
    // if there's no game loaded, there shouldn't be any active achievements or popups to clear - except maybe the
    // logging in messages, which we don't want to clear.
    if (ra::services::ServiceLocator::Get<ra::data::context::GameContext>().GameId() == 0U)
        return;

    // Temporarily disable achievements while the system is resetting. They will automatically re-enable when
    // DoAchievementsFrame is called if the trigger is not active. Prevents most unexpected triggering caused
    // by resetting the emulator.
    ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>().ResetRuntime();

    ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::OverlayManager>().ClearPopups();
}
