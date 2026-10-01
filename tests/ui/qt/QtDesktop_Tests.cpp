#ifndef _WIN32

#include "ui/qt/QtDesktop.hh"

#include "services/impl/HostThreadDispatcher.hh"

#include "ui/drawing/qt/QtSurface.hh"
#include "ui/drawing/qt/ScreenCapture.hh"
#include "ui/qt/DialogBase.hh"
#include "ui/qt/bindings/WindowBinding.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"

#include "tests/RA_UnitTestHelpers.h"
#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/BorrowedQtApplication.hh"
#include "tests/ui/qt/ModalCaller.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <mutex>
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

// Sets an environment variable - or unsets it, given nullptr - for as long as this lives, then puts back what was
// there. Declare it before the test's Qt host: the environment must not change while a Qt thread runs.
class EnvironmentOverride
{
public:
    EnvironmentOverride(const char* sName, const char* sValue) : m_sName(sName)
    {
        const char* sOld = std::getenv(sName);
        m_bHadValue = (sOld != nullptr);
        if (m_bHadValue)
            m_sOldValue = sOld;

        if (sValue != nullptr)
            setenv(sName, sValue, 1);
        else
            unsetenv(sName);
    }

    ~EnvironmentOverride() noexcept
    {
        if (m_bHadValue)
            setenv(m_sName.c_str(), m_sOldValue.c_str(), 1);
        else
            unsetenv(m_sName.c_str());
    }

    EnvironmentOverride(const EnvironmentOverride&) noexcept = delete;
    EnvironmentOverride& operator=(const EnvironmentOverride&) noexcept = delete;
    EnvironmentOverride(EnvironmentOverride&&) noexcept = delete;
    EnvironmentOverride& operator=(EnvironmentOverride&&) noexcept = delete;

private:
    std::string m_sName;
    std::string m_sOldValue;
    bool m_bHadValue = false;
};

class FakeQtApplicationHost : public ra::services::IQtApplicationHost
{
public:
    FakeQtApplicationHost() noexcept : m_Override(this) {}

    bool IsAvailable() const override { return m_bAvailable; }
    bool HasWidgets() const override { return m_bHasWidgets; }
    bool IsOnQtThread() const override { return m_bOnQtThread; }
    bool IsBorrowed() const noexcept override { return m_bBorrowed; }
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
    bool m_bBorrowed = false;
    mutable std::vector<std::function<void()>> m_vInvoked;
    mutable int m_nInvokeAndWait = 0;

private:
    ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> m_Override;
};

