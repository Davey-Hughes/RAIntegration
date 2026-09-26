#ifndef _WIN32

#include "QtApplicationHost.hh"

#include "services/ILogger.hh"
#include "services/ServiceLocator.hh"
#include "util/Log.hh"

#include <QApplication>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QMetaObject>
#include <QSessionManager>
#include <QThread>
#include <QTimer>
#include <QtGlobal>

#include <dlfcn.h>
#include <pthread.h>

#include <condition_variable>
#include <cstdio>

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
    bool bQuitRequested = false; // set by Stop() as it asks the loop to end: no other quit ends it
    bool bExited = false;        // set as the thread function returns
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

std::atomic<QtMessageHandler> s_fPreviousHandler{nullptr};

// Qt's messages while the library owns the application go to RALog.txt rather
// than the emulator's stderr (the B3 rule). Once no logger is registered - late
// in shutdown - they go wherever they went before.
void ForwardQtMessage(QtMsgType nType, const QMessageLogContext& oContext, const QString& sMessage)
{
    if (!ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        const QtMessageHandler fPrevious = s_fPreviousHandler.load();
        if (fPrevious != nullptr)
            fPrevious(nType, oContext, sMessage);
        else
            std::fprintf(stderr, "%s\n", qFormatLogMessage(nType, oContext, sMessage).toLocal8Bit().constData());
        return;
    }

    switch (nType)
    {
        case QtDebugMsg:
            break;
        case QtInfoMsg:
            RA_LOG_INFO("Qt: %s", sMessage.toStdString().c_str());
            break;
        case QtWarningMsg:
            RA_LOG_WARN("Qt: %s", sMessage.toStdString().c_str());
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            RA_LOG_ERR("Qt: %s", sMessage.toStdString().c_str());
            break;
    }
}

// Qt's D-Bus connection manager starts a thread (named "QDBusConnection") with
// the first GUI application and keeps it until the process exits; no API stops
// it. It runs code in QtDBus and QtCore. If a loader dlclose()s this library
// and nothing else holds those libraries, they could be unmapped under that
// thread - measured 2026-09-25 they stay mapped anyway, but only implicitly.
// RTLD_NODELETE makes it explicit.
void PinQtLibraries()
{
    for (const char* sLibrary : {"libQt6Core.so.6", "libQt6DBus.so.6"})
    {
        if (::dlopen(sLibrary, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) == nullptr)
        {
            const char* sError = ::dlerror();
            RA_LOG_WARN("Could not pin %s: %s", sLibrary, sError ? sError : "not loaded");
        }
    }
}

// Called just before the Qt thread is detached - a start or a stop that timed
// out. That thread is still in this library's code (RunOwnedThread, and the
// functors queued on its loop), and nothing will ever join it, so a loader
// that dlclose()s the library as RA_Shutdown returns would unmap the code it
// is running or will return into. RTLD_NODELETE on our own module keeps it
// mapped for the rest of the process. The cost - a later dlopen of the same
// path gets this copy back, statics and all, instead of a fresh one - falls
// only on a process whose Qt thread has already hung.
void PinOwnLibrary()
{
    Dl_info oInfo{};
    if (::dladdr(reinterpret_cast<void*>(&PinOwnLibrary), &oInfo) == 0 || oInfo.dli_fname == nullptr)
    {
        RA_LOG_WARN("Could not pin the library under its detached Qt thread: dladdr found no module");
        return;
    }

    if (::dlopen(oInfo.dli_fname, RTLD_NOW | RTLD_NOLOAD | RTLD_NODELETE) == nullptr)
    {
        const char* sError = ::dlerror();
        RA_LOG_WARN("Could not pin %s under its detached Qt thread: %s", oInfo.dli_fname,
                    sError ? sError : "not loaded");
    }
}

