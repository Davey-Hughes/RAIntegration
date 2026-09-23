#ifndef RA_UI_NULL_DESKTOP_HH
#define RA_UI_NULL_DESKTOP_HH
#pragma once

#include "ui/IDesktop.hh"

#include "util/Log.hh"

namespace ra {
namespace ui {
namespace null {

// Satisfies the composition root on platforms with no view layer yet. Every
// dialog request is dropped, not queued - a caller that needs an answer gets
// DialogResult::None, which the view models already treat as "cancelled".
class NullDesktop : public ra::ui::IDesktop
{
public:
    void ShowWindow(WindowViewModelBase&) const override {}

    ra::ui::DialogResult ShowModal(WindowViewModelBase&) const override
    {
        return ra::ui::DialogResult::None;
    }

    ra::ui::DialogResult ShowModal(WindowViewModelBase&, const WindowViewModelBase&) const override
    {
        return ra::ui::DialogResult::None;
    }

    void CloseWindow(WindowViewModelBase&) const override {}

    void GetWorkArea(_Out_ ra::ui::Position& oUpperLeftCorner, _Out_ ra::ui::Size& oSize) const override
    {
        oUpperLeftCorner.X = 0;
        oUpperLeftCorner.Y = 0;
        oSize.Width = 1920;
        oSize.Height = 1080;
    }

    ra::ui::Size GetClientSize(const WindowViewModelBase&) const override { return {0, 0}; }

    std::wstring GetRunningExecutable() const override { return m_sExecutable; }

    std::string GetOSVersionString() const override { return "Linux"; }

    void OpenUrl(const std::string& sUrl) const override { RA_LOG_INFO("OpenUrl: %s", sUrl.c_str()); }

    std::unique_ptr<ra::ui::drawing::ISurface> CaptureClientArea(const WindowViewModelBase&) const override
    {
        return nullptr;
    }

    bool IsOnUIThread() const override { return true; }

    void InvokeOnUIThread(std::function<void()> fAction) const override { fAction(); }

    void Shutdown() override {}

private:
    std::wstring m_sExecutable;
};

} // namespace null
} // namespace ui
} // namespace ra

#endif // !RA_UI_NULL_DESKTOP_HH
