#ifndef RA_DATA_GAMECONTEXT_HH
#define RA_DATA_GAMECONTEXT_HH
#pragma once

#include "context/IGameContext.hh"

#include "data/models/GameAssets.hh"
#include "data/models/LocalBadgesModel.hh"

#include "services/ServiceLocator.hh"

#include <string>
#include <atomic>
#include <functional>
#include <mutex>

struct rc_api_fetch_game_sets_response_t;

namespace ra { namespace services { class AchievementRuntimeExports; } }

namespace ra {
namespace data {
namespace context {

class GameContext : public ra::context::IGameContext
{
public:
    GSL_SUPPRESS_F6 GameContext()
        : m_IGameContextOverride(this)
    {
    }
    virtual ~GameContext() noexcept = default;
    GameContext(const GameContext&) noexcept = delete;
    GameContext& operator=(const GameContext&) noexcept = delete;
    GameContext(GameContext&&) noexcept = delete;
    GameContext& operator=(GameContext&&) noexcept = delete;

    enum class Mode
    {
        Normal,
        CompatibilityTest,
    };

    /// <summary>
    /// Loads the data for the specified game id.
    /// </summary>
    virtual void LoadGame(unsigned int nGameId, const std::string& sGameHash, Mode nMode = Mode::Normal);

    /// <summary>
    /// Loads the data from the game currently loaded in the AchievementRuntime.
    /// </summary>
    void InitializeFromAchievementRuntime(const std::map<uint32_t, std::string> mAchievementDefinitions,
                                          const std::map<uint32_t, std::string> mLeaderboardDefinitions);

    /// <summary>
    /// Determines whether a game is being loaded.
    /// </summary>
    bool IsGameLoading() const noexcept { return m_nLoadCount != 0; }

    /// <summary>
    /// Locks out anything that unloads or replaces the game while a load completion reads or writes it.
    /// </summary>
    /// <remarks>
    /// Anything that unloads the game, resets the runtime or starts a new load holds this lock while it frees or
    /// clears game state, and calls <see cref="InvalidatePendingLoads" /> first. A load completion holds it while
    /// it reads or writes game state, and checks <see cref="IsLoadCurrent" /> first. The lock is recursive, so a
    /// load completion must never call anything that can unload the game while holding it: it would not wait.
    /// </remarks>
    std::unique_lock<std::recursive_mutex> LockLoad() { return std::unique_lock<std::recursive_mutex>(m_mLoadMutex); }

    /// <summary>
    /// Marks every load that is still completing as superseded. Call while holding <see cref="LockLoad" />.
    /// </summary>
    void InvalidatePendingLoads() noexcept { ++m_nLoadGeneration; }

    /// <summary>
    /// Gets the generation that a load started now belongs to. Call while holding <see cref="LockLoad" />.
    /// </summary>
    uint32_t GetLoadGeneration() const noexcept { return m_nLoadGeneration; }

    /// <summary>
    /// Records that a load is starting, and gets the generation that it belongs to. Call while holding
    /// <see cref="LockLoad" />. A load calls it when it pauses the runtime, and again once it has unloaded the
    /// previous game, which is when it gets the generation it keeps.
    /// </summary>
    uint32_t BeginPendingLoad() noexcept
    {
        m_nPendingLoadGeneration = m_nLoadGeneration;
        m_bLoadPending = true;
        return m_nPendingLoadGeneration;
    }

    /// <summary>
    /// Determines whether nothing has unloaded or replaced the game since the load that belongs to
    /// <paramref name="nLoadGeneration" /> began. Call while holding <see cref="LockLoad" />, or the answer can
    /// change before it is acted on.
    /// </summary>
    bool IsLoadCurrent(uint32_t nLoadGeneration) const noexcept { return nLoadGeneration == m_nLoadGeneration; }

    /// <summary>
    /// Calls <paramref name="fChangeConsole" /> if the load that belongs to <paramref name="nLoadGeneration" /> is
    /// still current. These console changes never overlap, so a load that was replaced cannot change the console
    /// after the load that replaced it. Do not call while holding <see cref="LockLoad" />.
    /// </summary>
    void ChangeConsoleIfLoadCurrent(uint32_t nLoadGeneration, const std::function<void()>& fChangeConsole);

    /// <summary>
    /// Sets the game title.
    /// </summary>
    void SetGameTitle(const std::wstring& sGameTitle) { m_sGameTitle = sGameTitle; }

    /// <summary>
    /// Gets the hash of the currently loaded game.
    /// </summary>
    const std::string& GameHash() const noexcept { return m_sGameHash; }

    /// <summary>
    /// Sets the game hash.
    /// </summary>
    void SetGameHash(const std::string& sGameHash) { m_sGameHash = sGameHash; }

    /// <summary>
    /// Gets the current game play mode.
    /// </summary>
    Mode GetMode() const noexcept { return m_nMode; }

    /// <summary>
    /// Sets the game play mode.
    /// </summary>
    void SetMode(Mode nMode)
    {
        if (m_nMode != nMode)
        {
            m_nMode = nMode;

            if (m_nGameId != 0)
                OnActiveGameChanged();
        }
    }

