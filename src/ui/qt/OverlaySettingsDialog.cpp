#include "ui/qt/OverlaySettingsDialog.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::viewmodels::LookupItemViewModelCollection;
using ra::ui::viewmodels::OverlaySettingsViewModel;

bool OverlaySettingsDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const OverlaySettingsViewModel*>(&vmWindow) != nullptr;
}

void OverlaySettingsDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    RA_LOG_WARN("Overlay settings \"%s\" shown without waiting for an answer - not shown; use ShowModal",
                ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
}

std::unique_ptr<QDialog> OverlaySettingsDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    return std::make_unique<OverlaySettingsDialog>(dynamic_cast<OverlaySettingsViewModel&>(vmWindow));
}

OverlaySettingsDialog::OverlaySettingsDialog(OverlaySettingsViewModel& vmSettings)
    : ModalDialogBase(vmSettings),
      m_bindAchievementTriggerLocation(vmSettings),
      m_bindScreenshotAchievementTrigger(vmSettings),
      m_bindMasteryLocation(vmSettings),
      m_bindScreenshotMastery(vmSettings),
      m_bindLeaderboardStartedLocation(vmSettings),
      m_bindLeaderboardCanceledLocation(vmSettings),
      m_bindLeaderboardTrackerLocation(vmSettings),
      m_bindLeaderboardScoreboardLocation(vmSettings),
      m_bindActiveChallengeLocation(vmSettings),
      m_bindProgressTrackerLocation(vmSettings),
      m_bindMessageLocation(vmSettings),
      m_bindScreenshotLocation(vmSettings),
      m_bindUpdateReminders(vmSettings)
{
    auto* pForm = new QFormLayout();
    // labels on the left, as in Win32's dialog, whatever the style says (KDE's puts a form's on the right)
    pForm->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // Win32's rows and labels (RA_Shared.rc, IDD_RA_OVERLAYSETTINGS), and its lists: PopupLocationsNoMiddle for the
    // scoreboard, challenge and progress indicators.
    AddLocation(*pForm, QStringLiteral("AchievementTriggerLocation"), QStringLiteral("&Achievement triggered notification"),
                m_bindAchievementTriggerLocation, vmSettings.PopupLocations(),
                OverlaySettingsViewModel::AchievementTriggerLocationProperty);
    AddCheckBox(*pForm, QStringLiteral("ScreenshotAchievementTrigger"),
                QStringLiteral("&Capture achievement triggered screenshot"), m_bindScreenshotAchievementTrigger,
                OverlaySettingsViewModel::ScreenshotAchievementTriggerProperty);
    AddLocation(*pForm, QStringLiteral("MasteryLocation"), QStringLiteral("Game &mastery notification"),
                m_bindMasteryLocation, vmSettings.PopupLocations(), OverlaySettingsViewModel::MasteryLocationProperty);
    AddCheckBox(*pForm, QStringLiteral("ScreenshotMastery"), QStringLiteral("Capture &game mastery screenshot"),
                m_bindScreenshotMastery, OverlaySettingsViewModel::ScreenshotMasteryProperty);
    AddLocation(*pForm, QStringLiteral("LeaderboardStartedLocation"), QStringLiteral("&Leaderboard started notification"),
                m_bindLeaderboardStartedLocation, vmSettings.PopupLocations(),
                OverlaySettingsViewModel::LeaderboardStartedLocationProperty);
    AddLocation(*pForm, QStringLiteral("LeaderboardCanceledLocation"),
                QStringLiteral("Leader&board canceled notification"), m_bindLeaderboardCanceledLocation,
                vmSettings.PopupLocations(), OverlaySettingsViewModel::LeaderboardCanceledLocationProperty);
    AddLocation(*pForm, QStringLiteral("LeaderboardTrackerLocation"),
                QStringLiteral("Active leaderboard &time/score values"), m_bindLeaderboardTrackerLocation,
                vmSettings.PopupLocations(), OverlaySettingsViewModel::LeaderboardTrackerLocationProperty);
    AddLocation(*pForm, QStringLiteral("LeaderboardScoreboardLocation"),
                QStringLiteral("&Scoreboard after submitting leaderboard entry"), m_bindLeaderboardScoreboardLocation,
                vmSettings.PopupLocationsNoMiddle(), OverlaySettingsViewModel::LeaderboardScoreboardLocationProperty);
    AddLocation(*pForm, QStringLiteral("ActiveChallengeLocation"), QStringLiteral("Active c&hallenge indicator"),
                m_bindActiveChallengeLocation, vmSettings.PopupLocationsNoMiddle(),
                OverlaySettingsViewModel::ActiveChallengeLocationProperty);
    AddLocation(*pForm, QStringLiteral("ProgressTrackerLocation"), QStringLiteral("&Progress indicator"),
                m_bindProgressTrackerLocation, vmSettings.PopupLocationsNoMiddle(),
                OverlaySettingsViewModel::ProgressTrackerLocationProperty);
    AddLocation(*pForm, QStringLiteral("MessageLocation"), QStringLiteral("&Informational notifications"),
                m_bindMessageLocation, vmSettings.PopupLocations(), OverlaySettingsViewModel::MessageLocationProperty);

    // Win32 puts it in the same place: after the last notification row, before the screenshot location
    AddCheckBox(*pForm, QStringLiteral("UpdateReminders"), QStringLiteral("Tell me when a new client &version is available"),
                m_bindUpdateReminders, OverlaySettingsViewModel::UpdateRemindersProperty);

    // Read-only, as on Win32 (ES_READONLY): the location changes only through Browse.
    auto* pLocation = new QLineEdit(this);
    pLocation->setObjectName(QStringLiteral("ScreenshotLocation"));
    pLocation->setReadOnly(true);
    auto* pBrowse = new QPushButton(QStringLiteral("..."), this);
    pBrowse->setObjectName(QStringLiteral("Browse"));
    connect(pBrowse, &QPushButton::clicked, this, [&vmSettings]() { vmSettings.BrowseLocation(); });
    auto* pLocationRow = new QHBoxLayout();
    pLocationRow->addWidget(pLocation);
    pLocationRow->addWidget(pBrowse);
    auto* pLocationLabel = new QLabel(QStringLiteral("Sc&reenshot Location"), this);
    pLocationLabel->setBuddy(pLocation);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addLayout(pForm);
    pLayout->addWidget(pLocationLabel);
    pLayout->addLayout(pLocationRow);
    pLayout->addWidget(CreateButtons());

    m_bindScreenshotLocation.BindText(OverlaySettingsViewModel::ScreenshotLocationProperty,
                                      ra::ui::qt::bindings::TextBoxBinding::UpdateMode::None);
    m_bindScreenshotLocation.SetControl(*pLocation);
    RegisterBinding(m_bindScreenshotLocation); // one-way: nothing to flush

    m_bindWindow.SetWidget(*this);
}

