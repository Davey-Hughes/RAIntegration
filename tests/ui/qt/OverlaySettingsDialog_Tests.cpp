#ifndef _WIN32

#include "ui/qt/OverlaySettingsDialog.hh"

#include "ui/viewmodels/FileDialogViewModel.hh"

#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockWindowConfiguration.hh"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

using ra::services::Feature;
using ra::ui::viewmodels::FileDialogViewModel;
using ra::ui::viewmodels::OverlaySettingsViewModel;
using ra::ui::viewmodels::Popup;
using ra::ui::viewmodels::PopupLocation;

namespace {

OverlaySettingsDialog* Open(QtTestHost& oQt, OverlaySettingsViewModel& vmSettings)
{
    OverlaySettingsDialog* pDialog = nullptr;
    oQt.RunOnQt([&vmSettings, &pDialog]() {
        pDialog = new OverlaySettingsDialog(vmSettings);
        pDialog->show();
    });
    return pDialog;
}

void Delete(QtTestHost& oQt, QDialog* pDialog)
{
    oQt.RunOnQt([pDialog]() { delete pDialog; });
}

} // namespace

TEST_CLASS(OverlaySettingsDialog_Tests)
{
public:
    TEST_METHOD(TestItShowsTheConfiguration)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockWindowConfiguration.SetPopupLocation(Popup::AchievementTriggered, PopupLocation::TopRight);
        mockConfiguration.SetFeatureEnabled(Feature::AchievementTriggeredScreenshot, true);
        mockConfiguration.SetScreenshotDirectory(L"/tmp/shots/");
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        std::wstring sTrigger, sLocation, sTitle;
        bool bScreenshot = false, bLocationReadOnly = false;
        oQt.RunOnQt([pDialog, &sTrigger, &sLocation, &sTitle, &bScreenshot, &bLocationReadOnly]() {
            sTrigger = pDialog->findChild<QComboBox*>(QStringLiteral("AchievementTriggerLocation"))->currentText().toStdWString();
            bScreenshot = pDialog->findChild<QCheckBox*>(QStringLiteral("ScreenshotAchievementTrigger"))->isChecked();
            auto* pLocation = pDialog->findChild<QLineEdit*>(QStringLiteral("ScreenshotLocation"));
            sLocation = pLocation->text().toStdWString();
            bLocationReadOnly = pLocation->isReadOnly();
            sTitle = pDialog->windowTitle().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"Top Right"), sTrigger);
        Assert::IsTrue(bScreenshot);
        Assert::AreEqual(std::wstring(L"/tmp/shots/"), sLocation);
        Assert::IsTrue(bLocationReadOnly, L"the location is typed, not browsed, as on Win32");
        Assert::AreEqual(std::wstring(L"Overlay Settings"), sTitle);
    }

    TEST_METHOD(TestTheNineLocationsUseTheRightLists)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        int nCombos = 0, nWithMiddle = 0, nWithoutMiddle = 0;
        oQt.RunOnQt([pDialog, &nCombos, &nWithMiddle, &nWithoutMiddle]() {
            for (auto* pCombo : pDialog->findChildren<QComboBox*>())
            {
                ++nCombos;
                if (pCombo->count() == 7)
                    ++nWithMiddle;
                else if (pCombo->count() == 5)
                    ++nWithoutMiddle;
            }
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(9, nCombos);
        Assert::AreEqual(6, nWithMiddle);
        Assert::AreEqual(3, nWithoutMiddle);
    }

    TEST_METHOD(TestChangingControlsWritesTheViewModel)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        oQt.RunOnQt([pDialog]() {
            auto* pCombo = pDialog->findChild<QComboBox*>(QStringLiteral("MasteryLocation"));
            const int nIndex = pCombo->findText(QStringLiteral("Bottom Right"));
            pCombo->setCurrentIndex(nIndex);
            Q_EMIT pCombo->activated(nIndex);
            pDialog->findChild<QCheckBox*>(QStringLiteral("ScreenshotMastery"))->click();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(ra::etoi(PopupLocation::BottomRight), ra::etoi(vmSettings.GetMasteryLocation()));
        Assert::IsTrue(vmSettings.ScreenshotMastery());
    }

    TEST_METHOD(TestTheUpdateRemindersCheckBoxIsBound)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockConfiguration.SetFeatureEnabled(Feature::UpdateReminders, false);
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        std::wstring sLabel;
        bool bFound = false;
        bool bShownChecked = true;
        bool bCheckedAfterClick = false;
        oQt.RunOnQt([pDialog, &sLabel, &bFound, &bShownChecked, &bCheckedAfterClick]() {
            auto* pCheckBox = pDialog->findChild<QCheckBox*>(QStringLiteral("UpdateReminders"));
            bFound = (pCheckBox != nullptr);
            if (bFound)
            {
                sLabel = pCheckBox->text().toStdWString();
                bShownChecked = pCheckBox->isChecked();
                pCheckBox->click();
                bCheckedAfterClick = pCheckBox->isChecked();
            }
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bFound, L"no UpdateReminders checkbox");
        Assert::AreEqual(std::wstring(L"Tell me when a new client &version is available"), sLabel);
        Assert::IsFalse(bShownChecked, L"the checkbox shows the configuration");
        Assert::IsTrue(bCheckedAfterClick);
        Assert::IsTrue(vmSettings.UpdateReminders(), L"a click writes the view model");
    }

    TEST_METHOD(TestBrowseShowsTheChosenFolder)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        ra::ui::mocks::MockDesktop mockDesktop;
        mockDesktop.ExpectWindow<FileDialogViewModel>([](FileDialogViewModel& vmFolder) {
            vmFolder.SetFileName(L"/tmp/chosen");
            return DialogResult::OK;
        });
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        std::wstring sShown;
        oQt.RunOnQt([pDialog, &sShown]() {
            pDialog->findChild<QPushButton*>(QStringLiteral("Browse"))->click();
            sShown = pDialog->findChild<QLineEdit*>(QStringLiteral("ScreenshotLocation"))->text().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"/tmp/chosen/"), sShown); // BrowseLocation adds the separator
    }

    TEST_METHOD(TestOkLeavesTheCommitToTheCaller)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockWindowConfiguration.SetPopupLocation(Popup::Mastery, PopupLocation::TopMiddle);
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        oQt.RunOnQt([pDialog]() {
            auto* pCombo = pDialog->findChild<QComboBox*>(QStringLiteral("MasteryLocation"));
            const int nIndex = pCombo->findText(QStringLiteral("Bottom Left"));
            pCombo->setCurrentIndex(nIndex);
            Q_EMIT pCombo->activated(nIndex);
            pDialog->accept();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(DialogResult::OK, vmSettings.GetDialogResult());
        Assert::AreEqual(ra::etoi(PopupLocation::TopMiddle), ra::etoi(mockWindowConfiguration.GetPopupLocation(Popup::Mastery)),
                         L"the dialog committed; ShowOverlaySettings does that");
    }

    TEST_METHOD(TestCancelAnswersCancel)
    {
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        OverlaySettingsViewModel vmSettings;
        vmSettings.Initialize();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmSettings);

        oQt.RunOnQt([pDialog]() { pDialog->reject(); });
        Delete(oQt, pDialog);

        Assert::AreEqual(DialogResult::Cancel, vmSettings.GetDialogResult());
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