    /// <summary>
    /// Sets the game play mode of a game that is still being loaded, without raising OnActiveGameChanged.
    /// </summary>
    /// <remarks>
    /// Call while holding <see cref="LockLoad" />, for a load that is still completing. The load raises
    /// OnActiveGameChanged itself when it completes.
    /// </remarks>
    void InitializeMode(Mode nMode) noexcept { m_nMode = nMode; }

    enum HashCompatibility
    {
        Compatible = 0,
        Incompatible = 10,
        Untested = 11,
        PatchRequired = 12,
    };

    /// <summary>
    /// Extracts the hash compatibility information from a virtual game identifier
    /// </summary>
    static constexpr HashCompatibility GetHashCompatibility(uint32_t nGameId) noexcept
    {
        return ra::itoe<HashCompatibility>(nGameId / 100000000);
    }

    /// <summary>
    /// Gets the assets for the current game.
    /// </summary>
    ra::data::models::GameAssets& Assets() noexcept { return m_vAssets; }
    const ra::data::models::GameAssets& Assets() const noexcept { return m_vAssets; }

    void DoFrame();

    void InitializeAchievementSets(const rc_api_fetch_game_sets_response_t* game_data_response);
    uint32_t GetGameId(uint32_t nSubsetId) const;

    ra::data::models::MemoryNotesModel& MemoryNotes() override
    {
        auto* pNotes = m_vAssets.FindMemoryNotes();
        return pNotes ? *pNotes : m_oDefaultNotes;
    }
    const ra::data::models::MemoryNotesModel& MemoryNotes() const override
    {
        auto* pNotes = m_vAssets.FindMemoryNotes();
        return pNotes ? *pNotes : m_oDefaultNotes;
    }

    ra::data::models::LocalBadgesModel& LocalBadges() override
    {
        auto* pLocalBadges = dynamic_cast<ra::data::models::LocalBadgesModel*>(m_vAssets.FindAsset(ra::data::models::AssetType::LocalBadges, 0));
        return pLocalBadges ? *pLocalBadges : m_oDefaultLocalBadges;
    }
    const ra::data::models::LocalBadgesModel& LocalBadges() const override
    {
        auto* pLocalBadges = dynamic_cast<const ra::data::models::LocalBadgesModel*>(m_vAssets.FindAsset(ra::data::models::AssetType::LocalBadges, 0));
        return pLocalBadges ? *pLocalBadges : m_oDefaultLocalBadges;
    }

private:
    void FinishLoadGame(int nResult, const char* sErrorMessage, bool bWasPaused, uint32_t nLoadGeneration);
    // returns true if the load count reached zero. the caller must then call OnEndGameLoad, after it has released
    // the load lock.
    bool DiscardLoad(uint32_t nLoadGeneration, bool bWasPaused);
    bool DecrementLoadCount();
    void OnEndGameLoad();
    // a load that has not completed yet, and that nothing has superseded. call while holding the load lock.
    bool IsLoadPending() const noexcept { return m_bLoadPending && m_nPendingLoadGeneration == m_nLoadGeneration; }
    void MigrateSubsetUserFiles();

    friend class ra::services::AchievementRuntimeExports;
    bool BeginLoadGame(unsigned int nGameId, Mode nMode, bool& bWasPaused);
    // returns false if the load was discarded because something unloaded or replaced the game since it began
    bool EndLoadGame(int nResult, bool bWasPaused, bool bShowCasualWarning, uint32_t nLoadGeneration);

protected:
    void OnBeforeActiveGameChanged();
    void OnActiveGameChanged();
    void OnMemoryNoteChanged(ra::data::ByteAddress nAddress, const std::wstring& sNewNote);
    void OnMemoryNoteMoved(ra::data::ByteAddress nOldAddress, ra::data::ByteAddress nNewAddress, const std::wstring& sNote);
    void BeginLoad();
    void EndLoad();

    std::string m_sGameHash;
    Mode m_nMode{};

private:
    ra::data::models::GameAssets m_vAssets;

    std::atomic<int> m_nLoadCount = 0;
    int m_nMasteryPopupId = 0;

    std::recursive_mutex m_mLoadMutex;
    std::atomic<uint32_t> m_nLoadGeneration = 0;
    // the generation of the last load that started, until that load completes. a load that was superseded does
    // not count as pending: its generation is no longer the current one. only read or written while holding
    // m_mLoadMutex.
    uint32_t m_nPendingLoadGeneration = 0;
    bool m_bLoadPending = false;
    // a discarded load left the runtime paused for the load that replaced it, which began while it was paused and
    // is still pending. that load unpauses the runtime when it completes or is discarded. only read or written
    // while holding m_mLoadMutex.
    bool m_bUnpauseOwed = false;
    // held from a load's last check that it is current until it has changed the console
    std::mutex m_mConsoleChangeMutex;

    // use the mock helper to register as both GameContext and IGameContext
    ra::services::ServiceLocator::ServiceOverride<ra::context::IGameContext> m_IGameContextOverride;
    ra::data::models::MemoryNotesModel m_oDefaultNotes;
    ra::data::models::LocalBadgesModel m_oDefaultLocalBadges;
};

} // namespace context
} // namespace data
} // namespace ra

#endif // !RA_DATA_GAMECONTEXT_HH
