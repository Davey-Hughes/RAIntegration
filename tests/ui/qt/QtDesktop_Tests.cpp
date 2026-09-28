#ifndef _WIN32

#include "ui/qt/QtDesktop.hh"

#include "services/impl/HostThreadDispatcher.hh"

#include "ui/qt/DialogBase.hh"
#include "ui/qt/bindings/WindowBinding.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"

#include "tests/RA_UnitTestHelpers.h"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/ModalCaller.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QMessageBox>
#include <QString>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

namespace {

class TestViewModel : public WindowViewModelBase
{
public:
    explicit TestViewModel(const std::wstring& sTitle) { SetWindowTitle(sTitle); }
};

class OtherViewModel : public WindowViewModelBase
{
public:
    OtherViewModel() = default;
    explicit OtherViewModel(const std::wstring& sTitle) { SetWindowTitle(sTitle); }
};

// Answers as every modal must: DialogResult first, then finished(), and never
// the view model again.
class TestDialog : public QDialog
{
public:
    explicit TestDialog(WindowViewModelBase& vmWindow) : m_pViewModel(&vmWindow)
    {
        setWindowTitle(QString::fromStdWString(vmWindow.GetWindowTitle()));
    }

    void done(int nResult) override
    {
        if (m_pViewModel != nullptr)
        {
            m_pViewModel->SetDialogResult(nResult == QDialog::Accepted ? DialogResult::OK : DialogResult::Cancel);
            m_pViewModel = nullptr;
        }

        QDialog::done(nResult);
    }

private:
    WindowViewModelBase* m_pViewModel;
};

class TestPresenter : public IDialogPresenter
{
public:
    bool IsSupported(const WindowViewModelBase& vmWindow) override
    {
        return dynamic_cast<const TestViewModel*>(&vmWindow) != nullptr;
    }

    void ShowWindow(WindowViewModelBase&) override
    {
        nShowWindowThread = std::this_thread::get_id();
        ++nShowWindow;
    }

    std::unique_ptr<QDialog> CreateModal(WindowViewModelBase& vmWindow) override
    {
        ++nCreateModal;
        if (bNoModalForm)
            return nullptr;

        return std::make_unique<TestDialog>(vmWindow);
    }

    std::atomic<int> nShowWindow{0};
    std::atomic<int> nCreateModal{0};
    std::thread::id nShowWindowThread;
    bool bNoModalForm = false;
};

class TestWindow : public DialogBase
{
public:
    explicit TestWindow(WindowViewModelBase& vmWindow) : DialogBase(vmWindow) { m_bindWindow.SetWidget(*this); }
};

// The visible TestDialog titled sTitle, or nullptr. Qt thread.
TestDialog* FindDialog(const QString& sTitle)
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pDialog = dynamic_cast<TestDialog*>(pWidget);
        if (pDialog != nullptr && pDialog->isVisible() && pDialog->windowTitle() == sTitle)
            return pDialog;
    }

    return nullptr;
}

// Rejects every visible TestDialog. Qt thread. Tests call it before asserting,
// so a failure fails instead of leaving a worker waiting on a dialog.
void RejectAll()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pDialog = dynamic_cast<TestDialog*>(pWidget);
        if (pDialog != nullptr && pDialog->isVisible())
            pDialog->reject();
    }
}

// The "not available" notices open now, and the text of the last one found. Qt thread.
struct NoticeState
{
    int nOpen = 0;
    std::wstring sText;
};

NoticeState ReadNotices()
{
    NoticeState oState;
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pNotice = qobject_cast<QMessageBox*>(pWidget);
        if (pNotice != nullptr && pNotice->isVisible() &&
            pNotice->objectName() == QLatin1String(QtDesktop::NotAvailableNoticeName))
        {
            ++oState.nOpen;
            oState.sText = pNotice->text().toStdWString();
        }
    }

    return oState;
}

