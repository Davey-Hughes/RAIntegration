#ifndef RA_TESTUTIL_DETACHED_CALL_HH
#define RA_TESTUTIL_DETACHED_CALL_HH
#pragma once

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace ra {
namespace tests {

/// <summary>
/// Runs a call on its own detached thread, so a test can time it out and fail rather than hang on a deadlock.
/// </summary>
/// <remarks>
/// A timed-out call keeps running, so everything it uses must outlive it: the tests heap-allocate that state and
/// delete it only after every DetachedCall reported it finished, leaking it on purpose when one did not.
/// </remarks>
class DetachedCall
{
public:
    template<typename TAction>
    explicit DetachedCall(TAction fAction)
    {
        auto pDone = std::make_shared<std::promise<void>>();
        m_fDone = pDone->get_future();
        std::thread([pDone, fAction]() mutable {
            fAction();
            pDone->set_value();
        }).detach();
    }

    /// <summary>Waits up to <paramref name="tTimeout" /> for the call to return. May be called again.</summary>
    bool FinishedWithin(std::chrono::milliseconds tTimeout)
    {
        return m_fDone.wait_for(tTimeout) == std::future_status::ready;
    }

private:
    std::future<void> m_fDone;
};

} // namespace tests
} // namespace ra

#endif // !RA_TESTUTIL_DETACHED_CALL_HH