// A 2 x 1 emulator picture for CaptureClientArea, alpha 0 as a core's RGBX frame can be.
int CaptureTwoPixels(int* pWidth, int* pHeight, const void** ppPixels, int* pStride)
{
    static const uint32_t pPixels[2] = {0x00112233, 0x00445566};
    *pWidth = 2;
    *pHeight = 1;
    *ppPixels = pPixels;
    *pStride = 8;
    return 1;
}

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

    // --- a borrowed host: the Qt thread is the emulator's own, pumped once a frame ---

    TEST_METHOD(TestABorrowedHostWaitsForAModalWithoutAClock)
    {
        TestViewModel vmWindow(L"A");
        BorrowedQtApplication oApp;
        ra::services::impl::QtApplicationHost oHost;
        oHost.Start();
        ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> oOverride(&oHost);
        Assert::IsTrue(oHost.IsBorrowed(), L"the host did not borrow the test's application");
        {
            QtDesktop oDesktop;
            oDesktop.AddPresenter(std::make_unique<TestPresenter>());
            oDesktop.SetModalStartTimeout(50ms); // must not apply

            ModalCaller oCaller(oDesktop, vmWindow);
            std::this_thread::sleep_for(300ms); // the emulator is busy: nothing pumps
            const bool bGaveUp = oCaller.Returned(0ms);

            // the emulator's next frame: the dialog opens; answer it
            const bool bOpened = oApp.PumpUntil([]() { return FindDialog(QStringLiteral("A")) != nullptr; }, 5s);
            if (bOpened)
                FindDialog(QStringLiteral("A"))->accept();
            const bool bReturned = oApp.PumpUntil([&oCaller]() { return oCaller.Returned(0ms); }, 5s);
            if (!bReturned)
                RejectAll();

            Assert::IsFalse(bGaveUp, L"the caller gave up on the clock");
            Assert::IsTrue(bOpened, L"the dialog never opened once the host pumped");
            Assert::IsTrue(bReturned, L"answering the dialog did not release the caller");
            Assert::AreEqual(DialogResult::OK, oCaller.Result());
            Assert::AreEqual(size_t(0), oDesktop.ModalNotStartedCount());
            Assert::AreEqual(size_t(1), oDesktop.StillWaitingForHostCount(), L"the long wait was not logged, once");
        }
        oHost.Stop();
    }

    TEST_METHOD(TestABorrowedHostReleasesAWaitingWorkerWhenTheDesktopCloses)
    {
        TestViewModel vmWindow(L"A");
        BorrowedQtApplication oApp;
        ra::services::impl::QtApplicationHost oHost;
        oHost.Start();
        ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> oOverride(&oHost);
        {
            QtDesktop oDesktop;
            auto pPresenter = std::make_unique<TestPresenter>();
            auto& oPresenter = *pPresenter;
            oDesktop.AddPresenter(std::move(pPresenter));

            ModalCaller oCaller(oDesktop, vmWindow);
            std::this_thread::sleep_for(200ms); // queued on the host's loop, never pumped
            const bool bWaiting = !oCaller.Returned(0ms);

            oDesktop.Shutdown(); // inline: this thread is the Qt thread. CloseAll marks the desktop closed.
            const bool bReleased = oCaller.Returned(2s);

            // the queued call runs on the next pump and must open nothing
            oApp.Pump();
            oApp.Pump();

            Assert::IsTrue(bWaiting, L"the caller returned before anything happened");
            Assert::IsTrue(bReleased, L"closing the desktop did not release the waiting caller");
            Assert::AreEqual(DialogResult::No, oCaller.Result());
            Assert::AreEqual(size_t(1), oDesktop.ModalNotStartedCount());
            Assert::AreEqual(0, oPresenter.nCreateModal.load());
            Assert::IsTrue(FindDialog(QStringLiteral("A")) == nullptr, L"the queued call opened the dialog after its caller was answered");
        }
        oHost.Stop();
    }

    TEST_METHOD(TestShutdownDestroysAWorkersModalWithoutAPump)
    {
        // Nothing pumps between _RA_Shutdown and the loader's dlclose: a rejected dialog left to deleteLater would
        // outlive the library, with its connections into library code.
        TestViewModel vmWindow(L"A");
        BorrowedQtApplication oApp;
        ra::services::impl::QtApplicationHost oHost;
        oHost.Start();
        ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> oOverride(&oHost);
        {
            QtDesktop oDesktop;
            oDesktop.AddPresenter(std::make_unique<TestPresenter>());

            ModalCaller oCaller(oDesktop, vmWindow);
            const bool bOpened = oApp.PumpUntil([]() { return FindDialog(QStringLiteral("A")) != nullptr; }, 5s);

            bool bDestroyed = false;
            QObject oWatcher; // ends the watch with the test, whatever became of the dialog
            if (bOpened)
            {
                QObject::connect(FindDialog(QStringLiteral("A")), &QObject::destroyed, &oWatcher,
                                 [&bDestroyed]() { bDestroyed = true; });
            }

            oDesktop.Shutdown(); // inline: this thread is the Qt thread
            const bool bDestroyedWithoutAPump = bDestroyed;
            const bool bReleased = oCaller.Returned(2s);

            // whatever was left to a deferred delete goes now, inside the test
            oApp.Pump();
            if (!bReleased)
                RejectAll();

            Assert::IsTrue(bOpened, L"the dialog never opened once the host pumped");
            Assert::IsTrue(bDestroyedWithoutAPump, L"the rejected dialog outlived Shutdown, waiting for a pump");
            Assert::IsTrue(bReleased, L"Shutdown left the worker waiting on its dialog");
            // the reject's own answer, not the destroyed-unanswered No: finished() released the caller first
            Assert::AreEqual(DialogResult::Cancel, oCaller.Result());
            Assert::AreEqual(size_t(1), oDesktop.ClosedForShutdownCount());
        }
        oHost.Stop();
    }

    TEST_METHOD(TestAnOwnedHostStillGivesUpOnTheClock)
    {
        // TestAModalThatCannotOpenInTimeAnswersNo covers the behaviour; this pins the mode it belongs to.
        QtTestHost oQt;
        Assert::IsFalse(oQt.Host().IsBorrowed());
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

    // --- RA_AUTO_ANSWER_DIALOGS: the headless runs' hook ---

    TEST_METHOD(TestAutoAnswerAnswersModalsWithoutShowingThem)
    {
        TestViewModel vmOnQt(L"A");
        TestViewModel vmFromWorker(L"B");
        EnvironmentOverride oAutoAnswer("RA_AUTO_ANSWER_DIALOGS", "1");
        QtTestHost oQt;
        QtDesktop oDesktop;
        auto pPresenter = std::make_unique<TestPresenter>();
        auto& oPresenter = *pPresenter;
        oDesktop.AddPresenter(std::move(pPresenter));

        // from the Qt thread, which would exec() the dialog
        std::atomic<int> nOnQt{-1};
        oQt.Host().Invoke([&oDesktop, &vmOnQt, &nOnQt]() { nOnQt = ra::etoi(oDesktop.ShowModal(vmOnQt)); });
        const bool bOnQtReturned = oQt.WaitOnQt([&nOnQt]() { return nOnQt.load() != -1; }, 2s);
        if (!bOnQtReturned)
        {
            oQt.RunOnQt(RejectAll); // it is in exec(): answer it, and let the nested loop unwind
            oQt.WaitOnQt([&nOnQt]() { return nOnQt.load() != -1; });
        }

        // from a worker, which would wait for the Qt thread to open it
        ModalCaller oCaller(oDesktop, vmFromWorker);
        const bool bWorkerReturned = oCaller.Returned(2s);
        if (!bWorkerReturned)
        {
            oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("B")) != nullptr; }, 2s);
            oQt.RunOnQt(RejectAll);
            oCaller.Returned(2s);
        }

        Assert::IsTrue(bOnQtReturned, L"the Qt thread's ShowModal showed its dialog");
        Assert::IsTrue(bWorkerReturned, L"the worker's ShowModal showed its dialog");
        Assert::AreEqual(DialogResult::No, ra::itoe<DialogResult>(nOnQt.load()));
        Assert::AreEqual(DialogResult::No, oCaller.Result());
        Assert::AreEqual(0, oPresenter.nCreateModal.load(), L"a dialog was created");
        Assert::AreEqual(size_t(2), oDesktop.AutoAnsweredCount());
    }

    TEST_METHOD(TestModalsAreShownWhenAutoAnswerIsOff)
    {
        // unset, empty and "0" all leave the hook off
        for (const char* sValue : {static_cast<const char*>(nullptr), "", "0"})
        {
            TestViewModel vmWindow(L"A");
            EnvironmentOverride oAutoAnswer("RA_AUTO_ANSWER_DIALOGS", sValue);
            QtTestHost oQt;
            QtDesktop oDesktop;
            auto pPresenter = std::make_unique<TestPresenter>();
            auto& oPresenter = *pPresenter;
            oDesktop.AddPresenter(std::move(pPresenter));

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
            Assert::AreEqual(1, oPresenter.nCreateModal.load());
            Assert::AreEqual(size_t(0), oDesktop.AutoAnsweredCount());
        }
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

    // --- CaptureClientArea: what OverlayManager::CaptureScreenshot stores, and draws over, unchecked ---

    TEST_METHOD(TestCaptureClientAreaIsEmptyNotNullWithoutAScreenCapture)
    {
        TestViewModel vmEmulator(L"Emulator");
        QtDesktop oDesktop;

        const auto pSurface = oDesktop.CaptureClientArea(vmEmulator);
        Assert::IsNotNull(pSurface.get());
        Assert::AreEqual(0U, pSurface->GetWidth());
        Assert::AreEqual(0U, pSurface->GetHeight());
    }

    TEST_METHOD(TestCaptureClientAreaIsEmptyNotNullWhenTheCaptureIsRefused)
    {
        TestViewModel vmEmulator(L"Emulator");
        ra::ui::drawing::qt::QtSurfaceFactory oFactory;
        ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> oFactoryOverride(&oFactory);
        ra::ui::drawing::qt::ScreenCapture oCapture; // nothing installed
        ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::qt::ScreenCapture> oCaptureOverride(&oCapture);
        QtDesktop oDesktop;

        const auto pSurface = oDesktop.CaptureClientArea(vmEmulator);
        Assert::IsNotNull(pSurface.get());
        Assert::AreEqual(0U, pSurface->GetWidth());
    }

    TEST_METHOD(TestCaptureClientAreaIsTheEmulatorsPicture)
    {
        TestViewModel vmEmulator(L"Emulator");
        ra::ui::drawing::qt::QtSurfaceFactory oFactory;
        ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::ISurfaceFactory> oFactoryOverride(&oFactory);
        ra::ui::drawing::qt::ScreenCapture oCapture;
        oCapture.SetCaptureFunction(CaptureTwoPixels);
        ra::services::ServiceLocator::ServiceOverride<ra::ui::drawing::qt::ScreenCapture> oCaptureOverride(&oCapture);
        QtDesktop oDesktop;

        const auto pSurface = oDesktop.CaptureClientArea(vmEmulator);
        const auto* pQtSurface = dynamic_cast<const ra::ui::drawing::qt::QtSurface*>(pSurface.get());
        Assert::IsNotNull(pQtSurface, L"not the captured surface");
        Assert::AreEqual(2U, pQtSurface->GetWidth());
        Assert::AreEqual(1U, pQtSurface->GetHeight());
        Assert::AreEqual(0xFF112233U, pQtSurface->GetImage().pixel(0, 0));
        Assert::AreEqual(0xFF445566U, pQtSurface->GetImage().pixel(1, 0));
    }
    // --- OpenUrl. On Wayland QDesktopServices::openUrl launches the browser only when an activation token arrives for
    // the focus window, so a modal dialog that asked for a URL and closed at once took the launch with it: Report
    // Achievement Problem opened nothing (owner's L2 sign-off; measured under a headless KWin). ---

    TEST_METHOD(TestOpenUrlWithNoModalOpensAtOnce)
    {
        QtTestHost oQt;
        QtDesktop oDesktop;
        std::mutex oMutex;
        std::vector<std::string> vOpened;
        oDesktop.SetUrlOpener([&oMutex, &vOpened](const QUrl& oUrl) {
            std::lock_guard<std::mutex> oLock(oMutex);
            vOpened.push_back(oUrl.toString().toStdString());
            return true;
        });

        oDesktop.OpenUrl("https://host/game/1");
        const bool bOpened = QtTestHost::WaitFor([&oMutex, &vOpened]() {
            std::lock_guard<std::mutex> oLock(oMutex);
            return !vOpened.empty();
        });

        Assert::IsTrue(bOpened, L"the URL was never opened");
        std::lock_guard<std::mutex> oLock(oMutex);
        Assert::AreEqual(size_t{1}, vOpened.size());
        Assert::AreEqual(std::string("https://host/game/1"), vOpened.front());
    }

    TEST_METHOD(TestOpenUrlFromAModalOpensOnceTheModalIsGone)
    {
        // As Report Achievement Problem does: its OK asks for the URL, then the dialog closes, and the ShowModal that
        // owns it deletes it.
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());
        auto pDialog = std::make_shared<QPointer<QDialog>>();
        std::atomic<int> nOpened{0};
        std::atomic<bool> bDialogAliveAtOpen{false};
        oDesktop.SetUrlOpener([&nOpened, &bDialogAliveAtOpen, pDialog](const QUrl&) {
            bDialogAliveAtOpen = !pDialog->isNull();
            ++nOpened;
            return true;
        });

        std::atomic<int> nResult{-1};
        oQt.Host().Invoke([&oDesktop, &vmWindow, &nResult]() { nResult = ra::etoi(oDesktop.ShowModal(vmWindow)); });
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        int nOpenedWhileUp = -1;
        oQt.RunOnQt([&oDesktop, pDialog, &nOpened, &nOpenedWhileUp]() {
            *pDialog = FindDialog(QStringLiteral("A"));
            if (pDialog->isNull())
                return;
            oDesktop.OpenUrl("https://host/achievement/3/report-issue");
            nOpenedWhileUp = nOpened.load();
            (*pDialog)->accept();
        });
        const bool bOpened = QtTestHost::WaitFor([&nOpened]() { return nOpened.load() > 0; }, std::chrono::seconds(3));
        oQt.RunOnQt([]() {});

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::AreEqual(0, nOpenedWhileUp, L"the URL was opened while the dialog that asked for it was up");
        Assert::IsTrue(bOpened, L"the URL was never opened");
        Assert::AreEqual(1, nOpened.load());
        Assert::IsFalse(bDialogAliveAtOpen.load(), L"the URL was opened before the dialog was gone");
    }

    TEST_METHOD(TestOpenUrlFromAModalOpensAsSoonAsAnotherWindowHasTheFocus)
    {
        // The emulator's window takes the focus back when the dialog goes, and the URL opens then - with that window's
        // activation token on Wayland - not after the fallback wait. Offscreen has no compositor to hand the focus
        // back (measured: it stays with no window), so the test does what KWin does: it activates the main window
        // once the dialog is gone.
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());
        std::atomic<int> nOpened{0};
        oDesktop.SetUrlOpener([&nOpened](const QUrl&) {
            ++nOpened;
            return true;
        });
        QWidget* pMain = nullptr;
        oQt.RunOnQt([&pMain]() {
            pMain = new QWidget();
            pMain->resize(200, 100);
            pMain->show();
            pMain->activateWindow();
        });
        const bool bMainFocused = oQt.WaitOnQt([]() { return QGuiApplication::focusWindow() != nullptr; });

        std::atomic<int> nResult{-1};
        oQt.Host().Invoke([&oDesktop, &vmWindow, &nResult]() { nResult = ra::etoi(oDesktop.ShowModal(vmWindow)); });
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        auto tAccepted = std::chrono::steady_clock::now();
        oQt.RunOnQt([&oDesktop, &tAccepted]() {
            auto* pDialog = FindDialog(QStringLiteral("A"));
            if (pDialog == nullptr)
                return;
            oDesktop.OpenUrl("https://host/achievement/3/report-issue");
            tAccepted = std::chrono::steady_clock::now();
            pDialog->accept();
        });
        const bool bGone = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) == nullptr; });
        oQt.RunOnQt([pMain]() { pMain->activateWindow(); });
        const bool bOpened = QtTestHost::WaitFor([&nOpened]() { return nOpened.load() > 0; }, std::chrono::seconds(3));
        const auto tTaken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - tAccepted);
        oQt.RunOnQt([pMain]() { delete pMain; });

        Assert::IsTrue(bMainFocused, L"the main window never took the focus");
        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bGone, L"the dialog never went");
        Assert::IsTrue(bOpened, L"the URL was never opened");
        Assert::IsTrue(tTaken < std::chrono::milliseconds(300),
                       (L"the URL waited " + std::to_wstring(tTaken.count()) + L" ms: the fallback, not the focus").c_str());
    }

    TEST_METHOD(TestOpenUrlFromAModalOpensAtOnceWhenAnotherWindowAlreadyHasTheFocus)
    {
        // A dialog deleted later than it closes - a worker's, deleteLater after finished - can be destroyed with the
        // focus already on another window: no focus change follows, and the URL must not wait for the fallback. The
        // test orders it itself: the dialog closes, the main window takes the focus, then the dialog is destroyed.
        QtTestHost oQt;
        QtDesktop oDesktop;
        std::atomic<int> nOpened{0};
        oDesktop.SetUrlOpener([&nOpened](const QUrl&) {
            ++nOpened;
            return true;
        });

        std::atomic<bool> bDone{false};
        std::atomic<bool> bMainFocused{false};
        std::atomic<int> nOpenedBeforeDestroyed{-1};
        auto tDestroyed = std::make_shared<std::chrono::steady_clock::time_point>();
        oQt.Host().Invoke([&oDesktop, &nOpened, &bDone, &bMainFocused, &nOpenedBeforeDestroyed, tDestroyed]() {
            QWidget oMain;
            oMain.resize(200, 100);
            oMain.show();
            auto pDialog = std::make_unique<QDialog>();
            pDialog->setWindowModality(Qt::ApplicationModal);
            QTimer::singleShot(0, pDialog.get(), [&oDesktop, pDialog = pDialog.get()]() {
                oDesktop.OpenUrl("https://host/achievement/3/report-issue");
                pDialog->accept();
            });
            pDialog->exec();

            oMain.activateWindow();
            QElapsedTimer oWait;
            oWait.start();
            while (QGuiApplication::focusWindow() != oMain.windowHandle() && oWait.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            bMainFocused = (QGuiApplication::focusWindow() == oMain.windowHandle());

            nOpenedBeforeDestroyed = nOpened.load();
            *tDestroyed = std::chrono::steady_clock::now();
            pDialog.reset();

            oWait.restart();
            while (nOpened.load() == 0 && oWait.elapsed() < 2000)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            bDone = true;
        });
        const bool bFinished = QtTestHost::WaitFor([&bDone]() { return bDone.load(); }, std::chrono::seconds(6));
        const auto tTaken = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - *tDestroyed);

        Assert::IsTrue(bFinished, L"the Qt thread never finished the sequence");
        Assert::IsTrue(bMainFocused.load(), L"the main window never took the focus: not the case under test");
        Assert::AreEqual(0, nOpenedBeforeDestroyed.load(), L"the URL was opened before the dialog was destroyed");
        Assert::AreEqual(1, nOpened.load(), L"the URL was not opened once");
        Assert::IsTrue(tTaken < std::chrono::milliseconds(300),
                       (L"the URL waited " + std::to_wstring(tTaken.count()) + L" ms: the fallback, not the focus").c_str());
    }

    TEST_METHOD(TestAUrlWaitingForAModalIsDroppedAtShutdown)
    {
        // Nothing may call into the library once it is unloaded: shutdown drops what still waits.
        TestViewModel vmWindow(L"A");
        QtTestHost oQt;
        QtDesktop oDesktop;
        oDesktop.AddPresenter(std::make_unique<TestPresenter>());
        std::atomic<int> nOpened{0};
        oDesktop.SetUrlOpener([&nOpened](const QUrl&) {
            ++nOpened;
            return true;
        });

        std::atomic<int> nResult{-1};
        oQt.Host().Invoke([&oDesktop, &vmWindow, &nResult]() { nResult = ra::etoi(oDesktop.ShowModal(vmWindow)); });
        const bool bShown = oQt.WaitOnQt([]() { return FindDialog(QStringLiteral("A")) != nullptr; });
        oQt.RunOnQt([&oDesktop]() { oDesktop.OpenUrl("https://host/achievement/3/report-issue"); });
        oDesktop.Shutdown(); // rejects the dialog, whose ShowModal then returns and deletes it
        const bool bReturned = oQt.WaitOnQt([&nResult]() { return nResult.load() != -1; });
        std::this_thread::sleep_for(std::chrono::seconds(1)); // past any wait for the focus
        oQt.RunOnQt([]() {});

        Assert::IsTrue(bShown, L"the dialog never appeared");
        Assert::IsTrue(bReturned, L"ShowModal never returned");
        Assert::AreEqual(0, nOpened.load(), L"a URL opened after shutdown");
    }

};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