// Every "not available" notice, visible or not: one the user dismissed is
// hidden at once but deleted only later. Qt thread.
int CountNotices()
{
    int nNotices = 0;
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        if (qobject_cast<QMessageBox*>(pWidget) != nullptr &&
            pWidget->objectName() == QLatin1String(QtDesktop::NotAvailableNoticeName))
        {
            ++nNotices;
        }
    }

    return nNotices;
}

// Clicks OK on the visible notice, as the user dismisses it. Returns whether
// there was one to click. Qt thread.
bool ClickNoticeOk()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pNotice = qobject_cast<QMessageBox*>(pWidget);
        if (pNotice != nullptr && pNotice->isVisible() &&
            pNotice->objectName() == QLatin1String(QtDesktop::NotAvailableNoticeName))
        {
            auto* pOk = pNotice->button(QMessageBox::Ok);
            if (pOk == nullptr)
                return false;

            pOk->click();
            return true;
        }
    }

    return false;
}

// Deletes every notice, so none outlives the test's Qt application - even
// when the code under test failed to. Qt thread.
void DeleteNotices()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pNotice = qobject_cast<QMessageBox*>(pWidget);
        if (pNotice != nullptr && pNotice->objectName() == QLatin1String(QtDesktop::NotAvailableNoticeName))
            delete pNotice;
    }
}

class FakeQtApplicationHost : public ra::services::IQtApplicationHost
{
public:
    FakeQtApplicationHost() noexcept : m_Override(this) {}

    bool IsAvailable() const override { return m_bAvailable; }
    bool HasWidgets() const override { return m_bHasWidgets; }
    bool IsOnQtThread() const override { return m_bOnQtThread; }
    void Invoke(std::function<void()> fAction) const override { m_vInvoked.push_back(std::move(fAction)); }
    bool InvokeAndWait(std::function<void()> fAction, std::chrono::milliseconds) const override
    {
        ++m_nInvokeAndWait;
        fAction();
        return true;
    }
    void AddStopHook(std::function<void()>) override {}
    void Stop() override {}

    bool m_bAvailable = true;
    bool m_bHasWidgets = false; // a borrowed QGuiApplication
    bool m_bOnQtThread = false;
    mutable std::vector<std::function<void()>> m_vInvoked;
    mutable int m_nInvokeAndWait = 0;

private:
    ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> m_Override;
};

} // namespace