// RA_LOG from the Qt thread, which can run when no logger is registered - a
// thread detached by a timed-out Stop() outlives RA_Shutdown - where RA_LOG's
// ServiceLocator::Get would throw. The check ForwardQtMessage makes, for the
// library's own messages.
void LogInfoFromQtThread(const char* sMessage)
{
    if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        RA_LOG_INFO("%s", sMessage);
    }
}

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
    // Qt's glib dispatcher uses glib's DEFAULT main context for the thread that
    // owns the application - the context a glib-based emulator iterates on its
    // own main thread. Both threads would then dispatch each other's callbacks
    // (measured: 39 of 39 of a glib host's timer callbacks ran on the Qt
    // thread). QT_NO_GLIB selects Qt's own dispatcher. It is read as the
    // application is constructed, and set only on this path, where no other Qt
    // application exists. setenv is process-global and races a concurrent
    // getenv on another thread; this runs once, during _RA_Init.
    if (m_oOptions.bUseNonGlibDispatcher)
        qputenv("QT_NO_GLIB", "1");

    // Record what Qt reports as the previous handler only if it is not this
    // forwarder itself - which it would be if an earlier owned host installed
    // it and was never stopped - so Stop()'s restore can never reinstall a
    // handler that calls itself and recurses until the stack overflows.
    const QtMessageHandler fInstalled = qInstallMessageHandler(&ForwardQtMessage);
    if (fInstalled != &ForwardQtMessage)
        s_fPreviousHandler = fInstalled;

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
    bool bExited = false;
    {
        std::unique_lock<std::mutex> oLock(pState->oMutex);
        pState->cvChanged.wait_for(oLock, m_oOptions.tStartTimeout,
                                   [&pState]() { return pState->pApplication != nullptr || pState->bExited; });
        pApplication = pState->pApplication;
        pContext = pState->pContext;
        sPlatform = pState->sPlatform;
        bExited = pState->bExited;
    }

    // An application that has already exited is no start either: once the
    // thread sets bExited, pApplication and pContext point at destroyed
    // objects.
    if (pApplication == nullptr || bExited)
    {
        if (bExited)
        {
            // it has returned, or is returning: nothing is left to wait for
            m_oThread.join();
        }
        else
        {
            // Tearing down a half-built application from this thread is not
            // safe; leave the thread to finish (or not) on its own. It holds
            // everything it uses, argv included, in pState.
            PinOwnLibrary();
            m_oThread.detach();
        }
        m_pOwnedState.reset();
        qInstallMessageHandler(s_fPreviousHandler.load());
        MarkUnavailable(bExited ? std::string("the Qt application exited as it started")
                                : "the Qt application did not start within " +
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
    PinQtLibraries();
    RA_LOG_INFO("Started a Qt application on its own thread (platform %s)", sPlatform.c_str());
}

void QtApplicationHost::RunOwnedThread(std::shared_ptr<OwnedThreadState> pState)
{
    // so that it is never mistaken for one of Qt's own threads (see PinQtLibraries)
    pthread_setname_np(pthread_self(), "RA-Qt");

    {
        QApplication oApplication(pState->nArgc, pState->vArgv.data());

        // Only Stop() may end the library's event loop; the loop below
        // re-enters it after any other quit. These two keep Qt from asking in
        // the first place: closing the last view window, and the last
        // QEventLoopLocker going. Both are process-global and never restored,
        // so an application the emulator creates after RA_Shutdown inherits
        // them (a disclosed divergence).
        QGuiApplication::setQuitOnLastWindowClosed(false);
        QCoreApplication::setQuitLockEnabled(false);

        // At an X11 session's logout the session manager asks each client to
        // save its state, and unless told otherwise Qt registers "RAIntegration
        // -session <id>" as the command that brings this client back at the
        // next login. The client is the emulator's process, under our argv[0].
        // Whether the emulator comes back is not the library's call.
        QObject::connect(&oApplication, &QGuiApplication::saveStateRequest, &oApplication,
                         [](QSessionManager& oManager) { oManager.setRestartHint(QSessionManager::RestartNever); });

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
        //
        // A quit Stop() did not ask for - the X11 session manager's "die" at
        // logout, or a quit() or QEvent::Quit from anywhere in the process -
        // enters the loop again. Leaving it would destroy the application and
        // the context under a host that still holds both: the next Post,
        // IsOnQtThread or Stop() would use freed objects.
        bool bReportedUnrequestedQuit = false;
        for (;;)
        {
            oApplication.exec();

            {
                std::lock_guard<std::mutex> oLock(pState->oMutex);
                if (pState->bQuitRequested)
                    break;
            }

            if (!bReportedUnrequestedQuit)
            {
                bReportedUnrequestedQuit = true;
                LogInfoFromQtThread("The Qt event loop was told to quit by something other than the library; "
                                    "running it again");
            }
        }
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

        // Only a quit with bQuitRequested set ends the loop (see
        // RunOwnedThread). The quit is queued under the lock the thread takes
        // to read the flag: otherwise a quit nobody asked for, ending exec()
        // between the two, would let the thread see the flag and destroy the
        // application before invokeMethod reached it. Queued first, the event
        // is at worst discarded with the application.
        {
            std::lock_guard<std::mutex> oLock(pState->oMutex);
            pState->bQuitRequested = true;
            QMetaObject::invokeMethod(pApplication, []() { QCoreApplication::quit(); }, Qt::QueuedConnection);
        }

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
            PinOwnLibrary();
            m_oThread.detach();
        }

        qInstallMessageHandler(s_fPreviousHandler.load());
    }

    m_bHasWidgets = false;
    m_nMode = Mode::Stopped;
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
