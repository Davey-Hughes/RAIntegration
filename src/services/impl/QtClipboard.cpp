#include "QtClipboard.hh"

#include "util/Log.hh"

#include <QClipboard>
#include <QGuiApplication>
#include <QString>

#include <chrono>

namespace ra {
namespace services {
namespace impl {

// QClipboard belongs to the thread that owns the application. SetText is queued
// there; GetText waits for the answer, which cannot deadlock because the Qt
// thread never waits on another thread (see IQtApplicationHost). A caller that
// blocks the Qt thread itself - a borrowed application's thread joining the
// caller - gets an empty answer after the timeout instead.
static constexpr std::chrono::seconds CLIPBOARD_TIMEOUT{2};

void QtClipboard::SetText(const std::wstring& sValue) const
{
    if (!m_pHost.IsAvailable())
    {
        RA_LOG_WARN("Clipboard SetText ignored: Qt services are unavailable");
        return;
    }

    const QString sText = QString::fromStdWString(sValue);
    m_pHost.Invoke([sText]() { QGuiApplication::clipboard()->setText(sText); });
}

std::wstring QtClipboard::GetText() const
{
    if (!m_pHost.IsAvailable())
    {
        RA_LOG_WARN("Clipboard GetText ignored: Qt services are unavailable");
        return std::wstring();
    }

    std::wstring sText;
    if (!m_pHost.InvokeAndWait([&sText]() { sText = QGuiApplication::clipboard()->text().toStdWString(); },
                               CLIPBOARD_TIMEOUT))
    {
        // false both when the call timed out and when Stop() began between
        // the check above and the call: the message covers either
        RA_LOG_WARN("Clipboard GetText got no answer (timed out after %d s, or Qt services stopping)",
                    static_cast<int>(CLIPBOARD_TIMEOUT.count()));
        return std::wstring();
    }

    return sText;
}

} // namespace impl
} // namespace services
} // namespace ra
