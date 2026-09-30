#include "CppUnitTest.h"

#include "services/impl/LoginService.hh"

#include "tests/ui/UIAsserts.hh"
#include "tests/devkit/context/mocks/MockEmulatorMemoryContext.hh"
#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/context/mocks/MockUserContext.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/mocks/MockAchievementRuntime.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockEmulatorContext.hh"
#include "tests/mocks/MockGameContext.hh"
#include "tests/mocks/MockGameIdentifier.hh"
#include "tests/mocks/MockLoginService.hh"
#include "tests/mocks/MockOverlayManager.hh"
#include "tests/mocks/MockServer.hh"
#include "tests/mocks/MockSessionTracker.hh"
#include "tests/mocks/MockWindowManager.hh"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

using ra::ui::viewmodels::MessageBoxViewModel;
using ra::ui::DialogResult;

namespace ra {
namespace services {
namespace tests {

TEST_CLASS(LoginService_Tests)
{
private:
    class LoginServiceHarness : public impl::LoginService
    {
    public:
        ra::context::mocks::MockEmulatorMemoryContext mockEmulatorMemoryContext;
        ra::context::mocks::MockRcClient mockRcClient;
        ra::context::mocks::MockUserContext mockUserContext;
        ra::data::context::mocks::MockEmulatorContext mockEmulatorContext;
        ra::data::context::mocks::MockGameContext mockGameContext;
        ra::services::mocks::MockGameIdentifier mockGameIdentifier;
        ra::data::context::mocks::MockSessionTracker mockSessionTracker;
        ra::services::mocks::MockAchievementRuntime mockAchievementRuntime;
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockLoginService mockLoginService;
        ra::ui::mocks::MockDesktop mockDesktop;
        ra::ui::viewmodels::mocks::MockOverlayManager mockOverlayManager;
        ra::ui::viewmodels::mocks::MockWindowManager mockWindowManager;
    };

public:
    TEST_METHOD(TestLoginSuccessful)
    {
        LoginServiceHarness pLogin;
        pLogin.mockRcClient.MockResponse("r=login2&u=user&p=Pa%24%24w0rd",
            "{\"Success\":true,\"User\":\"User\","
            "\"Token\":\"ApiToken\",\"Score\":12345,\"SoftcoreScore\":123,"
            "\"Messages\":0,\"Permissions\":1,\"AccountType\":\"Registered\"}");

        bool bWasMenuRebuilt = false;
        pLogin.mockEmulatorContext.SetRebuildMenuFunction([&bWasMenuRebuilt] { bWasMenuRebuilt = true; });

        Assert::IsTrue(pLogin.Login("user", "Pa$$w0rd"));
        Assert::IsFalse(pLogin.mockDesktop.WasDialogShown());

        // values should be updated in UserContext, including API token and score
        Assert::AreEqual(std::string("User"), pLogin.mockUserContext.GetUsername());
        Assert::AreEqual(std::string("ApiToken"), pLogin.mockUserContext.GetApiToken());
        Assert::AreEqual(12345U, pLogin.mockUserContext.GetScore());

        // session tracker should know user name
        Assert::AreEqual(std::wstring(L"User"), pLogin.mockSessionTracker.GetUsername());

        // emulator should have been notified to rebuild the RetroAchievements menu
        Assert::IsTrue(bWasMenuRebuilt);

        // app title should not be updated as _RA_UpdateAppTitle was not previously called
        Assert::AreEqual(std::wstring(L"Window"), pLogin.mockWindowManager.Emulator.GetWindowTitle());
    }

    TEST_METHOD(TestLoginInvalidPassword)
    {
        LoginServiceHarness pLogin;
        pLogin.mockRcClient.MockResponse("r=login2&u=User&p=Pa%24%24w0rd",
            "{\"Success\":false,\"Error\":\"Invalid User/Password combination. Please try again\"}");

        pLogin.mockDesktop.ExpectWindow<MessageBoxViewModel>([](MessageBoxViewModel& vmMessageBox)
        {
            Assert::AreEqual(std::wstring(L"Failed to login"), vmMessageBox.GetHeader());
            Assert::AreEqual(std::wstring(L"Invalid User/Password combination. Please try again"), vmMessageBox.GetMessage());
            Assert::AreEqual(MessageBoxViewModel::Icon::Error, vmMessageBox.GetIcon());
            return DialogResult::OK;
        });
        bool bWasMenuRebuilt = false;
        pLogin.mockEmulatorContext.SetRebuildMenuFunction([&bWasMenuRebuilt] { bWasMenuRebuilt = true; });

        Assert::IsFalse(pLogin.Login("User", "Pa$$w0rd"));
        Assert::IsTrue(pLogin.mockDesktop.WasDialogShown());

        // session tracker should not know user name
        Assert::AreEqual(std::wstring(L""), pLogin.mockSessionTracker.GetUsername());

        // emulator should not have been notified to rebuild the RetroAchievements menu
        Assert::IsFalse(bWasMenuRebuilt);

        // app title should not be updated
        Assert::AreEqual(std::wstring(L"Window"), pLogin.mockWindowManager.Emulator.GetWindowTitle());
    }

    TEST_METHOD(TestLogoutForgetsTheSavedToken)
    {
        LoginServiceHarness pLogin;
        pLogin.mockUserContext.Initialize("User", "User", "ApiToken");
        pLogin.mockConfiguration.SetUsername("User");
        pLogin.mockConfiguration.SetApiToken("ApiToken");

        pLogin.mockDesktop.ExpectWindow<MessageBoxViewModel>([](MessageBoxViewModel& vmMessageBox)
        {
            Assert::AreEqual(std::wstring(L"You are now logged out."), vmMessageBox.GetMessage());
            return DialogResult::OK;
        });

        pLogin.Logout();

        Assert::IsTrue(pLogin.mockUserContext.GetApiToken().empty());

        // the configuration must not keep the token, or Save() writes it back and the next start logs straight in
        Assert::AreEqual(std::string(""), pLogin.mockConfiguration.GetApiToken());

        // the username stays so the login box is still filled in
        Assert::AreEqual(std::string("User"), pLogin.mockConfiguration.GetUsername());
    }
};

} // namespace tests
} // namespace services
} // namespace ra
