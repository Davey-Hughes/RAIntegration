#ifndef _WIN32

#include "QtApplicationHost.hh"

#include "util/Log.hh"

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
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
    std::mutex oMutex;
    std::condition_variable cvChanged;
    QObject* pApplication = nullptr; // set once exec() is running
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
        {
            std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
            m_pApplication = pExisting;
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
    m_vArgumentStorage.clear();
    m_vArgumentStorage.emplace_back("RAIntegration");
    for (const auto& sArgument : m_oOptions.vArguments)
        m_vArgumentStorage.push_back(sArgument);

    m_vArgv.clear();
    for (auto& sArgument : m_vArgumentStorage)
        m_vArgv.push_back(sArgument.data());
    m_vArgv.push_back(nullptr);
    m_nArgc = static_cast<int>(m_vArgumentStorage.size());

    auto pState = std::make_shared<OwnedThreadState>();
    m_pOwnedState = pState;
    m_oThread = std::thread([this, pState]() { RunOwnedThread(pState); });

    QObject* pApplication = nullptr;
    std::string sPlatform;
    {
        std::unique_lock<std::mutex> oLock(pState->oMutex);
        pState->cvChanged.wait_for(oLock, m_oOptions.tStartTimeout,
                                   [&pState]() { return pState->pApplication != nullptr || pState->bExited; });
        pApplication = pState->pApplication;
        sPlatform = pState->sPlatform;
    }

    if (pApplication == nullptr)
    {
        // Tearing down a half-built application from this thread is not safe;
        // leave the thread to finish (or not) on its own.
        m_oThread.detach();
        m_pOwnedState.reset();
        MarkUnavailable("the Qt application did not start within " +
                        std::to_string(m_oOptions.tStartTimeout.count()) + " ms");
        return;
    }

    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        m_pApplication = pApplication;
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
        QApplication oApplication(m_nArgc, m_vArgv.data());
        QTimer::singleShot(0, &oApplication, [pState, &oApplication]() {
            {
                std::lock_guard<std::mutex> oLock(pState->oMutex);
                pState->sPlatform = QGuiApplication::platformName().toStdString();
                pState->pApplication = &oApplication;
            }
            pState->cvChanged.notify_all();
        });

        oApplication.exec();

        // Deferred deletes still pending - an effect that QtAudioSystem's
        // reaping handed to deleteLater(), or anything a stop hook scheduled -
        // are only guaranteed to run at event-loop level 0, which is here,
        // after exec() has returned. The application's destructor does not
        // run them.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
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
    if (m_pApplication == nullptr || (!bIgnoreGate && !m_pGate->bOpen.load()))
        return false;

    auto pGate = m_pGate;
    QMetaObject::invokeMethod(
        m_pApplication,
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

    // 1. refuse new work, and make queued-but-unrun work skip itself
    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        m_pGate->bOpen = false;
    }

    // 2. the stop hooks, on the Qt thread, while the application still exists
    std::vector<std::function<void()>> vHooks;
    {
        std::lock_guard<std::mutex> oLock(m_oHooksMutex);
        vHooks.swap(m_vStopHooks);
    }

    const auto tHookTimeout = (nMode == Mode::Owned) ? m_oOptions.tStopTimeout : m_oOptions.tBorrowedStopTimeout;
    if (!RunAndWait(
            [&vHooks]() {
                for (auto& fHook : vHooks)
                    fHook();
            },
            tHookTimeout, true))
    {
        RA_LOG_WARN("Qt stop hooks did not finish within %d ms", static_cast<int>(tHookTimeout.count()));
    }

    // 3. an owned application: quit it, and join its thread
    if (nMode == Mode::Owned)
    {
        QMetaObject::invokeMethod(m_pApplication, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);

        bool bExited = false;
        {
            std::unique_lock<std::mutex> oLock(m_pOwnedState->oMutex);
            bExited = m_pOwnedState->cvChanged.wait_for(oLock, m_oOptions.tStopTimeout,
                                                        [this]() { return m_pOwnedState->bExited; });
        }

        if (bExited)
        {
            m_oThread.join();
        }
        else
        {
            RA_LOG_WARN("Qt thread did not exit within %d ms; detaching it",
                        static_cast<int>(m_oOptions.tStopTimeout.count()));
            m_oThread.detach();
        }

        m_pOwnedState.reset();
    }

    {
        std::unique_lock<std::shared_mutex> oLock(m_oLifetimeMutex);
        m_pApplication = nullptr;
    }
    m_bHasWidgets = false;
    m_nMode = Mode::Stopped;
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
