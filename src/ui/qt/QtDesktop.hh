#ifndef RA_UI_QT_QTDESKTOP_HH
#define RA_UI_QT_QTDESKTOP_HH
#pragma once

#include "ui/null/NullDesktop.hh"
#include "ui/qt/IDialogPresenter.hh"

#include <QPointer>
#include <QMessageBox>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class QUrl;

namespace ra {
namespace services {
class IQtApplicationHost;
} // namespace services
} // namespace ra

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// The Linux desktop. Shows the view models' windows on the Qt application's thread (RA-Qt); sends RA's UI work
/// there and the emulator's callbacks back to the emulator's thread. A view model with no Qt window yet - or no Qt
/// application that can host widgets - is answered without one: logged, with a message box's escape answer (Cancel,
/// else No, else OK) and No for anything else. When widgets can be shown, its title is also listed in a "not
/// available" notice.
///
/// A test hook for headless runs: with the environment variable RA_AUTO_ANSWER_DIALOGS set to anything but empty or
/// "0", a modal dialog that would be shown is not - from the Qt thread or any other - and ShowModal returns at once
/// with that same escape answer, logged at WARN ("auto-answered") and counted (AutoAnsweredCount). It is read on
/// every call. ShowWindow is unaffected. Only for runs with no one to answer: a headless gate sets it so that the
/// login box, say, cannot hang the run.
/// </summary>
class QtDesktop : public ra::ui::null::NullDesktop
{
public:
    QtDesktop();
    ~QtDesktop() noexcept override = default;

    void ShowWindow(WindowViewModelBase& vmWindow) const override;
    bool CanShowWindow(const WindowViewModelBase& vmWindow) const override;
    ra::ui::DialogResult ShowModal(WindowViewModelBase& vmWindow) const override;
    ra::ui::DialogResult ShowModal(WindowViewModelBase& vmWindow, const WindowViewModelBase& vmParentWindow) const override;
    void CloseWindow(WindowViewModelBase& vmWindow) const override;
    void OpenUrl(const std::string& sUrl) const override;

    /// <summary>
    /// The emulator's picture, for an achievement screenshot: what the emulator hands over through ScreenCapture,
    /// where Windows BitBlts its window. vmViewModel is ignored: OverlayManager only ever asks for the emulator's. Any
    /// failure - logged - is an empty surface, never nullptr: OverlayManager draws over the result unchecked.
    /// </summary>
    std::unique_ptr<ra::ui::drawing::ISurface> CaptureClientArea(const WindowViewModelBase& vmViewModel) const override;

    bool IsOnUIThread() const override;
    void InvokeOnUIThread(std::function<void()> fAction) const override;
    void InvokeOnHostThread(std::function<void()> fAction) const override;
    void Shutdown() override;

    /// <summary>Adds a presenter after the built-in ones. For tests: call before anything is shown.</summary>
    void AddPresenter(std::unique_ptr<IDialogPresenter> pPresenter);

    /// <summary>
    /// How long ShowModal waits for its dialog to open on the Qt thread before answering without it (see RefusalAnswer).
    /// A borrowed host (IsBorrowed) ignores it: that wait ends only at shutdown or when the desktop closes - a clock
    /// cannot tell a busy emulator from a hung one - and past this timeout it only logs, once, that it still waits.
    /// </summary>
    void SetModalStartTimeout(std::chrono::milliseconds tTimeout) noexcept { m_tModalStartTimeout = tTimeout; }

    /// <summary>How long Shutdown waits for the Qt thread to start closing the windows before queuing it instead.</summary>
    void SetShutdownCloseTimeout(std::chrono::milliseconds tTimeout) noexcept { m_tShutdownCloseTimeout = tTimeout; }

    /// <summary>
    /// What opens a URL in place of QDesktopServices::openUrl, answering whether it did. For tests: call before
    /// anything opens one.
    /// </summary>
    void SetUrlOpener(std::function<bool(const QUrl&)> fOpener) { m_pState->fUrlOpener = std::move(fOpener); }

    /// <summary>
    /// Whether Shutdown (or CloseAll) has closed the windows: from then on no dialog opens, and a modal dialog's OK
    /// cancels instead of starting its work. Any thread.
    /// </summary>
    bool IsClosedForShutdown() const noexcept { return m_pState->bClosed.load(); }

