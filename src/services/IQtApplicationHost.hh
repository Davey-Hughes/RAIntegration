#ifndef RA_SERVICES_IQTAPPLICATIONHOST_HH
#define RA_SERVICES_IQTAPPLICATIONHOST_HH
#pragma once

#ifndef _WIN32

#include <chrono>
#include <functional>

namespace ra {
namespace services {

/// <summary>
/// The Qt application the Linux services (and later the views) run on, and the thread it runs on.
/// </summary>
/// <remarks>
/// Owned: the host has no Qt application, so the library created a QApplication on a private thread.
/// Borrowed: the host already had a GUI application; it is used where it is and never created, quit or destroyed.
/// Unavailable: no display, or the host has only a non-GUI QCoreApplication; every call below is then a no-op.
/// The Qt thread never waits on any other thread, which is what keeps InvokeAndWait deadlock-free.
/// </remarks>
class IQtApplicationHost
{
public:
    virtual ~IQtApplicationHost() noexcept = default;
    IQtApplicationHost(const IQtApplicationHost&) noexcept = delete;
    IQtApplicationHost& operator=(const IQtApplicationHost&) noexcept = delete;
    IQtApplicationHost(IQtApplicationHost&&) noexcept = delete;
    IQtApplicationHost& operator=(IQtApplicationHost&&) noexcept = delete;

    /// <summary>Returns <c>true</c> from a successful start until <see cref="Stop" /> begins.</summary>
    virtual bool IsAvailable() const = 0;

    /// <summary>Returns <c>true</c> if the application can host widgets (it is a QApplication).</summary>
    virtual bool HasWidgets() const = 0;

    /// <summary>Returns <c>true</c> on the thread that owns the application; <c>false</c> everywhere when unavailable.</summary>
    virtual bool IsOnQtThread() const = 0;

    /// <summary>
    /// Runs <paramref name="fAction" /> on the Qt thread: inline when already there, queued otherwise. Dropped when
    /// unavailable - and a queued call that has not run by the time <see cref="Stop" /> begins never runs.
    /// </summary>
    virtual void Invoke(std::function<void()> fAction) const = 0;

    /// <summary>
    /// Like <see cref="Invoke" />, but waits: up to <paramref name="tTimeout" /> for <paramref name="fAction" /> to
    /// start, and once it has started, for as long as it takes to finish - so a hung <paramref name="fAction" />
    /// hangs the caller. Returns <c>false</c> if it was dropped or had not started in time; it then never runs, so
    /// <paramref name="fAction" /> may capture the caller's locals by reference.
    /// </summary>
    virtual bool InvokeAndWait(std::function<void()> fAction, std::chrono::milliseconds tTimeout) const = 0;

    /// <summary>
    /// Registers a function <see cref="Stop" /> runs on the Qt thread before the application goes away.
    /// <see cref="Stop" /> clears the list.
    /// </summary>
    /// <remarks>
    /// The hooks run after <see cref="Stop" /> has begun refusing work: <see cref="Invoke" /> and
    /// <see cref="InvokeAndWait" /> are refused inside a hook - on the Qt thread too, where they would otherwise run
    /// inline - so a hook must do its work directly, and must not use either.
    /// </remarks>
    virtual void AddStopHook(std::function<void()> fHook) = 0;

    /// <summary>
    /// Refuses further work, runs the stop hooks on the Qt thread and - when the application is owned - ends and
    /// joins that thread. The hooks are waited for as <see cref="InvokeAndWait" /> waits: skipped if the Qt thread
    /// does not start them in time, but once started, waited for without limit - a hung hook hangs Stop.
    /// </summary>
    virtual void Stop() = 0;

protected:
    IQtApplicationHost() noexcept = default;
};

} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_IQTAPPLICATIONHOST_HH
