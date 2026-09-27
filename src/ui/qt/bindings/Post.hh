#ifndef RA_UI_QT_BINDINGS_POST_HH
#define RA_UI_QT_BINDINGS_POST_HH
#pragma once

#include <QMetaObject>
#include <QObject>
#include <QThread>

#include <utility>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Runs <paramref name="fAction" /> on <paramref name="oContext" />'s thread without waiting for it: inline when the
/// caller is already there, queued otherwise. A queued call is discarded, unrun, if <paramref name="oContext" /> is
/// destroyed first - so the context is the object the call updates, never something that outlives it.
/// </summary>
/// <remarks>
/// The one way a binding reaches a widget. A binding is notified on whatever thread set the property - a pool
/// worker, the emulator's thread - and must never wait on the Qt thread there: the Qt thread may itself be waiting on
/// the host thread (see IQtApplicationHost). Never Qt::BlockingQueuedConnection, never InvokeAndWait.
/// </remarks>
template<typename TAction>
void Post(QObject& oContext, TAction&& fAction)
{
    if (QThread::currentThread() == oContext.thread())
        fAction();
    else
        QMetaObject::invokeMethod(&oContext, std::forward<TAction>(fAction), Qt::QueuedConnection);
}

/// <summary>
/// Queues <paramref name="fAction" /> on <paramref name="oContext" />'s thread even when the caller is already there,
/// and discards it, unrun, if <paramref name="oContext" /> is destroyed first. For work that must not run inside the
/// caller's stack: a close that finishes a modal dialog, whose caller may then destroy the view model that is still
/// notifying.
/// </summary>
template<typename TAction>
void PostQueued(QObject& oContext, TAction&& fAction)
{
    QMetaObject::invokeMethod(&oContext, std::forward<TAction>(fAction), Qt::QueuedConnection);
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_POST_HH
