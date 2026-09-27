#include "ui/qt/QtDesktop.hh"

#include "services/IQtApplicationHost.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/HostThreadDispatcher.hh"

#include "ui/qt/bindings/WindowBinding.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QDesktopServices>
#include <QDialog>
#include <QString>
#include <QUrl>
#include <QWidget>

#include <algorithm>
#include <condition_variable>
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
    bool bDone = false;

    void SetDone()
    {
        {
            std::lock_guard<std::mutex> oLock(oMutex);
            bDone = true;
        }
        cvDone.notify_all();
    }
};

} // namespace

QtDesktop::QtDesktop() : m_pState(std::make_shared<State>())
{
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
        return;
    }

    // The view model outlives the queued call, as with Win32's queued
    // ShowWindow: every caller's is a WindowManager member.
    pHost->Invoke([pState = m_pState, pPresenter, &vmWindow]() {
        (void)pState; // keeps the presenter alive
        pPresenter->ShowWindow(vmWindow);
    });
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
        // NullDesktop logs "No view layer to show dialog" - ra_linux_smoke
        // looks for that line - and answers No.
        ++m_pState->nNoViewLayer;
        return NullDesktop::ShowModal(vmWindow);
    }

    if (pHost->IsOnQtThread())
        return ShowModalOnQtThread(*pPresenter, vmWindow);

    return ShowModalFromOtherThread(*pHost, *pPresenter, vmWindow);
}

ra::ui::DialogResult QtDesktop::ShowModalOnQtThread(IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const
{
    // A caller on the Qt thread can only be answered from a nested event loop.
    auto pDialog = oPresenter.CreateModal(vmWindow);
    if (pDialog == nullptr)
        return vmWindow.GetDialogResult();

    pDialog->setWindowModality(Qt::ApplicationModal);
    m_pState->vOpenModals.emplace_back(pDialog.get());
    pDialog->exec();
    ForgetModal(*m_pState, pDialog.get());

    return vmWindow.GetDialogResult();
}

ra::ui::DialogResult QtDesktop::ShowModalFromOtherThread(ra::services::IQtApplicationHost& oHost,
                                                         IDialogPresenter& oPresenter,
                                                         WindowViewModelBase& vmWindow) const
{
    // Opened, not exec()'d: this thread waits, and the Qt thread goes back to
    // its own loop. Two modals from two threads are then answered
    // independently; nested exec() loops would hold the first caller until the
    // second dialog closed.
    auto pWait = std::make_shared<ModalWait>();
    auto pState = m_pState;
    const bool bOpened = oHost.InvokeAndWait(
        [pState, pWait, &oPresenter, &vmWindow]() {
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

            pOpened->setWindowModality(Qt::ApplicationModal);
            pOpened->open();
        },
        m_tModalStartTimeout);

    if (!bOpened)
    {
        ++pState->nModalNotStarted;
        RA_LOG_WARN("Dialog \"%s\" could not be opened on the Qt thread - returning No",
                    ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
        return ra::ui::DialogResult::No;
    }

    std::unique_lock<std::mutex> oLock(pWait->oMutex);
    pWait->cvDone.wait(oLock, [&pWait]() { return pWait->bDone; });
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
        RA_LOG_INFO("Closing dialog \"%s\" for shutdown", pDialog->windowTitle().toStdString().c_str());
        pDialog->reject();
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
        RA_LOG_INFO("Closing window \"%s\" for shutdown", pWindow->windowTitle().toStdString().c_str());
        pWindow->close();      // IsVisible false, as when the user closes it
        delete pWindow.data(); // WA_DeleteOnClose only schedules the delete
    }
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
    // Runs first in Initialization::Shutdown, before the thread pool drains: a
    // worker waiting in ShowModal has to be answered before the drain can end.
    auto* pHost = GetHost();
    if (pHost == nullptr || !pHost->IsAvailable())
        return;

    auto pState = m_pState;
    if (!pHost->InvokeAndWait([pState]() { CloseAll(*pState); }, std::chrono::seconds(5)))
        RA_LOG_WARN("Windows were not closed for shutdown: the Qt thread did not start the call");
}

} // namespace qt
} // namespace ui
} // namespace ra
