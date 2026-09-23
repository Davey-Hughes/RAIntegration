#include "QtClipboard.hh"

#include "util/Log.hh"

#include <QClipboard>
#include <QGuiApplication>
#include <QString>
#include <QThread>

namespace ra {
namespace services {
namespace impl {

// QClipboard must be touched from the thread that owns the application object.
// Every caller in the tree reaches the clipboard from a view model, which runs
// on the UI thread, so this logs rather than marshalling - a blocking call from
// a worker thread into the GUI thread is a deadlock waiting for a reason.
static bool CanUseClipboard(const char* sOperation)
{
    auto* pApplication = QGuiApplication::instance();
    if (pApplication == nullptr)
    {
        RA_LOG_WARN("Clipboard %s ignored: no QGuiApplication", sOperation);
        return false;
    }

    if (QThread::currentThread() != pApplication->thread())
    {
        RA_LOG_WARN("Clipboard %s ignored: not on the GUI thread", sOperation);
        return false;
    }

    return true;
}

void QtClipboard::SetText(const std::wstring& sValue) const
{
    if (!CanUseClipboard("SetText"))
        return;

    QGuiApplication::clipboard()->setText(QString::fromStdWString(sValue));
}

std::wstring QtClipboard::GetText() const
{
    if (!CanUseClipboard("GetText"))
        return std::wstring();

    return QGuiApplication::clipboard()->text().toStdWString();
}

} // namespace impl
} // namespace services
} // namespace ra
