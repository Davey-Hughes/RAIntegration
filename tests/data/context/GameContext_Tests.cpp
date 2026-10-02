#include "CppUnitTest.h"

#include "Exports.hh"

#include "data\context\GameContext.hh"

#include "data\models\AchievementModel.hh"

#include "services\AchievementRuntime.hh"
#include "services\AchievementRuntimeExports.hh"

#include "ui\viewmodels\MessageBoxViewModel.hh"
#include "ui\viewmodels\UnknownGameViewModel.hh"

#include <rcheevos\src\rc_version.h>
#include <rcheevos\include\rc_api_runtime.h>
#include <rcheevos\src\rc_client_external.h>
#include <rcheevos\src\rc_client_internal.h>

#include "tests\RA_UnitTestHelpers.h"
#include "tests\data\DataAsserts.hh"
#include "tests\ui\UIAsserts.hh"

#include "tests\devkit\context\mocks\MockConsoleContext.hh"
#include "tests\devkit\context\mocks\MockDevKitContext.hh"
#include "tests\devkit\context\mocks\MockEmulatorMemoryContext.hh"
#include "tests\devkit\context\mocks\MockRcClient.hh"
#include "tests\devkit\context\mocks\MockUserContext.hh"
#include "tests\devkit\services\mocks\MockClock.hh"
#include "tests\devkit\services\mocks\MockConfiguration.hh"
#include "tests\devkit\services\mocks\MockLocalStorage.hh"
#include "tests\devkit\services\mocks\MockMessageDispatcher.hh"
#include "tests\devkit\services\mocks\MockThreadPool.hh"
#include "tests\devkit\testutil\AchievementSetAsserts.hh"
#include "tests\devkit\testutil\AssetAsserts.hh"
#include "tests\devkit\ui\mocks\MockImageRepository.hh"
#include "tests\mocks\MockAchievementRuntime.hh"
#include "tests\mocks\MockAudioSystem.hh"
#include "tests\mocks\MockEmulatorContext.hh"
#include "tests\mocks\MockDesktop.hh"
#include "tests\mocks\MockFrameEventQueue.hh"
#include "tests\mocks\MockGameContext.hh"
#include "tests\mocks\MockLoginService.hh"
#include "tests\mocks\MockOverlayManager.hh"
#include "tests\mocks\MockServer.hh"
#include "tests\mocks\MockSessionTracker.hh"
#include "tests\mocks\MockWindowManager.hh"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

extern "C" API int CCONV _Rcheevos_GetExternalClient(rc_client_external_t* pClientExternal, int nVersion);

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

using ra::services::StorageItemType;

namespace ra {
namespace data {
namespace context {
namespace tests {

TEST_CLASS(GameContext_Tests)
{
private:
    static void RemoveFirstLine(std::string& sString)
    {
        const auto nIndex = sString.find('\n');
        if (nIndex == std::string::npos)
            sString.clear();
        else
            sString.erase(0, nIndex + 1);
    }

public:
    class GameContextHarness : public GameContext
    {
    public:
        GameContextHarness() noexcept :
            m_OverrideGameContext(this)
        {
        }

        ra::api::mocks::MockServer mockServer;
        ra::context::mocks::MockEmulatorMemoryContext mockEmulatorMemoryContext;
        ra::context::mocks::MockDevKitContext mockDevKitContext;
        ra::context::mocks::MockRcClient mockRcClient;
        ra::context::mocks::MockUserContext mockUser;
        ra::services::mocks::MockClock mockClock;
        ra::services::mocks::MockConfiguration mockConfiguration;
        ra::services::mocks::MockLocalStorage mockStorage;
        ra::services::mocks::MockThreadPool mockThreadPool;
        ra::services::mocks::MockAudioSystem mockAudioSystem;
        ra::services::mocks::MockAchievementRuntime mockAchievementRuntime;
        ra::services::mocks::MockFrameEventQueue mockFrameEventQueue;
        ra::services::mocks::MockMessageDispatcher mockMessageDispatcher;
        ra::ui::mocks::MockImageRepository mockImageRepository;
        ra::ui::viewmodels::mocks::MockOverlayManager mockOverlayManager;
        ra::context::mocks::MockConsoleContext mockConsoleContext;
        ra::data::context::mocks::MockEmulatorContext mockEmulator;
        ra::data::context::mocks::MockSessionTracker mockSessionTracker;
        ra::ui::mocks::MockDesktop mockDesktop;
        ra::ui::viewmodels::mocks::MockWindowManager mockWindowManager;

        void SetGameId(unsigned int nGameId) noexcept { m_nGameId = nGameId; }

        void SetRichPresenceFromFile(bool bValue)
        {
            auto* pRichPresence = GetRichPresence();
            Expects(pRichPresence != nullptr);

            if (bValue)
                pRichPresence->SetScript("Display:\nThis differs\n");
            else
                pRichPresence->SetScript("Display:\nTest\n");
        }

        void ReloadRichPresenceScript()
        {
            auto* pRichPresence = GetRichPresence();
            Expects(pRichPresence != nullptr);
            pRichPresence->ReloadRichPresenceScript();
        }

        bool HasRichPresence() const
        {
            return Assets().FindRichPresence() != nullptr;
        }

        bool IsRichPresenceFromFile() const
        {
            const auto* pRichPresence = Assets().FindRichPresence();
            return pRichPresence != nullptr && pRichPresence->GetChanges() != ra::data::models::AssetChanges::None;
        }

        ra::data::models::AchievementModel& MockAchievement()
        {
            return ra::data::context::mocks::MockGameContext::MockAchievement(Assets(), ra::data::models::AssetCategory::Promoted);
        }

        ra::data::models::LeaderboardModel& MockLeaderboard()
        {
            return ra::data::context::mocks::MockGameContext::MockLeaderboard(Assets(), ra::data::models::AssetCategory::Promoted);
        }

        void RemoveNonAchievementAssets()
        {
            for (gsl::index nIndex = Assets().Count() - 1; nIndex >= 0; nIndex--)
            {
                auto* pAsset = Assets().GetItemAt(nIndex);
                if (!pAsset || pAsset->GetType() != ra::data::models::AssetType::Achievement)
                    Assets().RemoveAt(nIndex);
            }
        }

        void MockLoadGameAPIs(unsigned nGameID, const std::string& sHash,
            const std::string& sUnlocks = "", const std::string& sAchievements = "",
            const std::string& sLeaderboards = "", const std::string& sRichPresence = "",
            ConsoleID nConsoleID = ConsoleID::MegaDrive)
        {
            MockLoadGameAPIsWithoutSession(nGameID, sHash, sAchievements, sLeaderboards, sRichPresence, nConsoleID);
            MockStartSession(nGameID, sHash, sUnlocks);
        }

        // mocks everything a load needs but the session, so the load waits for MockStartSession
        void MockLoadGameAPIsWithoutSession(unsigned nGameID, const std::string& sHash,
            const std::string& sAchievements = "", const std::string& sLeaderboards = "",
            const std::string& sRichPresence = "", ConsoleID nConsoleID = ConsoleID::MegaDrive)
        {
            mockConsoleContext.SetId(nConsoleID);
            mockConsoleContext.SetName(ra::util::String::Widen(rc_console_name(static_cast<int>(nConsoleID))));

            mockAchievementRuntime.MockUser("Username", "ApiToken");
            mockRcClient.MockResponse(
                "r=achievementsets&u=Username&t=ApiToken&m=" + sHash + "&v=2",
                "{\"Success\":true,"
                  "\"GameId\":" + std::to_string(nGameID) + ","
                  "\"Title\":\"GameTitle\","
                  "\"ConsoleId\":" + std::to_string(ra::etoi(nConsoleID)) + ","
                  "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                  "\"RichPresencePatch\":\"" + sRichPresence + "\","
                  "\"Sets\":[{"
                    "\"AchievementSetId\":1111,"
                    "\"GameId\":" + std::to_string(nGameID) + ","
                    "\"Title\":null,\"Type\":\"core\","
                    "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                    "\"Achievements\":[" + sAchievements + "],"
                    "\"Leaderboards\":[" + sLeaderboards + "]"
                "}]}");
        }

        // mocks the session request. a load that is waiting for it completes inside this call.
        void MockStartSession(unsigned nGameID, const std::string& sHash, const std::string& sUnlocks = "")
        {
            mockRcClient.MockResponse(
                "r=startsession&u=Username&t=ApiToken&g=" + std::to_string(nGameID) +
                    "&h=1&m=" + sHash + "&l=" RCHEEVOS_VERSION_STRING,
                sUnlocks.empty() ? "{\"Success\":true}" : "{\"Success\":true," + sUnlocks + "}");
        }

        std::string MockAchievementJson(unsigned nAchievementID, unsigned nPoints,
            const std::string sMemAddr = "1=1")
        {
            const std::string sAchievementID = std::to_string(nAchievementID);
            return "{\"ID\":" + sAchievementID + ",\"Title\":\"Ach" + sAchievementID + "\"," +
                "\"Description\":\"Desc" + sAchievementID + "\",\"Flags\":3," +
                "\"Points\":" + std::to_string(nPoints) + ",\"MemAddr\":\"" + sMemAddr + "\"," +
                "\"Author\":\"User1\",\"BadgeName\":\"00234\"," +
                "\"Created\":1367266583,\"Modified\":1376929305}";
        }

    private:
        ra::data::models::RichPresenceModel* GetRichPresence()
        {
            auto* pRichPresence = Assets().FindRichPresence();
            if (!pRichPresence)
            {
                auto pNewRichPresence = std::make_unique<ra::data::models::RichPresenceModel>();
                pNewRichPresence->SetScript("Display:\nTest\n");
                pNewRichPresence->CreateServerCheckpoint();
                pNewRichPresence->CreateLocalCheckpoint();
                Assets().Append(std::move(pNewRichPresence));

                pRichPresence = Assets().FindRichPresence();
            }

            return pRichPresence;
        }

        ra::services::ServiceLocator::ServiceOverride<ra::data::context::GameContext> m_OverrideGameContext;
    };

    // holds a load's completion inside its first badge prefetch, where it is walking the loaded game while
    // holding the load lock
    class PrefetchGate
    {
    public:
        explicit PrefetchGate(ra::ui::mocks::MockImageRepository& pImageRepository)
        {
            pImageRepository.SetFetchImageHandler([this](ra::ui::ImageType, const std::string&) { Hold(); });
        }

        // waits for the completion running on tCompletion to reach the prefetch. if it never does, fails the test
        // instead of waiting forever.
        void WaitUntilHeld(std::thread& tCompletion)
        {
            bool bHeld = false;
            {
                std::unique_lock<std::mutex> lock(m_mMutex);
                bHeld = m_cvChanged.wait_for(lock, std::chrono::seconds(10), [this]() noexcept { return m_bHeld; });
            }

            if (!bHeld)
            {
                // the completion uses the test's objects. let it through and wait for it before failing.
                Release();
                tCompletion.join();
                Assert::Fail(L"the load's completion did not reach the badge prefetch");
            }
        }

        void Release()
        {
            {
                std::lock_guard<std::mutex> lock(m_mMutex);
                m_bReleased = true;
            }
            m_cvChanged.notify_all();
        }

    private:
        void Hold()
        {
            std::unique_lock<std::mutex> lock(m_mMutex);
            if (m_bHeld)
                return;

            m_bHeld = true;
            m_cvChanged.notify_all();
            m_cvChanged.wait(lock, [this]() noexcept { return m_bReleased; });
        }

        std::mutex m_mMutex;
        std::condition_variable m_cvChanged;
        bool m_bHeld = false;
        bool m_bReleased = false;
    };

    // records whether the load lock is free when a notification is raised, by having another thread take it
    class LoadLockProbe : public GameContext::NotifyTarget
    {
    public:
        explicit LoadLockProbe(GameContext& pGameContext) noexcept : m_pGameContext(pGameContext) {}
        GSL_SUPPRESS_F6 ~LoadLockProbe() noexcept { Join(); }

        LoadLockProbe(const LoadLockProbe&) noexcept = delete;
        LoadLockProbe& operator=(const LoadLockProbe&) noexcept = delete;
        LoadLockProbe(LoadLockProbe&&) noexcept = delete;
        LoadLockProbe& operator=(LoadLockProbe&&) noexcept = delete;

        int nNotified = 0;
        bool bLockWasFree = false;

        // waits for the other thread. call after the load lock has been released, and before asserting. the
        // destructor does it too, so that a test that throws first fails instead of ending the process.
        void Join()
        {
            if (m_tProbe.joinable())
                m_tProbe.join();
        }

    protected:
        // call from the notification being probed
        void Probe()
        {
            if (++nNotified > 1)
                return;

            m_tProbe = std::thread([this]() {
                auto pLock = m_pGameContext.LockLoad();
                {
                    std::lock_guard<std::mutex> lock(m_mMutex);
                    m_bLocked = true;
                }
                m_cvChanged.notify_all();
            });

            // if this thread is holding the load lock, the other thread cannot take it until this returns
            std::unique_lock<std::mutex> lock(m_mMutex);
            bLockWasFree = m_cvChanged.wait_for(lock, std::chrono::seconds(5), [this]() noexcept { return m_bLocked; });
        }

    private:
        GameContext& m_pGameContext;
        std::thread m_tProbe;
        std::mutex m_mMutex;
        std::condition_variable m_cvChanged;
        bool m_bLocked = false;
    };

    class EndGameLoadLockProbe : public LoadLockProbe
    {
    public:
        using LoadLockProbe::LoadLockProbe;

    protected:
        void OnEndGameLoad() override { Probe(); }
    };

    class ActiveGameChangedLockProbe : public LoadLockProbe
    {
    public:
        using LoadLockProbe::LoadLockProbe;

    protected:
        void OnActiveGameChanged() override { Probe(); }
    };

    // counts OnActiveGameChanged, and records whether the last one found achievement 5 loaded in compatibility mode
    class CompatibilityModeObserver : public GameContext::NotifyTarget
    {
    public:
        explicit CompatibilityModeObserver(const GameContext& pGameContext) noexcept : m_pGameContext(pGameContext) {}

        int nNotified = 0;
        bool bLoadedForTesting = false;

    protected:
        void OnActiveGameChanged() override
        {
            ++nNotified;
            bLoadedForTesting = (m_pGameContext.GetMode() == GameContext::Mode::CompatibilityTest &&
                m_pGameContext.Assets().FindAchievement(5U) != nullptr);
        }

    private:
        const GameContext& m_pGameContext;
    };

    // _Rcheevos_GetExternalClient puts the library in external client mode. leaves it as the next test expects.
    struct ExternalClientReset
    {
        ExternalClientReset() noexcept = default;
        ~ExternalClientReset() noexcept { ResetExternalRcheevosClient(); }

        ExternalClientReset(const ExternalClientReset&) noexcept = delete;
        ExternalClientReset& operator=(const ExternalClientReset&) noexcept = delete;
        ExternalClientReset(ExternalClientReset&&) noexcept = delete;
        ExternalClientReset& operator=(ExternalClientReset&&) noexcept = delete;
    };

    class GameContextNotifyTarget : public GameContext::NotifyTarget
    {
    public:
        bool GetActiveGameChanged() const noexcept { return m_bActiveGameChanged; }

        const std::wstring* GetNewMemoryNote(ra::data::ByteAddress nAddress)
        {
            const auto pIter = m_vNotesChanged.find(nAddress);
            if (pIter != m_vNotesChanged.end())
                return &pIter->second;

            return nullptr;
        }

    protected:
        void OnActiveGameChanged() noexcept override
        {
            m_bActiveGameChanged = true;
            m_vNotesChanged.clear();
        }

        void OnMemoryNoteChanged(ra::data::ByteAddress nAddress, const std::wstring& sNewNote) override
        {
            m_vNotesChanged.insert_or_assign(nAddress, sNewNote);
        }

    private:
        bool m_bActiveGameChanged = false;
        std::map<ra::data::ByteAddress, std::wstring> m_vNotesChanged;
    };

private:
    // loads game 1, and has a load of game 2 replace it while the first load is completing: after it has read the
    // game for its popup, and before it merges the assets. unless the test has mocked the second load's session,
    // the first load is discarded while the second load is still waiting for it.
    static void LoadGameReplacedWhileCompleting(GameContextHarness& game)
    {
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));
        game.MockLoadGameAPIsWithoutSession(2U, "fedcba9876543210123456789abcdef", game.MockAchievementJson(7U, 10U));