TEST_CLASS(QtDesktop_Tests)
{
public:
    // --- routing, carried over from LinuxDesktop ---

    TEST_METHOD(TestUIWorkGoesToTheQtThread)
    {
        FakeQtApplicationHost oHost;
        QtDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnUIThread([&bRan]() { bRan = true; });
        Assert::IsFalse(bRan, L"ran inline instead of on the Qt thread");
        Assert::AreEqual(size_t(1), oHost.m_vInvoked.size());
    }

    TEST_METHOD(TestUIWorkRunsInlineWithoutQt)
    {
        FakeQtApplicationHost oHost;
        oHost.m_bAvailable = false;
        QtDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnUIThread([&bRan]() { bRan = true; });
        Assert::IsTrue(bRan);
        Assert::IsTrue(oDesktop.IsOnUIThread());
    }

    TEST_METHOD(TestIsOnUIThreadAsksTheHost)
    {
        FakeQtApplicationHost oHost;
        QtDesktop oDesktop;
        Assert::IsFalse(oDesktop.IsOnUIThread());
        oHost.m_bOnQtThread = true;
        Assert::IsTrue(oDesktop.IsOnUIThread());
    }

    TEST_METHOD(TestHostWorkGoesThroughTheDispatcher)
    {
        ra::services::impl::HostThreadDispatcher oDispatcher; // this thread is the host's
        ra::services::ServiceLocator::ServiceOverride<ra::services::impl::HostThreadDispatcher> oOverride(&oDispatcher);
        QtDesktop oDesktop;

        std::atomic<bool> bRan{false};
        std::thread([&]() { oDesktop.InvokeOnHostThread([&bRan]() { bRan = true; }); }).join();
        Assert::IsFalse(bRan.load(), L"host work ran on the worker");
        Assert::AreEqual(size_t(1), oDispatcher.PendingCount());

        bool bInline = false;
        oDesktop.InvokeOnHostThread([&bInline]() { bInline = true; });
        Assert::IsTrue(bInline, L"host work from the host thread did not run inline");
    }

    TEST_METHOD(TestHostWorkRunsInlineWithoutADispatcher)
    {
        QtDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnHostThread([&bRan]() { bRan = true; });
        Assert::IsTrue(bRan);
    }

    // --- no view layer: NullDesktop's answer ---

    TEST_METHOD(TestShowModalWithoutWidgetsAnswersNo)
    {
        TestViewModel vmWindow(L"A");
        FakeQtApplicationHost oHost;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));

        Assert::AreEqual(DialogResult::No, oDesktop.ShowModal(vmWindow));
        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::AreEqual(0, oPresenter.nCreateModal.load());
    }

    TEST_METHOD(TestShowModalWithoutAPresenterAnswersNoAndPostsANotice)
    {
        OtherViewModel vmWindow(L"Game Hash");
        QtTestHost oQt;
        QtDesktop oDesktop;

        const auto nAnswer = oDesktop.ShowModal(vmWindow);
        NoticeState oNotice;
        const bool bShown = oQt.WaitOnQt([&oNotice]() {
            oNotice = ReadNotices();
            return oNotice.nOpen == 1;
        });
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::AreEqual(DialogResult::No, nAnswer);
        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::IsTrue(bShown, L"no notice opened");
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nGame Hash"), oNotice.sText);
        Assert::AreEqual(size_t(1), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestShowModalWithoutWidgetsPostsNoNotice)
    {
        OtherViewModel vmWindow(L"Game Hash");
        FakeQtApplicationHost oHost; // a borrowed QGuiApplication: no widgets
        QtDesktop oDesktop;

        Assert::AreEqual(DialogResult::No, oDesktop.ShowModal(vmWindow));
        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::AreEqual(size_t(0), oHost.m_vInvoked.size(), L"something was queued for the Qt thread");
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestNoViewAndNoHostIsHarmless)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtDesktop oDesktop; // no IQtApplicationHost registered at all

        oDesktop.ShowWindow(vmWindow);
        const auto nAnswer = oDesktop.ShowModal(vmWindow);

        Assert::AreEqual(DialogResult::No, nAnswer);
        Assert::AreEqual(size_t(2), oDesktop.NoViewLayerCount());
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestAMessageBoxThatCannotBeShownGetsItsEscapeAnswer)
    {
        using ra::ui::viewmodels::MessageBoxViewModel;
        MessageBoxViewModel vmOkCancel(L"message");
        vmOkCancel.SetButtons(MessageBoxViewModel::Buttons::OKCancel);
        MessageBoxViewModel vmYesNo(L"message");
        vmYesNo.SetButtons(MessageBoxViewModel::Buttons::YesNo);
        MessageBoxViewModel vmOk(L"message");
        FakeQtApplicationHost oHost; // a borrowed QGuiApplication: no widgets
        QtDesktop oDesktop;

        Assert::AreEqual(DialogResult::Cancel, oDesktop.ShowModal(vmOkCancel));
        Assert::AreEqual(DialogResult::No, oDesktop.ShowModal(vmYesNo));
        Assert::AreEqual(DialogResult::OK, oDesktop.ShowModal(vmOk));
        Assert::AreEqual(size_t(3), oDesktop.NoViewLayerCount());
    }

    TEST_METHOD(TestShowWindowWithoutWidgetsIsDropped)
    {
        TestViewModel vmWindow(L"A");
        FakeQtApplicationHost oHost;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));

        oDesktop.ShowWindow(vmWindow);

        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::AreEqual(size_t(0), oHost.m_vInvoked.size());
        Assert::AreEqual(0, oPresenter.nShowWindow.load());
    }

    TEST_METHOD(TestShutdownWithoutWidgetsDoesNotWaitOnTheQtThread)
    {
        FakeQtApplicationHost oHost; // a borrowed QGuiApplication: available, no widgets
        QtDesktop oDesktop;

        oDesktop.Shutdown();

        Assert::AreEqual(0, oHost.m_nInvokeAndWait);
    }

    // --- windows ---

    TEST_METHOD(TestShowWindowGoesToThePresenterOnTheQtThread)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));
        std::thread::id nQtThread;
        oQt.RunOnQt([&nQtThread]() { nQtThread = std::this_thread::get_id(); });

        oDesktop.ShowWindow(vmWindow);

        const bool bShown = oQt.WaitOnQt([&oPresenter]() { return oPresenter.nShowWindow.load() == 1; });
        Assert::IsTrue(bShown, L"the presenter was never asked");
        Assert::IsTrue(oPresenter.nShowWindowThread == nQtThread, L"the presenter ran off the Qt thread");
    }

    TEST_METHOD(TestCloseWindowClosesTheBoundWindow)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oQt.RunOnQt([&vmWindow]() { (new TestWindow(vmWindow))->show(); });
        const bool bVisibleBefore = vmWindow.IsVisible();

        oDesktop.CloseWindow(vmWindow);

        const bool bGone =
            oQt.WaitOnQt([&vmWindow]() { return bindings::WindowBinding::GetBindingFor(vmWindow) == nullptr; });
        Assert::IsTrue(bVisibleBefore);
        Assert::IsTrue(bGone, L"the window was not closed and deleted");
        Assert::IsFalse(vmWindow.IsVisible());
    }

    // --- modals ---

    TEST_METHOD(TestShowModalFromAWorkerReturnsTheAnswer)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        if (bShown)
            oQt.RunOnQt([]() { FindDialog(QStringLiteral("A"))->accept(); });
        const bool bReturned = oCaller.Returned(2s);
        if (!bReturned)
            oQt.RunOnQt(RejectAll);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReturned, L"answering the dialog did not release the caller");
        Assert::AreEqual(DialogResult::OK, oCaller.Result());
    }

    TEST_METHOD(TestAModalFromAWorkerIsApplicationModal)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        bool bApplicationModal = false;
        oQt.RunOnQt([&bApplicationModal]() {
            auto* pDialog = FindDialog(QStringLiteral("A"));
            bApplicationModal = (pDialog != nullptr && pDialog->windowModality() == Qt::ApplicationModal);
        });
        oQt.RunOnQt(RejectAll);
        const bool bReturned = oCaller.Returned(2s);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bApplicationModal, L"the dialog does not block the other windows");
        Assert::IsTrue(bReturned);
    }

    TEST_METHOD(TestModalsFromTwoWorkersAreAnsweredIndependently)
    {
        TestViewModel vmFirst(L"First");
        TestViewModel vmSecond(L"Second");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oFirst(oDesktop, vmFirst);
        const bool bFirstShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("First")) != nullptr; });
        ModalCaller oSecond(oDesktop, vmSecond);
        const bool bSecondShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("Second")) != nullptr; });

        // answer the first while the second is still open
        if (bFirstShown)
            oQt.RunOnQt([]() { FindDialog(QStringLiteral("First"))->accept(); });
        const bool bFirstReturned = oFirst.Returned(2s);
        const bool bSecondWaiting = !oSecond.Returned(200ms);

        oQt.RunOnQt(RejectAll);
        const bool bBothReturned = oFirst.Returned(2s) && oSecond.Returned(2s);

        Assert::IsTrue(bFirstShown && bSecondShown, L"a dialog never appeared");
        Assert::IsTrue(bFirstReturned, L"the first caller waited for the second dialog");
        Assert::IsTrue(bSecondWaiting, L"the second caller returned before its dialog was answered");
        Assert::IsTrue(bBothReturned);
        Assert::AreEqual(DialogResult::OK, oFirst.Result());
        Assert::AreEqual(DialogResult::Cancel, oSecond.Result());
    }

    TEST_METHOD(TestShowModalOnTheQtThreadNests)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        std::atomic<int> nResult{-1};
        oQt.Host().Invoke([&oDesktop, &vmWindow, &nResult]() { nResult = ra::etoi(oDesktop.ShowModal(vmWindow)); });
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        bool bApplicationModal = false;
        oQt.RunOnQt([bShown, &bApplicationModal]() {
            if (bShown)
            {
                auto* pDialog = FindDialog(QStringLiteral("A"));
                bApplicationModal = (pDialog->windowModality() == Qt::ApplicationModal);
                pDialog->accept();
            }
            else
            {
                RejectAll();
            }
        });
        const bool bReturned = oQt.WaitOnQt([&nResult]() { return nResult.load() != -1; });

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReturned, L"the nested ShowModal never returned");
        Assert::AreEqual(DialogResult::OK, ra::itoe<DialogResult>(nResult.load()));
        Assert::IsTrue(bApplicationModal, L"the nested dialog does not block the other windows");
    }

    TEST_METHOD(TestAViewModelWithNoModalFormReturnsAtOnce)
    {
        TestViewModel vmWindow(L"A");
        vmWindow.SetDialogResult(DialogResult::Yes);
        QtTestHost oQt;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        pPresenter->bNoModalForm = true;
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));

        Assert::AreEqual(DialogResult::Yes, oDesktop.ShowModal(vmWindow));
        Assert::AreEqual(1, oPresenter.nCreateModal.load());
    }

    TEST_METHOD(TestAModalThatCannotOpenInTimeAnswersNo)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());
        oDesktop.SetModalStartTimeout(200ms);

        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(5s);
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        const auto nResult = oDesktop.ShowModal(vmWindow);
        oRelease.set_value();
        bool bOpenedLate = true;
        oQt.RunOnQt([&bOpenedLate]() { bOpenedLate = (FindDialog(QStringLiteral("A")) != nullptr); });

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::AreEqual(DialogResult::No, nResult);
        Assert::AreEqual(size_t(1), oDesktop.ModalNotStartedCount());
        Assert::IsFalse(bOpenedLate, L"the dialog opened after its caller had been answered");
    }

    // --- shutdown ---

    TEST_METHOD(TestShutdownAnswersAWaitingWorker)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        oDesktop.Shutdown();
        const bool bReleased = oCaller.Returned(2s);
        if (!bReleased)
            oQt.RunOnQt(RejectAll);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReleased, L"Shutdown left the worker waiting on its dialog");
        Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
        Assert::AreEqual(size_t(1), oDesktop.ClosedForShutdownCount());
    }

    TEST_METHOD(TestShutdownClosesAModalOnceABusyQtThreadIsFree)
    {
        // The Qt thread is busy when Shutdown asks it to close the windows - in a modal's CanAccept, say Login()
        // waiting on the server - so that call times out. The dialog must still close once the thread is free: the
        // worker waiting on it would otherwise hold up the thread pool's drain for good.
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        oDesktop.SetShutdownCloseTimeout(200ms);

        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(5s);
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        oDesktop.Shutdown(); // times out: the Qt thread is held
        const bool bReleasedWhileHeld = oCaller.Returned(0ms);
        oRelease.set_value();
        const bool bReleased = oCaller.Returned(2s);
        if (!bReleased)
            oQt.RunOnQt(RejectAll);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::IsFalse(bReleasedWhileHeld, L"the caller was released while the Qt thread was held");
        Assert::IsTrue(bReleased, L"the dialog was not closed once the Qt thread was free");
        Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
        Assert::AreEqual(size_t(1), oDesktop.ClosedForShutdownCount());
    }

    TEST_METHOD(TestADialogDestroyedUnansweredReleasesItsCaller)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        oQt.RunOnQt([]() { delete FindDialog(QStringLiteral("A")); }); // no done(), no finished()
        const bool bReleased = oCaller.Returned(2s);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReleased, L"the caller still waits on a dialog that no longer exists");
        Assert::AreEqual(DialogResult::No, oCaller.Result());
    }

    TEST_METHOD(TestAModalAfterShutdownAnswersNo)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));

        oDesktop.Shutdown();

        // from a worker, as a pool task still running during the drain calls it
        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bReturned = oCaller.Returned(2s);
        if (!bReturned)
            oQt.RunOnQt(RejectAll);

        Assert::IsTrue(bReturned, L"a modal opened after Shutdown and waited for an answer");
        Assert::AreEqual(DialogResult::No, oCaller.Result());
        Assert::AreEqual(size_t(1), oDesktop.RefusedAfterShutdownCount());
        Assert::AreEqual(0, oPresenter.nCreateModal.load());
    }

    TEST_METHOD(TestAnOkCancelBoxAfterShutdownAnswersCancel)
    {
        using ra::ui::viewmodels::MessageBoxViewModel;
        MessageBoxViewModel vmMessageBox(L"A newer client is required for hardcore mode.");
        vmMessageBox.SetButtons(MessageBoxViewModel::Buttons::OKCancel);
        QtTestHost oQt;
        QtDesktop oDesktop; // the built-in MessageBoxDialog presenter shows it

        oDesktop.Shutdown();

        ModalCaller oCaller(oDesktop, vmMessageBox);
        const bool bReturned = oCaller.Returned(2s);
        if (!bReturned)
            oQt.RunOnQt([]() {
                auto* pModal = QApplication::activeModalWidget();
                if (pModal != nullptr)
                    pModal->close();
            });

        Assert::IsTrue(bReturned, L"a message box opened after Shutdown and waited for an answer");
        Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
        Assert::AreEqual(size_t(1), oDesktop.RefusedAfterShutdownCount());
    }

    TEST_METHOD(TestShutdownClosesAndDeletesBoundWindows)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oQt.RunOnQt([&vmWindow]() { (new TestWindow(vmWindow))->show(); });
        const bool bVisibleBefore = vmWindow.IsVisible();

        oDesktop.Shutdown();

        bool bStillBound = true;
        oQt.RunOnQt([&vmWindow, &bStillBound]() {
            bStillBound = (bindings::WindowBinding::GetBindingFor(vmWindow) != nullptr);
        });
        Assert::IsTrue(bVisibleBefore);
        Assert::IsFalse(bStillBound, L"the window outlived Shutdown");
        Assert::IsFalse(vmWindow.IsVisible());
        Assert::AreEqual(size_t(1), oDesktop.ClosedForShutdownCount());
    }

    TEST_METHOD(TestStoppingTheQtHostAnswersAWaitingWorker)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop; // registers its stop hook with oQt's host
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        ModalCaller oCaller(oDesktop, vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        oQt.Host().Stop(); // what a second _RA_Init does, before it replaces this desktop
        const bool bReleased = oCaller.Returned(2s);

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReleased, L"stopping the Qt host left the worker waiting on its dialog");
        Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
        Assert::AreEqual(size_t(1), oDesktop.ClosedForShutdownCount());
    }

    // --- the "not available" notice ---

    TEST_METHOD(TestAWindowWithNoViewPostsANotice)
    {
        OtherViewModel vmWindow(L"Memory Inspector");
        QtTestHost oQt;
        QtDesktop oDesktop;

        oDesktop.ShowWindow(vmWindow);
        NoticeState oNotice;
        const bool bShown = oQt.WaitOnQt([&oNotice]() {
            oNotice = ReadNotices();
            return oNotice.nOpen == 1;
        });
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bShown, L"no notice opened");
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nMemory Inspector"), oNotice.sText);
        Assert::AreEqual(size_t(1), oDesktop.NotAvailableNoticeCount());
        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
    }

    TEST_METHOD(TestTitlesQueuedTogetherShareOneNotice)
    {
        OtherViewModel vmFirst(L"Assets List");
        OtherViewModel vmSecond(L"Memory Notes");
        QtTestHost oQt;
        QtDesktop oDesktop;

        // both titles are queued before the Qt thread shows anything
        const bool bHeld = oQt.HoldQtWhile([]() {},
                                           [&oDesktop, &vmFirst, &vmSecond]() {
                                               oDesktop.ShowWindow(vmFirst);
                                               oDesktop.ShowWindow(vmSecond);
                                           },
                                           []() {});
        NoticeState oNotice;
        oQt.RunOnQt([&oNotice]() { oNotice = ReadNotices(); });
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bHeld, L"the Qt thread was not held");
        Assert::AreEqual(1, oNotice.nOpen);
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nAssets List\nMemory Notes"), oNotice.sText);
        Assert::AreEqual(size_t(1), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestATitleArrivingWhileTheNoticeIsOpenIsAppended)
    {
        OtherViewModel vmFirst(L"Assets List");
        OtherViewModel vmSecond(L"Memory Notes");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmFirst);
        const bool bFirst = oQt.WaitOnQt([]() { return ReadNotices().nOpen == 1; });

        oDesktop.ShowWindow(vmSecond);
        NoticeState oNotice;
        const bool bAppended = oQt.WaitOnQt([&oNotice]() {
            oNotice = ReadNotices();
            return oNotice.sText.find(L"Memory Notes") != std::wstring::npos;
        });
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bFirst, L"no first notice");
        Assert::IsTrue(bAppended, L"the second title never appeared");
        Assert::AreEqual(1, oNotice.nOpen);
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nAssets List\nMemory Notes"), oNotice.sText);
        Assert::AreEqual(size_t(1), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestATitleAlreadyListedIsNotRepeated)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return ReadNotices().nOpen == 1; });

        oDesktop.ShowWindow(vmWindow);
        NoticeState oNotice;
        oQt.RunOnQt([&oNotice]() { oNotice = ReadNotices(); }); // queued behind the second title's call
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bShown, L"no notice opened");
        Assert::AreEqual(1, oNotice.nOpen);
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nAssets List"), oNotice.sText);
        Assert::AreEqual(size_t(1), oDesktop.NotAvailableNoticeCount());
        Assert::AreEqual(size_t(2), oDesktop.NoViewLayerCount());
    }

    TEST_METHOD(TestADismissedNoticeIsReplacedByANewOne)
    {
        OtherViewModel vmFirst(L"Assets List");
        OtherViewModel vmSecond(L"Memory Notes");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmFirst);
        const bool bFirst = oQt.WaitOnQt([]() { return ReadNotices().nOpen == 1; });

        // the user dismisses it: hidden at once, deleted only later
        bool bClicked = false;
        oQt.RunOnQt([&bClicked]() { bClicked = ClickNoticeOk(); });
        const bool bDismissed = oQt.WaitOnQt([]() { return ReadNotices().nOpen == 0; });

        oDesktop.ShowWindow(vmSecond);
        NoticeState oNotice;
        const bool bSecond = oQt.WaitOnQt([&oNotice]() {
            oNotice = ReadNotices();
            return oNotice.nOpen == 1 && oNotice.sText == L"Not available on Linux yet:\nMemory Notes";
        });
        oDesktop.Shutdown();
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bFirst, L"no first notice");
        Assert::IsTrue(bClicked, L"no notice to dismiss");
        Assert::IsTrue(bDismissed, L"the notice stayed open after OK");
        Assert::IsTrue(bSecond, L"the second title did not open a notice of its own");
        Assert::AreEqual(std::wstring(L"Not available on Linux yet:\nMemory Notes"), oNotice.sText);
        Assert::AreEqual(size_t(2), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestNoNoticeWithoutWidgets)
    {
        OtherViewModel vmWindow(L"Assets List");
        FakeQtApplicationHost oHost; // a borrowed QGuiApplication: no widgets
        QtDesktop oDesktop;

        oDesktop.ShowWindow(vmWindow);

        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::AreEqual(size_t(0), oHost.m_vInvoked.size(), L"something was queued for the Qt thread");
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestNoNoticeOnceClosedForShutdown)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.Shutdown();

        oDesktop.ShowWindow(vmWindow);
        NoticeState oNotice;
        oQt.RunOnQt([&oNotice]() { oNotice = ReadNotices(); }); // behind anything ShowWindow queued
        oQt.RunOnQt(DeleteNotices);

        Assert::AreEqual(0, oNotice.nOpen);
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestNoNoticeOnceClosedEvenWithWidgets)
    {
        // Nothing may even be queued once shutdown has begun: NoticeNotAvailable
        // checks before the Qt thread's own check, which then never runs.
        OtherViewModel vmWindow(L"Assets List");
        FakeQtApplicationHost oHost;
        oHost.m_bHasWidgets = true;
        QtDesktop oDesktop;
        oDesktop.Shutdown(); // runs CloseAll inline, which finds nothing open to touch
        const auto nInvokedAfterShutdown = oHost.m_vInvoked.size();

        oDesktop.ShowWindow(vmWindow);

        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
        Assert::AreEqual(nInvokedAfterShutdown, oHost.m_vInvoked.size(), L"something was queued for the Qt thread");
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestANoticeQueuedBeforeShutdownNeverOpens)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.SetShutdownCloseTimeout(std::chrono::milliseconds(50)); // the Qt thread is held below

        // The title is queued, then shutdown begins, all before the Qt thread
        // runs the queued call.
        const bool bHeld = oQt.HoldQtWhile([]() {},
                                           [&oDesktop, &vmWindow]() {
                                               oDesktop.ShowWindow(vmWindow);
                                               oDesktop.Shutdown();
                                           },
                                           []() {});
        NoticeState oNotice;
        oQt.RunOnQt([&oNotice]() { oNotice = ReadNotices(); });
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bHeld, L"the Qt thread was not held");
        Assert::AreEqual(0, oNotice.nOpen, L"a notice opened after shutdown began");
        Assert::AreEqual(size_t(0), oDesktop.NotAvailableNoticeCount());
    }

    TEST_METHOD(TestShutdownClosesTheNotice)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.ShowWindow(vmWindow);
        const bool bShown = oQt.WaitOnQt([]() { return ReadNotices().nOpen == 1; });

        oDesktop.Shutdown();
        NoticeState oNotice;
        int nNotices = -1;
        oQt.RunOnQt([&oNotice, &nNotices]() {
            oNotice = ReadNotices();
            nNotices = CountNotices();
        });
        oQt.RunOnQt(DeleteNotices);

        Assert::IsTrue(bShown, L"no notice opened");
        Assert::AreEqual(0, oNotice.nOpen, L"the notice outlived shutdown");
        Assert::AreEqual(0, nNotices, L"shutdown hid the notice instead of deleting it");
    }

    // --- CanShowWindow: whether a caller can expect a window ---

    TEST_METHOD(TestCanShowWindowWithAPresenter)
    {
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());

        Assert::IsTrue(oDesktop.CanShowWindow(vmWindow));
    }

    TEST_METHOD(TestCannotShowWindowWithWidgetsAndNoPresenter)
    {
        OtherViewModel vmWindow(L"Assets List");
        QtTestHost oQt;
        QtDesktop oDesktop;

        Assert::IsFalse(oDesktop.CanShowWindow(vmWindow));
    }

    TEST_METHOD(TestCanShowWindowWithoutWidgets)
    {
        // ShowWindow drops it silently, posting no notice: callers keep today's behaviour
        OtherViewModel vmWindow(L"Assets List");
        FakeQtApplicationHost oHost; // a borrowed QGuiApplication: no widgets
        QtDesktop oDesktop;

        Assert::IsTrue(oDesktop.CanShowWindow(vmWindow));
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
