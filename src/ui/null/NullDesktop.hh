#ifndef RA_UI_NULL_DESKTOP_HH
#define RA_UI_NULL_DESKTOP_HH
#pragma once

#include "ui/IDesktop.hh"
#include "ui/drawing/null/NullSurface.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <sys/utsname.h>

#include <filesystem>
#include <system_error>

namespace ra {
namespace ui {
namespace null {

// Satisfies the composition root on platforms with no view layer yet. Every
// dialog request is dropped, not queued, because there is nothing to show it
// on. No answer is truthfully correct here, but DialogResult::None is the
// worst of the choices available: WindowViewModelBase::ShowModal returns it
// verbatim, and most call sites test for the *negative* result (No/Cancel)
// rather than the positive one, so None silently takes the affirmative branch
// of destructive confirmations - discarding unsaved edits, deleting
// achievements, enabling hardcore mode with no prompt ever shown.
// DialogResult::No is returned instead, as the least-destructive answer at
// those sites. It is NOT correct everywhere: EmulatorContext's version-check
// prompt is OK/Cancel and treats anything but Cancel as "proceed", so No
// steers it down the same wrong branch None did. Every suppressed dialog is
// logged so a headless harness can see a decision was made on its behalf.
class NullDesktop : public ra::ui::IDesktop
{
public:
    NullDesktop() noexcept
    {
        // Same /proc/self/exe resolution LinuxFileSystem's constructor uses,
        // and the same failure handling: if procfs isn't available, leave
        // m_sExecutable at its default-constructed empty value rather than
        // guessing.
        std::error_code oError;
        const auto oExe = std::filesystem::read_symlink("/proc/self/exe", oError);
        if (!oError)
            m_sExecutable = ra::util::String::Widen(oExe.string());
    }

    void ShowWindow(WindowViewModelBase&) const override {}

    ra::ui::DialogResult ShowModal(WindowViewModelBase& vmViewModel) const override
    {
        RA_LOG_WARN("No view layer to show dialog \"%s\" - returning No",
                     ra::util::String::Narrow(vmViewModel.GetWindowTitle()).c_str());
        return ra::ui::DialogResult::No;
    }

    ra::ui::DialogResult ShowModal(WindowViewModelBase& vmViewModel, const WindowViewModelBase&) const override
    {
        RA_LOG_WARN("No view layer to show dialog \"%s\" - returning No",
                     ra::util::String::Narrow(vmViewModel.GetWindowTitle()).c_str());
        return ra::ui::DialogResult::No;
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

    std::string GetOSVersionString() const override
    {
        utsname oInfo{};
        if (::uname(&oInfo) == 0)
            return ra::util::String::Printf("%s %s", oInfo.sysname, oInfo.release);

        return "Linux";
    }

    void OpenUrl(const std::string& sUrl) const override { RA_LOG_INFO("OpenUrl: %s", sUrl.c_str()); }

    std::unique_ptr<ra::ui::drawing::ISurface> CaptureClientArea(const WindowViewModelBase&) const override
    {
        // Never nullptr: OverlayManager::CaptureScreenshot stores this with no
        // null check and later dereferences it unconditionally. It is
        // currently unreachable only because NullImageRepository::
        // IsImageAvailable always returns false; an empty surface keeps that
        // pairing safe instead of relying on it.
        return std::make_unique<ra::ui::drawing::null::NullSurface>(0, 0);
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
