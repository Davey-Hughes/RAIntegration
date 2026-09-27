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

#include <QApplication>
#include <QDialog>
#include <QString>

#include <atomic>
#include <chrono>
#include <future>
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

    TEST_METHOD(TestShowModalWithoutAPresenterAnswersNo)
    {
        OtherViewModel vmWindow;
        QtTestHost oQt;
        QtDesktop oDesktop;

        Assert::AreEqual(DialogResult::No, oDesktop.ShowModal(vmWindow));
        Assert::AreEqual(size_t(1), oDesktop.NoViewLayerCount());
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
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
