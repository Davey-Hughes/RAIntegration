#ifndef RA_SERVICES_QTAPPLICATIONHOST_HH
#define RA_SERVICES_QTAPPLICATIONHOST_HH
#pragma once

#ifndef _WIN32

#include "services/IQtApplicationHost.hh"
#include "services/impl/DisplayProbe.hh"

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

class QObject;

namespace ra {
namespace services {
namespace impl {

class QtApplicationHost : public ra::services::IQtApplicationHost
{
public:
    enum class Mode
    {
        Stopped,
        Owned,
        Borrowed,
        Unavailable,
    };

    struct Options
    {
        // empty: the real ProbeDisplay()
        std::function<DisplayProbeResult()> fProbe;
        // appended to the argv of an owned application, e.g. {"-platform", "offscreen"}
        std::vector<std::string> vArguments;
        std::chrono::milliseconds tStartTimeout{10000};
        std::chrono::milliseconds tStopTimeout{5000};
        std::chrono::milliseconds tBorrowedStopTimeout{1000};
        // false only in the glib guard test's negative control
        bool bUseNonGlibDispatcher = true;
    };

    QtApplicationHost();
    explicit QtApplicationHost(Options oOptions);
    ~QtApplicationHost() noexcept override;

    QtApplicationHost(const QtApplicationHost&) noexcept = delete;
    QtApplicationHost& operator=(const QtApplicationHost&) noexcept = delete;
    QtApplicationHost(QtApplicationHost&&) noexcept = delete;
    QtApplicationHost& operator=(QtApplicationHost&&) noexcept = delete;

    /// <summary>
    /// Borrows the host's GUI application, or creates one on a private thread, or records why neither is
    /// possible. Call on the emulator's thread, before anything uses the host. After <see cref="Stop" /> it may be
    /// called again.
    /// </summary>
    void Start();

    Mode GetMode() const noexcept { return m_nMode.load(); }
    const std::string& GetUnavailableReason() const noexcept { return m_sUnavailableReason; }

    bool IsAvailable() const override;
    bool HasWidgets() const override { return m_bHasWidgets.load(); }
    bool IsOnQtThread() const override;
    void Invoke(std::function<void()> fAction) const override;
    bool InvokeAndWait(std::function<void()> fAction, std::chrono::milliseconds tTimeout) const override;
    void AddStopHook(std::function<void()> fHook) override;
    void Stop() override;

private:
    // Carried by every queued call, and checked as the call runs. Stop() closes it before anything else, so a call
    // queued before Stop() began but not yet run does nothing - whether it runs ahead of the stop hooks or after
    // this object is gone. That covers this object's lifetime, not the library's: the queued event and the functor
    // it holds are this library's code, and in borrowed mode they sit in the host's event queue. So Stop() also
    // deletes m_pContext, which discards the calls still queued on it while the library is loaded (Stop() names
    // the cases that still leave an event behind).
    struct Gate
    {
        std::atomic<bool> bOpen{false};
        // calls turned away once closed, logged by Stop()
        std::atomic<size_t> nDropped{0};
    };

    struct OwnedThreadState; // defined in the .cpp

    enum class WaitOutcome
    {
        Ran,
        Refused,  // not posted: the gate was closed, or there is no application
        TimedOut, // posted, but had not started in time, so it never will
    };

    void StartOwned();
    // Static, and given only the state it shares with the host: a thread Start() or Stop() gave up on and detached
    // may outlive this object, so it must not reach back into it.
    static void RunOwnedThread(std::shared_ptr<OwnedThreadState> pState);
    void MarkUnavailable(std::string sReason);
    bool Post(std::function<void()> fAction, bool bIgnoreGate) const;
    void CountDropped() const;
    WaitOutcome RunAndWait(std::function<void()> fAction, std::chrono::milliseconds tTimeout, bool bIgnoreGate) const;

    Options m_oOptions;
    std::atomic<Mode> m_nMode{Mode::Stopped};
    std::atomic<bool> m_bHasWidgets{false};
    std::string m_sUnavailableReason;

    // Guards m_pGate, m_pApplication and m_pContext against Stop() while another thread is posting.
    mutable std::shared_mutex m_oLifetimeMutex;
    std::shared_ptr<Gate> m_pGate;
    QObject* m_pApplication = nullptr;
    // A plain QObject on the application's thread. Every call but Stop()'s own hook call is queued on it rather than
    // on the application, so that deleting it - which Stop() does on the Qt thread - discards what is still queued.
    QObject* m_pContext = nullptr;

    std::mutex m_oHooksMutex;
    std::vector<std::function<void()>> m_vStopHooks;

    // owned mode only
    std::thread m_oThread;
    std::shared_ptr<OwnedThreadState> m_pOwnedState;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_QTAPPLICATIONHOST_HH
