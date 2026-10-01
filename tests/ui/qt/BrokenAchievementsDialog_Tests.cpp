#ifndef _WIN32

#include "ui/qt/BrokenAchievementsDialog.hh"

#include "ui/qt/QtDesktop.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"

#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockThreadPool.hh"
#include "tests/mocks/MockDesktop.hh"
#include "tests/mocks/MockGameContext.hh"
#include "tests/mocks/MockWindowManager.hh"
#include "tests/ui/qt/ItemViewInput.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QPushButton>
#include <QDialogButtonBox>
#include <QDir>
#include <QHeaderView>
#include <QLabel>
#include <QPixmap>
#include <QTableView>

#include <cstdlib>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

using ra::ui::viewmodels::BrokenAchievementsViewModel;
using ra::ui::viewmodels::MessageBoxViewModel;

namespace {

constexpr int TickColumn = 0;
constexpr int DescriptionColumn = 2;

// The view model with what it reads: a loaded game with four achievements, two of them unlocked.
class BrokenAchievementsViewModelHarness : public BrokenAchievementsViewModel
{
public:
    ra::data::context::mocks::MockGameContext mockGameContext;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ra::services::mocks::MockThreadPool mockThreadPool; // the asset list needs it
    ra::ui::mocks::MockDesktop mockDesktop;
    ra::ui::viewmodels::mocks::MockWindowManager mockWindowManager;

    BrokenAchievementsViewModelHarness() noexcept
    {
        GSL_SUPPRESS_F6 mockWindowManager.AssetList.InitializeNotifyTargets();
        GSL_SUPPRESS_F6 mockConfiguration.SetHostUrl("http://host");
    }

    void MockAchievements()
    {
        mockGameContext.SetGameId(1U);
        mockGameContext.SetGameTitle(L"GAME");

        const auto fAdd = [this](const wchar_t* sName, const wchar_t* sDescription, bool bUnlocked) {
            auto& pAchievement = mockGameContext.MockAchievement();
            pAchievement.SetName(sName);
            pAchievement.SetDescription(sDescription);
            pAchievement.SetState(bUnlocked ? ra::data::models::AssetState::Inactive
                                            : ra::data::models::AssetState::Active);
        };
        fAdd(L"First Steps", L"Finish the first level", true);
        fAdd(L"Barrel Roll", L"Jump over ten barrels without losing a life", false);
        fAdd(L"Hammer Time", L"Smash three barrels with one hammer", true);
        fAdd(L"Rescue", L"Reach the top of the last level", false);

        Assert::IsTrue(InitializeAchievements());
    }
};

BrokenAchievementsDialog* Open(QtTestHost& oQt, BrokenAchievementsViewModel& vmBrokenAchievements)
{
    BrokenAchievementsDialog* pDialog = nullptr;
    oQt.RunOnQt([&vmBrokenAchievements, &pDialog]() {
        pDialog = new BrokenAchievementsDialog(vmBrokenAchievements);
        pDialog->show();
    });
    return pDialog;
}

void Delete(QtTestHost& oQt, QDialog* pDialog)
{
    oQt.RunOnQt([pDialog]() { delete pDialog; });
}

QTableView& List(QDialog& oDialog)
{
    return *oDialog.findChild<QTableView*>(QStringLiteral("Achievements"));
}

QPushButton& ReportButton(QDialog& oDialog)
{
    return *oDialog.findChild<QDialogButtonBox*>()->button(QDialogButtonBox::Ok);
}

// A click on the row's description, with the Qt thread's queue run between the press and the release, and after.
void ClickRow(QtTestHost& oQt, QDialog* pDialog, int nRow)
{
    QPoint ptWhere;
    oQt.RunOnQt([pDialog, nRow, &ptWhere]() {
        ptWhere = CellCentre(List(*pDialog), nRow, DescriptionColumn);
        PressMouse(List(*pDialog), ptWhere);
    });
    oQt.RunOnQt([pDialog, ptWhere]() { ReleaseMouse(List(*pDialog), ptWhere); });
    oQt.RunOnQt([]() {});
}

} // namespace

