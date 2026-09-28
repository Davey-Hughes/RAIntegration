#ifndef RA_UI_QT_QTDESKTOP_HH
#define RA_UI_QT_QTDESKTOP_HH
#pragma once

#include "ui/null/NullDesktop.hh"
#include "ui/qt/IDialogPresenter.hh"

#include <QPointer>
#include <QMessageBox>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

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
    bool IsOnUIThread() const override;
    void InvokeOnUIThread(std::function<void()> fAction) const override;
    void InvokeOnHostThread(std::function<void()> fAction) const override;
    void Shutdown() override;

    /// <summary>Adds a presenter after the built-in ones. For tests: call before anything is shown.</summary>
    void AddPresenter(std::unique_ptr<IDialogPresenter> pPresenter);

    /// <summary>
    /// How long ShowModal waits for its dialog to open on the Qt thread before answering without it (see RefusalAnswer).
    /// </summary>
    void SetModalStartTimeout(std::chrono::milliseconds tTimeout) noexcept { m_tModalStartTimeout = tTimeout; }

    /// <summary>How long Shutdown waits for the Qt thread to start closing the windows before queuing it instead.</summary>
    void SetShutdownCloseTimeout(std::chrono::milliseconds tTimeout) noexcept { m_tShutdownCloseTimeout = tTimeout; }

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

        // The "not available" notice. Titles wait here for the Qt thread,
        // which lists them all in one box.
        std::mutex oNoticeMutex;
        std::vector<std::wstring> vNoticeTitles; // under oNoticeMutex
        bool bNoticeQueued = false;              // under oNoticeMutex: a ShowNotAvailable call is queued
        QPointer<QMessageBox> pNotice;           // Qt thread only

        std::atomic<bool> bClosed{false}; // set by Shutdown and CloseAll: from then on nothing opens
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