        // a completion fetches the game's icon for its popup while it is not holding the load lock
        bool bReplaced = false;
        game.mockImageRepository.SetFetchImageHandler([&game, &bReplaced](ra::ui::ImageType nType, const std::string&) {
            if (nType == ra::ui::ImageType::Icon && !bReplaced)
            {
                bReplaced = true;
                game.LoadGame(2U, "fedcba9876543210123456789abcdef");
            }
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        game.mockImageRepository.SetFetchImageHandler(nullptr);

        Assert::IsTrue(bReplaced);
        Assert::AreEqual(2U, game.GameId());
    }

public:
    TEST_METHOD(TestLoadGameTitle)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::AreEqual(1U, game.GameId());
        Assert::AreEqual(ra::data::context::GameContext::Mode::Normal, game.GetMode());
        Assert::AreEqual(std::wstring(L"GameTitle"), game.GameTitle());
    }

    TEST_METHOD(TestLoadGamePopup)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            game.MockAchievementJson(123U, 5U));

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"1 achievements, 5 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatch)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.mockConsoleContext.SetId(ConsoleID::MasterSystem);
        game.mockConsoleContext.SetName(L"Master System");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>([&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel& vmMessageBox)
        {
            Assert::AreEqual(std::wstring(L"Identified game does not match expected console."), vmMessageBox.GetHeader());
            Assert::AreEqual(std::wstring(L"The game being loaded is associated to the Sega Genesis console, but the emulator has initialized "
                "the Master System console. This is not allowed as the memory maps may not be compatible between consoles."), vmMessageBox.GetMessage());
            bDialogShown = true;
            return ra::ui::DialogResult::OK;
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(0U, game.GameId());
        Assert::AreEqual(ra::data::context::GameContext::Mode::Normal, game.GetMode());
        Assert::AreEqual(std::wstring(L""), game.GameTitle());
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchUnknownConsole)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "", ConsoleID::UnknownConsoleID);

        game.mockConsoleContext.SetId(ConsoleID::MasterSystem);
        game.mockConsoleContext.SetName(L"Master System");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>([&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel& vmMessageBox)
        {
            Assert::AreEqual(std::wstring(L"Identified game does not match expected console."), vmMessageBox.GetHeader());
            Assert::AreEqual(std::wstring(L"The game being loaded is associated to the Unknown console, but the emulator has initialized "
                "the Master System console. This is not allowed as the memory maps may not be compatible between consoles."), vmMessageBox.GetMessage());
            bDialogShown = true;
            return ra::ui::DialogResult::OK;
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(0U, game.GameId());
        Assert::AreEqual(std::wstring(L""), game.GameTitle());
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchGBvGBC)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "", ConsoleID::GB);

        game.mockConsoleContext.SetId(ConsoleID::GBC);
        game.mockConsoleContext.SetName(L"GameBoy Color");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>([&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&)
        {
            bDialogShown = true;
            return ra::ui::DialogResult::OK;
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchDSvDSi)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "", ConsoleID::DS);

        game.mockConsoleContext.SetId(ConsoleID::DSi);
        game.mockConsoleContext.SetName(L"Nintendo DSi");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>([&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&)
        {
            bDialogShown = true;
            return ra::ui::DialogResult::OK;
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchNESvEvent)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "", static_cast<ConsoleID>(RC_CONSOLE_EVENTS));

        game.mockConsoleContext.SetId(ConsoleID::NES);
        game.mockConsoleContext.SetName(L"Nintendo");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&) {
                bDialogShown = true;
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchNESvStandalone)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "",
                              static_cast<ConsoleID>(RC_CONSOLE_STANDALONE));

