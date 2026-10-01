#include "ui/qt/QtDesktop.hh"

#include "services/ILogger.hh"
#include "services/IQtApplicationHost.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/HostThreadDispatcher.hh"

#include "ui/drawing/null/NullSurface.hh"
#include "ui/drawing/qt/QtSurface.hh"
#include "ui/drawing/qt/ScreenCapture.hh"
#include "ui/qt/bindings/WindowBinding.hh"
#include "ui/qt/FileDialog.hh"
#include "ui/qt/LoginDialog.hh"
#include "ui/qt/MessageBoxDialog.hh"
#include "ui/qt/OverlaySettingsDialog.hh"
#include "ui/qt/RichPresenceDialog.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QDesktopServices>
#include <QDialog>
#include <QMessageBox>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace ra {
namespace ui {
namespace qt {

namespace {

// A caller on another thread, waiting for its dialog to finish.
struct ModalWait
{
    std::mutex oMutex;
    std::condition_variable cvDone;
    bool bStarted = false; // the Qt thread has begun opening the dialog
    bool bGaveUp = false;  // the caller has already been answered: open nothing
    bool bDone = false;
    bool bRefused = false;

    void SetDone()
    {
        {
            std::lock_guard<std::mutex> oLock(oMutex);
            bDone = true;
        }
        cvDone.notify_all();
    }
};

const char* DialogResultName(ra::ui::DialogResult nResult) noexcept
{
    switch (nResult)
    {
        case ra::ui::DialogResult::OK:
            return "OK";
        case ra::ui::DialogResult::Cancel:
            return "Cancel";
        case ra::ui::DialogResult::Yes:
            return "Yes";
        case ra::ui::DialogResult::No:
            return "No";
        case ra::ui::DialogResult::Retry:
            return "Retry";
        default:
            return "None";
    }
}

// RA_AUTO_ANSWER_DIALOGS set to anything but empty or "0": the headless runs' hook (see QtDesktop.hh). Read on every
// call, so a test can turn it on and off.
bool AutoAnswerRequested()
{
    const char* sValue = std::getenv("RA_AUTO_ANSWER_DIALOGS");
    return sValue != nullptr && sValue[0] != '\0' && std::strcmp(sValue, "0") != 0;
}

} // namespace

QtDesktop::QtDesktop() : m_pState(std::make_shared<State>())
{
    // most common first, as Win32's Desktop orders them
    m_pState->vPresenters.push_back(std::make_unique<MessageBoxDialog::Presenter>());
    m_pState->vPresenters.push_back(std::make_unique<RichPresenceDialog::Presenter>());
    m_pState->vPresenters.push_back(std::make_unique<FileDialog::Presenter>());
    m_pState->vPresenters.push_back(std::make_unique<LoginDialog::Presenter>());
    m_pState->vPresenters.push_back(std::make_unique<OverlaySettingsDialog::Presenter>());

    // A second _RA_Init restarts the Qt host in place, without Shutdown(): the
    // host's Stop() runs this before its application goes, and before
    // RegisterServices replaces this desktop and the view models behind the
    // windows.
    auto* pHost = GetHost();
    if (pHost != nullptr)
    {
        std::weak_ptr<State> pWeakState = m_pState;
        pHost->AddStopHook([pWeakState]() {
            const auto pState = pWeakState.lock();
            if (pState != nullptr)
                CloseAll(*pState);
        });
    }
}

ra::services::IQtApplicationHost* QtDesktop::GetHost()
{
    return ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>()
               ? &ra::services::ServiceLocator::GetMutable<ra::services::IQtApplicationHost>()
               : nullptr;
}

bool QtDesktop::CanHostWidgets(const ra::services::IQtApplicationHost* pHost)
{
    return pHost != nullptr && pHost->IsAvailable() && pHost->HasWidgets();
}

ra::ui::DialogResult QtDesktop::RefusalAnswer(const WindowViewModelBase& vmWindow)
{
    const auto* vmMessageBox = dynamic_cast<const ra::ui::viewmodels::MessageBoxViewModel*>(&vmWindow);
    if (vmMessageBox != nullptr)
        return MessageBoxDialog::GetEscapeAnswer(vmMessageBox->GetButtons());

    return ra::ui::DialogResult::No;
}

IDialogPresenter* QtDesktop::FindPresenter(const WindowViewModelBase& vmWindow) const
{
    for (const auto& pPresenter : m_pState->vPresenters)
    {
        if (pPresenter->IsSupported(vmWindow))
            return pPresenter.get();
    }

    return nullptr;
}

void QtDesktop::AddPresenter(std::unique_ptr<IDialogPresenter> pPresenter)
{
    m_pState->vPresenters.push_back(std::move(pPresenter));
}

void QtDesktop::ShowWindow(WindowViewModelBase& vmWindow) const
{
    auto* pHost = GetHost();
    auto* pPresenter = FindPresenter(vmWindow);
    if (pPresenter == nullptr || !CanHostWidgets(pHost))
    {
        ++m_pState->nNoViewLayer;
        RA_LOG_WARN("No view layer to show window \"%s\"",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
        if (pPresenter == nullptr && CanHostWidgets(pHost))
            NoticeNotAvailable(*pHost, vmWindow.GetWindowTitle());
        return;
    }

    if (m_pState->bClosed.load())
    {
        ++m_pState->nRefusedAfterShutdown;
        return;
    }

    // The view model outlives the queued call, as with Win32's queued
    // ShowWindow: every caller's is a WindowManager member.
    pHost->Invoke([pState = m_pState, pPresenter, &vmWindow]() {
        // pState also keeps the presenter alive. CloseAll may have run since
        // the check above, and a window opened now would outlive its view model.
        if (pState->bClosed.load())
        {
            ++pState->nRefusedAfterShutdown;
            return;
        }

        pPresenter->ShowWindow(vmWindow);
    });
}

bool QtDesktop::CanShowWindow(const WindowViewModelBase& vmWindow) const
{
    // false exactly when ShowWindow and ShowModal answer with the "not available" notice
    return FindPresenter(vmWindow) != nullptr || !CanHostWidgets(GetHost());
}

ra::ui::DialogResult QtDesktop::ShowModal(WindowViewModelBase& vmWindow) const
{
    return DoShowModal(vmWindow);
}

ra::ui::DialogResult QtDesktop::ShowModal(WindowViewModelBase& vmWindow, const WindowViewModelBase&) const
{
    // No parenting yet: a parented QDialog is owned by its parent, and shutdown
    // deletes non-modal windows, which would delete a modal child under its own
    // exec(). Every modal is an application-modal top-level window.
    return DoShowModal(vmWindow);
}

ra::ui::DialogResult QtDesktop::DoShowModal(WindowViewModelBase& vmWindow) const
{
    auto* pHost = GetHost();
    auto* pPresenter = FindPresenter(vmWindow);
    if (pPresenter == nullptr || !CanHostWidgets(pHost))
    {
        // "No view layer to show dialog" is the line ra_linux_smoke looks for
        ++m_pState->nNoViewLayer;
        const auto nAnswer = RefusalAnswer(vmWindow);
        RA_LOG_WARN("No view layer to show dialog \"%s\" - returning %s",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str(), DialogResultName(nAnswer));
        if (pPresenter == nullptr && CanHostWidgets(pHost))
            NoticeNotAvailable(*pHost, vmWindow.GetWindowTitle());
        return nAnswer;
    }

    if (m_pState->bClosed.load())
    {
        ++m_pState->nRefusedAfterShutdown;
        const auto nAnswer = RefusalAnswer(vmWindow);
        RA_LOG_WARN("Dialog \"%s\" not shown: the windows are closed for shutdown - returning %s",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str(), DialogResultName(nAnswer));
        return nAnswer;
    }

    if (AutoAnswerRequested())
    {
        // shown to nobody: answered as a box closed unanswered is
        ++m_pState->nAutoAnswered;
        const auto nAnswer = RefusalAnswer(vmWindow);
        RA_LOG_WARN("Dialog \"%s\" auto-answered %s (RA_AUTO_ANSWER_DIALOGS)",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str(), DialogResultName(nAnswer));
        return nAnswer;
    }

    if (pHost->IsOnQtThread())
        return ShowModalOnQtThread(*pPresenter, vmWindow);

    return ShowModalFromOtherThread(*pHost, *pPresenter, vmWindow);
}

ra::ui::DialogResult QtDesktop::ShowModalOnQtThread(IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const
{
    auto pState = m_pState;
    if (pState->bClosed.load())
    {
        ++pState->nRefusedAfterShutdown;
        return RefusalAnswer(vmWindow);
    }

    // A caller on the Qt thread can only be answered from a nested event loop.
    auto pDialog = oPresenter.CreateModal(vmWindow);
    if (pDialog == nullptr)
        return vmWindow.GetDialogResult();

    pDialog->setWindowModality(Qt::ApplicationModal);
    pState->vOpenModals.emplace_back(pDialog.get());
    pDialog->exec();
    ForgetModal(*pState, pDialog.get());

    return vmWindow.GetDialogResult();
}

ra::ui::DialogResult QtDesktop::ShowModalFromOtherThread(ra::services::IQtApplicationHost& oHost,
                                                         IDialogPresenter& oPresenter,
                                                         WindowViewModelBase& vmWindow) const
{
    // Shown, not exec()'d: this thread waits, and the Qt thread goes back to
    // its own loop. Each caller is released as soon as its own dialog is
    // answered - the newest application-modal dialog still has to be answered
    // first, but nested exec() loops would also hold the first caller until
    // the second dialog closed.
    auto pWait = std::make_shared<ModalWait>();
    auto pState = m_pState;
    auto fOpen = [pState, pWait, &oPresenter, &vmWindow]() {
        {
            std::lock_guard<std::mutex> oLock(pWait->oMutex);
            if (pWait->bGaveUp)
                return; // the caller was answered while this waited in the queue
            pWait->bStarted = true;
        }
        pWait->cvDone.notify_all();

        if (pState->bClosed.load())
        {
            // CloseAll ran between the caller's check and this call
            ++pState->nRefusedAfterShutdown;
            {
                std::lock_guard<std::mutex> oLock(pWait->oMutex);
                pWait->bRefused = true;
            }
            pWait->SetDone();
            return;
        }

        auto pDialog = oPresenter.CreateModal(vmWindow);
        if (pDialog == nullptr)
        {
            pWait->SetDone();
            return;
        }

        QDialog* pOpened = pDialog.release();
        pState->vOpenModals.emplace_back(pOpened);
        QObject::connect(pOpened, &QDialog::finished, pOpened, [pState, pWait, pOpened]() {
            // the dialog has written the view model's DialogResult, and
            // does not touch it again
            ForgetModal(*pState, pOpened);
            pOpened->deleteLater();
            pWait->SetDone();
        });

        // Destroyed without finishing - deleted directly - it wrote no
        // DialogResult: release the caller anyway, with the refusal answer
        // rather than the view model's None. (Not a cure for skipped stop
        // hooks: destroying the application does not delete an open dialog,
        // so nothing is destroyed and its caller still waits.)
        QObject::connect(pOpened, &QObject::destroyed, [pWait]() {
            {
                std::lock_guard<std::mutex> oLock(pWait->oMutex);
                if (pWait->bDone)
                    return;
                pWait->bRefused = true;
            }
            pWait->SetDone();
        });

        // show(), not open(): open() forces Qt::WindowModal (measured, Qt
        // 6.11), and a window-modal dialog with no parent blocks nothing.
        // finished() still comes from done().
        pOpened->setWindowModality(Qt::ApplicationModal);
        pOpened->show();
    };

    if (oHost.IsBorrowed())
    {
        // The Qt thread is the emulator's own, and pumps only between frames -
        // not at all while it hashes a disc image. A clock cannot tell a busy
        // host from a hung one, so the wait for the dialog to start ends only
        // when the library is shutting down (the pool drain this caller would
        // otherwise hold up) or the desktop has closed. Past the modal start
        // timeout it says so, once, and keeps waiting.
        oHost.Invoke(fOpen);

        const auto tWarnAt = std::chrono::steady_clock::now() + m_tModalStartTimeout;
        bool bWarned = false;
        std::unique_lock<std::mutex> oLock(pWait->oMutex);
        while (!pWait->bStarted && !pWait->bDone)
        {
            if (ra::services::ServiceLocator::IsShuttingDown() || pState->bClosed.load())
            {
                pWait->bGaveUp = true;
                oLock.unlock();
                ++pState->nModalNotStarted;
                const auto nAnswer = RefusalAnswer(vmWindow);
                RA_LOG_WARN("Dialog \"%s\" could not be opened before shutdown - returning %s",
                            ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str(), DialogResultName(nAnswer));
                return nAnswer;
            }

            if (!bWarned && std::chrono::steady_clock::now() >= tWarnAt)
            {
                bWarned = true;
                oLock.unlock();
                ++pState->nStillWaitingForHost;
                RA_LOG_WARN("Still waiting for the host to pump; dialog \"%s\"",
                            ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
                oLock.lock();
                continue; // the state may have changed while unlocked
            }

            pWait->cvDone.wait_for(oLock, std::chrono::milliseconds(100));
        }
        pWait->cvDone.wait(oLock, [&pWait]() { return pWait->bDone; });
        if (pWait->bRefused)
            return RefusalAnswer(vmWindow);
        return vmWindow.GetDialogResult();
    }

    const bool bOpened = oHost.InvokeAndWait(fOpen, m_tModalStartTimeout);

    if (!bOpened)
    {
        ++pState->nModalNotStarted;
        const auto nAnswer = RefusalAnswer(vmWindow);
        RA_LOG_WARN("Dialog \"%s\" could not be opened on the Qt thread - returning %s",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str(), DialogResultName(nAnswer));
        return nAnswer;
    }

    std::unique_lock<std::mutex> oLock(pWait->oMutex);
    pWait->cvDone.wait(oLock, [&pWait]() { return pWait->bDone; });
    if (pWait->bRefused)
        return RefusalAnswer(vmWindow);
    return vmWindow.GetDialogResult();
}

void QtDesktop::ForgetModal(State& oState, const QDialog* pDialog)
{
    auto& vOpenModals = oState.vOpenModals;
    vOpenModals.erase(std::remove_if(vOpenModals.begin(), vOpenModals.end(),
                                     [pDialog](const QPointer<QDialog>& pOpen) {
                                         return pOpen.isNull() || pOpen.data() == pDialog;
                                     }),
                      vOpenModals.end());
}

void QtDesktop::CloseAll(State& oState)
{
    // From here on nothing opens: a pool task still running during the drain
    // could otherwise open a dialog and hold the drain up waiting for an answer.
    oState.bClosed = true;

    // Modal dialogs first. Rejecting one answers it - each dialog maps "no
    // button" to its escape answer - and releases its caller, so a pool worker
    // waiting in ShowModal cannot hold up the thread pool's drain. A copy,
    // because each reject() takes its dialog out of the list.
    const auto vOpenModals = oState.vOpenModals;
    for (const auto& pDialog : vOpenModals)
    {
        if (pDialog.isNull())
            continue;

        ++oState.nClosedForShutdown;
        if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
            RA_LOG_INFO("Closing dialog \"%s\" for shutdown", pDialog->windowTitle().toStdString().c_str());
        pDialog->reject();

        // A worker's dialog that the reject finished has been forgotten, its caller released, and its delete left to
        // deleteLater - but nothing may pump again before the loader's dlclose, and a dialog outliving the library
        // keeps connections into its code. Delete it now: Qt drops the pending deferred delete, and the destroyed
        // handler finds the caller already released. Still listed means not ours to delete: a Qt-thread caller's
        // exec(), which owns its dialog, has not returned yet, or the dialog put the reject off (ModalDialogBase,
        // while CanAccept is on the stack) and finishes when that returns.
        const bool bStillListed = std::any_of(oState.vOpenModals.begin(), oState.vOpenModals.end(),
                                              [&pDialog](const QPointer<QDialog>& pOpen) { return pOpen == pDialog; });
        if (!pDialog.isNull() && !bStillListed)
            delete pDialog.data();
    }

    // Then the non-modal windows, deleted now rather than later: the Qt
    // thread's loop may not run again before the view models behind them are
    // destroyed, and a binding must go before its view model.
    std::vector<QPointer<QWidget>> vWindows;
    for (auto* pWidget : ra::ui::qt::bindings::WindowBinding::GetBoundWidgets())
        vWindows.emplace_back(pWidget);

    for (const auto& pWindow : vWindows)
    {
        if (pWindow.isNull() || pWindow->windowModality() != Qt::NonModal)
            continue;

        ++oState.nClosedForShutdown;
        if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
            RA_LOG_INFO("Closing window \"%s\" for shutdown", pWindow->windowTitle().toStdString().c_str());
        pWindow->close();      // IsVisible false, as when the user closes it
        delete pWindow.data(); // WA_DeleteOnClose only schedules the delete
    }

    // The "not available" notice answers nothing and has no view model: just delete it.
    if (!oState.pNotice.isNull())
        delete oState.pNotice.data();
}

void QtDesktop::NoticeNotAvailable(const ra::services::IQtApplicationHost& oHost, const std::wstring& sTitle) const
{
    if (m_pState->bClosed.load())
        return; // shutting down: nothing opens

    {
        std::lock_guard<std::mutex> oLock(m_pState->oNoticeMutex);
        m_pState->vNoticeTitles.push_back(sTitle);
        if (m_pState->bNoticeQueued)
            return; // the queued call lists this title too
        m_pState->bNoticeQueued = true;
    }

    oHost.Invoke([pState = m_pState]() { ShowNotAvailable(*pState); });
}

void QtDesktop::ShowNotAvailable(State& oState)
{
    std::vector<std::wstring> vTitles;
    {
        std::lock_guard<std::mutex> oLock(oState.oNoticeMutex);
        vTitles.swap(oState.vNoticeTitles);
        oState.bNoticeQueued = false;
    }

    // CloseAll may have run since the titles were queued
    if (oState.bClosed.load() || vTitles.empty())
        return;

    // A notice the user dismissed is hidden at once but deleted only later
    // (WA_DeleteOnClose): titles must go to a new box, not to that one.
    if (!oState.pNotice.isNull() && !oState.pNotice->isVisible())
    {
        oState.pNotice->deleteLater();
        oState.pNotice.clear();
    }

    const bool bNew = oState.pNotice.isNull();
    if (bNew)
    {
        // Not modal: it answers nothing, and ShowWindow's caller must not wait on it.
        auto* pNotice = new QMessageBox(QMessageBox::Information, QStringLiteral("RetroAchievements"),
                                        QStringLiteral("Not available on Linux yet:"), QMessageBox::Ok);
        pNotice->setObjectName(QLatin1String(NotAvailableNoticeName));
        pNotice->setAttribute(Qt::WA_DeleteOnClose);
        pNotice->setWindowModality(Qt::NonModal);
        oState.pNotice = pNotice;
        ++oState.nNotAvailableNotices;
    }

    QStringList vLines = oState.pNotice->text().split(QLatin1Char('\n'));
    for (const auto& sTitle : vTitles)
    {
        const auto sLine = QString::fromStdWString(sTitle);
        if (!vLines.contains(sLine))
            vLines.append(sLine);
    }
    oState.pNotice->setText(vLines.join(QLatin1Char('\n')));

    if (bNew)
        oState.pNotice->show();
}

void QtDesktop::CloseWindow(WindowViewModelBase& vmWindow) const
{
    auto* pHost = GetHost();
    if (pHost == nullptr || !pHost->IsAvailable())
        return;

    // Only the address is compared, so a view model destroyed before this runs
    // is never touched.
    pHost->Invoke([pWindowViewModel = &vmWindow]() {
        auto* pBinding = ra::ui::qt::bindings::WindowBinding::GetBindingFor(*pWindowViewModel);
        if (pBinding != nullptr && pBinding->GetWidget() != nullptr)
            pBinding->GetWidget()->close();
    });
}

void QtDesktop::OpenUrl(const std::string& sUrl) const
{
    auto* pHost = GetHost();
    if (pHost == nullptr || !pHost->IsAvailable())
    {
        NullDesktop::OpenUrl(sUrl);
        return;
    }

    pHost->Invoke([sUrl]() {
        RA_LOG_INFO("Opening %s", sUrl.c_str());
        if (!QDesktopServices::openUrl(QUrl(QString::fromStdString(sUrl))))
            RA_LOG_WARN("Could not open %s", sUrl.c_str());
    });
}

std::unique_ptr<ra::ui::drawing::ISurface> QtDesktop::CaptureClientArea(const WindowViewModelBase&) const
{
    using ra::ui::drawing::qt::ScreenCapture;

    if (!ra::services::ServiceLocator::Exists<ScreenCapture>())
    {
        RA_LOG_INFO("No screen capture: no ScreenCapture registered");
        return std::make_unique<ra::ui::drawing::null::NullSurface>(0, 0);
    }

    std::unique_ptr<ra::ui::drawing::qt::QtSurface> pSurface;
    const auto nResult = ra::services::ServiceLocator::Get<ScreenCapture>().Capture(pSurface);
    if (nResult != ScreenCapture::Result::Captured)
    {
        RA_LOG_INFO("No screen capture: %s", ScreenCapture::Describe(nResult));
        return std::make_unique<ra::ui::drawing::null::NullSurface>(0, 0);
    }

    RA_LOG_INFO("Screen capture: %dx%d device pixels from the emulator", pSurface->GetImage().width(),
                pSurface->GetImage().height());
    return pSurface;
}

bool QtDesktop::IsOnUIThread() const
{
    const auto* pHost = GetHost();
    return (pHost == nullptr || !pHost->IsAvailable()) ? true : pHost->IsOnQtThread();
}

void QtDesktop::InvokeOnUIThread(std::function<void()> fAction) const
{
    const auto* pHost = GetHost();
    if (pHost == nullptr || !pHost->IsAvailable())
    {
        // no Qt application: nothing to marshal to, as NullDesktop
        fAction();
        return;
    }

    pHost->Invoke(std::move(fAction));
}

void QtDesktop::InvokeOnHostThread(std::function<void()> fAction) const
{
    if (!ra::services::ServiceLocator::Exists<ra::services::impl::HostThreadDispatcher>())
    {
        fAction();
        return;
    }

    ra::services::ServiceLocator::GetMutable<ra::services::impl::HostThreadDispatcher>().Invoke(std::move(fAction));
}

void QtDesktop::Shutdown()
{
    // Refuse new dialogs from here on, even if the Qt thread is slow to start
    // CloseAll below (CloseAll sets it too, for the stop-hook path).
    m_pState->bClosed = true;

    // Runs first in Initialization::Shutdown, before the thread pool drains: a
    // worker waiting in ShowModal has to be answered before the drain can end.
    // No widgets, no windows: nothing to close, and no reason to wait on the
    // Qt thread - which, borrowed, may be blocked on this very thread.
    auto* pHost = GetHost();
    if (!CanHostWidgets(pHost))
        return;

    auto pState = m_pState;
    if (!pHost->InvokeAndWait([pState]() { CloseAll(*pState); }, m_tShutdownCloseTimeout))
    {
        // The Qt thread is busy - a modal's CanAccept, say Login() waiting on the server - and the timed-out call was
        // dropped. Queue it again, to run the moment the thread is free: a pool worker waiting on that modal holds up
        // the drain that follows, and nothing else would ever close it.
        RA_LOG_WARN("Windows were not closed for shutdown: the Qt thread is busy - closing them when it is free");
        pHost->Invoke([pState]() { CloseAll(*pState); });
    }
}

} // namespace qt
} // namespace ui
} // namespace ra
