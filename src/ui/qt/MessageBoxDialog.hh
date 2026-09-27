#ifndef RA_UI_QT_MESSAGEBOXDIALOG_HH
#define RA_UI_QT_MESSAGEBOXDIALOG_HH
#pragma once

#include "ui/qt/IDialogPresenter.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"

#include <QMessageBox>

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// A MessageBoxViewModel as a QMessageBox: the Qt counterpart of ui/win32/MessageBoxDialog, laid out as its
/// TaskDialog is - the header as the main text, the message below it.
/// </summary>
class MessageBoxDialog : public QMessageBox
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;
    };

    explicit MessageBoxDialog(ra::ui::viewmodels::MessageBoxViewModel& vmMessageBox);

    /// <summary>
    /// The answer when the box closes with no button clicked: Esc, the close button, or shutdown. Cancel if the box
    /// has one, else No, else OK - never None, which callers read as the affirmative answer.
    /// </summary>
    static ra::ui::DialogResult GetEscapeAnswer(ra::ui::viewmodels::MessageBoxViewModel::Buttons nButtons) noexcept;

    /// <summary>Writes the answer to the view model - once - then closes as QMessageBox does.</summary>
    void done(int nResult) override;

private:
    ra::ui::viewmodels::MessageBoxViewModel* m_pViewModel; // cleared once answered: the caller may destroy it then
    ra::ui::viewmodels::MessageBoxViewModel::Buttons m_nButtons;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_MESSAGEBOXDIALOG_HH
