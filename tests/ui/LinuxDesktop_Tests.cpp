#ifndef _WIN32

#include "ui/null/LinuxDesktop.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <atomic>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace null {
namespace tests {

TEST_CLASS(LinuxDesktop_Tests)
{
private:
    class FakeQtApplicationHost : public ra::services::IQtApplicationHost
    {
    public:
        FakeQtApplicationHost() noexcept : m_Override(this) {}

        bool IsAvailable() const override { return m_bAvailable; }
        bool HasWidgets() const override { return false; }
        bool IsOnQtThread() const override { return m_bOnQtThread; }
        void Invoke(std::function<void()> fAction) const override { m_vInvoked.push_back(std::move(fAction)); }
        bool InvokeAndWait(std::function<void()> fAction, std::chrono::milliseconds) const override
        {
            fAction();
            return true;
        }
        void AddStopHook(std::function<void()>) override {}
        void Stop() override {}

        bool m_bAvailable = true;
        bool m_bOnQtThread = false;
        mutable std::vector<std::function<void()>> m_vInvoked;

    private:
        ra::services::ServiceLocator::ServiceOverride<ra::services::IQtApplicationHost> m_Override;
    };

public:
    TEST_METHOD(TestUIWorkGoesToTheQtThread)
    {
        FakeQtApplicationHost oHost;
        LinuxDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnUIThread([&bRan]() { bRan = true; });
        Assert::IsFalse(bRan, L"ran inline instead of on the Qt thread");
        Assert::AreEqual(size_t(1), oHost.m_vInvoked.size());
    }

    TEST_METHOD(TestUIWorkRunsInlineWithoutQt)
    {
        FakeQtApplicationHost oHost;
        oHost.m_bAvailable = false;
        LinuxDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnUIThread([&bRan]() { bRan = true; });
        Assert::IsTrue(bRan);
        Assert::IsTrue(oDesktop.IsOnUIThread());
    }

    TEST_METHOD(TestIsOnUIThreadAsksTheHost)
    {
        FakeQtApplicationHost oHost;
        LinuxDesktop oDesktop;
        Assert::IsFalse(oDesktop.IsOnUIThread());
        oHost.m_bOnQtThread = true;
        Assert::IsTrue(oDesktop.IsOnUIThread());
    }

    TEST_METHOD(TestHostWorkGoesThroughTheDispatcher)
    {
        ra::services::impl::HostThreadDispatcher oDispatcher; // this thread is the host's
        ra::services::ServiceLocator::ServiceOverride<ra::services::impl::HostThreadDispatcher> oOverride(&oDispatcher);
        LinuxDesktop oDesktop;

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
        LinuxDesktop oDesktop;
        bool bRan = false;
        oDesktop.InvokeOnHostThread([&bRan]() { bRan = true; });
        Assert::IsTrue(bRan);
    }
};

} // namespace tests
} // namespace null
} // namespace ui
} // namespace ra

#endif // !_WIN32
