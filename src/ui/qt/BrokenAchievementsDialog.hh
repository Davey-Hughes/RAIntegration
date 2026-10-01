#ifndef RA_UI_QT_BROKENACHIEVEMENTSDIALOG_HH
#define RA_UI_QT_BROKENACHIEVEMENTSDIALOG_HH
#pragma once

#include "ui/qt/IDialogPresenter.hh"
#include "ui/qt/ModalDialogBase.hh"
#include "ui/qt/bindings/GridBinding.hh"
#include "ui/viewmodels/BrokenAchievementsViewModel.hh"

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// The Report Achievement Problem dialog: the Qt counterpart of ui/win32/BrokenAchievementsDialog. Report Problem
/// asks the view model to submit, which opens the site's report page in the browser; the dialog stays open when there
/// is nothing to report.
/// </summary>
/// <remarks>
/// A disclosed Linux difference: selecting a row ticks it, because the grid binds the rows' selection to the same
/// IsSelected property its tick column shows. On Win32 only the tick counts, and a highlighted row with nothing ticked
/// answers "Please select an achievement."
/// </remarks>
class BrokenAchievementsDialog : public ModalDialogBase
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;
    };

    explicit BrokenAchievementsDialog(ra::ui::viewmodels::BrokenAchievementsViewModel& vmBrokenAchievements);

protected:
    bool CanAccept() override;

private:
    ra::ui::viewmodels::BrokenAchievementsViewModel& m_vmBrokenAchievements; // only until done() answers
    ra::ui::qt::bindings::GridBinding m_bindAchievements;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BROKENACHIEVEMENTSDIALOG_HH
