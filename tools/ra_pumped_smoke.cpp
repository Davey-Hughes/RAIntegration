// ra_pumped_smoke: the library inside an emulator that owns the QApplication
// and pumps it once a frame, with no exec() ever on the stack - the Qt
// RALibretro's shape (v3-qt-host-design.md, section 7). Offline, headless
// (QT_QPA_PLATFORM=offscreen), no account.
//
// Checks: the host borrows this application; a worker's modal is opened by a
// pump and answered from the pump; the closed dialog is destroyed by a later
// pump (counted through QObject::destroyed, not "no crash"); _RA_Shutdown
// leaves no library thread and no library QObject; this application outlives
// it.
//
// RA_PUMPED_SMOKE_NO_PUMP=1 is the gate's negative control: nothing pumps
// while the worker waits, so the modal check must fail and the worker must be
// released by the shutdown drain instead.

#include "Exports.hh" // _RA_InitClientOffline, _RA_Shutdown
#include "services/Initialization.hh"
#include "services/ServiceLocator.hh"
#include "services/IQtApplicationHost.hh"
#include "services/IThreadPool.hh"
#include "ui/viewmodels/MessageBoxViewModel.hh"

#include "SmokeReport.hh"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QMessageBox>
#include <QPointer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <thread>

static void Section(const char* sName) { std::printf("\n%s\n", sName); }

static void Pump()
{
    QCoreApplication::processEvents(QEventLoop::AllEvents);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

// Pumps until fDone or the deadline. With RA_PUMPED_SMOKE_NO_PUMP set it only waits.
static bool PumpUntil(const std::function<bool()>& fDone, std::chrono::milliseconds tTimeout, bool bPump)
{
    const auto tDeadline = std::chrono::steady_clock::now() + tTimeout;
    do
    {
        if (bPump)
            Pump();
        if (fDone())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < tDeadline);
    return false;
}

static QMessageBox* FindVisibleMessageBox()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pBox = qobject_cast<QMessageBox*>(pWidget);
        if (pBox != nullptr && pBox->isVisible())
            return pBox;
    }
    return nullptr;
}

int main(int argc, char* argv[])
{
    QApplication oApplication(argc, argv);
    std::printf("ra_pumped_smoke (platform %s)\n", QApplication::platformName().toUtf8().constData());
    std::fflush(stdout);

    const bool bPump = std::getenv("RA_PUMPED_SMOKE_NO_PUMP") == nullptr;
    if (!bPump)
        std::printf("negative control: the host will not pump while the worker waits\n");

    using namespace ra::services;

    Section("entry point");
    const int nInitialised = _RA_InitClientOffline(nullptr, "RAPumped", "0.0.0.0");
    Check(nInitialised == 1 && Initialization::IsInitialized(), "_RA_InitClientOffline",
          "returned " + std::to_string(nInitialised));

    const bool bHostAvailable = ServiceLocator::Exists<IQtApplicationHost>() &&
                                ServiceLocator::Get<IQtApplicationHost>().IsAvailable();
    Check(bHostAvailable, "Qt host available", bHostAvailable ? "yes" : "no host or unavailable");
    const bool bBorrowed = bHostAvailable && ServiceLocator::Get<IQtApplicationHost>().IsBorrowed();
    Check(bBorrowed, "host borrowed this application", bBorrowed ? "IsBorrowed() is true" : "not borrowed");
    const bool bOnQtThread = bHostAvailable && ServiceLocator::Get<IQtApplicationHost>().IsOnQtThread();
    Check(bOnQtThread, "main thread is the Qt thread", bOnQtThread ? "IsOnQtThread() is true" : "false");

    Section("a worker's modal, opened and answered by the pump");
    std::atomic<int> nResult{-1};
    std::atomic<bool> bWorkerDone{false};
    ServiceLocator::GetMutable<IThreadPool>().RunAsync([&nResult, &bWorkerDone]() {
        ra::ui::viewmodels::MessageBoxViewModel vmMessage(L"Pumped smoke: answer Yes");
        vmMessage.SetButtons(ra::ui::viewmodels::MessageBoxViewModel::Buttons::YesNo);
        nResult = static_cast<int>(vmMessage.ShowModal());
        bWorkerDone = true;
    });

    QPointer<QMessageBox> pBox;
    const bool bOpened = PumpUntil([&pBox]() { pBox = FindVisibleMessageBox(); return !pBox.isNull(); },
                                   std::chrono::seconds(5), bPump);
    Check(bOpened, "worker's modal opened by a pump", bOpened ? "QMessageBox visible" : "no box within 5 s");

    std::atomic<int> nDestroyed{0};
    if (bOpened)
    {
        QObject::connect(pBox.data(), &QObject::destroyed, [&nDestroyed]() { ++nDestroyed; });
        pBox->button(QMessageBox::Yes)->click();
    }
    const bool bAnswered = PumpUntil([&bWorkerDone]() { return bWorkerDone.load(); }, std::chrono::seconds(5), bPump);
    Check(bAnswered && nResult.load() == static_cast<int>(ra::ui::DialogResult::Yes), "worker got the answer",
          "result " + std::to_string(nResult.load()) + " (Yes is " +
              std::to_string(static_cast<int>(ra::ui::DialogResult::Yes)) + ")");

    const bool bDestroyed = PumpUntil([&nDestroyed]() { return nDestroyed.load() == 1; }, std::chrono::seconds(2), bPump);
    Check(bDestroyed, "closed dialog destroyed by a pump", "destroyed signals: " + std::to_string(nDestroyed.load()));

    Section("shutdown");
    // _RA_Shutdown() returns 0 on every path (see ra_linux_smoke.cpp's identical check).
    const int nShutdown = _RA_Shutdown();
    Check(nShutdown == 0, "_RA_Shutdown", "returned " + std::to_string(nShutdown));
    // a worker held by the negative control is released by the drain; either way it is done now
    const bool bWorkerReleased = PumpUntil([&bWorkerDone]() { return bWorkerDone.load(); }, std::chrono::seconds(5), true);
    Check(bWorkerReleased, "worker released by shutdown", bWorkerReleased ? "done" : "still waiting");

    Pump();
    const ThreadCount oThreads = CountThreadsBesideQtDBus();
    Check(oThreads.nOther == 1, "no library thread outlives shutdown",
          std::to_string(oThreads.nOther) + " thread(s) besides QDBusConnection (want 1: main)");
    Check(QApplication::instance() == &oApplication, "host application outlives the library", "instance intact");

    return Finish("ra_pumped_smoke");
}
