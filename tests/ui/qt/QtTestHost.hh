#ifndef RA_TESTS_UI_QT_QTTESTHOST_HH
#define RA_TESTS_UI_QT_QTTESTHOST_HH
#pragma once

#ifndef _WIN32

#include "services/ServiceLocator.hh"
#include "services/impl/QtApplicationHost.hh"

#include "ui/qt/bindings/WindowBinding.hh"

#include <QWidget>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>

namespace ra {
namespace ui {
namespace qt {
namespace tests {

// An owned, offscreen QApplication on a thread of its own - what the Qt views
// see as the library's RA-Qt thread - registered as the IQtApplicationHost for
// as long as this object lives. Anything a test creates on the Qt thread must
// be deleted there before this goes out of scope.
class QtTestHost
{
public:
    QtTestHost() : m_oHost(OffscreenOptions()), m_oOverride(&m_oHost)
    {
        m_oHost.Start();
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(m_oHost.HasWidgets(),
                                                                      L"no offscreen QApplication");
    }

    ~QtTestHost() noexcept
    {
        // A test that failed can leave a bound window behind. Delete it here,
        // while its view model - declared before this host - still exists, so
        // its binding leaves the view model and the process-wide binding list;
        // the next test's shutdown would otherwise find it. Only self-deleting
        // top-level windows (DialogBase): a test's own stack widgets are not
        // this helper's to delete.
        m_oHost.InvokeAndWait(
            []() {
                for (auto* pWidget : ra::ui::qt::bindings::WindowBinding::GetBoundWidgets())
                {
                    if (pWidget->testAttribute(Qt::WA_DeleteOnClose))
                        delete pWidget;
                }
            },
            std::chrono::seconds(5));
        m_oHost.Stop();
    }

    QtTestHost(const QtTestHost&) noexcept = delete;
    QtTestHost& operator=(const QtTestHost&) noexcept = delete;
    QtTestHost(QtTestHost&&) noexcept = delete;
    QtTestHost& operator=(QtTestHost&&) noexcept = delete;

    ra::services::impl::QtApplicationHost& Host() noexcept { return m_oHost; }

    // Runs fAction on the Qt thread and waits for it. Never assert inside
    // fAction: an exception thrown on the Qt thread takes the process with it.
    // Capture what the test needs and assert on the test's thread.
    void RunOnQt(const std::function<void()>& fAction)
    {
        Microsoft::VisualStudio::CppUnitTestFramework::Assert::IsTrue(
            m_oHost.InvokeAndWait(fAction, std::chrono::seconds(5)), L"the Qt thread did not run the call");
    }

    // Asks fDone on the Qt thread until it answers true or tTimeout passes.
    bool WaitOnQt(const std::function<bool()>& fDone, std::chrono::milliseconds tTimeout = std::chrono::seconds(5))
    {
        const auto tDeadline = std::chrono::steady_clock::now() + tTimeout;
        do
        {
            bool bDone = false;
            RunOnQt([&fDone, &bDone]() { bDone = fDone(); });
            if (bDone)
                return true;

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < tDeadline);

        return false;
    }

    // Asks fDone on this thread - for waiting while the Qt thread is being held on purpose.
    static bool WaitFor(const std::function<bool()>& fDone,
                        std::chrono::milliseconds tTimeout = std::chrono::seconds(5))
    {
        const auto tDeadline = std::chrono::steady_clock::now() + tTimeout;
        while (!fDone())
        {
            if (std::chrono::steady_clock::now() >= tDeadline)
                return false;

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        return true;
    }

    // Holds the Qt thread - after running fFirst there - while fMeanwhile runs on a thread of its own, so whatever
    // fMeanwhile posts is queued. Then runs fThen on the Qt thread, still ahead of those posts, and returns once they
    // have run too: the user acting on what the control shows before a worker's change reaches it. Returns whether
    // the Qt thread was held. Never assert inside fFirst or fThen: they run on the Qt thread.
    bool HoldQtWhile(const std::function<void()>& fFirst, const std::function<void()>& fMeanwhile,
                     const std::function<void()>& fThen)
    {
        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        m_oHost.Invoke([&bHeld, fRelease, fFirst, fThen]() {
            fFirst();
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
            fThen();
        });
        const bool bWasHeld = WaitFor([&bHeld]() { return bHeld.load(); });

        std::thread(fMeanwhile).join();
        oRelease.set_value();

        RunOnQt([]() {}); // queued behind the held call, and so behind everything fMeanwhile posted
        return bWasHeld;
    }

private:
    static ra::services::impl::QtApplicationHost::Options OffscreenOptions()
    {
        ra::services::impl::QtApplicationHost::Options oOptions;
        oOptions.fProbe = []() { return ra::services::impl::DisplayProbeResult{true, "test: offscreen"}; };
        oOptions.vArguments = {"-platform", "offscreen"};
        return oOptions;
    }

    ra::services::impl::QtApplicationHost m_oHost;
    ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> m_oOverride;
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32

#endif // !RA_TESTS_UI_QT_QTTESTHOST_HH
