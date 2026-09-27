#ifndef RA_DATA_EMULATORCONTEXT_HH
#define RA_DATA_EMULATORCONTEXT_HH
#pragma once

#include "RAInterface/RA_Emulators.h"

#include "data/Types.hh"

#include <memory>
#include <string>

#include "data/NotifyTargetSet.hh"
#include "data/CapturedMemoryBlock.hh"

namespace ra {
namespace data {

class AsyncHandle;

namespace context {

class EmulatorContext
{
public:
    GSL_SUPPRESS_F6 EmulatorContext() = default;
    virtual ~EmulatorContext() noexcept = default;
    EmulatorContext(const EmulatorContext&) noexcept = delete;
    EmulatorContext& operator=(const EmulatorContext&) noexcept = delete;
    EmulatorContext(EmulatorContext&&) noexcept = delete;
    EmulatorContext& operator=(EmulatorContext&&) noexcept = delete;

    /// <summary>
    /// Initializes the emulator context.
    /// </summary>
    void Initialize(EmulatorID nEmulatorId, const char* sClientName);

    /// <summary>
    /// Sets the client version.
    /// </summary>
    void SetClientVersion(const std::string& sVersion) { m_sVersion = sVersion; UpdateUserAgent(); }

    /// <summary>
    /// Gets the version of the emulator this context was initialized with.
    /// </summary>
    const std::string& GetClientVersion() const noexcept { return m_sVersion; }

    /// <summary>
    /// Validates the client version against the latest version known by the server
    /// </summary>
    /// <remarks>
    /// May show dialogs prompting the user to acknowledge their version is out of date.
    /// The first time this is called, it makes a synchronous server call.
    /// </remarks>
    /// <returns>
    /// <c>true</c> if client version is sufficient for the current configuration, 
    /// <c>false</c> if not.
    /// </returns>
    bool ValidateClientVersion();

    /// <summary>
    /// Gets the unique identifier of the emulator this context was initialized with.
    /// </summary>
    EmulatorID GetEmulatorId() const noexcept { GSL_SUPPRESS_ENUM3 return m_nEmulatorId; }

    /// <summary>
    /// Gets the client name for the emulator this context was initialized with.
    /// </summary>
    const std::string& GetClientName() const noexcept { return m_sClientName; }

    /// <summary>
    /// Sets the client version.
    /// </summary>
    void SetClientUserAgentDetail(const std::string& sDetail) { m_sClientUserAgentDetail = sDetail; UpdateUserAgent(); }

    /// <summary>
    /// Gets the Integration/X.X.X.X portion of the User-Agent clause.
    /// </summary>
    std::string GetUserAgentClause() const;

    /// <summary>
    /// Prompts the user to confirm disabling hardcore mode to perform some activity.
    /// </summary>
    /// <param name="sActivity">The activity that is not allowed in hardcore mode.</param>
    /// <returns>
    /// <c>false</c> if the user does not want to switch out of hardcore mode - the calling code should not allow
    /// the activity to occur. <c>true</c> if hardcore mode was not enabled, or the user acknowledged the switch to
    /// non-hardcore mode, in which case hardcore mode will have been disabled before returning.
    /// </returns>
    /// <remarks>
    /// If <paramref="sActivity" /> is empty, just calls <see cref="DisableHardcoreMode" /> without prompting.
    /// </remarks>
    virtual bool WarnDisableHardcoreMode(const std::string& sActivity);

    /// <summary>
    /// Disables hardcore mode and notifies other services of that fact.
    /// </summary>
    virtual void DisableHardcoreMode();

    /// <summary>
    /// Enables hardcore mode and notifies other services of that fact.
    /// </summary>
    /// <param name="bShowWarning">If <c>true</c>, the user will be prompted to confirm the switch to hardcore mode</param>
    /// <returns>
    /// <c>true</c> if hardcore mode was enabled, <c>false</c> if the user chose not to based on prompts they were given.
    /// </returns>
    virtual bool EnableHardcoreMode(bool bShowWarning = true);

    /// <summary>
    /// Builds a string for displaying in the title bar of the emulator.
    /// </summary>
    /// <param name="sMessage">A custom message provided by the emulator to also display in the title bar.</param>
    std::wstring GetAppTitle(const std::string& sMessage) const;

    /// <summary>
    /// Gets the game title from the emulator.
    /// </summary>
    std::string GetGameTitle() const;
    
    /// <summary>
    /// Notifies the emulator that the RetroAchievements menu has changed.
    /// </summary>
    void RebuildMenu() const;

    /// <summary>
    /// Updates the state of a single menu item.
    /// </summary>
    /// <remarks>Indirectly calls RebuildMenu after raising an event for the singular menu item</remarks>
    void UpdateMenuState(int nMenuItemId) const;

