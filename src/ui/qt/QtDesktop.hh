#ifndef RA_UI_QT_QTDESKTOP_HH
#define RA_UI_QT_QTDESKTOP_HH
#pragma once

#include "ui/null/NullDesktop.hh"
#include "ui/qt/IDialogPresenter.hh"

#include <QPointer>

#include <atomic>
#include <chrono>
#include <memory>
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
/// application that can host widgets - gets NullDesktop's answer: logged, and No.
/// </summary>
class QtDesktop : public ra::ui::null::NullDesktop
{
public:
    QtDesktop();
    ~QtDesktop() noexcept override = default;

    void ShowWindow(WindowViewModelBase& vmWindow) const override;
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

    /// <summary>How long ShowModal waits for its dialog to open on the Qt thread before answering No.</summary>
    void SetModalStartTimeout(std::chrono::milliseconds tTimeout) noexcept { m_tModalStartTimeout = tTimeout; }

    // RA_LOG_* is a no-op under RA_UTEST, so each logged outcome is counted too.
    size_t NoViewLayerCount() const noexcept { return m_pState->nNoViewLayer.load(); }
    size_t ModalNotStartedCount() const noexcept { return m_pState->nModalNotStarted.load(); }
    size_t ClosedForShutdownCount() const noexcept { return m_pState->nClosedForShutdown.load(); }

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
    };

    static ra::services::IQtApplicationHost* GetHost();
    static bool CanHostWidgets(const ra::services::IQtApplicationHost* pHost);
    IDialogPresenter* FindPresenter(const WindowViewModelBase& vmWindow) const;

    ra::ui::DialogResult DoShowModal(WindowViewModelBase& vmWindow) const;
    ra::ui::DialogResult ShowModalOnQtThread(IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const;
    ra::ui::DialogResult ShowModalFromOtherThread(ra::services::IQtApplicationHost& oHost,
                                                  IDialogPresenter& oPresenter, WindowViewModelBase& vmWindow) const;

    static void ForgetModal(State& oState, const QDialog* pDialog);
    static void CloseAll(State& oState);

    std::shared_ptr<State> m_pState;
    std::chrono::milliseconds m_tModalStartTimeout{std::chrono::seconds(10)};
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_QTDESKTOP_HH
