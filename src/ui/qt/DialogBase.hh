#ifndef RA_UI_QT_DIALOGBASE_HH
#define RA_UI_QT_DIALOGBASE_HH
#pragma once

#include "ui/qt/bindings/WindowBinding.hh"

#include <QWidget>

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// A non-modal window bound to a view model: the Qt counterpart of ui/win32/DialogBase. It tells its binding when it
/// is shown, closed and resized, and deletes itself when closed, as a Win32 dialog's window is destroyed.
/// </summary>
/// <remarks>
/// A subclass builds its widgets, binds them through <c>m_bindWindow</c>, and calls
/// <c>m_bindWindow.SetWidget(*this)</c> last in its constructor. Qt thread only.
/// The view model must outlive the window: the binding is a member, and leaves the view model's notify targets
/// when the window is destroyed. QtDesktop's Shutdown and its Qt-host stop hook delete every window before the
/// view models go.
/// </remarks>
class DialogBase : public QWidget
{
public:
    explicit DialogBase(ra::ui::WindowViewModelBase& vmWindow);
    ~DialogBase() noexcept override = default;

    DialogBase(const DialogBase&) noexcept = delete;
    DialogBase& operator=(const DialogBase&) noexcept = delete;
    DialogBase(DialogBase&&) noexcept = delete;
    DialogBase& operator=(DialogBase&&) noexcept = delete;

protected:
    void showEvent(QShowEvent* pEvent) override;
    void closeEvent(QCloseEvent* pEvent) override;
    void resizeEvent(QResizeEvent* pEvent) override;

    ra::ui::qt::bindings::WindowBinding m_bindWindow;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_DIALOGBASE_HH
