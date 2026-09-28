#ifndef RA_SERVICES_ACHIEVEMENT_RUNTIME_HH
#define RA_SERVICES_ACHIEVEMENT_RUNTIME_HH
#pragma once

#include "data/Types.hh"
#include "data/context/EmulatorContext.hh"
#include "data/models/AchievementModel.hh"

#include "services/IThreadPool.hh"
#include "services/ServiceLocator.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <rcheevos/include/rc_client.h>

struct rc_api_fetch_game_sets_response_t;

namespace ra {
namespace services {

class AchievementRuntime
{
public:
    GSL_SUPPRESS_F6 AchievementRuntime();
    virtual ~AchievementRuntime();

    AchievementRuntime(const AchievementRuntime&) noexcept = delete;
    AchievementRuntime& operator=(const AchievementRuntime&) noexcept = delete;
    AchievementRuntime(AchievementRuntime&&) noexcept = delete;
    AchievementRuntime& operator=(AchievementRuntime&&) noexcept = delete;

    void BeginLoginWithToken(const std::string& sUsername, const std::string& sApiToken,
                             rc_client_callback_t fCallback, void* pCallbackData);
    void BeginLoginWithPassword(const std::string& sUsername, const std::string& sPassword,
                                rc_client_callback_t fCallback, void* pCallbackData);

    void BeginLoadGame(const std::string& sHash, unsigned id,
                       rc_client_callback_t fCallback, void* pCallbackData);
    void UnloadGame();

    /// <summary>
    /// Clears all active achievements/leaderboards/rich presence from the runtime.
    /// </summary>
    void ResetRuntime();

    static std::string GetAchievementBadge(const rc_client_achievement_t& pAchievement);

    void InitializeRcClient();
    void RaiseClientEvent(rc_client_achievement_info_t& pAchievement, uint32_t nEventType) const;

    void UpdateActiveAchievements() noexcept(false);
    void UpdateActiveLeaderboards();

    void InvalidateAddress(ra::data::ByteAddress nAddress);

    /// <summary>
    /// Processes all active achievements for the current frame.
    /// </summary>
    void DoFrame();

    /// <summary>
    /// Processes stuff not related to a frame.
    /// </summary>
    void Idle() const;

    /// <summary>
    /// Loads HitCount data for active achievements from a save state file.
    /// </summary>
    /// <param name="sLoadStateFilename">The name of the save state file.</param>
    /// <returns><c>true</c> if the achievement HitCounts were modified, <c>false</c> if not.</returns>
    bool LoadProgressFromFile(const char* sLoadStateFilename);

    /// <summary>
    /// Loads HitCount data for active achievements from a buffer.
    /// </summary>
    /// <param name="pBuffer">The buffer to read from.</param>
    /// <returns><c>true</c> if the achievement HitCounts were modified, <c>false</c> if not.</returns>
    bool LoadProgressFromBuffer(const uint8_t* pBuffer) noexcept(false);

    /// <summary>
    /// Writes HitCount data for active achievements to a save state file.
    /// </summary>
    /// <param name="sLoadStateFilename">The name of the save state file.</param>
    void SaveProgressToFile(const char* sSaveStateFilename) const;

    /// <summary>
    /// Writes HitCount data for active achievements to a buffer.
    /// </summary>
    /// <param name="pBuffer">The buffer to write to.</param>
    /// <param name="nBufferSize">The size of the buffer to write to.</param>
    /// <returns>
    /// The numberof bytes required to capture the HitCount data (may be larger than 
    /// nBufferSize - in which case the caller should allocate the specified amount
    /// and call again.
    /// </returns>
    int SaveProgressToBuffer(uint8_t* pBuffer, int nBufferSize) const noexcept(false);

    /// <summary>
    /// Gets whether achievement processing is temporarily suspended.
    /// </summary>
    bool IsPaused() const noexcept { return m_bPaused; }

    /// <summary>
    /// Sets whether achievement processing should be temporarily suspended.
    /// </summary>
    void SetPaused(bool bValue) noexcept { m_bPaused = bValue; }

    void QueueMemoryRead(std::function<void()>&& fCallback) const;

    /// <summary>
    /// As <see cref="QueueMemoryRead" />, for work that writes the emulator's memory: off Windows, work deferred to
    /// the frame thread is dropped if the emulator changes games or memory banks before it runs.
    /// </summary>
    void QueueMemoryWrite(std::function<void()>&& fCallback) const;

    /// <summary>
    /// Makes memory writes queued so far - off Windows, where <see cref="QueueMemoryWrite" /> defers work made off
    /// the frame thread - do nothing when they come up. Called when the emulator changes games or memory banks,
    /// after which a queued write's address may belong to another game. Queued reads still run: they read the
    /// current state when they do.
    /// </summary>
    static void InvalidateQueuedMemoryWork() noexcept;

