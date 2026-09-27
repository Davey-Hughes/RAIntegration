#include "ui/qt/LoginDialog.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::viewmodels::LoginViewModel;

bool LoginDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const LoginViewModel*>(&vmWindow) != nullptr;
}

void LoginDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    // As MessageBoxDialog's: a modal nobody waits for would answer a view model that may be gone.
    RA_LOG_WARN("Login dialog \"%s\" shown without waiting for an answer - not shown; use ShowModal",
                ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
}

std::unique_ptr<QDialog> LoginDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    return std::make_unique<LoginDialog>(dynamic_cast<LoginViewModel&>(vmWindow));
}

LoginDialog::LoginDialog(LoginViewModel& vmLogin)
    : ModalDialogBase(vmLogin),
      m_vmLogin(vmLogin),
      m_bindUsername(vmLogin),
      m_bindPassword(vmLogin),
      m_bindRememberMe(vmLogin)
{
    // Win32's caption comes from the dialog resource; the view model has no title, so the WindowBinding keeps this.
    setWindowTitle(QStringLiteral("Login"));

    auto* pInformation = new QGroupBox(QStringLiteral("Information"), this);
    auto* pInformationText = new QLabel(QStringLiteral("Please enter your RetroAchievements username and password. "
                                                       "To create an account, please visit www.retroachievements.org"),
                                        pInformation);
    pInformationText->setWordWrap(true);
    auto* pInformationLayout = new QVBoxLayout(pInformation);
    pInformationLayout->addWidget(pInformationText);

    auto* pUsername = new QLineEdit(this);
    pUsername->setObjectName(QStringLiteral("Username"));
    auto* pPassword = new QLineEdit(this);
    pPassword->setObjectName(QStringLiteral("Password"));
    pPassword->setEchoMode(QLineEdit::Password);
    auto* pRememberMe = new QCheckBox(QStringLiteral("&Keep Me Logged In"), this);
    pRememberMe->setObjectName(QStringLiteral("RememberMe"));

    auto* pUsernameLabel = new QLabel(QStringLiteral("&Username:"), this);
    pUsernameLabel->setBuddy(pUsername);
    auto* pPasswordLabel = new QLabel(QStringLiteral("&Password:"), this);
    pPasswordLabel->setBuddy(pPassword);

    auto* pForm = new QFormLayout();
    pForm->addRow(pUsernameLabel, pUsername);
    pForm->addRow(pPasswordLabel, pPassword);
    pForm->addRow(QString(), pRememberMe);

    auto* pButtons = CreateButtons();
    pButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("&Login"));
    pButtons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("&Cancel"));

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(pInformation);
    pLayout->addLayout(pForm);
    pLayout->addWidget(pButtons);

    m_bindUsername.BindText(LoginViewModel::UsernameProperty);
    m_bindUsername.SetControl(*pUsername);
    RegisterTextBox(m_bindUsername);

    m_bindPassword.BindText(LoginViewModel::PasswordProperty);
    m_bindPassword.SetControl(*pPassword);
    RegisterTextBox(m_bindPassword);

    m_bindRememberMe.BindCheck(LoginViewModel::IsPasswordRememberedProperty);
    m_bindRememberMe.SetControl(*pRememberMe);
    RegisterBinding(m_bindRememberMe);

    m_bindWindow.SetWidget(*this);
}

bool LoginDialog::CanAccept()
{
    // As Win32's OnCommand(IDOK): Login() shows why it failed - nested, on this thread - and a failure keeps the
    // dialog open.
    return m_vmLogin.Login();
}

} // namespace qt
} // namespace ui
} // namespace ra
