#ifndef _WIN32

#include "ui/qt/RichPresenceDialog.hh"

#include "ui/qt/QtDesktop.hh"
#include "ui/qt/bindings/WindowBinding.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockLocalStorage.hh"
#include "tests/mocks/MockAchievementRuntime.hh"
#include "tests/mocks/MockGameContext.hh"
#include "tests/mocks/MockWindowConfiguration.hh"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QApplication>
#include <QLabel>
#include <QString>

#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

namespace {

using ra::ui::viewmodels::RichPresenceMonitorViewModel;

// The monitor's view model with the services it uses when shown - set up as
// RichPresenceMonitorViewModel_Tests does - and SetDisplayString opened up.
class RichPresenceMonitorViewModelHarness : public RichPresenceMonitorViewModel
{
public:
    RichPresenceMonitorViewModelHarness()
    {
        mockRuntime.MockGame();
        InitializeNotifyTargets();
    }

    using RichPresenceMonitorViewModel::SetDisplayString;

    ra::context::mocks::MockRcClient mockRcClient;
    ra::data::context::mocks::MockGameContext mockGameContext;
    ra::services::mocks::MockAchievementRuntime mockRuntime;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ra::services::mocks::MockLocalStorage mockLocalStorage;
};

// The monitor's text label, or nullptr. Qt thread.
QLabel* FindMonitorText()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pDialog = dynamic_cast<RichPresenceDialog*>(pWidget);
        if (pDialog != nullptr && pDialog->isVisible())
            return pDialog->findChild<QLabel*>(QStringLiteral("RichPresenceText"));
    }

    return nullptr;
}

// Monitors that exist, shown or not. Qt thread.
int CountMonitors()
{
    int nCount = 0;
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        if (dynamic_cast<RichPresenceDialog*>(pWidget) != nullptr)
            ++nCount;
    }

    return nCount;
}

} // namespace