    /// <summary>Gets how many queued memory writes have been dropped in this process after being invalidated.</summary>
    static uint32_t DroppedQueuedMemoryWorkCount() noexcept;

    bool IsOnDoFrameThread() const noexcept
    {
        const auto nFrameThread = m_hDoFrameThread.load();
        return nFrameThread != std::thread::id{} && std::this_thread::get_id() == nFrameThread;
    }

    /// <summary>
    /// Returns <c>true</c> only when frames are known to run on another thread than the caller's - not before the
    /// first frame, when nothing is known.
    /// </summary>
    bool IsOffDoFrameThread() const noexcept
    {
        const auto nFrameThread = m_hDoFrameThread.load();
        return nFrameThread != std::thread::id{} && std::this_thread::get_id() != nFrameThread;
    }

    /// <summary>
    /// Waits on a thread of its own for an rc_client callback. Create it with std::make_shared, and hand the callback
    /// the userdata <see cref="Share" /> returns: the wait gives up at shutdown, and a callback that arrives after
    /// that must still find the object alive.
    /// </summary>
    class Synchronizer
    {
    public:
        /// <summary>
        /// Waits for <see cref="Complete" /> - or, once shutdown has started, gives up with RC_ABORTED: rc_client_destroy
        /// drops a pending callback, and the thread pool drops a request it has not started, so it might never come.
        /// Returns whether it completed. False means shutdown has begun: the caller reports nothing - nobody should
        /// see a box for an abandoned request.
        /// </summary>
        bool Wait()
        {
#ifdef RA_UTEST
            // unit tests are single-threaded. if it has not completed yet, it would wait indefinitely.
            if (!m_bDone)
                Microsoft::VisualStudio::CppUnitTestFramework::Assert::Fail(L"Sycnhronous request was not handled.");

            return true;
#else
            return WaitUntil(
                []() { return ra::services::ServiceLocator::Get<ra::services::IThreadPool>().IsShutdownRequested(); },
                std::chrono::milliseconds(100));
#endif
        }

        /// <summary>
        /// Waits until <see cref="Complete" /> runs, or until <paramref name="fGiveUp" /> - asked every
        /// <paramref name="tPoll" />, under this object's lock, so it must not call Complete - says to stop. On giving
        /// up, the result is RC_ABORTED, "Shutting down", and a later Complete is ignored. Returns whether it
        /// completed. In every build: the tests drive it from two threads.
        /// </summary>
        bool WaitUntil(const std::function<bool()>& fGiveUp, std::chrono::milliseconds tPoll)
        {
            std::unique_lock<std::mutex> lock(m_pMutex);
            while (!m_bDone)
            {
                // a predicate, so a spurious wake-up is not taken for a completion
                if (m_pCondVar.wait_for(lock, tPoll, [this]() noexcept { return m_bDone; }))
                    break;

                if (fGiveUp())
                {
                    m_nResult = RC_ABORTED;
                    m_sErrorMessage = "Shutting down";
                    m_bDone = true;
                    return false;
                }
            }

            return true;
        }

        /// <summary>
        /// The callback's side: records the result and wakes the waiter - unless the waiter has given up, when the
        /// result is dropped: nobody reads it any more. Any thread.
        /// </summary>
        void Complete(int nResult, const char* sErrorMessage)
        {
            std::lock_guard<std::mutex> lock(m_pMutex);
            if (m_bDone)
                return;

            m_nResult = nResult;
            if (sErrorMessage)
                m_sErrorMessage = sErrorMessage;
            else
                m_sErrorMessage.clear();

            m_bDone = true;
            m_pCondVar.notify_all();
        }

        /// <summary>
        /// Userdata for an rc_client callback: a heap copy of <paramref name="pSynchronizer" />, which
        /// <see cref="CompleteShared" /> releases. A callback that never comes - dropped at shutdown - leaks it: the
        /// process is ending.
        /// </summary>
        static void* Share(std::shared_ptr<Synchronizer> pSynchronizer)
        {
            GSL_SUPPRESS_R3
            return new std::shared_ptr<Synchronizer>(std::move(pSynchronizer));
        }

        /// <summary>
        /// The callback: completes the synchronizer <paramref name="pUserdata" /> (from <see cref="Share" />) holds,
        /// and releases the copy.
        /// </summary>
        static void CompleteShared(void* pUserdata, int nResult, const char* sErrorMessage)
        {
            auto* pShared = static_cast<std::shared_ptr<Synchronizer>*>(pUserdata);
            Expects(pShared != nullptr && *pShared != nullptr);

            (*pShared)->Complete(nResult, sErrorMessage);

            delete pShared;
        }

