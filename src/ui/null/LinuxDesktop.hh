#ifndef RA_UI_NULL_LINUXDESKTOP_HH
#define RA_UI_NULL_LINUXDESKTOP_HH
#pragma once

#ifndef _WIN32

#include "services/IQtApplicationHost.hh"
#include "services/ServiceLocator.hh"
#include "services/impl/HostThreadDispatcher.hh"

#include "ui/null/NullDesktop.hh"

namespace ra {
namespace ui {
namespace null {

// NullDesktop's dialogs - none yet: every modal is logged and answered No -
// with the two threads Linux actually has. RA's own UI work goes to the Qt
// application's thread; the emulator's callbacks go back to the emulator's
// thread. Both services are looked up when used rather than held, so a
// re-initialized toolkit's new ones are the ones used.
class LinuxDesktop : public NullDesktop
{
public:
    bool IsOnUIThread() const override
    {
        const auto* pHost = GetHost();
        return (pHost == nullptr || !pHost->IsAvailable()) ? true : pHost->IsOnQtThread();
    }

    void InvokeOnUIThread(std::function<void()> fAction) const override
    {
        const auto* pHost = GetHost();
        if (pHost == nullptr || !pHost->IsAvailable())
        {
            // no Qt application: nothing to marshal to, as NullDesktop
            fAction();
            return;
        }

        pHost->Invoke(std::move(fAction));
    }

    void InvokeOnHostThread(std::function<void()> fAction) const override
    {
        if (!ra::services::ServiceLocator::Exists<ra::services::impl::HostThreadDispatcher>())
        {
            fAction();
            return;
        }

        ra::services::ServiceLocator::GetMutable<ra::services::impl::HostThreadDispatcher>().Invoke(std::move(fAction));
    }

private:
    static const ra::services::IQtApplicationHost* GetHost()
    {
        return ra::services::ServiceLocator::Exists<ra::services::IQtApplicationHost>()
                   ? &ra::services::ServiceLocator::Get<ra::services::IQtApplicationHost>()
                   : nullptr;
    }
};

} // namespace null
} // namespace ui
} // namespace ra

#endif // !_WIN32

#endif // !RA_UI_NULL_LINUXDESKTOP_HH
