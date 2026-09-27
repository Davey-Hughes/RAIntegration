#ifndef RA_UI_QT_IDIALOGPRESENTER_HH
#define RA_UI_QT_IDIALOGPRESENTER_HH
#pragma once

#include "ui/WindowViewModelBase.hh"

#include <QDialog>

#include <memory>

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// Shows the windows of one kind of view model: the Qt counterpart of ui/win32/IDialogPresenter.
/// </summary>
class IDialogPresenter
{
public:
    virtual ~IDialogPresenter() noexcept = default;
    IDialogPresenter(const IDialogPresenter&) noexcept = delete;
    IDialogPresenter& operator=(const IDialogPresenter&) noexcept = delete;
    IDialogPresenter(IDialogPresenter&&) noexcept = delete;
    IDialogPresenter& operator=(IDialogPresenter&&) noexcept = delete;

    /// <summary>Whether this presenter shows windows for <paramref name="vmWindow" />. Any thread.</summary>
    virtual bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) = 0;

    /// <summary>Shows, or raises, a non-modal window for <paramref name="vmWindow" />. Qt thread.</summary>
    virtual void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) = 0;

    /// <summary>
    /// Creates the modal dialog for <paramref name="vmWindow" />, set up but not shown: the desktop shows it. Qt thread.
    /// </summary>
    /// <remarks>
    /// The dialog writes <paramref name="vmWindow" />'s DialogResult before it emits <c>finished()</c>, and never
    /// touches the view model after that: a caller on another thread may destroy the view model as soon as it wakes.
    /// Returns <c>nullptr</c> for a view model with no modal form; the presenter has then shown it as
    /// <see cref="ShowWindow" /> would, and ShowModal returns the view model's DialogResult at once, as Win32 does.
    /// The dialog is deleted after <c>finished()</c>, when the caller may already have destroyed the view model, so
    /// nothing in it may touch the view model on destruction - a BindingBase member, for one, whose destructor
    /// leaves the view model's notify targets. Presenters are owned by the desktop's shared state, which can be
    /// destroyed on any thread: a presenter's destructor must not touch widgets.
    /// </remarks>
    virtual std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) = 0;

protected:
    IDialogPresenter() noexcept = default;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_IDIALOGPRESENTER_HH
