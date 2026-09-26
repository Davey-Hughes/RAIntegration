#ifndef RA_SERVICES_HOSTTHREADDISPATCHER_HH
#define RA_SERVICES_HOSTTHREADDISPATCHER_HH
#pragma once

#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace ra {
namespace services {
namespace impl {

/// <summary>
/// Runs work on the emulator's own thread - the one that initialized the toolkit - when the request comes from any
/// other thread: a worker, or the Qt thread. The emulator's callbacks (pause, unpause, reset, rebuild menu) must run
/// there.
/// </summary>
/// <remarks>
/// On the emulator's thread the work runs inline. From anywhere else it is queued, and the emulator is asked to
/// drain the queue on its own thread through the post function it installed with RA_InstallHostDispatcher. Without
/// one, the queue waits for the next _RA_DoAchievementsFrame on the emulator's thread.
/// </remarks>
class HostThreadDispatcher
{
public:
    using WorkFunction = void (*)(void* pContext);
    using PostFunction = void (*)(WorkFunction fpWork, void* pContext);

    /// <summary>Records the calling thread as the emulator's thread.</summary>
    HostThreadDispatcher() noexcept;

    bool IsOnHostThread() const noexcept { return std::this_thread::get_id() == m_nHostThread; }

    void Invoke(std::function<void()> fAction);

    /// <summary>Installs (or, with <c>nullptr</c>, removes) the emulator's post function. Queued work is posted at once.</summary>
    void SetPostFunction(PostFunction fpPost);

    /// <summary>Runs everything queued - on the emulator's thread only; anywhere else it does nothing.</summary>
    void DrainIfOnHostThread();

    /// <summary>Discards queued work, and from now on refuses work from other threads.</summary>
    void Shutdown();

    size_t PendingCount() const;

    /// <summary>
    /// The work function handed to the emulator's post function. It drains whichever dispatcher is registered when
    /// it runs, so a post that outlives the dispatcher it came from does no harm.
    /// </summary>
    static void RunPosted(void* pContext);

private:
    const std::thread::id m_nHostThread;
    mutable std::mutex m_oMutex;
    std::deque<std::function<void()>> m_vPending;
    PostFunction m_fpPost = nullptr;
    bool m_bShutdown = false;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_HOSTTHREADDISPATCHER_HH
