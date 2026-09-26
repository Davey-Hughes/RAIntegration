#ifndef _WIN32

#include "QtApplicationHost.hh"

#include "util/Log.hh"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <condition_variable>

namespace ra {
namespace services {
namespace impl {

struct QtApplicationHost::OwnedThreadState
{
    // QApplication keeps a reference to argc and the argv pointer for its
    // whole life, and the thread can outlive the host (a timed-out start or
    // stop detaches it), so they live here, with the thread, and are written
    // only before it starts.
    std::vector<std::string> vArgumentStorage;
    std::vector<char*> vArgv;
    int nArgc = 0;

    std::mutex oMutex;
    std::condition_variable cvChanged;
    QObject* pApplication = nullptr; // set once exec() is running
    QObject* pContext = nullptr;     // set with pApplication
    std::string sPlatform;
    bool bExited = false; // set as the thread function returns
};

namespace {

struct WaitedCall
{
    enum class State
    {
        Pending,
        Running,
        Done,
        Cancelled,
    };

    std::mutex oMutex;
    std::condition_variable cvDone;
    State nState = State::Pending;
};

} // namespace

QtApplicationHost::QtApplicationHost() : QtApplicationHost(Options{}) {}

QtApplicationHost::QtApplicationHost(Options oOptions)
    : m_oOptions(std::move(oOptions)), m_pGate(std::make_shared<Gate>())
{
    if (!m_oOptions.fProbe)
        m_oOptions.fProbe = []() { return ProbeDisplay(); };
}

QtApplicationHost::~QtApplicationHost() noexcept
{
    // Initialization::Shutdown stops the host. This covers a host replaced or
    // destroyed without that - a second _RA_Init, or a test.
    const Mode nMode = m_nMode.load();
    if (nMode == Mode::Owned || nMode == Mode::Borrowed)
        Stop();
    else if (m_oThread.joinable())
        m_oThread.detach(); // a thread that never reached exec() (see StartOwned)
}

void QtApplicationHost::Start()
{
    if (m_nMode.load() != Mode::Stopped)
        return;

    auto* pExisting = QCoreApplication::instance();
    if (pExisting != nullptr)
    {
        if (qobject_cast<QGuiApplication*>(pExisting) == nullptr)
        {
            MarkUnavailable("the host's Qt application is not a GUI application");
            return;
        }

        m_bHasWidgets = (qobject_cast<QApplication*>(pExisting) != nullptr);

        // Work is queued on a context of our own, not on the host's
        // application, so that Stop() can take what is still queued out of the
        // host's event queue (see m_pContext). It must live on the
        // application's thread, and it is created here, on this one - which
        // may not be it.
        auto* pContext = new QObject();
        if (pContext->thread() != pExisting->thread())
            pContext->moveToThread(pExisting->thread());

        {
            std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
            m_pApplication = pExisting;
            m_pContext = pContext;
            m_pGate = std::make_shared<Gate>();
            m_pGate->bOpen = true;
        }
        m_nMode = Mode::Borrowed;
        RA_LOG_INFO("Using the host's Qt application%s",
                    m_bHasWidgets.load() ? "" : " (no widget support: views are unavailable)");
        return;
    }

    const auto oProbe = m_oOptions.fProbe();
    if (!oProbe.bUsable)
    {
        MarkUnavailable("no usable display: " + oProbe.sDetail);
        return;
    }

    RA_LOG_INFO("Display: %s", oProbe.sDetail.c_str());
    StartOwned();
}

void QtApplicationHost::MarkUnavailable(std::string sReason)
{
    m_sUnavailableReason = std::move(sReason);
    m_nMode = Mode::Unavailable;
    RA_LOG_WARN("Qt services unavailable: %s", m_sUnavailableReason.c_str());
}

void QtApplicationHost::StartOwned()
{
    auto pState = std::make_shared<OwnedThreadState>();
    pState->vArgumentStorage.emplace_back("RAIntegration");
    for (const auto& sArgument : m_oOptions.vArguments)
        pState->vArgumentStorage.push_back(sArgument);

    for (auto& sArgument : pState->vArgumentStorage)
        pState->vArgv.push_back(sArgument.data());
    pState->vArgv.push_back(nullptr);
    pState->nArgc = static_cast<int>(pState->vArgumentStorage.size());

    m_pOwnedState = pState;
    m_oThread = std::thread([pState]() { RunOwnedThread(pState); });

    QObject* pApplication = nullptr;
    QObject* pContext = nullptr;
    std::string sPlatform;
    {
        std::unique_lock<std::mutex> oLock(pState->oMutex);
        pState->cvChanged.wait_for(oLock, m_oOptions.tStartTimeout,
                                   [&pState]() { return pState->pApplication != nullptr || pState->bExited; });
        pApplication = pState->pApplication;
        pContext = pState->pContext;
        sPlatform = pState->sPlatform;
    }

    if (pApplication == nullptr)
    {
        // Tearing down a half-built application from this thread is not safe;
        // leave the thread to finish (or not) on its own. It holds everything
        // it uses, argv included, in pState.
        m_oThread.detach();
        m_pOwnedState.reset();
        MarkUnavailable("the Qt application did not start within " +
                        std::to_string(m_oOptions.tStartTimeout.count()) + " ms");
        return;
    }

    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        m_pApplication = pApplication;
        m_pContext = pContext;
        m_pGate = std::make_shared<Gate>();
        m_pGate->bOpen = true;
    }
    m_bHasWidgets = true;
    m_nMode = Mode::Owned;
    RA_LOG_INFO("Started a Qt application on its own thread (platform %s)", sPlatform.c_str());
}

void QtApplicationHost::RunOwnedThread(std::shared_ptr<OwnedThreadState> pState)
{
    {
        QApplication oApplication(pState->nArgc, pState->vArgv.data());

        // Otherwise closing the last view window would end the library's event
        // loop behind its back: only Stop() may end it.
        QGuiApplication::setQuitOnLastWindowClosed(false);

        // Work is queued on this, not on the application (see m_pContext).
        // Stop()'s hook call deletes it; as the application's child it still
        // goes with the application if that call never ran.
        auto* pContext = new QObject(&oApplication);

        QTimer::singleShot(0, &oApplication, [pState, pContext, &oApplication]() {
            {
                std::lock_guard<std::mutex> oLock(pState->oMutex);
                pState->sPlatform = QGuiApplication::platformName().toStdString();
                pState->pApplication = &oApplication;
                pState->pContext = pContext;
            }
            pState->cvChanged.notify_all();
        });

        // exec() delivers the pending DeferredDelete events itself before it
        // returns: its inlined QCoreApplicationPrivate::execCleanup() calls
        // sendPostedEvents(nullptr, QEvent::DeferredDelete) at loop level 0.
        // (On Qt 6.11.2 the deleting frame is QCoreApplication::exec()+0xff,
        // with no QEventLoop::exec below it.) So an object a stop hook - or
        // QtAudioSystem's reaping - handed to deleteLater() is deleted there,
        // while the application still exists. Nothing later would do it: the
        // application's destructor does not deliver a DeferredDelete still
        // pending, and neither does this thread's exit.
        oApplication.exec();
    }

    {
        std::lock_guard<std::mutex> oLock(pState->oMutex);
        pState->bExited = true;
    }
    pState->cvChanged.notify_all();
}

bool QtApplicationHost::IsAvailable() const
{
    std::shared_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
    return m_pGate->bOpen.load();
}

bool QtApplicationHost::IsOnQtThread() const
{
    std::shared_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
    return m_pApplication != nullptr && QThread::currentThread() == m_pApplication->thread();
}

bool QtApplicationHost::Post(std::function<void()> fAction, bool bIgnoreGate) const
{
    std::shared_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
    if (!bIgnoreGate && !m_pGate->bOpen.load())
        return false;

    // Only Stop()'s own hook call (bIgnoreGate) goes to the application: that
    // call deletes the context. Everything else goes to the context, so that
    // deleting it discards what is still queued.
    QObject* pTarget = bIgnoreGate ? m_pApplication : m_pContext;
    if (pTarget == nullptr)
        return false;

    auto pGate = m_pGate;
    QMetaObject::invokeMethod(
        pTarget,
        [pGate, bIgnoreGate, fAction = std::move(fAction)]() {
            // checked again as it runs: work queued before Stop() began, but not
            // yet run, must not run after it
            if (bIgnoreGate || pGate->bOpen.load())
                fAction();
        },
        Qt::QueuedConnection);
    return true;
}

void QtApplicationHost::Invoke(std::function<void()> fAction) const
{
    if (!IsAvailable())
        return;

    if (IsOnQtThread())
    {
        fAction();
        return;
    }

    Post(std::move(fAction), false);
}

bool QtApplicationHost::InvokeAndWait(std::function<void()> fAction, std::chrono::milliseconds tTimeout) const
{
    if (!IsAvailable())
        return false;

    return RunAndWait(std::move(fAction), tTimeout, false);
}

bool QtApplicationHost::RunAndWait(std::function<void()> fAction, std::chrono::milliseconds tTimeout,
                                   bool bIgnoreGate) const
{
    if (IsOnQtThread())
    {
        fAction();
        return true;
    }

    auto pCall = std::make_shared<WaitedCall>();
    const bool bPosted = Post(
        [pCall, fAction = std::move(fAction)]() {
            {
                std::lock_guard<std::mutex> oLock(pCall->oMutex);
                if (pCall->nState == WaitedCall::State::Cancelled)
                    return;
                pCall->nState = WaitedCall::State::Running;
            }

            fAction();

            {
                std::lock_guard<std::mutex> oLock(pCall->oMutex);
                pCall->nState = WaitedCall::State::Done;
            }
            pCall->cvDone.notify_all();
        },
        bIgnoreGate);
    if (!bPosted)
        return false;

    std::unique_lock<std::mutex> oLock(pCall->oMutex);
    if (pCall->cvDone.wait_for(oLock, tTimeout, [&pCall]() { return pCall->nState == WaitedCall::State::Done; }))
        return true;

    if (pCall->nState == WaitedCall::State::Pending)
    {
        // never started: make sure it never will, since fAction may point into
        // the caller's stack
        pCall->nState = WaitedCall::State::Cancelled;
        return false;
    }

    // already running: it cannot be stopped, and it must not outlive the
    // caller's locals, so let it finish
    pCall->cvDone.wait(oLock, [&pCall]() { return pCall->nState == WaitedCall::State::Done; });
    return true;
}

void QtApplicationHost::AddStopHook(std::function<void()> fHook)
{
    std::lock_guard<std::mutex> oLock(m_oHooksMutex);
    m_vStopHooks.push_back(std::move(fHook));
}

void QtApplicationHost::Stop()
{
    const Mode nMode = m_nMode.load();
    if (nMode != Mode::Owned && nMode != Mode::Borrowed)
    {
        std::lock_guard<std::mutex> oLock(m_oHooksMutex);
        m_vStopHooks.clear();
        m_nMode = Mode::Stopped;
        return;
    }

    // 1. refuse new work, and make queued-but-unrun work skip itself. The
    //    context leaves the members here too, since step 2 deletes it and
    //    nothing may be queued on it from now on.
    QObject* pContext = nullptr;
    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        m_pGate->bOpen = false;
        pContext = m_pContext;
        m_pContext = nullptr;
    }

