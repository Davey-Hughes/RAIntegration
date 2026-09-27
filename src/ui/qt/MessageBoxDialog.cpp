#include "ui/qt/MessageBoxDialog.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QAbstractButton>
#include <QString>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::viewmodels::MessageBoxViewModel;

bool MessageBoxDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const MessageBoxViewModel*>(&vmWindow) != nullptr;
}

void MessageBoxDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    // Win32 shows a message box modally even from ShowWindow. Here it would be
    // a window nobody waits for: shutdown could not find it, and its answer
    // would go to a view model that may be gone. No caller does this (checked
    // 2026-09-27), so it is refused visibly rather than opened.
    RA_LOG_WARN("Message box \"%s\" shown without waiting for an answer - not shown; use ShowModal",
                ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
}

std::unique_ptr<QDialog> MessageBoxDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    return std::make_unique<MessageBoxDialog>(dynamic_cast<MessageBoxViewModel&>(vmWindow));
}

static QMessageBox::StandardButton ToStandardButton(ra::ui::DialogResult nResult) noexcept
{
    switch (nResult)
    {
        case ra::ui::DialogResult::Cancel:
            return QMessageBox::Cancel;
        case ra::ui::DialogResult::No:
            return QMessageBox::No;
        default:
            return QMessageBox::Ok;
    }
}

MessageBoxDialog::MessageBoxDialog(MessageBoxViewModel& vmMessageBox)
    : m_pViewModel(&vmMessageBox), m_nButtons(vmMessageBox.GetButtons())
{
    setWindowTitle(QString::fromStdWString(vmMessageBox.GetWindowTitle()));

    // Plain text - the setting covers the informative text too (measured,
    // Qt 6.11): a game title or a server error containing '<' is text, not
    // markup.
    setTextFormat(Qt::PlainText);
    if (vmMessageBox.GetHeader().empty())
    {
        setText(QString::fromStdWString(vmMessageBox.GetMessage()));
    }
    else
    {
        setText(QString::fromStdWString(vmMessageBox.GetHeader()));
        setInformativeText(QString::fromStdWString(vmMessageBox.GetMessage()));
    }

    switch (vmMessageBox.GetIcon())
    {
        default:
        case MessageBoxViewModel::Icon::None:
            setIcon(QMessageBox::NoIcon);
            break;
        case MessageBoxViewModel::Icon::Info:
            setIcon(QMessageBox::Information);
            break;
        case MessageBoxViewModel::Icon::Warning:
            setIcon(QMessageBox::Warning);
            break;
        case MessageBoxViewModel::Icon::Error:
            setIcon(QMessageBox::Critical);
            break;
    }

    switch (m_nButtons)
    {
        default:
        case MessageBoxViewModel::Buttons::OK:
            setStandardButtons(QMessageBox::Ok);
            break;
        case MessageBoxViewModel::Buttons::OKCancel:
            setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
            break;
        case MessageBoxViewModel::Buttons::YesNo:
            setStandardButtons(QMessageBox::Yes | QMessageBox::No);
            break;
        case MessageBoxViewModel::Buttons::YesNoCancel:
            setStandardButtons(QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel);
            break;
        case MessageBoxViewModel::Buttons::RetryCancel:
            setStandardButtons(QMessageBox::Retry | QMessageBox::Cancel);
            break;
    }

    // Set rather than left to Qt's detection, so Esc and done()'s "no button"
    // case are one rule.
    setEscapeButton(ToStandardButton(GetEscapeAnswer(m_nButtons)));
}

ra::ui::DialogResult MessageBoxDialog::GetEscapeAnswer(MessageBoxViewModel::Buttons nButtons) noexcept
{
    switch (nButtons)
    {
        case MessageBoxViewModel::Buttons::OKCancel:
        case MessageBoxViewModel::Buttons::YesNoCancel:
        case MessageBoxViewModel::Buttons::RetryCancel:
            return ra::ui::DialogResult::Cancel;

        case MessageBoxViewModel::Buttons::YesNo:
            return ra::ui::DialogResult::No;

        default:
        case MessageBoxViewModel::Buttons::OK:
            return ra::ui::DialogResult::OK;
    }
}

void MessageBoxDialog::done(int nResult)
{
    if (m_pViewModel != nullptr)
    {
        // Esc clicks the escape button; close() and reject() - the close
        // button, or shutdown - click nothing (measured, Qt 6.11), so "no
        // button" is mapped to the escape answer here.
        auto nAnswer = GetEscapeAnswer(m_nButtons);
        auto* pClicked = clickedButton();
        if (pClicked != nullptr)
        {
            switch (standardButton(pClicked))
            {
                case QMessageBox::Ok:
                    nAnswer = ra::ui::DialogResult::OK;
                    break;
                case QMessageBox::Cancel:
                    nAnswer = ra::ui::DialogResult::Cancel;
                    break;
                case QMessageBox::Yes:
                    nAnswer = ra::ui::DialogResult::Yes;
                    break;
                case QMessageBox::No:
                    nAnswer = ra::ui::DialogResult::No;
                    break;
                case QMessageBox::Retry:
                    nAnswer = ra::ui::DialogResult::Retry;
                    break;
                default:
                    break;
            }
        }

        // before finished() is emitted: the waiting caller reads it as soon as
        // it wakes, and may destroy the view model right after
        m_pViewModel->SetDialogResult(nAnswer);
        m_pViewModel = nullptr;
    }

    QMessageBox::done(nResult);
}

} // namespace qt
} // namespace ui
} // namespace ra