TEST_CLASS(RichPresenceDialog_Tests)
{
public:
    TEST_METHOD(TestShowWindowOpensTheMonitor)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;

        oDesktop.ShowWindow(vmRichPresence);

        QString sTitle, sText;
        bool bVisible = false;
        const bool bShown = oQt.WaitOnQt([&]() {
            auto* pText = FindMonitorText();
            if (pText == nullptr)
                return false;

            sTitle = pText->window()->windowTitle();
            sText = pText->text();
            bVisible = vmRichPresence.IsVisible();
            return true;
        });
        oDesktop.Shutdown();

        Assert::IsTrue(bShown, L"the monitor never appeared");
        Assert::AreEqual(std::wstring(L"Rich Presence Monitor"), sTitle.toStdWString());
        Assert::AreEqual(std::wstring(L"No game loaded."), sText.toStdWString());
        Assert::IsTrue(bVisible);
        Assert::IsFalse(vmRichPresence.IsVisible(), L"Shutdown left IsVisible set");
    }

    TEST_METHOD(TestTheTextFollowsAStringSetOnAWorker)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmRichPresence);
        const bool bShown = oQt.WaitOnQt([]() { return FindMonitorText() != nullptr; });

        std::thread([&vmRichPresence]() { vmRichPresence.SetDisplayString(L"World 1-1 <3 lives>"); }).join();

        bool bPlain = false;
        const bool bUpdated = oQt.WaitOnQt([&bPlain]() {
            auto* pText = FindMonitorText();
            if (pText == nullptr || pText->text() != QStringLiteral("World 1-1 <3 lives>"))
                return false;

            bPlain = (pText->textFormat() == Qt::PlainText);
            return true;
        });
        oDesktop.Shutdown();

        Assert::IsTrue(bShown, L"the monitor never appeared");
        Assert::IsTrue(bUpdated, L"the label never showed the worker's string");
        Assert::IsTrue(bPlain, L"the label is not plain text");
    }

    TEST_METHOD(TestShowingItAgainReusesTheWindow)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;

        oDesktop.ShowWindow(vmRichPresence);
        const bool bShown = oQt.WaitOnQt([]() { return FindMonitorText() != nullptr; });
        oDesktop.ShowWindow(vmRichPresence);
        oQt.RunOnQt([]() {}); // the second ShowWindow has run
        int nMonitors = 0;
        oQt.RunOnQt([&nMonitors]() { nMonitors = CountMonitors(); });
        oDesktop.Shutdown();

        Assert::IsTrue(bShown);
        Assert::AreEqual(1, nMonitors);
    }

    TEST_METHOD(TestClosingItClearsIsVisibleAndDeletesIt)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmRichPresence);
        const bool bShown = oQt.WaitOnQt([]() { return FindMonitorText() != nullptr; });
        const bool bVisibleWhileOpen = vmRichPresence.IsVisible();

        oDesktop.CloseWindow(vmRichPresence);

        const bool bGone = oQt.WaitOnQt(
            [&vmRichPresence]() { return bindings::WindowBinding::GetBindingFor(vmRichPresence) == nullptr; });
        oDesktop.Shutdown();

        Assert::IsTrue(bShown);
        Assert::IsTrue(bVisibleWhileOpen);
        Assert::IsTrue(bGone, L"the closed monitor was not deleted");
        Assert::IsFalse(vmRichPresence.IsVisible());
    }

    TEST_METHOD(TestShowingItWhileItClosesOpensANewWindow)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;

        oQt.RunOnQt([&vmRichPresence]() {
            RichPresenceDialog::Presenter oPresenter;
            oPresenter.ShowWindow(vmRichPresence);
            auto* pText = FindMonitorText();
            if (pText != nullptr)
                pText->window()->close();         // schedules the delete
            oPresenter.ShowWindow(vmRichPresence); // before that delete has run
        });

        int nMonitors = 0;
        const bool bReopened = oQt.WaitOnQt([&nMonitors]() {
            nMonitors = CountMonitors();
            return FindMonitorText() != nullptr && nMonitors == 1;
        });
        const bool bVisible = vmRichPresence.IsVisible();

        Assert::IsTrue(bReopened, L"the monitor was lost to the pending delete");
        Assert::IsTrue(bVisible);
    }

    TEST_METHOD(TestItsSizeIsSavedAndRestored)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockWindowConfiguration.SetWindowSize("Rich Presence Monitor", ra::ui::Size{420, 160});
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmRichPresence);

        int nWidth = 0, nHeight = 0;
        const bool bShown = oQt.WaitOnQt([&nWidth, &nHeight]() {
            auto* pText = FindMonitorText();
            if (pText == nullptr)
                return false;

            nWidth = pText->window()->width();
            nHeight = pText->window()->height();
            return true;
        });

        oQt.RunOnQt([]() {
            auto* pText = FindMonitorText();
            if (pText != nullptr)
                pText->window()->resize(500, 200);
        });
        const bool bSaved = oQt.WaitOnQt([&mockWindowConfiguration]() {
            const auto oSize = mockWindowConfiguration.GetWindowSize("Rich Presence Monitor");
            return oSize.Width == 500 && oSize.Height == 200;
        });
        oDesktop.Shutdown();

        Assert::IsTrue(bShown);
        Assert::AreEqual(420, nWidth);
        Assert::AreEqual(160, nHeight);
        Assert::IsTrue(bSaved, L"the new size was not saved under the Win32 key");
    }

    TEST_METHOD(TestShowModalShowsItAndReturnsAtOnce)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        RichPresenceMonitorViewModelHarness vmRichPresence;
        QtTestHost oQt;
        QtDesktop oDesktop;

        const auto nResult = oDesktop.ShowModal(vmRichPresence);
        const bool bShown = oQt.WaitOnQt([]() { return FindMonitorText() != nullptr; });
        oDesktop.Shutdown();

        Assert::AreEqual(DialogResult::None, nResult);
        Assert::IsTrue(bShown, L"ShowModal did not show the monitor");
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
