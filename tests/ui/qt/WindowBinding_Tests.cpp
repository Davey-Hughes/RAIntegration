#ifndef _WIN32

#include "ui/qt/bindings/WindowBinding.hh"

#include "tests/devkit/testutil/DetachedCall.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QLabel>
#include <QString>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {
namespace tests {

using ra::ui::qt::tests::QtTestHost;

namespace {

class TextViewModel : public WindowViewModelBase
{
public:
    static const StringModelProperty TextProperty;
    void SetText(const std::wstring& sValue) { SetValue(TextProperty, sValue); }
};

const StringModelProperty TextViewModel::TextProperty("TextViewModel", "Text", L"initial");

// A window with one bound label, built on the Qt thread and not yet attached.
// The widget is declared before the binding, so the binding goes first when
// this is deleted, as it does inside a DialogBase.
struct BoundWindow
{
    explicit BoundWindow(TextViewModel& vmText) : oBinding(vmText)
    {
        pLabel = new QLabel(&oWidget);
        oBinding.BindLabel(*pLabel, TextViewModel::TextProperty);
    }

    void Attach() { oBinding.SetWidget(oWidget); }

    QWidget oWidget;
    WindowBinding oBinding;
    QLabel* pLabel = nullptr;
};

} // namespace

TEST_CLASS(WindowBinding_Tests)
{
public:
    TEST_METHOD(TestSetWidgetPushesTheCurrentValues)
    {
        TextViewModel vmText;
        vmText.SetWindowTitle(L"Title");
        vmText.SetText(L"before");
        QtTestHost oQt;

        std::wstring sTitle, sText;
        oQt.RunOnQt([&vmText, &sTitle, &sText]() {
            BoundWindow oWindow(vmText);
            oWindow.Attach();
            sTitle = oWindow.oWidget.windowTitle().toStdWString();
            sText = oWindow.pLabel->text().toStdWString();
        });

        Assert::AreEqual(std::wstring(L"Title"), sTitle);
        Assert::AreEqual(std::wstring(L"before"), sText);
    }

    TEST_METHOD(TestAChangeBeforeSetWidgetIsNotLost)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        BoundWindow* pWindow = nullptr;
        oQt.RunOnQt([&vmText, &pWindow]() { pWindow = new BoundWindow(vmText); }); // bound, not attached

        vmText.SetText(L"between"); // notified with no widget: SetWidget has to read it

        std::wstring sText;
        oQt.RunOnQt([pWindow, &sText]() {
            pWindow->Attach();
            sText = pWindow->pLabel->text().toStdWString();
        });
        oQt.RunOnQt([pWindow]() { delete pWindow; });

        Assert::AreEqual(std::wstring(L"between"), sText);
    }

    TEST_METHOD(TestChangesFromAnotherThreadReachTheWindow)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        BoundWindow* pWindow = nullptr;
        oQt.RunOnQt([&vmText, &pWindow]() {
            pWindow = new BoundWindow(vmText);
            pWindow->Attach();
        });

        std::thread([&vmText]() {
            vmText.SetText(L"from a worker");
            vmText.SetWindowTitle(L"New title");
        }).join();

        const bool bArrived = oQt.WaitOnQt([pWindow]() {
            return pWindow->pLabel->text() == QStringLiteral("from a worker") &&
                   pWindow->oWidget.windowTitle() == QStringLiteral("New title");
        });
        oQt.RunOnQt([pWindow]() { delete pWindow; });

