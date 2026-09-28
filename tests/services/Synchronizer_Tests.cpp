#include "services/AchievementRuntime.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace tests {

using ra::services::AchievementRuntime;

TEST_CLASS(Synchronizer_Tests)
{
public:
    TEST_METHOD(TestCompleteOnAnotherThreadWakesTheWaiter)
    {
        // The poll is far longer than the test's deadline: only Complete's notify can wake the waiter in time.
        struct State
        {
            AchievementRuntime::Synchronizer oSynchronizer;
            std::atomic<bool> bCompleted{false};
        };
        auto* pState = new State();

        ra::tests::DetachedCall oWaiter([pState]() {
            pState->bCompleted = pState->oSynchronizer.WaitUntil([]() noexcept { return false; }, std::chrono::seconds(30));
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // let it start waiting
        pState->oSynchronizer.Complete(RC_OK, nullptr);
        const bool bReturned = oWaiter.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bReturned, L"Complete did not wake the waiter");
        Assert::IsTrue(pState->bCompleted.load());
        Assert::AreEqual(static_cast<int>(RC_OK), pState->oSynchronizer.GetResult());

        delete pState; // reached only when the waiter returned
    }

    TEST_METHOD(TestACompletionBeforeTheWaitReturnsAtOnce)
    {
        AchievementRuntime::Synchronizer oSynchronizer;
        oSynchronizer.Complete(RC_INVALID_STATE, "Login already in progress");

        bool bAskedToGiveUp = false;
        const bool bCompleted = oSynchronizer.WaitUntil([&bAskedToGiveUp]() noexcept {
            bAskedToGiveUp = true;
            return true;
        }, std::chrono::milliseconds(10));

        Assert::IsTrue(bCompleted);
        Assert::IsFalse(bAskedToGiveUp, L"asked to give up although it had completed");
        Assert::AreEqual(static_cast<int>(RC_INVALID_STATE), oSynchronizer.GetResult());
        Assert::AreEqual(std::string("Login already in progress"), oSynchronizer.GetErrorMessage());
    }

    TEST_METHOD(TestItGivesUpWhenToldAndIgnoresALateComplete)
    {
        struct State
        {
            AchievementRuntime::Synchronizer oSynchronizer;
            std::atomic<bool> bShuttingDown{false};
            std::atomic<int> nCompleted{-1};
        };
        auto* pState = new State();

        ra::tests::DetachedCall oWaiter([pState]() {
            pState->nCompleted = pState->oSynchronizer.WaitUntil(
                [pState]() noexcept { return pState->bShuttingDown.load(); }, std::chrono::milliseconds(10)) ? 1 : 0;
        });
        pState->bShuttingDown = true;
        const bool bReturned = oWaiter.FinishedWithin(std::chrono::seconds(5));
        pState->oSynchronizer.Complete(RC_OK, nullptr); // the callback, arriving after it gave up

        Assert::IsTrue(bReturned, L"it never gave up");
        Assert::AreEqual(0, pState->nCompleted.load(), L"it said it completed");
        Assert::AreEqual(static_cast<int>(RC_ABORTED), pState->oSynchronizer.GetResult(),
                         L"a late Complete changed the result");
        Assert::AreEqual(std::string("Shutting down"), pState->oSynchronizer.GetErrorMessage());

        delete pState;
    }

    TEST_METHOD(TestALateCallbackFindsItAliveAfterTheWaiterHasGone)
    {
        // The waiter gives up and drops its reference; the callback's copy keeps the object alive. Under AddressSanitizer
        // a freed object would be a report.
        auto pSynchronizer = std::make_shared<AchievementRuntime::Synchronizer>();
        void* pUserdata = AchievementRuntime::Synchronizer::Share(pSynchronizer);
        const std::weak_ptr<AchievementRuntime::Synchronizer> pWatch = pSynchronizer;

        pSynchronizer->WaitUntil([]() noexcept { return true; }, std::chrono::milliseconds(1));
        pSynchronizer.reset(); // the waiter has gone

        const bool bAliveForTheCallback = !pWatch.expired();
        AchievementRuntime::Synchronizer::CompleteShared(pUserdata, RC_OK, nullptr);

        Assert::IsTrue(bAliveForTheCallback, L"the callback's copy did not keep it alive");
        Assert::IsTrue(pWatch.expired(), L"the callback did not release its copy");
    }
};

} // namespace tests
} // namespace services
} // namespace ra
