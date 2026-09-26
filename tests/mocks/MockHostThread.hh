#ifndef RA_SERVICES_MOCK_HOST_THREAD_HH
#define RA_SERVICES_MOCK_HOST_THREAD_HH
#pragma once

#ifndef _WIN32

#include "services/ServiceLocator.hh"
#include "services/impl/HostThreadDispatcher.hh"

#include "util/LibraryUiThread.hh"

#include <functional>
#include <thread>

namespace ra {
namespace services {
namespace mocks {

/// <summary>
/// Makes the test's own thread the emulator's thread, as far as the registered HostThreadDispatcher can tell, so
/// that work queued from any other thread waits until the test drains it. Construct it on the test's thread. With
/// it registered, AchievementRuntime::QueueMemoryRead defers every read made off the frame thread.
/// </summary>
class MockHostThread
{
public:
    MockHostThread() noexcept : m_Override(&m_oDispatcher) {}

    ra::services::impl::HostThreadDispatcher& Dispatcher() noexcept { return m_oDispatcher; }

    /// <summary>
    /// Runs <paramref name="fAction" /> on a new thread and waits for it: the view's side of a call. Assert on the
    /// test's thread afterwards, never inside - an assertion that throws on the new thread terminates the process.
    /// </summary>
    static void RunElsewhere(const std::function<void()>& fAction) { std::thread(fAction).join(); }

    /// <summary>
    /// Like <see cref="RunElsewhere" />, on a thread marked as the library's own UI thread - as the owned Qt thread
    /// is - so that EmulatorMemoryContext counts any emulator memory <paramref name="fAction" /> touches directly.
    /// </summary>
    static void RunOnLibraryUiThread(const std::function<void()>& fAction)
    {
        std::thread([&fAction]() {
            ra::util::MarkLibraryUiThread();
            fAction();
        }).join();
    }

    /// <summary>Runs what other threads queued, on this thread: the emulator's next chance to run it.</summary>
    void Drain() { m_oDispatcher.DrainIfOnHostThread(); }

    size_t PendingCount() const { return m_oDispatcher.PendingCount(); }

private:
    // declared first, so it exists before the override registers it
    ra::services::impl::HostThreadDispatcher m_oDispatcher;
    ServiceLocator::ServiceOverride<ra::services::impl::HostThreadDispatcher> m_Override;
};

} // namespace mocks
} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_MOCK_HOST_THREAD_HH
