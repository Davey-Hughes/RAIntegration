#ifndef RA_TESTS_UI_QT_BORROWEDQTAPPLICATION_HH
#define RA_TESTS_UI_QT_BORROWEDQTAPPLICATION_HH
#pragma once

#ifndef _WIN32

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>

#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace ra {
namespace ui {
namespace qt {
namespace tests {

// An offscreen QApplication on the test's own thread, so a QtApplicationHost
// started while this lives takes Borrowed mode: the shape of an emulator that
// owns the application (the Qt RALibretro). The test is the emulator's frame
// loop, and pumps when it chooses to.
class BorrowedQtApplication
{
public:
    BorrowedQtApplication()
    {
        static char sProgram[] = "ra_tests";
        static char sFlag[] = "-platform";
        static char sPlatform[] = "offscreen";
        m_vArguments = {sProgram, sFlag, sPlatform, nullptr};
        m_nArgc = 3;
        m_pApplication = std::make_unique<QApplication>(m_nArgc, m_vArguments.data());
    }
    ~BorrowedQtApplication() noexcept = default;
    BorrowedQtApplication(const BorrowedQtApplication&) noexcept = delete;
    BorrowedQtApplication& operator=(const BorrowedQtApplication&) noexcept = delete;
    BorrowedQtApplication(BorrowedQtApplication&&) noexcept = delete;
    BorrowedQtApplication& operator=(BorrowedQtApplication&&) noexcept = delete;

    // One frame's worth: what QtHost::pump() does in RALibretro.
    void Pump()
    {
        QCoreApplication::processEvents(QEventLoop::AllEvents);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    // Pumps until fDone answers true or tTimeout passes.
    bool PumpUntil(const std::function<bool()>& fDone, std::chrono::milliseconds tTimeout)
    {
        const auto tDeadline = std::chrono::steady_clock::now() + tTimeout;
        do
        {
            Pump();
            if (fDone())
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (std::chrono::steady_clock::now() < tDeadline);
        return false;
    }

private:
    int m_nArgc = 0;
    std::vector<char*> m_vArguments;
    std::unique_ptr<QApplication> m_pApplication;
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
#endif // !RA_TESTS_UI_QT_BORROWEDQTAPPLICATION_HH