void OverlaySettingsDialog::AddLocation(QFormLayout& oForm, const QString& sName, const QString& sLabel,
                                        ra::ui::qt::bindings::ComboBoxBinding& oBinding,
                                        const LookupItemViewModelCollection& vmItems, const IntModelProperty& pProperty)
{
    auto* pComboBox = new QComboBox(this);
    pComboBox->setObjectName(sName);
    auto* pLabel = new QLabel(sLabel, this);
    pLabel->setBuddy(pComboBox);
    oForm.addRow(pLabel, pComboBox);

    oBinding.BindItems(vmItems); // const: fixed lists, not tracked, as on Win32
    oBinding.BindSelectedItem(pProperty);
    oBinding.SetControl(*pComboBox);
    RegisterBinding(oBinding);
}

void OverlaySettingsDialog::AddCheckBox(QFormLayout& oForm, const QString& sName, const QString& sLabel,
                                        ra::ui::qt::bindings::CheckBoxBinding& oBinding,
                                        const BoolModelProperty& pProperty)
{
    auto* pCheckBox = new QCheckBox(sLabel, this);
    pCheckBox->setObjectName(sName);
    oForm.addRow(pCheckBox);

    oBinding.BindCheck(pProperty);
    oBinding.SetControl(*pCheckBox);
    RegisterBinding(oBinding);
}

} // namespace qt
} // namespace ui
} // namespace ra
