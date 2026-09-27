#ifndef _WIN32

#include "ui/qt/LoginDialog.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/context/mocks/MockUserContext.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockLoginService.hh"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QCheckBox>
#include <QLineEdit>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

using ra::ui::viewmodels::LoginViewModel;
using ra::ui::viewmodels::MessageBoxViewModel;

namespace {

// The services Login() uses; declared before the view model and the Qt host.
struct LoginServices
{
    ra::context::mocks::MockRcClient mockRcClient;
    ra::context::mocks::MockUserContext mockUserContext;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ra::services::mocks::MockLoginService mockLoginService;
    ra::ui::mocks::MockDesktop mockDesktop;
    std::wstring sLastMessage; // what Login() showed; written on the Qt thread, read after RunOnQt

    LoginServices()
    {
        // No Assert here: the handler runs on the Qt thread.
        mockDesktop.ExpectWindow<MessageBoxViewModel>([this](MessageBoxViewModel& vmMessageBox) {
            sLastMessage = vmMessageBox.GetMessage();
            return DialogResult::OK;
        });
    }
};

LoginDialog* Open(QtTestHost& oQt, LoginViewModel& vmLogin)
{
    LoginDialog* pDialog = nullptr;
    oQt.RunOnQt([&vmLogin, &pDialog]() {
        pDialog = new LoginDialog(vmLogin);
        pDialog->show();
    });
    return pDialog;
}

void Delete(QtTestHost& oQt, QDialog* pDialog)
{
    oQt.RunOnQt([pDialog]() { delete pDialog; });
}

// Fills the form as a user would, without leaving the fields: OK must still see the values. An edit marks the text
// modified, which is what OK's FlushPendingEdit writes.
void Type(QLineEdit& oLineEdit, const QString& sText)
{
    oLineEdit.setText(sText);
    oLineEdit.setModified(true);
}

void Fill(LoginDialog& oDialog, const QString& sUsername, const QString& sPassword, bool bRemember)
{
    Type(*oDialog.findChild<QLineEdit*>(QStringLiteral("Username")), sUsername);
    Type(*oDialog.findChild<QLineEdit*>(QStringLiteral("Password")), sPassword);
    auto* pRemember = oDialog.findChild<QCheckBox*>(QStringLiteral("RememberMe"));
    if (pRemember->isChecked() != bRemember)
        pRemember->click();
}

} // namespace

TEST_CLASS(LoginDialog_Tests)
{
public:
    TEST_METHOD(TestItShowsTheConfiguredUsernameAndHidesThePassword)
    {
        LoginServices oServices;
        oServices.mockConfiguration.SetUsername("Flower");
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        std::wstring sUsername, sTitle;
        bool bMasked = false;
        oQt.RunOnQt([pDialog, &sUsername, &sTitle, &bMasked]() {
            sUsername = pDialog->findChild<QLineEdit*>(QStringLiteral("Username"))->text().toStdWString();
            bMasked = pDialog->findChild<QLineEdit*>(QStringLiteral("Password"))->echoMode() == QLineEdit::Password;
            sTitle = pDialog->windowTitle().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"Flower"), sUsername);
        Assert::IsTrue(bMasked, L"the password is shown");
        Assert::AreEqual(std::wstring(L"Login"), sTitle);
    }

    TEST_METHOD(TestAMissingPasswordKeepsItOpen)
    {
        LoginServices oServices;
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        bool bVisible = false;
        oQt.RunOnQt([pDialog, &bVisible]() {
            Fill(*pDialog, QStringLiteral("User"), QString(), false);
            pDialog->accept();
            bVisible = pDialog->isVisible();
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bVisible, L"closed without a password");
        Assert::AreEqual(std::wstring(L"Password is required."), oServices.sLastMessage);
        Assert::AreEqual(DialogResult::None, vmLogin.GetDialogResult()); // unanswered: still open
    }

    TEST_METHOD(TestAFailedLoginKeepsItOpen)
    {
        LoginServices oServices;
        oServices.mockLoginService.MockLoginFailure(true);
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        bool bVisible = false;
        oQt.RunOnQt([pDialog, &bVisible]() {
            Fill(*pDialog, QStringLiteral("User"), QStringLiteral("wrong"), false);
            pDialog->accept();
            bVisible = pDialog->isVisible();
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bVisible, L"closed after a failed login");
        Assert::IsFalse(oServices.mockLoginService.IsLoggedIn());
    }

    TEST_METHOD(TestASuccessfulLoginClosesItAndRemembersTheToken)
    {
        LoginServices oServices;
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        bool bVisible = true;
        oQt.RunOnQt([pDialog, &bVisible]() {
            Fill(*pDialog, QStringLiteral("User"), QStringLiteral("pw"), true);
            pDialog->accept();
            bVisible = pDialog->isVisible();
        });
        Delete(oQt, pDialog);

        Assert::IsFalse(bVisible, L"still open after a successful login");
        Assert::AreEqual(DialogResult::OK, vmLogin.GetDialogResult());
        Assert::IsTrue(oServices.mockLoginService.IsLoggedIn());
        Assert::AreEqual(std::string("APITOKEN"), oServices.mockConfiguration.GetApiToken());
    }

    TEST_METHOD(TestWithoutRememberMeNoTokenIsSaved)
    {
        LoginServices oServices;
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        oQt.RunOnQt([pDialog]() {
            Fill(*pDialog, QStringLiteral("User"), QStringLiteral("pw"), false);
            pDialog->accept();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(DialogResult::OK, vmLogin.GetDialogResult());
        Assert::AreEqual(std::string(), oServices.mockConfiguration.GetApiToken());
    }

    TEST_METHOD(TestThePresenterCreatesItForALoginViewModelOnly)
    {
        LoginServices oServices;
        LoginViewModel vmLogin;
        MessageBoxViewModel vmMessage(L"not a login");
        QtTestHost oQt;

        bool bSupportsLogin = false, bSupportsMessage = true, bCreated = false;
        oQt.RunOnQt([&vmLogin, &vmMessage, &bSupportsLogin, &bSupportsMessage, &bCreated]() {
            LoginDialog::Presenter oPresenter;
            bSupportsLogin = oPresenter.IsSupported(vmLogin);
            bSupportsMessage = oPresenter.IsSupported(vmMessage);
            auto pDialog = oPresenter.CreateModal(vmLogin);
            bCreated = (dynamic_cast<LoginDialog*>(pDialog.get()) != nullptr);
        });

        Assert::IsTrue(bSupportsLogin);
        Assert::IsFalse(bSupportsMessage);
        Assert::IsTrue(bCreated);
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
