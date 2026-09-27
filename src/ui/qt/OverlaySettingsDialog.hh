#ifndef RA_UI_QT_OVERLAYSETTINGSDIALOG_HH
#define RA_UI_QT_OVERLAYSETTINGSDIALOG_HH
#pragma once

#include "ui/qt/IDialogPresenter.hh"
#include "ui/qt/ModalDialogBase.hh"
#include "ui/qt/bindings/CheckBoxBinding.hh"
#include "ui/qt/bindings/ComboBoxBinding.hh"
#include "ui/qt/bindings/TextBoxBinding.hh"
#include "ui/viewmodels/OverlaySettingsViewModel.hh"

class QFormLayout;

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// The Overlay Settings dialog: the Qt counterpart of ui/win32/OverlaySettingsDialog. OK only answers: the caller
/// commits (IntegrationMenuViewModel::ShowOverlaySettings).
/// </summary>
class OverlaySettingsDialog : public ModalDialogBase
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;
    };

    explicit OverlaySettingsDialog(ra::ui::viewmodels::OverlaySettingsViewModel& vmSettings);

private:
    void AddLocation(QFormLayout& oForm, const QString& sName, const QString& sLabel,
                     ra::ui::qt::bindings::ComboBoxBinding& oBinding,
                     const ra::ui::viewmodels::LookupItemViewModelCollection& vmItems, const IntModelProperty& pProperty);
    void AddScreenshot(QFormLayout& oForm, const QString& sName, const QString& sLabel,
                       ra::ui::qt::bindings::CheckBoxBinding& oBinding, const BoolModelProperty& pProperty);

    ra::ui::qt::bindings::ComboBoxBinding m_bindAchievementTriggerLocation;
    ra::ui::qt::bindings::CheckBoxBinding m_bindScreenshotAchievementTrigger;
    ra::ui::qt::bindings::ComboBoxBinding m_bindMasteryLocation;
    ra::ui::qt::bindings::CheckBoxBinding m_bindScreenshotMastery;
    ra::ui::qt::bindings::ComboBoxBinding m_bindLeaderboardStartedLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindLeaderboardCanceledLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindLeaderboardTrackerLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindLeaderboardScoreboardLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindActiveChallengeLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindProgressTrackerLocation;
    ra::ui::qt::bindings::ComboBoxBinding m_bindMessageLocation;
    ra::ui::qt::bindings::TextBoxBinding m_bindScreenshotLocation;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_OVERLAYSETTINGSDIALOG_HH
