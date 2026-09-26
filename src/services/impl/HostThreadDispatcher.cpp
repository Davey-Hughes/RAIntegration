#include "HostThreadDispatcher.hh"

#include "services/ServiceLocator.hh"

#include "util/Log.hh"

namespace ra {
namespace services {
namespace impl {

HostThreadDispatcher::HostThreadDispatcher() noexcept : m_nHostThread(std::this_thread::get_id()) {}

void HostThreadDispatcher::Invoke(std::function<void()> fAction)
{
    // On the emulator's thread there is nothing to move - during shutdown too,
    // as with the Windows desktop's InvokeOnUIThread.
    if (IsOnHostThread())
    {
        fAction();
        return;
    }

    PostFunction fpPost = nullptr;
    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        if (m_bShutdown)
            return;

        m_vPending.push_back(std::move(fAction));
        fpPost = m_fpPost;
    }

    if (fpPost != nullptr)
        fpPost(&HostThreadDispatcher::RunPosted, nullptr);
}

void HostThreadDispatcher::SetPostFunction(PostFunction fpPost)
{
    bool bFlush = false;
    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        m_fpPost = fpPost;
        bFlush = (fpPost != nullptr && !m_vPending.empty());
    }

    if (bFlush)
        fpPost(&HostThreadDispatcher::RunPosted, nullptr);
}

void HostThreadDispatcher::DrainIfOnHostThread()
{
    if (!IsOnHostThread())
        return;

    std::deque<std::function<void()>> vWork;
    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        vWork.swap(m_vPending);
    }

    // outside the lock: a callback may itself ask for more host-thread work
    for (auto& fAction : vWork)
        fAction();
}

void HostThreadDispatcher::Shutdown()
{
    size_t nDiscarded = 0;
    {
        std::lock_guard<std::mutex> oLock(m_oMutex);
        m_bShutdown = true;
        nDiscarded = m_vPending.size();
        m_vPending.clear();
    }

    if (nDiscarded > 0)
        RA_LOG_INFO("Discarded %zu host-thread call(s) at shutdown", nDiscarded);
}

size_t HostThreadDispatcher::PendingCount() const
{
    std::lock_guard<std::mutex> oLock(m_oMutex);
    return m_vPending.size();
}

void HostThreadDispatcher::RunPosted(void*)
{
    if (ra::services::ServiceLocator::Exists<HostThreadDispatcher>())
        ra::services::ServiceLocator::GetMutable<HostThreadDispatcher>().DrainIfOnHostThread();
}

} // namespace impl
} // namespace services
} // namespace ra
