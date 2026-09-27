#ifndef RA_UI_QT_RICHPRESENCEDIALOG_HH
#define RA_UI_QT_RICHPRESENCEDIALOG_HH
#pragma once

#include "ui/qt/DialogBase.hh"
#include "ui/qt/IDialogPresenter.hh"
#include "ui/viewmodels/RichPresenceMonitorViewModel.hh"

#include <QPointer>

class QLabel;

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// The Rich Presence Monitor: the Qt counterpart of ui/win32/RichPresenceDialog. One label, bound to the display
/// string that Pulse() sets on a pool worker every second while the window is open.
/// </summary>
class RichPresenceDialog : public DialogBase
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;

    private:
        QPointer<RichPresenceDialog> m_pDialog;
    };

    explicit RichPresenceDialog(ra::ui::viewmodels::RichPresenceMonitorViewModel& vmRichPresence);

private:
    QLabel* m_pText = nullptr;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_RICHPRESENCEDIALOG_HH