    /// <summary>
    /// Gets whether or not the emulator can be paused.
    /// </summary>
    bool CanPause() const noexcept { return m_fPauseEmulator != nullptr; }

    /// <summary>
    /// Pauses the emulator.
    /// </summary>
    void Pause() const;

    /// <summary>
    /// Unpauses the emulator.
    /// </summary>
    void Unpause() const;

    /// <summary>
    /// Resets the emulator.
    /// </summary>
    void Reset() const;

    /// <summary>
    /// Sets a function to call to reset the emulator.
    /// </summary>
    void SetResetFunction(std::function<void()>&& fResetEmulator) noexcept { m_fResetEmulator = std::move(fResetEmulator); }

    /// <summary>
    /// Sets a function to call to pause the emulator.
    /// </summary>
    void SetPauseFunction(std::function<void()>&& fPauseEmulator) noexcept { m_fPauseEmulator = std::move(fPauseEmulator); }

    /// <summary>
    /// Sets a function to call to unpause the emulator.
    /// </summary>
    void SetUnpauseFunction(std::function<void()>&& fUnpauseEmulator) noexcept { m_fUnpauseEmulator = std::move(fUnpauseEmulator); }

    /// <summary>
    /// Sets a function to call to get the game title from the emulator.
    /// </summary>
    void SetGetGameTitleFunction(std::function<void(char*)>&& fGetGameTitle) noexcept { m_fGetGameTitle = std::move(fGetGameTitle); }

    /// <summary>
    /// Sets a function to call to notify the emulator that the RetroAchievements menu has changed.
    /// </summary>
    void SetRebuildMenuFunction(std::function<void()>&& fRebuildMenu) noexcept { m_fRebuildMenu = std::move(fRebuildMenu); }

    /// <summary>
    /// Gets the text to display for the Accept button in the overlay navigation panel.
    /// </summary>
    const wchar_t* GetAcceptButtonText() const noexcept { return m_sAcceptButtonText; }

    /// <summary>
    /// Gets the text to display for the Cancel button in the overlay navigation panel.
    /// </summary>
    const wchar_t* GetCancelButtonText() const noexcept { return m_sCancelButtonText; }

    class DispatchesReadMemory
    {
    protected:
        static void DispatchMemoryRead(std::function<void()>&& fFunction);

        /// <summary>
        /// As the overload above, for work on an object that can be destroyed before deferred work runs - a
        /// bookmark the user removes. Deferred work runs only if the object behind <paramref name="pAsyncHandle" />
        /// still exists, and keeps it from being destroyed while it runs. Work that runs inline - on the caller's
        /// thread, before this call returns - is not guarded: the caller is still in one of the object's methods,
        /// and taking the guard there would deadlock work that dispatches again for the same object. Work that
        /// starts on another thread is always guarded, even if it starts before this call returns.
        /// </summary>
        /// <remarks>
        /// Guarded work must never destroy its own object (the destructor would wait on the guard it holds), and
        /// must never wait on the library's UI thread (a view destroying the object waits for it).
        /// </remarks>
        static void DispatchMemoryRead(std::function<void()>&& fFunction,
                                       std::shared_ptr<ra::data::AsyncHandle> pAsyncHandle);

        /// <summary>
        /// As <see cref="DispatchMemoryRead" />, for work that writes the emulator's memory: off Windows, work
        /// deferred to the frame thread is dropped if the emulator changes games or memory banks before it runs,
        /// as its address may then belong to another game (AchievementRuntime::QueueMemoryWrite).
        /// </summary>
        static void DispatchMemoryWrite(std::function<void()>&& fFunction);

        /// <summary>
        /// As the guarded <see cref="DispatchMemoryRead" /> overload, for work that writes the emulator's memory.
        /// </summary>
        static void DispatchMemoryWrite(std::function<void()>&& fFunction,
                                        std::shared_ptr<ra::data::AsyncHandle> pAsyncHandle);
    };

protected:
    void UpdateUserAgent();
    virtual bool ValidateClientVersion(bool& bHardcore);
    bool FetchLatestVersion();

    EmulatorID m_nEmulatorId = EmulatorID::UnknownEmulator;
    std::string m_sVersion;
    std::string m_sMinimumVersion;
    std::string m_sLatestVersion;
    std::string m_sLatestVersionError;
    std::string m_sClientName;
    std::string m_sClientUserAgentDetail;
    const wchar_t* m_sAcceptButtonText = L"\u24B6"; // encircled A
    const wchar_t* m_sCancelButtonText = L"\u24B7"; // encircled B

    std::function<void()> m_fResetEmulator;
    std::function<void()> m_fPauseEmulator;
    std::function<void()> m_fUnpauseEmulator;
    std::function<void(char*)> m_fGetGameTitle;
    std::function<void()> m_fRebuildMenu;
};

} // namespace context
} // namespace data
} // namespace ra

#endif // !RA_DATA_EMULATORCONTEXT_HH
