#ifndef RA_TESTS_UI_QT_MODALCALLER_HH
#define RA_TESTS_UI_QT_MODALCALLER_HH
#pragma once

#ifndef _WIN32

#include "ui/IDesktop.hh"

#include <chrono>
#include <future>
#include <memory>
#include <thread>

namespace ra {
namespace ui {
namespace qt {
namespace tests {

// ShowModal on a thread of its own, as a pool worker calls it.
class ModalCaller
{
public:
    ModalCaller(const ra::ui::IDesktop& oDesktop, ra::ui::WindowViewModelBase& vmWindow)
    {
        auto pResult = std::make_shared<std::promise<ra::ui::DialogResult>>();
        m_fResult = pResult->get_future().share();
        m_oThread = std::thread([pResult, &oDesktop, &vmWindow]() { pResult->set_value(oDesktop.ShowModal(vmWindow)); });
    }

    // Joins a caller that returned. One that never did is detached: the test
    // has already failed. If something wakes it later - a stop hook's CloseAll
    // - it reads its view model, which may be gone by then; a failure path
    // only.
    ~ModalCaller() noexcept
    {
        if (Returned(std::chrono::milliseconds(0)))
            m_oThread.join();
        else
            m_oThread.detach();
    }

    ModalCaller(const ModalCaller&) noexcept = delete;
    ModalCaller& operator=(const ModalCaller&) noexcept = delete;
    ModalCaller(ModalCaller&&) noexcept = delete;
    ModalCaller& operator=(ModalCaller&&) noexcept = delete;

    bool Returned(std::chrono::milliseconds tTimeout)
    {
        return m_fResult.wait_for(tTimeout) == std::future_status::ready;
    }

    // Only after Returned() said true.
    ra::ui::DialogResult Result() { return m_fResult.get(); }

private:
    // shared_future: get() leaves it valid, so the destructor's Returned()
    // still works after Result() was read (a std::future's get() invalidates
    // it, and wait_for on an invalid future throws - inside a noexcept
    // destructor, std::terminate; caught in Task 4).
    std::shared_future<ra::ui::DialogResult> m_fResult;
    std::thread m_oThread;
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32

#endif // !RA_TESTS_UI_QT_MODALCALLER_HH