        // Read after Wait returns: Complete wrote them under the lock before waking it, and a later Complete is
        // ignored.
        int GetResult() const noexcept { return m_nResult; }

        const std::string& GetErrorMessage() const noexcept { return m_sErrorMessage; }

    private:
        std::mutex m_pMutex;
        std::condition_variable m_pCondVar;
        std::string m_sErrorMessage;
        int m_nResult = 0;
        bool m_bDone = false;
    };

protected:
    AchievementRuntime(bool bInitializeRcClient);

private:
    bool m_bPaused = false;
    // written by DoFrame() on the frame thread, read from any thread (QueueMemoryWork, IsOnDoFrameThread)
    std::atomic<std::thread::id> m_hDoFrameThread{};

    int m_nRichPresenceParseResult = RC_OK;
    int m_nRichPresenceErrorLine = 0;

    // QueueMemoryRead and QueueMemoryWrite; only a write is dropped by InvalidateQueuedMemoryWork
    void QueueMemoryWork(std::function<void()>&& fCallback, bool bDropIfStale) const;

    static uint32_t ReadMemory(uint32_t nAddress, uint8_t* pBuffer, uint32_t nBytes, rc_client_t* pClient);
    static void EventHandler(const rc_client_event_t* pEvent, rc_client_t* pClient);

    class CallbackWrapper
    {
    public:
        CallbackWrapper(rc_client_t* client, rc_client_callback_t callback, void* callback_userdata) noexcept :
            m_pClient(client), m_fCallback(callback), m_pCallbackUserdata(callback_userdata)
        {}

        void DoCallback(int nResult, const char* sErrorMessage) noexcept
        {
            m_fCallback(nResult, sErrorMessage, m_pClient, m_pCallbackUserdata);
        }

        static void Dispatch(int nResult, const char* sErrorMessage, rc_client_t*, void* pUserdata)
        {
            auto* pWrapper = static_cast<CallbackWrapper*>(pUserdata);
            Expects(pWrapper != nullptr);

            pWrapper->DoCallback(nResult, sErrorMessage);

            delete pWrapper;
        }

    private:
        rc_client_t* m_pClient;
        rc_client_callback_t m_fCallback;
        void* m_pCallbackUserdata;
    };

    friend class AchievementRuntimeExports;

    rc_client_async_handle_t* BeginLoginWithPassword(const char* sUsername, const char* sPassword,
                                                     CallbackWrapper* pCallbackWrapper);
    rc_client_async_handle_t* BeginLoginWithToken(const char* sUsername, const char* sApiToken,
                                                  CallbackWrapper* pCallbackWrapper);
    static void LoginCallback(int nResult, const char* sErrorMessage, rc_client_t* pClient, void* pUserdata);

    class LoadGameCallbackWrapper : public CallbackWrapper
    {
    public:
        LoadGameCallbackWrapper(rc_client_t* client, rc_client_callback_t callback, void* callback_userdata) noexcept :
            CallbackWrapper(client, callback, callback_userdata)
        {}

        std::map<uint32_t, std::string> m_mAchievementDefinitions;
        std::map<uint32_t, std::string> m_mLeaderboardDefinitions;
    };

    rc_client_async_handle_t* BeginLoadGame(const char* sHash, unsigned id, CallbackWrapper* pCallbackWrapper);
    static void LoadGameCallback(int nResult, const char* sErrorMessage, rc_client_t* pClient, void* pUserdata);

    rc_client_async_handle_t* BeginIdentifyAndLoadGame(uint32_t console_id, const char* file_path,
                                                       const uint8_t* data, size_t data_size,
                                                       CallbackWrapper* pCallbackWrapper);

    rc_client_async_handle_t* BeginIdentifyAndChangeMedia(const char* file_path, const uint8_t* data, size_t data_size,
                                                          CallbackWrapper* pCallbackWrapper);
    rc_client_async_handle_t* BeginChangeMedia(const char* sHash, CallbackWrapper* pCallbackWrapper);
    static void ChangeMediaCallback(int nResult, const char* sErrorMessage, rc_client_t*, void* pUserdata);

    static void PostProcessGameDataResponse(const rc_api_server_response_t* server_response,
                                            struct rc_api_fetch_game_sets_response_t* game_data_response,
                                            rc_client_t* client, void* pUserdata);
};

} // namespace services
} // namespace ra

extern "C" unsigned int rc_peek_callback(unsigned int nAddress, uint8_t* buffer, unsigned int nBytes, _UNUSED void* pData);

#endif // !RA_SERVICES_ACHIEVEMENT_RUNTIME_HH
