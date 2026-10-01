#ifndef _WIN32

#include "ui/qt/LoginDialog.hh"
#include "ui/qt/QtDesktop.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/context/mocks/MockUserContext.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockLoginService.hh"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/ModalCaller.hh"
#include "tests/ui/qt/QtTestHost.hh"
#include "tests/ui/qt/RightAligningStyle.hh"

#include <QApplication>
#include <QCheckBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>

#include <chrono>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

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

// The visible LoginDialog, or nullptr. Qt thread.
LoginDialog* FindLoginDialog()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pDialog = dynamic_cast<LoginDialog*>(pWidget);
        if (pDialog != nullptr && pDialog->isVisible())
            return pDialog;
    }

    return nullptr;
}

// Rejects every visible LoginDialog. Qt thread. A test calls it before asserting, so a failure fails instead of
// leaving its caller waiting on a dialog.
void RejectAll()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pDialog = dynamic_cast<LoginDialog*>(pWidget);
        if (pDialog != nullptr && pDialog->isVisible())
            pDialog->reject();
    }
}

} // namespace

TEST_CLASS(LoginDialog_Tests)
{
public:
    TEST_METHOD(TestLabelsAreOnTheLeftWhateverTheStyle)
    {
        // Win32's dialog has its labels on the left; KDE's style would put a form's on the right
        LoginServices oServices;
        LoginViewModel vmLogin;
        QtTestHost oQt;

        int nAlignment = 0;
        oQt.RunOnQt([&vmLogin, &nAlignment]() {
            ra::ui::qt::tests::RightAligningStyle oStyle;
            auto* pDialog = new LoginDialog(vmLogin);
            pDialog->setStyle(&oStyle);
            nAlignment = static_cast<int>(pDialog->findChild<QFormLayout*>()->labelAlignment() & Qt::AlignHorizontal_Mask);
            delete pDialog;
        });

        Assert::AreEqual(static_cast<int>(Qt::AlignLeft), nAlignment);
    }

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
        std::wstring sError;
        oQt.RunOnQt([pDialog, &bVisible, &sError]() {
            Fill(*pDialog, QStringLiteral("User"), QString(), false);
            pDialog->accept();
            bVisible = pDialog->isVisible();
            sError = pDialog->findChild<QLabel*>(QStringLiteral("ErrorMessage"))->text().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bVisible, L"closed without a password");
        Assert::AreEqual(std::wstring(), oServices.sLastMessage, L"the box shows it inline: no message box");
        Assert::AreEqual(std::wstring(L"Password is required."), sError);
        Assert::AreEqual(DialogResult::None, vmLogin.GetDialogResult()); // unanswered: still open
    }

    TEST_METHOD(TestAFailedLoginKeepsItOpen)
    {
        LoginServices oServices;
        oServices.mockLoginService.MockLoginFailure(true, L"Invalid username/password combination. Please try again.");
        LoginViewModel vmLogin;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmLogin);

        bool bVisible = false, bLabelShown = false;
        std::wstring sError;
        oQt.RunOnQt([pDialog, &bVisible, &bLabelShown, &sError]() {
            Fill(*pDialog, QStringLiteral("User"), QStringLiteral("wrong"), false);
            pDialog->accept();
            bVisible = pDialog->isVisible();
            auto* pLabel = pDialog->findChild<QLabel*>(QStringLiteral("ErrorMessage"));
            bLabelShown = pLabel->isVisible();
            sError = pLabel->text().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bVisible, L"closed after a failed login");
        Assert::IsFalse(oServices.mockLoginService.IsLoggedIn());
        Assert::IsTrue(bLabelShown, L"the error label is hidden");
        Assert::AreEqual(std::wstring(L"Failed to login: Invalid username/password combination. Please try again."), sError);
        Assert::AreEqual(std::wstring(), oServices.sLastMessage, L"the reason belongs inside the box, not in a message box");
        Assert::AreEqual(DialogResult::None, vmLogin.GetDialogResult());
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
        Assert::AreEqual(std::wstring(), oServices.sLastMessage, L"a successful login closes the box without a message box");
        Assert::AreEqual(DialogResult::OK, vmLogin.GetDialogResult());
        Assert::IsTrue(oServices.mockLoginService.IsLoggedIn());
        Assert::AreEqual(std::string("APITOKEN"), oServices.mockConfiguration.GetApiToken());
    }

    TEST_METHOD(TestWithoutRememberMeNoTokenIsSaved)
    {
        LoginServices oServices;
        LoginViewModel vmLogin;
        vmLogin.SetPasswordRemembered(true); // the box starts checked: unchecking it is a user click the binding writes
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

    TEST_METHOD(TestTheDesktopShowsItForALoginViewModel)
    {
        // QtDesktop registers the Login presenter; without it, ShowModal answers without a dialog.
        LoginServices oServices;
        LoginViewModel vmLogin;
        QtTestHost oQt;
        QtDesktop oDesktop;

        // from a worker, as the token login's failure callback calls it
        ModalCaller oCaller(oDesktop, vmLogin);
        const bool bShown = oQt.WaitOnQt([]() { return FindLoginDialog() != nullptr; });
        if (bShown)
            oQt.RunOnQt([]() { FindLoginDialog()->reject(); });
        const bool bReturned = oCaller.Returned(2s);
        if (!bReturned)
            oQt.RunOnQt(RejectAll);

        Assert::IsTrue(bShown, L"the desktop showed no Login dialog");
        Assert::IsTrue(bReturned, L"rejecting the dialog did not release the caller");
        Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