        Assert::IsTrue(bArrived, L"the label or the title never changed");
    }

    TEST_METHOD(TestChangesFromAnotherThreadNeverWaitForTheQtThread)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        BoundWindow* pWindow = nullptr;
        oQt.RunOnQt([&vmText, &pWindow]() {
            pWindow = new BoundWindow(vmText);
            pWindow->Attach();
        });

        // Hold the Qt thread for up to 5 s, as a slow view or a nested dialog can.
        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        const auto tStart = std::chrono::steady_clock::now();
        vmText.SetText(L"one");
        vmText.SetText(L"two");
        const auto tElapsed = std::chrono::steady_clock::now() - tStart;
        oRelease.set_value();

        const bool bArrived =
            oQt.WaitOnQt([pWindow]() { return pWindow->pLabel->text() == QStringLiteral("two"); });
        oQt.RunOnQt([pWindow]() { delete pWindow; });

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::IsTrue(tElapsed < std::chrono::seconds(1), L"SetValue waited for the busy Qt thread");
        Assert::IsTrue(bArrived, L"the last value never reached the label");
    }

    TEST_METHOD(TestADialogResultClosesTheWindow)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        BoundWindow* pWindow = nullptr;
        oQt.RunOnQt([&vmText, &pWindow]() {
            pWindow = new BoundWindow(vmText);
            pWindow->Attach();
            pWindow->oWidget.show();
        });

        vmText.SetDialogResult(DialogResult::OK);

        const bool bClosed = oQt.WaitOnQt([pWindow]() { return !pWindow->oWidget.isVisible(); });
        oQt.RunOnQt([pWindow]() { delete pWindow; });

        Assert::IsTrue(bClosed, L"the window stayed open");
    }

    TEST_METHOD(TestGetBindingFor)
    {
        TextViewModel vmText;
        TextViewModel vmOther;
        QtTestHost oQt;

        bool bFound = false;
        bool bOtherFound = true;
        bool bFoundAfterDelete = true;
        oQt.RunOnQt([&]() {
            auto* pWindow = new BoundWindow(vmText);
            bFound = (WindowBinding::GetBindingFor(vmText) == &pWindow->oBinding);
            bOtherFound = (WindowBinding::GetBindingFor(vmOther) != nullptr);
            delete pWindow;
            bFoundAfterDelete = (WindowBinding::GetBindingFor(vmText) != nullptr);
        });

        Assert::IsTrue(bFound);
        Assert::IsFalse(bOtherFound);
        Assert::IsFalse(bFoundAfterDelete);
    }

    TEST_METHOD(TestABindingCanBeDestroyedWhileAWorkerChangesItsViewModel)
    {
        // A worker changes the view model the whole time windows are created and
        // destroyed on the Qt thread. The binding leaves the notify targets first
        // in its destructor, which waits for a handler in progress, so no handler
        // runs in a half-destroyed binding. A regression shows up here as a crash
        // or, under AddressSanitizer, a use-after-free report - not reliably as an
        // assertion (measured: 10 of 10 runs caught the destructor half reverted
        // under ASan); the deterministic proofs are ViewModelBase_Tests'
        // TestRemoveNotifyTargetAndWaitWaitsForAnotherThreadsCall and BindingBase_Tests.
        //
        // A destructor that deadlocks instead fails the test rather than hanging
        // or killing the run: every cycle and the worker are waited for with a
        // timeout, both are stopped before anything asserts, and what they use is
        // on the heap, leaked on a failure. The cycles are posted, not run with
        // InvokeAndWait, which waits without limit for a call that has started.
        // Checked with a handler made to wait on the Qt thread: the run ended
        // with this test's FAIL line, where the RunOnQt version hung.
        struct State
        {
            TextViewModel vmText;
            std::atomic<bool> bStop{false};
            std::atomic<int> nChanges{0};
            std::atomic<int> nWindows{0};
        };
        auto* pState = new State();
        QtTestHost oQt;

        ra::tests::DetachedCall oWorker([pState]() {
            int i = 0;
            while (!pState->bStop)
            {
                pState->vmText.SetText(std::to_wstring(++i));
                ++pState->nChanges;
            }
        });

        bool bCycleFinished = true;
        for (int nCycle = 0; nCycle < 200 && bCycleFinished; ++nCycle)
        {
            auto pDone = std::make_shared<std::promise<void>>();
            auto fDone = pDone->get_future();
            oQt.Host().Invoke([pState, pDone]() {
                auto* pWindow = new BoundWindow(pState->vmText);
                pWindow->Attach();
                delete pWindow;
                ++pState->nWindows;
                pDone->set_value();
            });
            bCycleFinished = (fDone.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
        }

        pState->bStop = true;
        const bool bWorkerFinished = oWorker.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bCycleFinished, L"a window was not created and destroyed within 5 s: is a destructor stuck?");
        Assert::IsTrue(bWorkerFinished, L"the worker did not stop: is a SetText stuck?");
        Assert::AreEqual(200, pState->nWindows.load());
        Assert::IsTrue(pState->nChanges > 0, L"the worker never ran");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