TEST_CLASS(BrokenAchievementsDialog_Tests)
{
public:
    TEST_METHOD(TestQtDesktopHasAPresenterForIt)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        QtTestHost oQt;
        QtDesktop oDesktop;

        Assert::IsTrue(oDesktop.CanShowWindow(vmBrokenAchievements),
                       L"Report Achievement Problem would get the not-available notice");
    }

    TEST_METHOD(TestItShowsWin32sTextAndColumns)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        vmBrokenAchievements.MockAchievements();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmBrokenAchievements);

        QString sTitle, sPrompt, sButton;
        QStringList vHeaders;
        int nRows = 0, nMinimumWidth = 0, nMinimumHeight = 0, nWidth = 0, nHeight = 0, nAchievedWidth = 0;
        QStringList vTitles, vAchieved;
        size_t nSelected = 99;
        oQt.RunOnQt([pDialog, &sTitle, &sPrompt, &sButton, &vHeaders, &nRows, &nMinimumWidth, &nMinimumHeight, &nWidth,
                     &nHeight, &nAchievedWidth, &vTitles, &vAchieved, &nSelected]() {
            sTitle = pDialog->windowTitle();
            sPrompt = pDialog->findChild<QLabel*>()->text();
            sButton = ReportButton(*pDialog).text();
            const auto& oList = List(*pDialog);
            const auto* pModel = oList.model();
            for (int nColumn = 0; nColumn < pModel->columnCount(); ++nColumn)
                vHeaders.append(pModel->headerData(nColumn, Qt::Horizontal, Qt::DisplayRole).toString());
            nRows = pModel->rowCount();
            for (int nRow = 0; nRow < nRows; ++nRow)
            {
                vTitles.append(pModel->index(nRow, 1).data().toString());
                vAchieved.append(pModel->index(nRow, 3).data().toString());
            }
            nMinimumWidth = pDialog->minimumWidth();
            nMinimumHeight = pDialog->minimumHeight();
            nWidth = pDialog->width();
            nHeight = pDialog->height();
            nAchievedWidth = oList.columnWidth(3);
            nSelected = static_cast<size_t>(oList.selectionModel()->selectedRows().size());
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"Report Achievement Problem"), sTitle.toStdWString());
        Assert::AreEqual(std::wstring(L"Which achievement would you like to report?"), sPrompt.toStdWString());
        Assert::AreEqual(std::wstring(L"Report Problem"), sButton.toStdWString());
        Assert::IsTrue(vHeaders == QStringList({QString(), QStringLiteral("Title"), QStringLiteral("Description"),
                                                QStringLiteral("Achieved")}),
                       L"wrong headers");
        Assert::AreEqual(4, nRows);
        Assert::IsTrue(vTitles == QStringList({QStringLiteral("First Steps"), QStringLiteral("Barrel Roll"),
                                               QStringLiteral("Hammer Time"), QStringLiteral("Rescue")}),
                       L"wrong titles");
        Assert::IsTrue(vAchieved == QStringList({QStringLiteral("Yes"), QStringLiteral("No"), QStringLiteral("Yes"),
                                                 QStringLiteral("No")}),
                       L"wrong Achieved column");
        Assert::AreEqual(416, nMinimumWidth);
        Assert::AreEqual(322, nMinimumHeight);
        Assert::AreEqual(593, nWidth, L"not Win32's starting size");
        Assert::AreEqual(416, nHeight, L"not Win32's starting size");
        Assert::AreEqual(64, nAchievedWidth);
        Assert::AreEqual(size_t{0}, nSelected, L"a row is selected before the user chose one");
    }

    TEST_METHOD(TestClickingARowTicksItAndAPictureCanBeTaken)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        vmBrokenAchievements.MockAchievements();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmBrokenAchievements);

        ClickRow(oQt, pDialog, 1);
        int nCheck = 0;
        bool bSaved = true;
        const char* sShots = std::getenv("RA_DIALOG_SHOTS"); // a directory for the owner to look at
        oQt.RunOnQt([pDialog, sShots, &nCheck, &bSaved]() {
            nCheck = List(*pDialog).model()->index(1, TickColumn).data(Qt::CheckStateRole).toInt();
            if (sShots != nullptr && sShots[0] != '\0')
            {
                const QString sPath = QDir(QString::fromUtf8(sShots)).filePath(QStringLiteral("report-achievement-problem.png"));
                bSaved = pDialog->grab().save(sPath);
            }
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(1, vmBrokenAchievements.GetSelectedIndex(), L"clicking the row did not select its achievement");
        Assert::AreEqual(static_cast<int>(Qt::Checked), nCheck, L"the clicked row shows no tick");
        Assert::IsTrue(bSaved, L"the picture was not saved");
    }

    TEST_METHOD(TestReportProblemWithNothingTickedSaysSoAndStaysOpen)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        vmBrokenAchievements.MockAchievements();
        std::wstring sMessage;
        vmBrokenAchievements.mockDesktop.ExpectWindow<MessageBoxViewModel>([&sMessage](const MessageBoxViewModel& vmMessageBox) {
            sMessage = vmMessageBox.GetMessage(); // on the Qt thread: no asserts here
            return ra::ui::DialogResult::OK;
        });
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmBrokenAchievements);

        bool bVisible = false;
        oQt.RunOnQt([pDialog]() { ReportButton(*pDialog).click(); });
        oQt.RunOnQt([pDialog, &bVisible]() { bVisible = pDialog->isVisible(); });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"Please select an achievement."), sMessage);
        Assert::IsTrue(bVisible, L"the dialog closed with nothing to report");
        Assert::AreEqual(std::string(), vmBrokenAchievements.mockDesktop.LastOpenedUrl());
    }

    TEST_METHOD(TestReportProblemOpensTheTickedAchievementsPage)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        vmBrokenAchievements.MockAchievements();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmBrokenAchievements);

        ClickRow(oQt, pDialog, 2);
        bool bVisible = true;
        oQt.RunOnQt([pDialog]() { ReportButton(*pDialog).click(); });
        oQt.RunOnQt([pDialog, &bVisible]() { bVisible = pDialog->isVisible(); });
        const auto nResult = vmBrokenAchievements.GetDialogResult();
        Delete(oQt, pDialog);

        Assert::AreEqual(std::string("http://host/achievement/3/report-issue"),
                         vmBrokenAchievements.mockDesktop.LastOpenedUrl());
        Assert::IsFalse(bVisible, L"the dialog stayed open after reporting");
        Assert::AreEqual(static_cast<int>(ra::ui::DialogResult::OK), static_cast<int>(nResult));
        Assert::IsFalse(vmBrokenAchievements.mockDesktop.WasDialogShown(), L"an error box was shown");
    }

    TEST_METHOD(TestEnterInTheListReportsTheTickedAchievement)
    {
        BrokenAchievementsViewModelHarness vmBrokenAchievements;
        vmBrokenAchievements.MockAchievements();
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmBrokenAchievements);

        ClickRow(oQt, pDialog, 3);
        oQt.RunOnQt([pDialog]() { PressKey(List(*pDialog), Qt::Key_Return); });
        oQt.RunOnQt([]() {});
        Delete(oQt, pDialog);

        Assert::AreEqual(std::string("http://host/achievement/4/report-issue"),
                         vmBrokenAchievements.mockDesktop.LastOpenedUrl());
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
