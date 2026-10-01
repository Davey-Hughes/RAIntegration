#include "LoginService.hh"

#include "Exports.hh"
#include "util/Strings.hh"

#include "context/IRcClient.hh"
#include "context/UserContext.hh"

#include "api/impl/DisconnectedServer.hh"

#include "data/context/EmulatorContext.hh"
#include "data/context/SessionTracker.hh"

#include "services/AchievementRuntime.hh"
#include "services/AchievementRuntimeExports.hh"
#include "services/IConfiguration.hh"

#include "ui/viewmodels/MessageBoxViewModel.hh"
#include "ui/viewmodels/OverlayManager.hh"
#include "ui/viewmodels/WindowManager.hh"

namespace ra {
namespace services {
namespace impl {

bool LoginService::IsLoggedIn() const
{
    const auto& pUserContext = ra::services::ServiceLocator::Get<ra::context::UserContext>();
    return !pUserContext.GetApiToken().empty();
}

bool LoginService::Login(const std::string& sUsername, const std::string& sPassword, std::wstring& sErrorMessage)
{
    const auto pSynchronizer = std::make_shared<ra::services::AchievementRuntime::Synchronizer>();

    auto& pRuntime = ra::services::ServiceLocator::GetMutable<ra::services::AchievementRuntime>();
    pRuntime.BeginLoginWithPassword(sUsername, sPassword,
        [](int nResult, const char* sErrorMessage, rc_client_t*, void* pUserdata) {
            ra::services::AchievementRuntime::Synchronizer::CompleteShared(pUserdata, nResult, sErrorMessage);
        },
        ra::services::AchievementRuntime::Synchronizer::Share(pSynchronizer));

    if (!pSynchronizer->Wait())
    {
        // shutdown has begun: the login was abandoned, so there is no reason to show
        sErrorMessage.clear();
        return false;
    }

    if (pSynchronizer->GetResult() != RC_OK)
    {
        sErrorMessage = ra::util::String::Widen(pSynchronizer->GetErrorMessage());
        return false;
    }

    sErrorMessage.clear();
    return true;
}

void LoginService::Logout()
{
    auto& pUserContext = ra::services::ServiceLocator::GetMutable<ra::context::UserContext>();

    if (pUserContext.GetApiToken().empty())
        return;

    _RA_ActivateGame(0U);

    auto* pClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>().GetClient();
    rc_client_logout(pClient);

    pUserContext.Initialize("", "", "");

    // Forget the saved login too: the configuration keeps its own copy of the token (LoginViewModel saves it), and
    // saving it unchanged meant the next start logged straight back in. The username stays, so the login box is still
    // filled in.
    auto& pConfiguration = ra::services::ServiceLocator::GetMutable<ra::services::IConfiguration>();
    pConfiguration.SetApiToken("");
    pConfiguration.Save();

    ra::services::ServiceLocator::GetMutable<ra::ui::viewmodels::WindowManager>().Emulator.UpdateWindowTitle();
    ra::services::ServiceLocator::Get<ra::data::context::EmulatorContext>().RebuildMenu();
    RaiseClientExternalMenuChanged();

    ra::ui::viewmodels::MessageBoxViewModel::ShowInfoMessage(L"You are now logged out.");

    // update the global IServer instance to the disconnected API
    auto serverApi = std::make_unique<ra::api::impl::DisconnectedServer>(pConfiguration.GetHostUrl());
    ra::services::ServiceLocator::Provide<ra::api::IServer>(std::move(serverApi));
}

} // namespace impl
} // namespace services
} // namespace ra