    // RA_LOG_* is a no-op under RA_UTEST, so each logged outcome is counted too.
    size_t NoViewLayerCount() const noexcept { return m_pState->nNoViewLayer.load(); }
    size_t ModalNotStartedCount() const noexcept { return m_pState->nModalNotStarted.load(); }
    size_t ClosedForShutdownCount() const noexcept { return m_pState->nClosedForShutdown.load(); }
    size_t RefusedAfterShutdownCount() const noexcept { return m_pState->nRefusedAfterShutdown.load(); }
    size_t NotAvailableNoticeCount() const noexcept { return m_pState->nNotAvailableNotices.load(); }
    size_t AutoAnsweredCount() const noexcept { return m_pState->nAutoAnswered.load(); }
    size_t StillWaitingForHostCount() const noexcept { return m_pState->nStillWaitingForHost.load(); }

    /// <summary>The objectName of the "not available" notice, for tests.</summary>
    static constexpr const char* NotAvailableNoticeName = "RANotAvailableNotice";

private:
    // Shared with the stop hook, which may run after this object is gone (a
    // test's host, stopped after its desktop).
    struct State
    {
        std::vector<std::unique_ptr<IDialogPresenter>> vPresenters; // fixed once anything is shown
        std::vector<QPointer<QDialog>> vOpenModals;                 // Qt thread only
        std::atomic<size_t> nNoViewLayer{0};
        std::atomic<size_t> nModalNotStarted{0};
        std::atomic<size_t> nClosedForShutdown{0};
        std::atomic<size_t> nRefusedAfterShutdown{0};
        std::atomic<size_t> nNotAvailableNotices{0}; // "not available" notice boxes opened
        std::atomic<size_t> nAutoAnswered{0};        // modals answered unshown (RA_AUTO_ANSWER_DIALOGS)
        std::atomic<size_t> nStillWaitingForHost{0}; // borrowed waits that outlasted m_tModalStartTimeout

        // The "not available" notice. Titles wait here for the Qt thread,
        // which lists them all in one box.
        std::mutex oNoticeMutex;
        std::vector<std::wstring> vNoticeTitles; // under oNoticeMutex
        bool bNoticeQueued = false;              // under oNoticeMutex: a ShowNotAvailable call is queued
        QPointer<QMessageBox> pNotice;           // Qt thread only

        std::atomic<bool> bClosed{false}; // set by Shutdown and CloseAll: from then on nothing opens

        std::function<bool(const QUrl&)> fUrlOpener; // a test's stand-in for QDesktopServices::openUrl, set first
        QPointer<QObject> pPendingUrls;              // Qt thread only: the context of URLs waiting for a modal to go
    };

    static ra::services::IQtApplicationHost* GetHost();
    static bool CanHostWidgets(const ra::services::IQtApplicationHost* pHost);

    // The answer for a dialog that is never shown: a message box's escape
    // answer - what closing it unanswered gives: Cancel, else No, else OK - and
    // No for anything else. Never None, which callers read as yes; and not
    // always No, which the OK/Cancel minimum-version box reads as "log out".
    static ra::ui::DialogResult RefusalAnswer(const WindowViewModelBase& vmWindow);

    IDialogPresenter* FindPresenter(const WindowViewModelBase& vmWindow) const;

    ra::ui::DialogResult DoShowModal(WindowViewModelBase& vmWindow) const;
    ra::ui::DialogResult ShowModalOnQtThread(IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const;
    ra::ui::DialogResult ShowModalFromOtherThread(ra::services::IQtApplicationHost& oHost,
                                                  IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const;

    static void OpenUrlNow(State& oState, const std::string& sUrl); // Qt thread
    static void OpenUrlOnceFocused(const std::shared_ptr<State>& pState, const std::string& sUrl, QObject& oContext);
    static void ForgetModal(State& oState, const QDialog* pDialog);
    static void CloseAll(State& oState);

    // Lists sTitle in the "not available" notice. Any thread: the box opens on the Qt thread.
    void NoticeNotAvailable(const ra::services::IQtApplicationHost& oHost, const std::wstring& sTitle) const;
    static void ShowNotAvailable(State& oState); // Qt thread

    std::shared_ptr<State> m_pState;
    std::chrono::milliseconds m_tModalStartTimeout{std::chrono::seconds(10)};
    std::chrono::milliseconds m_tShutdownCloseTimeout{std::chrono::seconds(5)};
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_QTDESKTOP_HH
