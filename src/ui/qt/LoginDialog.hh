#ifndef RA_UI_QT_LOGINDIALOG_HH
#define RA_UI_QT_LOGINDIALOG_HH
#pragma once

#include "ui/qt/IDialogPresenter.hh"
#include "ui/qt/ModalDialogBase.hh"
#include "ui/qt/bindings/CheckBoxBinding.hh"
#include "ui/qt/bindings/TextBoxBinding.hh"
#include "ui/viewmodels/LoginViewModel.hh"

class QLabel;

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// The Login dialog: the Qt counterpart of ui/win32/LoginDialog. OK logs in on the Qt thread, as Win32 logs in on its
/// UI thread; a failure keeps the dialog open and shows its reason in the box.
/// </summary>
class LoginDialog : public ModalDialogBase
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;
    };

    explicit LoginDialog(ra::ui::viewmodels::LoginViewModel& vmLogin);

protected:
    bool CanAccept() override;

private:
    ra::ui::viewmodels::LoginViewModel& m_vmLogin;
    QLabel* m_pError = nullptr; // owned by the dialog
    ra::ui::qt::bindings::TextBoxBinding m_bindUsername;
    ra::ui::qt::bindings::TextBoxBinding m_bindPassword;
    ra::ui::qt::bindings::CheckBoxBinding m_bindRememberMe;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_LOGINDIALOG_HH