        game.mockConsoleContext.SetId(ConsoleID::NES);
        game.mockConsoleContext.SetName(L"Nintendo");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&) {
                bDialogShown = true;
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchNESvFDS)
    {
        // Emulator updated after server, or is out-of-date
        // Emulator sets mode to NES, server says game is FDS. Allow
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "",
                              static_cast<ConsoleID>(RC_CONSOLE_FAMICOM_DISK_SYSTEM));

        game.mockConsoleContext.SetId(ConsoleID::NES);
        game.mockConsoleContext.SetName(L"Nintendo");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&) {
                bDialogShown = true;
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameTitleConsoleMismatchFDSvNES)
    {
        // Emulator updated before server
        // Emulator sets mode to FDS, server says game is NES. Allow
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", "",
                              static_cast<ConsoleID>(RC_CONSOLE_NINTENDO));

        game.mockConsoleContext.SetId(ConsoleID::FamicomDiskSystem);
        game.mockConsoleContext.SetName(L"Famicom Disk System");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [&bDialogShown](ra::ui::viewmodels::MessageBoxViewModel&) {
                bDialogShown = true;
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.mockAudioSystem.WasAudioFilePlayed(std::wstring(L"Overlay\\info.wav")));

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"0 achievements, 0 points"), pPopup->GetDescription());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());

        Assert::IsFalse(bDialogShown);
    }

    TEST_METHOD(TestLoadGameNotify)
    {
        class NotifyHarness : public GameContext::NotifyTarget
        {
        public:
            bool m_bNotified = false;

        protected:
            void OnActiveGameChanged() noexcept override { m_bNotified = true; }
        };
        NotifyHarness notifyHarness;

        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.MockLoadGameAPIs(2U, "fedcba9876543210123456789abcdef");

        game.AddNotifyTarget(notifyHarness);
        game.LoadGame(0U, "");

        Assert::AreEqual(0U, game.GameId());
        Assert::IsFalse(notifyHarness.m_bNotified);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::AreEqual(1U, game.GameId());
        Assert::IsTrue(notifyHarness.m_bNotified);

        notifyHarness.m_bNotified = false;
        game.LoadGame(2U, "fedcba9876543210123456789abcdef");
        Assert::AreEqual(2U, game.GameId());
        Assert::IsTrue(notifyHarness.m_bNotified);

        notifyHarness.m_bNotified = false;
        game.LoadGame(0U, "");
        Assert::AreEqual(0U, game.GameId());
        Assert::IsTrue(notifyHarness.m_bNotified);

        notifyHarness.m_bNotified = false;
        game.RemoveNotifyTarget(notifyHarness);
        game.LoadGame(2U, "fedcba9876543210123456789abcdef");
        Assert::AreEqual(2U, game.GameId());
        Assert::IsFalse(notifyHarness.m_bNotified);
    }

    TEST_METHOD(TestLoadGameRichPresence)
    {
        GameContextHarness game;
        const std::string sRichPresence = "Display:\nHello, World";
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", sRichPresence);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.HasRichPresence());
        Assert::AreEqual(std::string("Display:\nHello, World\n"),
            game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsFalse(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNotNull(pRichPresence);
        Ensures(pRichPresence != nullptr);
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pRichPresence->GetCategory());
        Assert::AreEqual(ra::data::models::AssetChanges::None, pRichPresence->GetChanges());
        Assert::IsTrue(pRichPresence->IsActive());
    }

    TEST_METHOD(TestLoadGameDoesNotOverwriteRichPresenceFromFile)
    {
        GameContextHarness game;
        const std::string sRichPresence = "Display:\nHello, World";
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", sRichPresence);

        game.mockStorage.MockStoredData(ra::services::StorageItemType::RichPresence, L"1", "Display:\nFrom File\n");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.HasRichPresence());
        Assert::AreEqual(std::string("Display:\nFrom File\n"),
            game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsTrue(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNotNull(pRichPresence);
        Ensures(pRichPresence != nullptr);
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pRichPresence->GetCategory());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, pRichPresence->GetChanges());

        // modified rich presence should not be active unless monitor is opened
        Assert::IsFalse(pRichPresence->IsActive());
    }

    TEST_METHOD(TestLoadGameRichPresenceUpdatedFromServer)
    {
        GameContextHarness game;
        const std::string sRichPresence = "Display:\nHello, World";
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", sRichPresence);

        game.mockStorage.MockStoredData(ra::services::StorageItemType::GameData, L"1", "{\"RichPresencePatch\": \"Display:\\nOld\\n\"}");
        game.mockStorage.MockStoredData(ra::services::StorageItemType::RichPresence, L"1", "Display:\nOld\n");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.HasRichPresence());
        Assert::AreEqual(std::string("Display:\nHello, World\n"),
            game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsFalse(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNotNull(pRichPresence);
        Ensures(pRichPresence != nullptr);
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pRichPresence->GetCategory());
        Assert::AreEqual(ra::data::models::AssetChanges::None, pRichPresence->GetChanges());
        Assert::IsTrue(pRichPresence->IsActive());
    }

    TEST_METHOD(TestLoadGameRichPresenceUpdatedFromServerLocalModifications)
    {
        GameContextHarness game;
        const std::string sRichPresence = "Display:\nHello, World";
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "", "", sRichPresence);

        game.mockStorage.MockStoredData(ra::services::StorageItemType::GameData, L"1", "{\"RichPresencePatch\": \"Display:\\nOld\\n\"}");
        game.mockStorage.MockStoredData(ra::services::StorageItemType::RichPresence, L"1", "Display:\nLocal\n");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.HasRichPresence());
        Assert::AreEqual(std::string("Display:\nLocal\n"),
            game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsTrue(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNotNull(pRichPresence);
        Ensures(pRichPresence != nullptr);
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pRichPresence->GetCategory());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, pRichPresence->GetChanges());
        Assert::IsFalse(pRichPresence->IsActive());
    }

    TEST_METHOD(TestLoadGameRichPresenceNotOnServer)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.mockStorage.MockStoredData(ra::services::StorageItemType::GameData, L"1", "{\"RichPresencePatch\": null}");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsFalse(game.HasRichPresence());
        Assert::IsFalse(game.mockStorage.HasStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsFalse(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNull(pRichPresence);
    }

    TEST_METHOD(TestLoadGameNoRichPresence)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsFalse(game.HasRichPresence());
        Assert::AreEqual(std::string(),
                         game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsFalse(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNull(pRichPresence);
    }

    TEST_METHOD(TestLoadGameRichPresenceOnlyFromFile)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.mockStorage.MockStoredData(ra::services::StorageItemType::RichPresence, L"1", "Display:\nFrom File\n");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(game.HasRichPresence());
        Assert::AreEqual(std::string("Display:\nFrom File\n"),
                         game.mockStorage.GetStoredData(ra::services::StorageItemType::RichPresence, L"1"));
        Assert::IsTrue(game.IsRichPresenceFromFile());

        const auto* pRichPresence = game.Assets().FindRichPresence();
        Assert::IsNotNull(pRichPresence);
        Ensures(pRichPresence != nullptr);
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pRichPresence->GetCategory());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, pRichPresence->GetChanges());
        Assert::IsFalse(pRichPresence->IsActive());
    }

    TEST_METHOD(TestLoadGameAchievements)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
             "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
               "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        game.RemoveNonAchievementAssets();
        Assert::AreEqual({ 2U }, game.Assets().Count());

        const auto* vmAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(vmAch1);
        Ensures(vmAch1 != nullptr);
        Assert::AreEqual(5U, vmAch1->GetID());
        Assert::AreEqual(std::wstring(L"Ach1"), vmAch1->GetName());
        Assert::AreEqual(std::wstring(L"Desc1"), vmAch1->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth1"), vmAch1->GetAuthor());
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, vmAch1->GetCategory());
        Assert::AreEqual(5, vmAch1->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch1->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch1->GetTrigger());
        Assert::IsFalse(vmAch1->IsModified());

        const auto* vmAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(vmAch2);
        Ensures(vmAch2 != nullptr);
        Assert::AreEqual(7U, vmAch2->GetID());
        Assert::AreEqual(std::wstring(L"Ach2"), vmAch2->GetName());
        Assert::AreEqual(std::wstring(L"Desc2"), vmAch2->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth2"), vmAch2->GetAuthor());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, vmAch2->GetCategory());
        Assert::AreEqual(15, vmAch2->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch2->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch2->GetTrigger());
        Assert::IsFalse(vmAch2->IsModified());

        const auto pPatchData = game.mockStorage.GetStoredData(ra::services::StorageItemType::GameData, L"1");
        Assert::AreNotEqual(pPatchData.find("\"GameId\":1,"), std::string::npos);
        Assert::AreEqual(pPatchData.find("\"PatchData\":"), std::string::npos); /* PatchData should be stripped out */
    }

    TEST_METHOD(TestLoadGameInvalidAchievementFlags)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
             "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":0," // not a valid Flags
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
               "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        game.RemoveNonAchievementAssets();
        Assert::AreEqual({ 2U }, game.Assets().Count());

        const auto* vmAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(vmAch1);
        Ensures(vmAch1 != nullptr);
        Assert::AreEqual(5U, vmAch1->GetID());
        Assert::AreEqual(std::wstring(L"Ach1"), vmAch1->GetName());
        Assert::AreEqual(std::wstring(L"Desc1"), vmAch1->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth1"), vmAch1->GetAuthor());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, vmAch1->GetCategory());
        Assert::AreEqual(5, vmAch1->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch1->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch1->GetTrigger());
        Assert::IsFalse(vmAch1->IsModified());

        const auto* vmAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(vmAch2);
        Ensures(vmAch2 != nullptr);
        Assert::AreEqual(7U, vmAch2->GetID());
        Assert::AreEqual(std::wstring(L"Ach2"), vmAch2->GetName());
        Assert::AreEqual(std::wstring(L"Desc2"), vmAch2->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, vmAch2->GetCategory());
        Assert::AreEqual(15, vmAch2->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch2->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch2->GetTrigger());
        Assert::IsFalse(vmAch2->IsModified());
    }

    TEST_METHOD(TestLoadGameMergeLocalAchievements)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
             "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
               "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":9,\"Title\":\"Ach3\",\"Description\":\"Desc3\",\"Flags\":3,"
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth3\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockStorage.MockStoredData(ra::services::StorageItemType::UserAchievements, L"1",
            "Version\n"
            "Game\n"
            "7:1=2:Ach2b:Desc2b::::Auth2b:25:1234554321:1234555555:::54321\n"
            "9:1=2:Ach3b:Desc3b::::Auth3b:25:1234554321:1234555555:::54321\n"
            "0:\"1=1\":\"Ach3\":\"Desc3\"::::Auth3:20:1234511111:1234500000:::555\n"
            "0:R:1=1:Ach4:Desc4::::Auth4:10:1234511111:1234500000:::556\n"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        auto* pAch = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach1"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc1"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth1"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"12345"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pAch->GetCategory());
        Assert::AreEqual(5, pAch->GetPoints());
        Assert::AreEqual(std::string("1=1"), pAch->GetTrigger());

        // local achievement data for 7 should be merged with server achievement data
        pAch = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach2b"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc2b"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth2"), pAch->GetAuthor()); // author not merged
        Assert::AreEqual(std::wstring(L"54321"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, pAch->GetCategory()); // category not merged
        Assert::AreEqual(25, pAch->GetPoints());
        Assert::AreEqual(std::string("1=2"), pAch->GetTrigger());

        const auto* pTrigger = pAch->GetRuntimeTrigger();
        Assert::AreEqual(2U, pTrigger->requirement->conditions->operand2.value.num); // runtime trigger updated

        // local achievement data for 9 should be merged with server achievement data
        pAch = game.Assets().FindAchievement(9U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach3b"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc3b"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth3"), pAch->GetAuthor()); // author not merged
        Assert::AreEqual(std::wstring(L"54321"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pAch->GetCategory()); // category not merged
        Assert::AreEqual(25, pAch->GetPoints());
        Assert::AreEqual(std::string("1=2"), pAch->GetTrigger());

        pTrigger = pAch->GetRuntimeTrigger();
        Assert::AreEqual(2U, pTrigger->requirement->conditions->operand2.value.num); // runtime trigger updated

        // no server achievement, assign FirstLocalId
        pAch = game.Assets().FindAchievement(ra::data::models::GameAssets::FirstLocalId);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach3"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc3"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth3"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00555"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(20, pAch->GetPoints());
        Assert::AreEqual(std::string("1=1"), pAch->GetTrigger());

        // no server achievement, assign next local id
        pAch = game.Assets().FindAchievement(ra::data::models::GameAssets::FirstLocalId + 1);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach4"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc4"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth4"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00556"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(10, pAch->GetPoints());
        Assert::AreEqual(std::string("R:1=1"), pAch->GetTrigger());
    }

    TEST_METHOD(TestLoadGameMergeLocalAchievementsWithIds)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        game.mockStorage.MockStoredData(ra::services::StorageItemType::UserAchievements, L"1",
            "Version\n"
            "Game\n"
            "7:1=2:Ach2b:Desc2b::::Auth2b:25:1234554321:1234555555:::54321\n"
            "999000001:1=1:Ach3:Desc3::::Auth3:20:1234511111:1234500000:::555\n"
            "999000003:1=1:Ach4:Desc4::::Auth4:10:1234511111:1234500000:::556\n"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        game.RemoveNonAchievementAssets();
        Assert::AreEqual({ 3U }, game.Assets().Count());

        // 7 is not a known ID for this game, it should be loaded into a local achievement
        auto* vmAch = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(vmAch);
        Ensures(vmAch != nullptr);
        Assert::AreEqual(7U, vmAch->GetID());
        Assert::AreEqual(std::wstring(L"Ach2b"), vmAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc2b"), vmAch->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, vmAch->GetCategory());
        Assert::AreEqual(25, vmAch->GetPoints());
        Assert::AreEqual(std::wstring(L"54321"), vmAch->GetBadge());
        Assert::AreEqual(std::string("1=2"), vmAch->GetTrigger());
        Assert::IsFalse(vmAch->IsModified());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, vmAch->GetChanges());

        // explicit ID should be honored
        vmAch = game.Assets().FindAchievement(999000001U);
        Assert::IsNotNull(vmAch);
        Ensures(vmAch != nullptr);
        Assert::AreEqual(999000001U, vmAch->GetID()); // non-vms get first id and first id + 1
        Assert::AreEqual(std::wstring(L"Ach3"), vmAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc3"), vmAch->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, vmAch->GetCategory());
        Assert::AreEqual(20, vmAch->GetPoints());
        Assert::AreEqual(std::wstring(L"00555"), vmAch->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch->GetTrigger());
        Assert::IsFalse(vmAch->IsModified());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, vmAch->GetChanges());

        // explicit ID should be honored
        vmAch = game.Assets().FindAchievement(999000003U);
        Assert::IsNotNull(vmAch);
        Ensures(vmAch != nullptr);
        Assert::AreEqual(999000003U, vmAch->GetID()); // non-vms get first id and first id + 1
        Assert::AreEqual(std::wstring(L"Ach4"), vmAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc4"), vmAch->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, vmAch->GetCategory());
        Assert::AreEqual(10, vmAch->GetPoints());
        Assert::AreEqual(std::wstring(L"00556"), vmAch->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch->GetTrigger());
        Assert::IsFalse(vmAch->IsModified());
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, vmAch->GetChanges());


        // new achievement should be allocated an ID higher than the largest existing local
        // ID, even if intermediate values are available
        const auto& pAch2 = ra::data::context::mocks::MockGameContext::MockAchievement(game.Assets(), ra::data::models::AssetCategory::Local);
        Assert::AreEqual(999000004U, pAch2.GetID());
    }

    TEST_METHOD(TestLoadGameMergeLocalAchievementsFromSubsets)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse(
                "r=achievementsets&u=Username&t=ApiToken&m=0123456789abcdeffedcba987654321&v=2",
                "{\"Success\":true,"
                 "\"GameId\":1,"
                 "\"Title\":\"GameTitle\","
                 "\"ConsoleId\":1,"
                 "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                 "\"RichPresenceGameId\":1,"
                 "\"RichPresencePatch\":\"\","
                 "\"Sets\":[{"
                    "\"AchievementSetId\":5,\"GameId\":1,\"Title\":null,\"Type\":\"core\","
                    "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                    "\"Achievements\":["
                       "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
                        "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
                        "\"Created\":1234567890,\"Modified\":123459999}"
                    "],"
                    "\"Leaderboards\":[]"
                 "},{"
                    "\"AchievementSetId\":3,\"GameId\":2,\"Title\":\"SetTitle\",\"Type\":\"bonus\","
                    "\"ImageIconUrl\":\"\",\"Achievements\":["
                       "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
                        "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
                        "\"Created\":1234567890,\"Modified\":123459999}"
                    "],\"Leaderboards\":[]"
                 "}]}");
        game.mockStorage.MockStoredData(ra::services::StorageItemType::UserAchievements, L"1",
                                        "Version\n"
                                        "Game\n"
                                        "7:1=2:Ach2b:Desc2b::::Auth2b:25:1234554321:1234555555:::54321\n"
                                        "111000002:\"1=1\":\"Ach3\":\"Desc3\"::::Auth3:20:1234511111:1234500000:::555\n"
                                        "111000003:\"R:1=1\":Ach4:Desc4::::Auth4:10:1234511111:1234500000:::556\n");
        game.mockStorage.MockStoredData(ra::services::StorageItemType::UserAchievements, L"2",
                                        "Version2\n"
                                        "Subset\n"
                                        "5:1=2:Ach1b:Desc1b::::Auth1b:25:1234554321:1234555555:::54321\n"
                                        "111000001:\"1=1\":\"Ach5\":\"Desc5\"::::Auth5:20:1234511111:1234500000:::555\n"
                                        "111000002:\"R:1=1\":Ach6:Desc6::::Auth6:10:1234511111:1234500000:::556\n");

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::AreEqual({2U}, game.Assets().AchievementSets().Count());
        Assert::AreEqual(2U, game.GetGameId(3U));

        // local achievement data for 5 should be merged with server achievement data
        auto* pAch = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach1b"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc1b"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth1"), pAch->GetAuthor()); // author not merged
        Assert::AreEqual(std::wstring(L"54321"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, pAch->GetCategory()); // category not merged
        Assert::AreEqual(25, pAch->GetPoints());
        Assert::AreEqual(std::string("1=2"), pAch->GetTrigger());

        // local achievement data for 7 should be merged with server achievement data
        pAch = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach2b"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc2b"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth2"), pAch->GetAuthor()); // author not merged
        Assert::AreEqual(std::wstring(L"54321"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, pAch->GetCategory()); // category not merged
        Assert::AreEqual(25, pAch->GetPoints());
        Assert::AreEqual(std::string("1=2"), pAch->GetTrigger());

        // IDs from core file are kept
        pAch = game.Assets().FindAchievement(111000002U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach3"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc3"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth3"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00555"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(20, pAch->GetPoints());
        Assert::AreEqual(std::string("1=1"), pAch->GetTrigger());

        // IDs from core file are kept
        pAch = game.Assets().FindAchievement(111000003U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach4"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc4"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth4"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00556"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(10, pAch->GetPoints());
        Assert::AreEqual(std::string("R:1=1"), pAch->GetTrigger());

        // IDs from subset file are incrementally assigned after last core ID
        pAch = game.Assets().FindAchievement(111000004U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach5"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc5"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth5"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00555"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(20, pAch->GetPoints());
        Assert::AreEqual(std::string("1=1"), pAch->GetTrigger());

        // IDs from subset file are incrementally assigned after last core ID
        pAch = game.Assets().FindAchievement(111000005U);
        Assert::IsNotNull(pAch);
        Ensures(pAch != nullptr);
        Assert::AreEqual(std::wstring(L"Ach6"), pAch->GetName());
        Assert::AreEqual(std::wstring(L"Desc6"), pAch->GetDescription());
        Assert::AreEqual(std::wstring(L"Auth6"), pAch->GetAuthor());
        Assert::AreEqual(std::wstring(L"00556"), pAch->GetBadge());
        Assert::AreEqual(ra::data::models::AssetCategory::Local, pAch->GetCategory());
        Assert::AreEqual(10, pAch->GetPoints());
        Assert::AreEqual(std::string("R:1=1"), pAch->GetTrigger());

        // migration should have moved the records in 2-User.txt into 1-User.txt and deleted 2-User.txt
        Assert::IsFalse(game.mockStorage.HasStoredData(StorageItemType::UserAchievements, L"2"));
        Assert::AreEqual(std::string(
                "Version\n"
                "Game\n"
                "7:1=2:Ach2b:Desc2b::::Auth2b:25:1234554321:1234555555:::54321\n"
                "111000002:\"1=1\":\"Ach3\":\"Desc3\"::::Auth3:20:1234511111:1234500000:::555\n"
                "111000003:\"R:1=1\":Ach4:Desc4::::Auth4:10:1234511111:1234500000:::556\n"
                "5:1=2:Ach1b:Desc1b::::Auth1b:25:1234554321:1234555555:::54321\n"
                "0|3:\"1=1\":\"Ach5\":\"Desc5\"::::Auth5:20:1234511111:1234500000:::555\n"
                "0|3:\"R:1=1\":Ach6:Desc6::::Auth6:10:1234511111:1234500000:::556\n"
            ), game.mockStorage.GetStoredData(StorageItemType::UserAchievements, L"1"));
    }

    TEST_METHOD(TestLoadGameLeaderboards)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", "",
            "{\"ID\":7,\"Title\":\"LB1\",\"Description\":\"Desc1\","
             "\"Mem\":\"STA:1=1::CAN:1=1::SUB:1=1::VAL:1\",\"Format\":\"SECS\"},"
            "{\"ID\":8,\"Title\":\"LB2\",\"Description\":\"Desc2\","
             "\"Mem\":\"STA:1=1::CAN:1=1::SUB:1=1::VAL:1\",\"Format\":\"FRAMES\"}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto* pLb1 = game.Assets().FindLeaderboard(7U);
        Assert::IsNotNull(pLb1);
        Ensures(pLb1 != nullptr);
        Assert::AreEqual(std::wstring(L"LB1"), pLb1->GetName());
        Assert::AreEqual(std::wstring(L"Desc1"), pLb1->GetDescription());

        const auto* pLb2 = game.Assets().FindLeaderboard(8U);
        Assert::IsNotNull(pLb2);
        Ensures(pLb2 != nullptr);
        Assert::AreEqual(std::wstring(L"LB2"), pLb2->GetName());
        Assert::AreEqual(std::wstring(L"Desc2"), pLb2->GetDescription());
    }

    TEST_METHOD(TestLoadGameReplacesAchievements)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
             "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
               "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        game.MockLoadGameAPIs(2U, "fedcba9876543210123456789abcdef", "",
             "{\"ID\":9,\"Title\":\"Ach9\",\"Description\":\"Desc9\",\"Flags\":3,"
               "\"Points\":9,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":11,\"Title\":\"Ach11\",\"Description\":\"Desc11\",\"Flags\":5,"
               "\"Points\":11,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(2U, "fedcba9876543210123456789abcdef");

        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNull(game.Assets().FindAchievement(7U));

        game.RemoveNonAchievementAssets();
        Assert::AreEqual({ 2U }, game.Assets().Count());
        Assert::IsFalse(game.Assets().IsUpdating());

        const auto* vmAch1 = game.Assets().FindAchievement(9U);
        Assert::IsNotNull(vmAch1);
        Ensures(vmAch1 != nullptr);
        Assert::AreEqual(9U, vmAch1->GetID());
        Assert::AreEqual(std::wstring(L"Ach9"), vmAch1->GetName());
        Assert::AreEqual(std::wstring(L"Desc9"), vmAch1->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Promoted, vmAch1->GetCategory());
        Assert::AreEqual(9, vmAch1->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch1->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch1->GetTrigger());
        Assert::IsFalse(vmAch1->IsModified());

        const auto* vmAch2 = game.Assets().FindAchievement(11U);
        Assert::IsNotNull(vmAch2);
        Ensures(vmAch2 != nullptr);
        Assert::AreEqual(11U, vmAch2->GetID());
        Assert::AreEqual(std::wstring(L"Ach11"), vmAch2->GetName());
        Assert::AreEqual(std::wstring(L"Desc11"), vmAch2->GetDescription());
        Assert::AreEqual(ra::data::models::AssetCategory::Unpromoted, vmAch2->GetCategory());
        Assert::AreEqual(11, vmAch2->GetPoints());
        Assert::AreEqual(std::wstring(L"12345"), vmAch2->GetBadge());
        Assert::AreEqual(std::string("1=1"), vmAch2->GetTrigger());
        Assert::IsFalse(vmAch2->IsModified());
    }

    TEST_METHOD(TestLoadGameZeroRemovesAchievements)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
             "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
               "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999},"
             "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
               "\"Points\":15,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
               "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        game.RemoveNonAchievementAssets();
        Assert::AreEqual({ 2U }, game.Assets().Count());
        Assert::IsNotNull(rc_client_get_game_info(game.mockAchievementRuntime.GetClient()));

        game.LoadGame(0U, "");

        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNull(game.Assets().FindAchievement(7U));

        Assert::AreEqual({ 0U }, game.Assets().Count());
        Assert::IsNull(rc_client_get_game_info(game.mockAchievementRuntime.GetClient()));
        Assert::IsFalse(game.Assets().IsUpdating());
    }

    TEST_METHOD(TestLoadGameZeroSupersedesPendingLoads)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto nLoadGeneration = game.GetLoadGeneration();
        Assert::IsTrue(game.IsLoadCurrent(nLoadGeneration));

        game.LoadGame(0U, "");

        Assert::IsFalse(game.IsLoadCurrent(nLoadGeneration));
    }

    TEST_METHOD(TestLoadGameSupersedesPendingLoads)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto nLoadGeneration = game.GetLoadGeneration();

        game.MockLoadGameAPIs(2U, "fedcba9876543210123456789abcdef");
        game.LoadGame(2U, "fedcba9876543210123456789abcdef");

        Assert::IsFalse(game.IsLoadCurrent(nLoadGeneration));
    }

    TEST_METHOD(TestLoadGameSupersedesPendingLoadsBeforeTearingDown)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsFalse(game.IsGameLoading());

        // OnBeginGameLoad is raised after the old game's assets are cleared, and before the old game is unloaded
        class SupersededObserver : public GameContext::NotifyTarget
        {
        public:
            SupersededObserver(const GameContext& pGameContext, uint32_t nLoadGeneration) noexcept
                : m_pGameContext(pGameContext), m_nLoadGeneration(nLoadGeneration)
            {
            }

            bool bCalled = false;
            bool bWasCurrent = false;

        protected:
            void OnBeginGameLoad() noexcept override
            {
                bCalled = true;
                bWasCurrent = m_pGameContext.IsLoadCurrent(m_nLoadGeneration);
            }

        private:
            const GameContext& m_pGameContext;
            uint32_t m_nLoadGeneration;
        } observer(game, game.GetLoadGeneration());
        game.AddNotifyTarget(observer);

        game.MockLoadGameAPIs(2U, "fedcba9876543210123456789abcdef");
        game.LoadGame(2U, "fedcba9876543210123456789abcdef");

        Assert::IsTrue(observer.bCalled);
        Assert::IsFalse(observer.bWasCurrent);
    }

    TEST_METHOD(TestLoadGameSupersededBeforeCompletionIsDiscarded)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321",
            game.MockAchievementJson(5U, 5U) + "," + game.MockAchievementJson(7U, 10U));
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.IsGameLoading());

        // something unloads or replaces the game before the load's completion gets to it
        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");

        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNull(game.Assets().FindAchievement(7U));
        Assert::IsNull(game.mockOverlayManager.GetMessage(1));
        Assert::IsFalse(game.mockDesktop.WasDialogShown());
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::AreEqual(1U, game.GameId());
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
    }

    TEST_METHOD(TestLoadGameSupersededBeforeGameDataIsNotInitializedFromIt)
    {
        GameContextHarness game;

        // the server knows the hash as game 5, so the game data carries other ids than the load began with
        game.MockLoadGameAPIsWithoutSession(5U, "0123456789abcdeffedcba987654321",
            game.MockAchievementJson(5U, 5U), "", "Display:\nTest\n");

        // something unloads or replaces the game while the load is waiting for the game data
        game.mockRcClient.OnBeforeResponse("r=achievementsets&u=Username&t=ApiToken&m=0123456789abcdeffedcba987654321&v=2",
            [&game]() {
                auto pLock = game.LockLoad();
                game.InvalidatePendingLoads();
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // the game data has been processed, and the load is waiting for its session
        Assert::IsTrue(game.IsGameLoading());
        Assert::IsFalse(game.HasRichPresence());
        Assert::AreEqual(1U, game.GameId());
        Assert::AreEqual(1U, game.ActiveGameId());
        Assert::AreEqual({ 0U }, game.Assets().AchievementSets().Count());
        Assert::IsFalse(game.mockStorage.HasStoredData(ra::services::StorageItemType::GameData, L"0"));
        Assert::IsFalse(game.mockStorage.HasStoredData(ra::services::StorageItemType::GameData, L"1"));
        Assert::IsFalse(game.mockStorage.HasStoredData(ra::services::StorageItemType::GameData, L"5"));

        // let the load finish. it is discarded.
        game.MockStartSession(5U, "0123456789abcdeffedcba987654321");

        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
    }

    TEST_METHOD(TestLoadGameSupersededWhileRuntimePausedStaysPaused)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321", game.MockAchievementJson(5U, 5U));
        game.mockAchievementRuntime.SetPaused(true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");

        // the runtime was paused before the load began, so discarding the load must not unpause it
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
    }

    TEST_METHOD(TestLoadGameSupersededByLoadInProgressLeavesUnpauseToIt)
    {
        GameContextHarness game;
        game.mockRcClient.MockResponse("r=codenotes2&g=2", "{\"Success\":true,\"CodeNotes\":[]}");
        LoadGameReplacedWhileCompleting(game);

        // the second load is still in progress, and it began while the first load had the runtime paused
        Assert::IsTrue(game.IsGameLoading());
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused(), L"unpaused while a load was in progress");
        Assert::IsTrue(game.Assets().IsUpdating(), L"asset update closed while a load was in progress");

        game.MockStartSession(2U, "fedcba9876543210123456789abcdef");

        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNotNull(game.Assets().FindAchievement(7U));
        Assert::AreEqual(2U, game.GameId());

        // nothing is owed any more: a load that begins while the runtime is paused leaves it paused
        game.mockAchievementRuntime.SetPaused(true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
    }

    TEST_METHOD(TestLoadGameSupersededByLoadThatIsAlsoDiscardedUnpausesRuntime)
    {
        GameContextHarness game;
        LoadGameReplacedWhileCompleting(game);
        Assert::IsTrue(game.IsGameLoading());

        // something unloads the game before the second load's completion gets to it
        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(2U, "fedcba9876543210123456789abcdef");

        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");

        // nothing is owed any more: a load that begins while the runtime is paused leaves it paused
        game.mockAchievementRuntime.SetPaused(true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
    }

    TEST_METHOD(TestLoadGameSupersededByLoadThatIsDiscardedFirstUnpausesRuntime)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));
        game.MockLoadGameAPIsWithoutSession(2U, "fedcba9876543210123456789abcdef", game.MockAchievementJson(7U, 10U));

        // a load of game 2 replaces game 1 while the first load is completing, as in
        // LoadGameReplacedWhileCompleting. something then unloads the game, and the second load's completion
        // arrives and is discarded, all before the first load gets to its own discard.
        bool bReplaced = false;
        game.mockImageRepository.SetFetchImageHandler([&game, &bReplaced](ra::ui::ImageType nType, const std::string&) {
            if (nType == ra::ui::ImageType::Icon && !bReplaced)
            {
                bReplaced = true;
                game.LoadGame(2U, "fedcba9876543210123456789abcdef");
                {
                    auto pLock = game.LockLoad();
                    game.InvalidatePendingLoads();
                }
                game.MockStartSession(2U, "fedcba9876543210123456789abcdef");
            }
        });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        game.mockImageRepository.SetFetchImageHandler(nullptr);

        Assert::IsTrue(bReplaced);
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");

        // nothing is owed any more: a load that begins while the runtime is paused leaves it paused
        game.mockAchievementRuntime.SetPaused(true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
    }

    TEST_METHOD(TestLoadGameSupersededByLoadThatCompletesFirstUnpausesRuntime)
    {
        GameContextHarness game;

        // the second load has everything it needs, so it completes before the first load is discarded. it began
        // while the first load had the runtime paused, so it does not unpause it.
        game.MockStartSession(2U, "fedcba9876543210123456789abcdef");
        game.mockRcClient.MockResponse("r=codenotes2&g=2", "{\"Success\":true,\"CodeNotes\":[]}");
        LoadGameReplacedWhileCompleting(game);

        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNotNull(game.Assets().FindAchievement(7U));

        // nothing is owed any more: a load that begins while the runtime is paused leaves it paused
        game.mockAchievementRuntime.SetPaused(true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
    }

    TEST_METHOD(TestLoadGameSupersededWhileCodeNotesAreLoadingUnpausesRuntime)
    {
        GameContextHarness game;

        // the first game is loaded, but its code notes are still being fetched
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.IsGameLoading());
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused());

        game.MockLoadGameAPIsWithoutSession(2U, "fedcba9876543210123456789abcdef", game.MockAchievementJson(7U, 10U));
        game.LoadGame(2U, "fedcba9876543210123456789abcdef");
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused());

        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(2U, "fedcba9876543210123456789abcdef");

        // a code notes fetch is not a load that will unpause the runtime when it completes
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
    }

    TEST_METHOD(TestLoadGameSupersededWhileUnknownGameDialogIsOpenIsDiscarded)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // the server knows the hash as an untested version of game 1
        game.MockLoadGameAPIs(1100000001U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [&game, &bDialogShown](ra::ui::viewmodels::UnknownGameViewModel& vmUnknownGame) {
                bDialogShown = true;
                vmUnknownGame.SetTestMode(true);

                // something unloads or replaces the game while the dialog is open
                auto pLock = game.LockLoad();
                game.InvalidatePendingLoads();
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(ra::data::context::GameContext::Mode::Normal, game.GetMode());
        game.mockRcClient.AssertNumRequestsHandled(3); // game data, session, the dialog's game list: no second load
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
    }

    TEST_METHOD(TestExternalLoadGameSupersededWhileUnknownGameDialogIsOpenKeepsConsole)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // the server knows the hash as an untested version of game 1. the emulator has another console active
        // than the one the game belongs to.
        game.MockLoadGameAPIs(1100000001U, "0123456789abcdeffedcba987654321");
        game.mockConsoleContext.SetId(ConsoleID::MasterSystem);

        rc_client_external_t pExternalClient;
        memset(&pExternalClient, 0, sizeof(pExternalClient));
        _Rcheevos_GetExternalClient(&pExternalClient, 1);
        ExternalClientReset pExternalClientReset;

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [&game, &bDialogShown](ra::ui::viewmodels::UnknownGameViewModel&) {
                bDialogShown = true;

                // something unloads or replaces the game while the dialog is open
                auto pLock = game.LockLoad();
                game.InvalidatePendingLoads();
                return ra::ui::DialogResult::Cancel;
            });

        int nLoadResult = RC_OK;
        pExternalClient.begin_load_game(game.mockAchievementRuntime.GetClient(), "0123456789abcdeffedcba987654321",
            [](int nResult, const char*, rc_client_t*, void* pUserdata) noexcept {
                *static_cast<int*>(pUserdata) = nResult;
            },
            &nLoadResult);

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual((int)RC_ABORTED, nLoadResult);
        Assert::AreEqual(ra::etoi(ConsoleID::MasterSystem),
            ra::etoi(ra::services::ServiceLocator::Get<ra::context::IConsoleContext>().Id()));
    }

    TEST_METHOD(TestLoadGameUnknownGameDialogTestModeReloadsInCompatibilityMode)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // the server knows the hash as an untested version of game 1. loading it for testing fetches game 1 by id.
        game.MockLoadGameAPIs(1100000001U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=achievementsets&u=Username&t=ApiToken&g=1&v=2",
            "{\"Success\":true,\"GameId\":1,\"Title\":\"GameTitle\",\"ConsoleId\":1,"
              "\"ImageIconUrl\":\"http://server/Images/9743.png\",\"RichPresencePatch\":\"\","
              "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1,\"Title\":null,\"Type\":\"core\","
                "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                "\"Achievements\":[" + game.MockAchievementJson(5U, 5U) + "],\"Leaderboards\":[]"
            "}]}");
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [&bDialogShown](ra::ui::viewmodels::UnknownGameViewModel& vmUnknownGame) {
                bDialogShown = true;
                vmUnknownGame.SetTestMode(true);
                return ra::ui::DialogResult::OK;
            });

        // the mode must change without a notification of its own: the load that follows raises it when it is done
        CompatibilityModeObserver observer(game);
        game.AddNotifyTarget(observer);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(ra::data::context::GameContext::Mode::CompatibilityTest, game.GetMode());
        Assert::IsNotNull(game.Assets().FindAchievement(5U));
        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, observer.nNotified);
        Assert::IsTrue(observer.bLoadedForTesting);
    }

    TEST_METHOD(TestLoadGameUnknownGameDialogTestModeRaisesEndGameLoadWithoutHoldingLoadLock)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // every response is mocked, so the load restarted for testing completes before BeginLoadGame returns
        game.MockLoadGameAPIs(1100000001U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=achievementsets&u=Username&t=ApiToken&g=1&v=2",
            "{\"Success\":true,\"GameId\":1,\"Title\":\"GameTitle\",\"ConsoleId\":1,"
              "\"ImageIconUrl\":\"http://server/Images/9743.png\",\"RichPresencePatch\":\"\","
              "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1,\"Title\":null,\"Type\":\"core\","
                "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                "\"Achievements\":[" + game.MockAchievementJson(5U, 5U) + "],\"Leaderboards\":[]"
            "}]}");
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");

        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [](ra::ui::viewmodels::UnknownGameViewModel& vmUnknownGame) {
                vmUnknownGame.SetTestMode(true);
                return ra::ui::DialogResult::OK;
            });

        EndGameLoadLockProbe probe(game);
        game.AddNotifyTarget(probe);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        probe.Join();

        Assert::AreEqual(ra::data::context::GameContext::Mode::CompatibilityTest, game.GetMode());
        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnEndGameLoad raised while holding the load lock");
    }

    TEST_METHOD(TestLoadGameSupersededWhileUnidentifiedHashDialogIsOpenKeepsMode)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // the server does not know the hash, so rc_client asks which game it is while the load is in progress.
        // once the dialog says that it is game 1, rc_client fetches game 1 by id.
        game.MockLoadGameAPIsWithoutSession(0U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=achievementsets&u=Username&t=ApiToken&g=1&v=2",
            "{\"Success\":true,\"GameId\":1,\"Title\":\"GameTitle\",\"ConsoleId\":1,"
              "\"ImageIconUrl\":\"http://server/Images/9743.png\",\"RichPresencePatch\":\"\","
              "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1,\"Title\":null,\"Type\":\"core\","
                "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                "\"Achievements\":[" + game.MockAchievementJson(5U, 5U) + "],\"Leaderboards\":[]"
            "}]}");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [&game, &bDialogShown](ra::ui::viewmodels::UnknownGameViewModel& vmUnknownGame) {
                bDialogShown = true;
                vmUnknownGame.SetSelectedGameId(1);
                vmUnknownGame.SetTestMode(true);

                // something unloads or replaces the game while the dialog is open
                auto pLock = game.LockLoad();
                game.InvalidatePendingLoads();
                return ra::ui::DialogResult::OK;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(ra::data::context::GameContext::Mode::Normal, game.GetMode());

        // rc_client was still given the selected game, and went on to fetch it. had it been told that the game is
        // unknown, it would have made an unknown game the loaded game, over whatever replaced this load.
        game.mockRcClient.AssertCalled("r=achievementsets&u=Username&t=ApiToken&g=1&v=2");
        Assert::IsNull(rc_client_get_game_info(game.mockAchievementRuntime.GetClient()));

        // let the load finish
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
    }

    TEST_METHOD(TestLoadGameUnidentifiedHashDialogTestModeLoadsInCompatibilityMode)
    {
        GameContextHarness game;
        ra::services::mocks::MockLoginService mockLoginService;
        mockLoginService.SetLoggedIn(true);

        // the server does not know the hash. once the dialog says that it is game 1, rc_client fetches game 1 by id.
        game.MockLoadGameAPIsWithoutSession(0U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=achievementsets&u=Username&t=ApiToken&g=1&v=2",
            "{\"Success\":true,\"GameId\":1,\"Title\":\"GameTitle\",\"ConsoleId\":1,"
              "\"ImageIconUrl\":\"http://server/Images/9743.png\",\"RichPresencePatch\":\"\","
              "\"Sets\":[{\"AchievementSetId\":1111,\"GameId\":1,\"Title\":null,\"Type\":\"core\","
                "\"ImageIconUrl\":\"http://server/Images/9743.png\","
                "\"Achievements\":[" + game.MockAchievementJson(5U, 5U) + "],\"Leaderboards\":[]"
            "}]}");
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");

        bool bDialogShown = false;
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::UnknownGameViewModel>(
            [&bDialogShown](ra::ui::viewmodels::UnknownGameViewModel& vmUnknownGame) {
                bDialogShown = true;
                vmUnknownGame.SetSelectedGameId(1);
                vmUnknownGame.SetTestMode(true);
                return ra::ui::DialogResult::OK;
            });

        // the mode must change without a notification of its own: the load raises it when it is done
        CompatibilityModeObserver observer(game);
        game.AddNotifyTarget(observer);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bDialogShown);
        Assert::AreEqual(ra::data::context::GameContext::Mode::CompatibilityTest, game.GetMode());
        Assert::IsNotNull(game.Assets().FindAchievement(5U));
        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, observer.nNotified);
        Assert::IsTrue(observer.bLoadedForTesting);
    }

    TEST_METHOD(TestLoadGameAbortedByRcClientIsDiscarded)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));

        // without background memory reads, rc_client finishes a load in the next frame or idle call
        auto* pClient = game.mockAchievementRuntime.GetClient();
        rc_client_set_allow_background_memory_reads(pClient, 0);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsTrue(game.IsGameLoading());

        // the game is unloaded before that, so rc_client reports the load as aborted
        game.LoadGame(0U, "");
        rc_client_idle(pClient);

        Assert::IsFalse(game.mockDesktop.WasDialogShown());
        Assert::AreEqual(0U, game.GameId());
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::IsFalse(game.Assets().IsUpdating(), L"asset update left open");
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused(), L"runtime left paused");
    }

    TEST_METHOD(TestLoadGameRaisesEndGameLoadWithoutHoldingLoadLock)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");

        EndGameLoadLockProbe probe(game);
        game.AddNotifyTarget(probe);

        // the code notes are already there, so the load count reaches zero as the load merges its assets
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        probe.Join();

        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnEndGameLoad raised while holding the load lock");
    }

    TEST_METHOD(TestLoadGameSupersededRaisesEndGameLoadWithoutHoldingLoadLock)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321", game.MockAchievementJson(5U, 5U));
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        EndGameLoadLockProbe probe(game);
        game.AddNotifyTarget(probe);

        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
        probe.Join();

        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnEndGameLoad raised while holding the load lock");
    }

    TEST_METHOD(TestExternalLoadGameSupersededRaisesEndGameLoadWithoutHoldingLoadLock)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321", game.MockAchievementJson(5U, 5U));

        rc_client_external_t pExternalClient;
        memset(&pExternalClient, 0, sizeof(pExternalClient));
        _Rcheevos_GetExternalClient(&pExternalClient, 1);
        ExternalClientReset pExternalClientReset;

        pExternalClient.begin_load_game(game.mockAchievementRuntime.GetClient(), "0123456789abcdeffedcba987654321",
            [](int, const char*, rc_client_t*, void*) noexcept {}, nullptr);

        EndGameLoadLockProbe probe(game);
        game.AddNotifyTarget(probe);

        // the external client's completion finds out that it is stale when it gets to the assets
        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");
        probe.Join();

        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnEndGameLoad raised while holding the load lock");
    }

    TEST_METHOD(TestLoadGameZeroRaisesActiveGameChangedWithoutHoldingLoadLock)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "", game.MockAchievementJson(5U, 5U));
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");
        Assert::IsFalse(game.IsGameLoading());
        Assert::AreEqual(1U, game.GameId());

        // attached once the game is loaded, so that the load's own OnActiveGameChanged is not the one probed
        ActiveGameChangedLockProbe probe(game);
        game.AddNotifyTarget(probe);

        game.LoadGame(0U, "");
        probe.Join();

        Assert::AreEqual(0U, game.GameId());
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnActiveGameChanged raised while holding the load lock");
    }

    TEST_METHOD(TestUnloadWaitsForLoadCompletionInProgress)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321",
            game.MockAchievementJson(5U, 5U) + "," + game.MockAchievementJson(7U, 10U));
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        PrefetchGate gate(game.mockImageRepository);

        // rc_client completes the load on another thread, as it does when the server answers
        std::thread tCompletion([&game]() { game.MockStartSession(1U, "0123456789abcdeffedcba987654321"); });
        gate.WaitUntilHeld(tCompletion);

        std::atomic<bool> bUnloaded{ false };
        std::thread tUnload([&game, &bUnloaded]() {
            game.LoadGame(0U, "");
            bUnloaded = true;
        });

        // the unload must not free the game while the completion is walking it
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool bUnloadedWhileHeld = bUnloaded;

        gate.Release();
        tCompletion.join();
        tUnload.join();

        Assert::IsFalse(bUnloadedWhileHeld, L"unloaded while the completion was walking the game");
        Assert::AreEqual(0U, game.GameId());
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsNull(game.Assets().FindAchievement(7U));
        Assert::IsNull(rc_client_get_game_info(game.mockAchievementRuntime.GetClient()));
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
    }

    TEST_METHOD(TestDirectUnloadWaitsForLoadCompletionInProgress)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321",
            game.MockAchievementJson(5U, 5U) + "," + game.MockAchievementJson(7U, 10U));
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        PrefetchGate gate(game.mockImageRepository);

        std::thread tCompletion([&game]() { game.MockStartSession(1U, "0123456789abcdeffedcba987654321"); });
        gate.WaitUntilHeld(tCompletion);

        // _RA_ActivateGame(0) unloads through the runtime directly, without GameContext::LoadGame
        std::atomic<bool> bUnloaded{ false };
        std::thread tUnload([&game, &bUnloaded]() {
            game.mockAchievementRuntime.UnloadGame();
            bUnloaded = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool bUnloadedWhileHeld = bUnloaded;

        gate.Release();
        tCompletion.join();
        tUnload.join();

        Assert::IsFalse(bUnloadedWhileHeld, L"unloaded while the completion was walking the game");
        Assert::IsNull(rc_client_get_game_info(game.mockAchievementRuntime.GetClient()));
    }

    TEST_METHOD(TestResetWaitsForLoadCompletionInProgressAndKeepsIt)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321",
            game.MockAchievementJson(5U, 5U) + "," + game.MockAchievementJson(7U, 10U));
        game.mockRcClient.MockResponse("r=codenotes2&g=1", "{\"Success\":true,\"CodeNotes\":[]}");
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        PrefetchGate gate(game.mockImageRepository);

        std::thread tCompletion([&game]() { game.MockStartSession(1U, "0123456789abcdeffedcba987654321"); });
        gate.WaitUntilHeld(tCompletion);

        std::atomic<bool> bReset{ false };
        std::thread tReset([&game, &bReset]() {
            game.mockAchievementRuntime.ResetRuntime();
            bReset = true;
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const bool bResetWhileHeld = bReset;

        gate.Release();
        tCompletion.join();
        tReset.join();

        // the reset keeps the game, so it must not discard the load that was completing
        Assert::IsFalse(bResetWhileHeld, L"reset while the completion was walking the game");
        Assert::AreEqual(1U, game.GameId());
        Assert::IsNotNull(game.Assets().FindAchievement(5U));
        Assert::IsNotNull(game.Assets().FindAchievement(7U));
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
    }

    TEST_METHOD(TestExternalLoadGameSupersededReportsAborted)
    {
        GameContextHarness game;
        game.MockLoadGameAPIsWithoutSession(1U, "0123456789abcdeffedcba987654321", game.MockAchievementJson(5U, 5U));

        rc_client_external_t pExternalClient;
        memset(&pExternalClient, 0, sizeof(pExternalClient));
        _Rcheevos_GetExternalClient(&pExternalClient, 1);

        ExternalClientReset pExternalClientReset;

        struct LoadResult
        {
            bool bCalled = false;
            int nResult = 0;
            std::string sErrorMessage;
        } pLoadResult;

        pExternalClient.begin_load_game(game.mockAchievementRuntime.GetClient(), "0123456789abcdeffedcba987654321",
            [](int nResult, const char* sErrorMessage, rc_client_t*, void* pUserdata) {
                auto* pResult = static_cast<LoadResult*>(pUserdata);
                pResult->bCalled = true;
                pResult->nResult = nResult;
                pResult->sErrorMessage = sErrorMessage ? sErrorMessage : "";
            },
            &pLoadResult);

        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }
        game.MockStartSession(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(pLoadResult.bCalled);
        Assert::AreEqual((int)RC_ABORTED, pLoadResult.nResult);
        Assert::AreEqual(std::string("The requested game is no longer active"), pLoadResult.sErrorMessage);
        Assert::IsNull(game.Assets().FindAchievement(5U));
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
    }

    TEST_METHOD(TestExternalLogoutWhileLoadAwaitsLoginRaisesEndGameLoadWithoutHoldingLoadLock)
    {
        GameContextHarness game;

        rc_client_external_t pExternalClient;
        memset(&pExternalClient, 0, sizeof(pExternalClient));
        _Rcheevos_GetExternalClient(&pExternalClient, 1);
        ExternalClientReset pExternalClientReset;

        struct LoadResult
        {
            bool bCalled = false;
            int nResult = 0;
        } pLoadResult;

        // the load waits for the login to complete
        game.mockAchievementRuntime.GetClient()->state.user = RC_CLIENT_USER_STATE_LOGIN_REQUESTED;
        pExternalClient.begin_load_game(game.mockAchievementRuntime.GetClient(), "0123456789abcdeffedcba987654321",
            [](int nResult, const char*, rc_client_t*, void* pUserdata) {
                auto* pResult = static_cast<LoadResult*>(pUserdata);
                pResult->bCalled = true;
                pResult->nResult = nResult;
            },
            &pLoadResult);
        Assert::IsTrue(game.IsGameLoading());
        Assert::IsFalse(pLoadResult.bCalled);

        EndGameLoadLockProbe probe(game);
        game.AddNotifyTarget(probe);

        // rc_client_logout aborts the load before it returns
        pExternalClient.logout();
        probe.Join();

        Assert::IsTrue(pLoadResult.bCalled);
        Assert::AreEqual((int)RC_ABORTED, pLoadResult.nResult);
        Assert::IsFalse(game.IsGameLoading(), L"load count not balanced");
        Assert::AreEqual(1, probe.nNotified);
        Assert::IsTrue(probe.bLockWasFree, L"OnEndGameLoad raised while holding the load lock");
    }

    TEST_METHOD(TestChangeConsoleIfLoadCurrent)
    {
        GameContextHarness game;
        uint32_t nLoadGeneration = 0;
        {
            auto pLock = game.LockLoad();
            nLoadGeneration = game.GetLoadGeneration();
        }

        int nChanges = 0;
        game.ChangeConsoleIfLoadCurrent(nLoadGeneration, [&nChanges]() noexcept { ++nChanges; });
        Assert::AreEqual(1, nChanges);

        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
        }

        // a load that was superseded must not change the console
        game.ChangeConsoleIfLoadCurrent(nLoadGeneration, [&nChanges]() noexcept { ++nChanges; });
        Assert::AreEqual(1, nChanges);
    }

    TEST_METHOD(TestChangeConsoleIfLoadCurrentWaitsForChangeInProgress)
    {
        GameContextHarness game;
        uint32_t nFirstLoad = 0;
        {
            auto pLock = game.LockLoad();
            nFirstLoad = game.GetLoadGeneration();
        }

        std::promise<void> pFirstStarted;
        std::promise<void> pFirstRelease;
        auto fFirstRelease = pFirstRelease.get_future();
        std::mutex mOrder;
        std::vector<int> vOrder;

        // the first load has checked that it is current and is changing the console
        std::thread tFirst([&]() {
            game.ChangeConsoleIfLoadCurrent(nFirstLoad, [&]() {
                pFirstStarted.set_value();
                fFirstRelease.wait();
                std::lock_guard<std::mutex> lock(mOrder);
                vOrder.push_back(1);
            });
        });
        pFirstStarted.get_future().wait();

        // a second load replaces it, and gets to its own console change before the first one has finished
        uint32_t nSecondLoad = 0;
        {
            auto pLock = game.LockLoad();
            game.InvalidatePendingLoads();
            nSecondLoad = game.GetLoadGeneration();
        }
        std::thread tSecond([&]() {
            game.ChangeConsoleIfLoadCurrent(nSecondLoad, [&]() {
                std::lock_guard<std::mutex> lock(mOrder);
                vOrder.push_back(2);
            });
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        bool bSecondWaited = false;
        {
            std::lock_guard<std::mutex> lock(mOrder);
            bSecondWaited = vOrder.empty();
        }

        pFirstRelease.set_value();
        tFirst.join();
        tSecond.join();

        Assert::IsTrue(bSecondWaited, L"second console change did not wait for the first");
        Assert::AreEqual({2U}, vOrder.size());
        Assert::AreEqual(1, vOrder.at(0));
        Assert::AreEqual(2, vOrder.at(1));
    }

    TEST_METHOD(TestLoadGameUserUnlocks)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321",
            "\"HardcoreUnlocks\":[{\"ID\":7,\"When\":1234567890}]",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto* pAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch1);
        Ensures(pAch1 != nullptr);
        Assert::IsTrue(pAch1->IsActive());

        const auto* pAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch2);
        Ensures(pAch2 != nullptr);
        Assert::IsFalse(pAch2->IsActive());

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"2 achievements, 15 points"), pPopup->GetDescription());
        Assert::AreEqual(std::wstring(L"You have earned 1 achievements"), pPopup->GetDetail());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());
    }

    TEST_METHOD(TestLoadGameUserUnlocksUnpromoted)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321",
            "\"HardcoreUnlocks\":[{\"ID\":7,\"When\":1234567890},{\"ID\":9,\"When\":1234567890}]",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":9,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
              "\"Points\":25,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":11,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
              "\"Points\":50,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto* pAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch1);
        Ensures(pAch1 != nullptr);
        Assert::IsTrue(pAch1->IsActive());

        const auto* pAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch2);
        Ensures(pAch2 != nullptr);
        Assert::IsFalse(pAch2->IsActive());

        const auto* pAch3 = game.Assets().FindAchievement(9U);
        Assert::IsNotNull(pAch3);
        Ensures(pAch3 != nullptr);
        Assert::IsFalse(pAch3->IsActive());

        // unpromoted achievement should not be activated even if it wasn't unlocked
        // (expect unpromoted achievements to not be unlocked)
        const auto* pAch4 = game.Assets().FindAchievement(11U);
        Assert::IsNotNull(pAch4);
        Ensures(pAch4 != nullptr);
        Assert::IsFalse(pAch4->IsActive());

        // only core achievements should be tallied for the popup
        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"2 achievements, 15 points"), pPopup->GetDescription());
        Assert::AreEqual(std::wstring(L"You have earned 1 achievements"), pPopup->GetDetail());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());
    }

    TEST_METHOD(TestLoadGameUserUnlocksCompatibilityMode)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321",
            "\"HardcoreUnlocks\":[{\"ID\":7,\"When\":1234567890}]",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321", ra::data::context::GameContext::Mode::CompatibilityTest);

        const auto* pAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch1);
        Ensures(pAch1 != nullptr);
        Assert::IsTrue(pAch1->IsActive());

        const auto* pAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch2);
        Ensures(pAch2 != nullptr);
        Assert::IsTrue(pAch2->IsActive());
    }

    TEST_METHOD(TestLoadGameUserUnlocksLocalModifications)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockStorage.MockStoredData(ra::services::StorageItemType::UserAchievements, L"1",
            "Version\n"
            "Game\n"
            "7:1=2:Ach2b:Desc2b::::Auth2b:25:1234554321:1234555555:::54321\n"
        );

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        const auto* pAch1 = game.Assets().FindAchievement(5U);
        Assert::IsNotNull(pAch1);
        Ensures(pAch1 != nullptr);
        Assert::AreEqual(ra::data::models::AssetChanges::None, pAch1->GetChanges());
        Assert::IsTrue(pAch1->IsActive());

        const auto* pAch2 = game.Assets().FindAchievement(7U);
        Assert::IsNotNull(pAch2);
        Ensures(pAch2 != nullptr);
        Assert::AreEqual(ra::data::models::AssetChanges::Unpublished, pAch2->GetChanges());
        Assert::IsFalse(pAch2->IsActive()); // should not be active because it's modified

        const auto* pPopup = game.mockOverlayManager.GetMessage(1);
        Expects(pPopup != nullptr);
        Assert::IsNotNull(pPopup);
        Assert::AreEqual(std::wstring(L"Loaded GameTitle"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"2 achievements, 15 points"), pPopup->GetDescription());
        Assert::AreEqual(std::wstring(L"You have earned 0 achievements"), pPopup->GetDetail());
        Assert::AreEqual(std::string("9743"), pPopup->GetImage().Name());
    }

    TEST_METHOD(TestLoadGamePausesRuntime)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        bool bBeforeResponseCalled = false;
        game.mockRcClient.OnBeforeResponse("r=achievementsets&u=Username&t=ApiToken&m=0123456789abcdeffedcba987654321&v=2",
            [&game, &bBeforeResponseCalled]() {
                bBeforeResponseCalled = true;
                Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bBeforeResponseCalled); // paused during load
        Assert::IsFalse(game.mockAchievementRuntime.IsPaused()); // not paused after load
    }

    TEST_METHOD(TestLoadGameWhileRuntimePaused)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        bool bBeforeResponseCalled = false;
        game.mockRcClient.OnBeforeResponse("r=achievementsets&u=Username&t=ApiToken&m=0123456789abcdeffedcba987654321&v=2",
            [&game, &bBeforeResponseCalled]() {
                bBeforeResponseCalled = true;
                Assert::IsTrue(game.mockAchievementRuntime.IsPaused());
            });

        game.mockAchievementRuntime.SetPaused(true);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        Assert::IsTrue(bBeforeResponseCalled); // paused during load
        Assert::IsTrue(game.mockAchievementRuntime.IsPaused()); // paused after load
    }

    TEST_METHOD(TestSetModeNotify)
    {
        class NotifyHarness : public GameContext::NotifyTarget
        {
        public:
            bool m_bNotified = false;

        protected:
            void OnActiveGameChanged() noexcept override { m_bNotified = true; }
        };
        NotifyHarness notifyHarness;

        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321");

        Assert::AreEqual(GameContext::Mode::Normal, game.GetMode());

        game.AddNotifyTarget(notifyHarness);
        game.LoadGame(0U, "");

        Assert::AreEqual(0U, game.GameId());
        Assert::IsFalse(notifyHarness.m_bNotified);

        game.SetMode(GameContext::Mode::CompatibilityTest);
        Assert::AreEqual(GameContext::Mode::CompatibilityTest, game.GetMode());
        Assert::IsFalse(notifyHarness.m_bNotified);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321", GameContext::Mode::CompatibilityTest);
        Assert::AreEqual(1U, game.GameId());
        Assert::AreEqual(GameContext::Mode::CompatibilityTest, game.GetMode());
        Assert::IsTrue(notifyHarness.m_bNotified);

        notifyHarness.m_bNotified = false;
        game.SetMode(GameContext::Mode::Normal);
        Assert::AreEqual(GameContext::Mode::Normal, game.GetMode());
        Assert::IsTrue(notifyHarness.m_bNotified);

        // not changing mode should not notify again
        notifyHarness.m_bNotified = false;
        game.SetMode(GameContext::Mode::Normal);
        Assert::AreEqual(GameContext::Mode::Normal, game.GetMode());
        Assert::IsFalse(notifyHarness.m_bNotified);
    }


    TEST_METHOD(TestLoadGameNonHardcoreWarningHardcore)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, true);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // no dialogs when game with achievements loaded in hardcore mode
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsFalse(game.mockDesktop.WasDialogShown());
        Assert::IsNull(game.mockOverlayManager.GetMessage(2U)); // message 1 will be game placard
    }

    TEST_METHOD(TestLoadGameNonHardcoreWarningLeaderboards)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, false);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Leaderboards, true);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // should get popup message indicating leaderboards not supported in casual mode
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsFalse(game.mockDesktop.WasDialogShown());

        const auto* pPopup = game.mockOverlayManager.GetMessage(2U);
        Expects(pPopup != nullptr);
        Assert::AreEqual(std::wstring(L"Playing in Casual Mode"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L"Leaderboard entries will not be submitted."), pPopup->GetDescription());
    }

    TEST_METHOD(TestLoadGameNonHardcoreWarningLeaderboardsOff)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, false);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Leaderboards, false);
        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // should get popup message indicating playing in casual mode
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsFalse(game.mockDesktop.WasDialogShown());

        const auto* pPopup = game.mockOverlayManager.GetMessage(2U);
        Expects(pPopup != nullptr);
        Assert::AreEqual(std::wstring(L"Playing in Casual Mode"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L""), pPopup->GetDescription());
    }

    TEST_METHOD(TestLoadGameNonHardcoreWarningCancel)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, true);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [](ra::ui::viewmodels::MessageBoxViewModel& vmMessageBox) {
                Assert::AreEqual(std::wstring(L"Enable Hardcore mode?"), vmMessageBox.GetHeader());
                Assert::AreEqual(std::wstring(L"You are loading a game with achievements and do not currently have hardcore mode enabled."), vmMessageBox.GetMessage());
                Assert::AreEqual(ra::ui::viewmodels::MessageBoxViewModel::Icon::Warning, vmMessageBox.GetIcon());
                Assert::AreEqual(ra::ui::viewmodels::MessageBoxViewModel::Buttons::YesNo, vmMessageBox.GetButtons());
                return ra::ui::DialogResult::No;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // should get confirmation dialog about hardcore and no popups
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsTrue(game.mockDesktop.WasDialogShown());
        Assert::IsNull(game.mockOverlayManager.GetMessage(2U));

        // hardcore should remain disabled
        Assert::IsFalse(game.mockConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore));
    }

    TEST_METHOD(TestLoadGameNonHardcoreWarningAccept)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
            "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":3,"
              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999},"
            "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":3,"
              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
              "\"Created\":1234567890,\"Modified\":123459999}"
        );

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, true);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);
        game.mockDesktop.ExpectWindow<ra::ui::viewmodels::MessageBoxViewModel>(
            [](ra::ui::viewmodels::MessageBoxViewModel& vmMessageBox) {
                Assert::AreEqual(std::wstring(L"Enable Hardcore mode?"), vmMessageBox.GetHeader());
                Assert::AreEqual(std::wstring(L"You are loading a game with achievements and do not currently have hardcore mode enabled."), vmMessageBox.GetMessage());
                Assert::AreEqual(ra::ui::viewmodels::MessageBoxViewModel::Icon::Warning, vmMessageBox.GetIcon());
                Assert::AreEqual(ra::ui::viewmodels::MessageBoxViewModel::Buttons::YesNo, vmMessageBox.GetButtons());
                return ra::ui::DialogResult::Yes;
            });

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // should get confirmation dialog about hardcore and no popups
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsTrue(game.mockDesktop.WasDialogShown());
        Assert::IsNull(game.mockOverlayManager.GetMessage(2U));

        // hardcore should remain disabled
        Assert::IsTrue(game.mockConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore));
    }

    TEST_METHOD(TestLoadGameNonHardcoreWarningNoCoreAchievements)
    {
        GameContextHarness game;
        game.MockLoadGameAPIs(1U, "0123456789abcdeffedcba987654321", "",
                              "{\"ID\":5,\"Title\":\"Ach1\",\"Description\":\"Desc1\",\"Flags\":5,"
                              "\"Points\":5,\"MemAddr\":\"1=1\",\"Author\":\"Auth1\",\"BadgeName\":\"12345\","
                              "\"Created\":1234567890,\"Modified\":123459999},"
                              "{\"ID\":7,\"Title\":\"Ach2\",\"Description\":\"Desc2\",\"Flags\":5,"
                              "\"Points\":10,\"MemAddr\":\"1=1\",\"Author\":\"Auth2\",\"BadgeName\":\"12345\","
                              "\"Created\":1234567890,\"Modified\":123459999}");

        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::NonHardcoreWarning, true);
        game.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Hardcore, false);

        game.LoadGame(1U, "0123456789abcdeffedcba987654321");

        // should only get casual mode popup
        game.mockFrameEventQueue.DoFrame(); // hardcore warning gets queued for the UI thread
        Assert::IsFalse(game.mockDesktop.WasDialogShown());

        const auto* pPopup = game.mockOverlayManager.GetMessage(2U);
        Expects(pPopup != nullptr);
        Assert::AreEqual(std::wstring(L"Playing in Casual Mode"), pPopup->GetTitle());
        Assert::AreEqual(std::wstring(L""), pPopup->GetDescription());

        // hardcore should remain disabled
        Assert::IsFalse(game.mockConfiguration.IsFeatureEnabled(ra::services::Feature::Hardcore));
    }

    TEST_METHOD(TestInitializeSubsetsCoreOnly)
    {
        GameContextHarness game;
        rc_api_achievement_set_definition_t subsets[1];
        rc_api_fetch_game_sets_response_t sets;

        memset(&subsets, 0, sizeof(subsets));
        memset(&sets, 0, sizeof(sets));

        subsets[0].id = 11111;
        subsets[0].game_id = 2222;
        subsets[0].title = "Game Title";
        subsets[0].type = RC_ACHIEVEMENT_SET_TYPE_CORE;

        sets.id = sets.session_game_id = subsets[0].game_id;
        sets.sets = subsets;
        sets.num_sets = sizeof(subsets) / sizeof(subsets[0]);

        game.InitializeAchievementSets(&sets);

        Assert::AreEqual(2222U, game.GameId());
        Assert::AreEqual(2222U, game.ActiveGameId());

        Assert::AreEqual({1}, game.Assets().AchievementSets().Count());

        const auto& pSubset = *game.Assets().AchievementSets().GetItemAt(0);
        Assert::AreEqual(11111U, pSubset.GetID());
        Assert::AreEqual(2222U, pSubset.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Core, pSubset.GetType());
        Assert::AreEqual(std::wstring(L"Game Title"), pSubset.GetTitle());
    }

    TEST_METHOD(TestInitializeSubsetsCoreWithBonus)
    {
        GameContextHarness game;
        rc_api_achievement_set_definition_t subsets[3];
        rc_api_fetch_game_sets_response_t sets;

        memset(&subsets, 0, sizeof(subsets));
        memset(&sets, 0, sizeof(sets));

        subsets[0].id = 11111;
        subsets[0].game_id = 2222;
        subsets[0].title = "Game Title";
        subsets[0].type = RC_ACHIEVEMENT_SET_TYPE_CORE;

        subsets[1].id = 4444;
        subsets[1].game_id = 3333;
        subsets[1].title = "Bonus";
        subsets[1].type = RC_ACHIEVEMENT_SET_TYPE_BONUS;

        subsets[2].id = 6666;
        subsets[2].game_id = 5555;
        subsets[2].title = "Perfect Play";
        subsets[2].type = RC_ACHIEVEMENT_SET_TYPE_BONUS;

        sets.id = sets.session_game_id = subsets[0].game_id;
        sets.sets = subsets;
        sets.num_sets = sizeof(subsets) / sizeof(subsets[0]);

        game.InitializeAchievementSets(&sets);

        Assert::AreEqual(2222U, game.GameId());
        Assert::AreEqual(2222U, game.ActiveGameId());

        Assert::AreEqual({3}, game.Assets().AchievementSets().Count());

        const auto& pSubset = *game.Assets().AchievementSets().GetItemAt(0);
        Assert::AreEqual(11111U, pSubset.GetID());
        Assert::AreEqual(2222U, pSubset.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Core, pSubset.GetType());
        Assert::AreEqual(std::wstring(L"Game Title"), pSubset.GetTitle());

        const auto& pSubset2 = *game.Assets().AchievementSets().GetItemAt(1);
        Assert::AreEqual(4444U, pSubset2.GetID());
        Assert::AreEqual(3333U, pSubset2.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Bonus, pSubset2.GetType());
        Assert::AreEqual(std::wstring(L"Bonus"), pSubset2.GetTitle());

        const auto& pSubset3 = *game.Assets().AchievementSets().GetItemAt(2);
        Assert::AreEqual(6666U, pSubset3.GetID());
        Assert::AreEqual(5555U, pSubset3.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Bonus, pSubset3.GetType());
        Assert::AreEqual(std::wstring(L"Perfect Play"), pSubset3.GetTitle());
    }

    TEST_METHOD(TestInitializeSubsetsExclusive)
    {
        GameContextHarness game;
        rc_api_achievement_set_definition_t subsets[1];
        rc_api_fetch_game_sets_response_t sets;

        memset(&subsets, 0, sizeof(subsets));
        memset(&sets, 0, sizeof(sets));

        subsets[0].id = 1111;
        subsets[0].game_id = 2222;
        subsets[0].title = "Subset Title";
        subsets[0].type = RC_ACHIEVEMENT_SET_TYPE_EXCLUSIVE;

        sets.id = 3333;
        sets.title = "Game Title";
        sets.session_game_id = 2222;
        sets.sets = subsets;
        sets.num_sets = sizeof(subsets) / sizeof(subsets[0]);

        game.InitializeAchievementSets(&sets);

        Assert::AreEqual(3333U, game.GameId());
        Assert::AreEqual(2222U, game.ActiveGameId());

        Assert::AreEqual({1}, game.Assets().AchievementSets().Count());

        const auto& pSubset = *game.Assets().AchievementSets().GetItemAt(0);
        Assert::AreEqual(1111U, pSubset.GetID());
        Assert::AreEqual(2222U, pSubset.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Exclusive, pSubset.GetType());
        Assert::AreEqual(std::wstring(L"Subset Title (Game Title)"), pSubset.GetTitle());
    }

    TEST_METHOD(TestInitializeSubsetsSpecialtyWithBonus)
    {
        GameContextHarness game;
        rc_api_achievement_set_definition_t subsets[3];
        rc_api_fetch_game_sets_response_t sets;

        memset(&subsets, 0, sizeof(subsets));
        memset(&sets, 0, sizeof(sets));

        subsets[0].id = 6666;
        subsets[0].game_id = 2222;
        subsets[0].title = "Game Title";
        subsets[0].type = RC_ACHIEVEMENT_SET_TYPE_CORE;

        subsets[1].id = 7777;
        subsets[1].game_id = 5555;
        subsets[1].title = "Perfect Play";
        subsets[1].type = RC_ACHIEVEMENT_SET_TYPE_SPECIALTY;

        subsets[2].id = 4444;
        subsets[2].game_id = 3333;
        subsets[2].title = "Bonus";
        subsets[2].type = RC_ACHIEVEMENT_SET_TYPE_BONUS;

        sets.id = subsets[0].game_id;
        sets.session_game_id = subsets[1].game_id;
        sets.sets = subsets;
        sets.num_sets = sizeof(subsets) / sizeof(subsets[0]);

        game.InitializeAchievementSets(&sets);

        Assert::AreEqual(2222U, game.GameId());
        Assert::AreEqual(5555U, game.ActiveGameId());

        Assert::AreEqual({3}, game.Assets().AchievementSets().Count());

        const auto& pSubset = *game.Assets().AchievementSets().GetItemAt(0);
        Assert::AreEqual(6666U, pSubset.GetID());
        Assert::AreEqual(2222U, pSubset.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Core, pSubset.GetType());
        Assert::AreEqual(std::wstring(L"Game Title"), pSubset.GetTitle());

        const auto& pSubset2 = *game.Assets().AchievementSets().GetItemAt(1);
        Assert::AreEqual(7777U, pSubset2.GetID());
        Assert::AreEqual(5555U, pSubset2.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Specialty, pSubset2.GetType());
        Assert::AreEqual(std::wstring(L"Perfect Play"), pSubset2.GetTitle());

        const auto& pSubset3 = *game.Assets().AchievementSets().GetItemAt(2);
        Assert::AreEqual(4444U, pSubset3.GetID());
        Assert::AreEqual(3333U, pSubset3.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Bonus, pSubset3.GetType());
        Assert::AreEqual(std::wstring(L"Bonus"), pSubset3.GetTitle());
    }

    TEST_METHOD(TestInitializeSubsetsVirtual)
    {
        GameContextHarness game;
        rc_api_achievement_set_definition_t subsets[1];
        rc_api_fetch_game_sets_response_t sets;

        memset(&subsets, 0, sizeof(subsets));
        memset(&sets, 0, sizeof(sets));

        subsets[0].id = 11111;
        subsets[0].game_id = 2222 + 100000000;
        subsets[0].title = "Unsupported Game Version";
        subsets[0].type = RC_ACHIEVEMENT_SET_TYPE_CORE;

        sets.id = sets.session_game_id = subsets[0].game_id;
        sets.sets = subsets;
        sets.num_sets = sizeof(subsets) / sizeof(subsets[0]);

        game.InitializeAchievementSets(&sets);

        Assert::AreEqual(2222U, game.GameId());
        Assert::AreEqual(2222U, game.ActiveGameId());

        Assert::AreEqual({1}, game.Assets().AchievementSets().Count());

        const auto& pSubset = *game.Assets().AchievementSets().GetItemAt(0);
        Assert::AreEqual(11111U, pSubset.GetID());
        Assert::AreEqual(2222U, pSubset.GetBackingGameID());
        Assert::AreEqual(ra::data::models::AchievementSetType::Core, pSubset.GetType());
        Assert::AreEqual(std::wstring(L"Unsupported Game Version"), pSubset.GetTitle());
    }
};

} // namespace tests
} // namespace context
} // namespace data
} // namespace ra