    // 2. the stop hooks, on the Qt thread, while the application still exists;
    //    then the context, on the same thread.
    //
    //    This call goes to the application (bIgnoreGate), not to the context,
    //    so the context is not deleted inside its own event delivery. Deleting
    //    it discards the gate-skipped calls still queued on it, while this
    //    library is still loaded. On the Qt thread - the usual borrowed case -
    //    all of it runs inline and nothing is left queued.
    //
    //    One case still leaves this library's code in the host's queue:
    //    borrowed, with the application on a thread other than the one calling
    //    Stop(). The hook call is then itself a queued event, and the host's
    //    loop destroys that event - running this library's code - just after
    //    the call has woken this thread, so unloading the library as soon as
    //    Stop() returns can race it. The same holds, context included, for a
    //    hook call that timed out before it started.
    std::vector<std::function<void()>> vHooks;
    {
        std::lock_guard<std::mutex> oLock(m_oHooksMutex);
        vHooks.swap(m_vStopHooks);
    }

    const auto tHookTimeout = (nMode == Mode::Owned) ? m_oOptions.tStopTimeout : m_oOptions.tBorrowedStopTimeout;
    if (!RunAndWait(
            [&vHooks, pContext]() {
                for (auto& fHook : vHooks)
                    fHook();

                delete pContext;
            },
            tHookTimeout, true))
    {
        // RunAndWait only gives up on a call that has not started. Once the
        // hooks start they are waited for without limit - they use vHooks,
        // on this stack - so a hung hook hangs Stop().
        RA_LOG_WARN("Qt stop hooks skipped: the Qt thread did not start them within %d ms",
                    static_cast<int>(tHookTimeout.count()));
    }

    // 3. the application leaves the members before an owned one can be
    //    destroyed: from here Post() refuses and IsOnQtThread() is false, so
    //    neither can reach it once its thread has ended
    QObject* pApplication = nullptr;
    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        pApplication = m_pApplication;
        m_pApplication = nullptr;
    }

    // 4. an owned application: quit it, and join its thread
    if (nMode == Mode::Owned)
    {
        auto pState = std::move(m_pOwnedState);
        QMetaObject::invokeMethod(pApplication, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);

        bool bExited = false;
        {
            std::unique_lock<std::mutex> oLock(pState->oMutex);
            bExited =
                pState->cvChanged.wait_for(oLock, m_oOptions.tStopTimeout, [&pState]() { return pState->bExited; });
        }

        if (bExited)
        {
            m_oThread.join();
        }
        else
        {
            // it holds everything it uses, argv included, in pState
            RA_LOG_WARN("Qt thread did not exit within %d ms; detaching it",
                        static_cast<int>(m_oOptions.tStopTimeout.count()));
            m_oThread.detach();
        }
    }

    m_bHasWidgets = false;
    m_nMode = Mode::Stopped;
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
