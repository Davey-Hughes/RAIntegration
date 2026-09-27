#include "data/NotifyTargetSet.hh"

#include "testutil/CppUnitTest.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace ra {
namespace data {
namespace tests {

TEST_CLASS(NotifyTargetSet_Tests)
{
    using NotifyTargetSetString = NotifyTargetSet<std::string>;

    struct Counter
    {
        std::atomic<int> nCalls{ 0 };
    };

    // A ForEachTarget call into oTarget, made on another thread and held inside
    // the handler until Release(). Heap-allocate it, and delete it only after
    // Finished() returned true: a call that timed out is still using it.
    struct HeldCall
    {
        NotifyTargetSet<Counter> set;
        Counter oTarget;
        Counter oOther;
        std::promise<void> oEntered;
        std::promise<void> oRelease;
        std::future<void> fEntered = oEntered.get_future();
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::unique_ptr<ra::tests::DetachedCall> pNotify;

        void Start()
        {
            set.Add(oTarget);
            pNotify = std::make_unique<ra::tests::DetachedCall>([this]() {
                set.ForEachTarget([this](Counter& oCalled) {
                    ++oCalled.nCalls;
                    oEntered.set_value();
                    fRelease.wait();
                });
            });
        }

        bool WaitUntilEntered() { return fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready; }
        void Release() { oRelease.set_value(); }
        bool Finished() { return pNotify->FinishedWithin(std::chrono::seconds(5)); }
    };

    // Holds a call into the target on another thread, runs fPrepare, then checks
    // that RemoveAndWait on a third thread returns only once the call is released.
    template<typename TPrepare>
    static void AssertRemoveAndWaitWaitsForTheCall(TPrepare fPrepare)
    {
        auto* pHeld = new HeldCall();
        pHeld->Start();
        const bool bEntered = pHeld->WaitUntilEntered();

        fPrepare(pHeld->set, pHeld->oOther);

        ra::tests::DetachedCall oRemove([pHeld]() noexcept { pHeld->set.RemoveAndWait(pHeld->oTarget); });
        const bool bRemovedDuringTheCall = oRemove.FinishedWithin(std::chrono::milliseconds(200));

        pHeld->Release();
        const bool bRemovedAfterIt = oRemove.FinishedWithin(std::chrono::seconds(5));
        const bool bNotifyFinished = pHeld->Finished();

        Assert::IsTrue(bEntered, L"the call never started");
        Assert::IsFalse(bRemovedDuringTheCall, L"RemoveAndWait returned while another thread was inside the target");
        Assert::IsTrue(bRemovedAfterIt, L"RemoveAndWait never returned after the call ended");
        Assert::IsTrue(bNotifyFinished, L"the pass never finished");

        delete pHeld; // reached only when every call finished: on a failure above it is leaked on purpose
    }

public:
    TEST_METHOD(TestAddAndRemove)
    {
        NotifyTargetSetString set;
        std::string one = "one";
        std::string two = "two";
        std::string three = "three";
        std::string four = "four";

        Assert::AreEqual({ 0U }, set.Targets().size());

        set.Add(one);
        set.Add(two);
        Assert::AreEqual({ 2U }, set.Targets().size());

        set.Add(one);
        set.Add(three);
        Assert::AreEqual({ 3U }, set.Targets().size());

        set.Remove(two);
        Assert::AreEqual({ 2U }, set.Targets().size());

        set.Remove(two);
        set.Remove(one);
        Assert::AreEqual({ 1U }, set.Targets().size());

        set.Add(two);
        Assert::AreEqual({ 2U }, set.Targets().size());

        for (auto& str : set.Targets())
            str.push_back('!');

        Assert::AreEqual(std::string("one"), one);
        Assert::AreEqual(std::string("two!"), two);
        Assert::AreEqual(std::string("three!"), three);
        Assert::AreEqual(std::string("four"), four);

        set.Remove(three);
        set.Remove(two);
        Assert::AreEqual({ 0U }, set.Targets().size());

        for (auto& str : set.Targets())
            str.push_back('?');

        Assert::AreEqual(std::string("one"), one);
        Assert::AreEqual(std::string("two!"), two);
        Assert::AreEqual(std::string("three!"), three);
        Assert::AreEqual(std::string("four"), four);
    }

    TEST_METHOD(TestLockAndUnlock)
    {
        NotifyTargetSetString set;
        std::string one = "one";
        std::string two = "two";
        std::string three = "three";
        std::string four = "four";

        auto targets = set.Targets();
        Assert::AreEqual({ 0U }, targets.size());

        set.Lock();
        set.Add(one);
        Assert::AreEqual({ 1U }, set.Targets().size());
        Assert::AreEqual({ 0U }, targets.size());
        set.Unlock();
        targets = set.Targets();
        Assert::AreEqual({ 1U }, targets.size());

        set.Lock();
        set.Add(two);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 1U }, targets.size());
        set.Unlock();
        targets = set.Targets();
        Assert::AreEqual({ 2U }, targets.size());

        set.Lock();
        set.Remove(one);
        Assert::AreEqual({ 1U }, set.Targets().size());
        Assert::AreEqual({ 2U }, targets.size());
        set.Unlock();
        targets = set.Targets();
        Assert::AreEqual({ 1U }, targets.size());

        set.Lock();
        set.Remove(two);
        Assert::AreEqual({ 0U }, set.Targets().size());
        Assert::AreEqual({ 1U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 0U }, set.Targets().size());
    }

    TEST_METHOD(TestLockIfNotEmpty)
    {
        NotifyTargetSetString set;
        std::string one = "one";
        std::string two = "two";
        std::string three = "three";
        std::string four = "four";

        Assert::AreEqual({ 0U }, set.Targets().size());

        // empty set not locked by LockIfNotEmpty
        Assert::AreEqual(false, set.LockIfNotEmpty());
        set.Add(one);
        Assert::AreEqual({ 1U }, set.Targets().size());

        // no-op. set is not locked
        set.Unlock();
        Assert::AreEqual({ 1U }, set.Targets().size());

        // non empty set is locked by LockIfNotEmpty
        Assert::AreEqual(true, set.LockIfNotEmpty());
        auto targets = set.Targets();
        set.Add(two);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 1U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 2U }, set.Targets().size());

        // non empty set is locked by LockIfNotEmpty
        Assert::AreEqual(true, set.LockIfNotEmpty());
        targets = set.Targets();
        set.Remove(one);
        Assert::AreEqual({ 1U }, set.Targets().size());
        Assert::AreEqual({ 2U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 1U }, set.Targets().size());

        // non empty set is locked by LockIfNotEmpty
        Assert::AreEqual(true, set.LockIfNotEmpty());
        targets = set.Targets();
        set.Remove(two);
        Assert::AreEqual({ 0U }, set.Targets().size());
        Assert::AreEqual({ 1U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 0U }, set.Targets().size());

        // empty set not locked by LockIfNotEmpty
        Assert::AreEqual(false, set.LockIfNotEmpty());
    }

    TEST_METHOD(TestLockAndUnlockMultiple)
    {
        NotifyTargetSetString set;
        std::string one = "one";
        std::string two = "two";
        std::string three = "three";
        std::string four = "four";

        Assert::AreEqual({ 0U }, set.Targets().size());

        auto targets = set.Targets();
        set.Lock();
        set.Add(one);
        set.Add(two);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 0U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 2U }, set.Targets().size());

        targets = set.Targets();
        set.Lock();
        set.Add(three);
        set.Remove(one);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 2U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 2U }, set.Targets().size());

        targets = set.Targets();
        set.Lock();
        set.Add(three);
        set.Remove(one);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 2U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 2U }, set.Targets().size());

        targets = set.Targets();
        set.Lock();
        set.Add(four);
        set.Remove(four);
        Assert::AreEqual({ 2U }, set.Targets().size());
        Assert::AreEqual({ 2U }, targets.size());
        set.Unlock();
        Assert::AreEqual({ 2U }, set.Targets().size());

        for (auto& str : set.Targets())
            str.push_back('!');

        Assert::AreEqual(std::string("one"), one);
        Assert::AreEqual(std::string("two!"), two);
        Assert::AreEqual(std::string("three!"), three);
        Assert::AreEqual(std::string("four"), four);
    }

    TEST_METHOD(TestConcurrentChangesDuringTargetsPasses)
    {
        // Threads adding and removing targets while others walk the set, as the
        // Qt thread (bindings coming and going) and the emulator thread and pool
        // workers (notifying) do. Uses only the API that predates ForEachTarget.
        // In a normal build a race here usually goes unnoticed; run it under
        // checks/tsan-notify.sh, which reports one. The run is inside a
        // DetachedCall so a stuck pass fails the test instead of hanging ctest.
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oPermanent;
            std::array<Counter, 8> vTransient;
            std::atomic<bool> bStop{ false };
        };
        auto* pState = new State();
        pState->set.Add(pState->oPermanent);

        ra::tests::DetachedCall oRun([pState]() {
            std::vector<std::thread> vMutators;
            for (int nThread = 0; nThread < 2; ++nThread)
            {
                vMutators.emplace_back([pState, nThread]() {
                    for (int i = 0; i < 2000; ++i)
                    {
                        pState->set.Add(pState->vTransient.at(gsl::narrow_cast<size_t>((i + nThread) % 8)));
                        pState->set.Remove(pState->vTransient.at(gsl::narrow_cast<size_t>((i + nThread + 3) % 8)));
                    }
                });
            }

            std::vector<std::thread> vWalkers;
            for (int nThread = 0; nThread < 2; ++nThread)
            {
                vWalkers.emplace_back([pState]() noexcept {
                    do
                    {
                        if (pState->set.LockIfNotEmpty())
                        {
                            for (auto& oTarget : pState->set.Targets())
                                ++oTarget.nCalls;

                            pState->set.Unlock();
                        }
                    } while (!pState->bStop);
                });
            }

            for (auto& tMutator : vMutators)
                tMutator.join();
            pState->bStop = true;
            for (auto& tWalker : vWalkers)
                tWalker.join();
        });
        const bool bFinished = oRun.FinishedWithin(std::chrono::seconds(30));
        pState->bStop = true;

        Assert::IsTrue(bFinished, L"the stress run did not finish: a pass is stuck");

        Assert::IsTrue(pState->oPermanent.nCalls > 0, L"no pass ever ran");

        bool bPermanentFound = false;
        for (auto& oTarget : pState->set.Targets())
            bPermanentFound |= (&oTarget == &pState->oPermanent);
        Assert::IsTrue(bPermanentFound, L"a target nothing removed went missing");

        delete pState; // reached only when the run finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestConcurrentChangesDuringForEachTargetPasses)
    {
        // As above, through ForEachTarget, and the mutators remove with
        // RemoveAndWait, which also waits for any walker inside the target it
        // removed. The run is inside a DetachedCall so a stuck RemoveAndWait
        // fails the test instead of hanging ctest.
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oPermanent;
            std::array<Counter, 8> vTransient;
            std::atomic<bool> bStop{ false };
        };
        auto* pState = new State();
        pState->set.Add(pState->oPermanent);

        ra::tests::DetachedCall oRun([pState]() {
            std::vector<std::thread> vMutators;
            for (int nThread = 0; nThread < 2; ++nThread)
            {
                vMutators.emplace_back([pState, nThread]() {
                    for (int i = 0; i < 2000; ++i)
                    {
                        pState->set.Add(pState->vTransient.at(gsl::narrow_cast<size_t>((i + nThread) % 8)));
                        pState->set.RemoveAndWait(
                            pState->vTransient.at(gsl::narrow_cast<size_t>((i + nThread + 3) % 8)));
                    }
                });
            }

            std::vector<std::thread> vWalkers;
            for (int nThread = 0; nThread < 2; ++nThread)
            {
                vWalkers.emplace_back([pState]() {
                    do
                    {
                        pState->set.ForEachTarget([](Counter& oTarget) noexcept { ++oTarget.nCalls; });
                    } while (!pState->bStop);
                });
            }

            for (auto& tMutator : vMutators)
                tMutator.join();
            pState->bStop = true;
            for (auto& tWalker : vWalkers)
                tWalker.join();
        });
        const bool bFinished = oRun.FinishedWithin(std::chrono::seconds(30));
        pState->bStop = true;

        Assert::IsTrue(bFinished, L"the stress run did not finish: a RemoveAndWait or a pass is stuck");

        Assert::IsTrue(pState->oPermanent.nCalls > 0, L"no pass ever ran");
        Assert::IsFalse(pState->set.IsEmpty());

        delete pState; // reached only when the run finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestNoCallStartsAfterRemoveAndWaitReturns)
    {
        // Once RemoveAndWait returns, its caller may destroy the target, so no
        // pass may call it afterwards - not even one that took its list before
        // the removal. That holds because ForEachTarget checks that the target
        // is still listed and records the call in one critical section. Split
        // them, and a walker can pass the check, RemoveAndWait can find no call
        // to wait for and return, and the walker then calls the target. Here
        // one mutator adds each target, removes it with RemoveAndWait and then
        // retires it; a walker handed a retired target counts a violation.
        // Measured with the two split: 20 of 20 runs failed, each with 1,600 to
        // 2,500 violations in its 40,000 removals. The run is inside a
        // DetachedCall so a stuck RemoveAndWait fails the test instead of
        // hanging ctest.
        struct Target
        {
            std::atomic<bool> bRetired{ false };
        };
        struct State
        {
            NotifyTargetSet<Target> set;
            std::array<Target, 8> vTargets;
            std::atomic<bool> bStop{ false };
            std::atomic<int> nCalls{ 0 };
            std::atomic<int> nViolations{ 0 };
        };
        auto* pState = new State();

        ra::tests::DetachedCall oRun([pState]() {
            std::vector<std::thread> vWalkers;
            for (int nThread = 0; nThread < 2; ++nThread)
            {
                vWalkers.emplace_back([pState]() {
                    do
                    {
                        pState->set.ForEachTarget([pState](Target& oTarget) noexcept {
                            ++pState->nCalls;
                            if (oTarget.bRetired)
                                ++pState->nViolations;
                        });
                    } while (!pState->bStop);
                });
            }

            std::thread tMutator([pState]() noexcept {
                for (int i = 0; i < 5000; ++i)
                {
                    for (auto& oTarget : pState->vTargets)
                    {
                        oTarget.bRetired = false;
                        pState->set.Add(oTarget);
                        std::this_thread::yield();
                        pState->set.RemoveAndWait(oTarget);
                        oTarget.bRetired = true;
                    }
                }
            });

            tMutator.join();
            pState->bStop = true;
            for (auto& tWalker : vWalkers)
                tWalker.join();
        });
        const bool bFinished = oRun.FinishedWithin(std::chrono::seconds(30));
        pState->bStop = true;

        Assert::IsTrue(bFinished, L"the stress run did not finish: a RemoveAndWait or a pass is stuck");

        Assert::IsTrue(pState->nCalls > 0, L"no target was ever called");
        Assert::AreEqual(0, pState->nViolations.load(), L"a target was called after RemoveAndWait returned");

        delete pState; // reached only when the run finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestRemoveAndWaitWaitsForAnotherThreadsCallToThatTarget)
    {
        AssertRemoveAndWaitWaitsForTheCall([](NotifyTargetSet<Counter>&, Counter&) noexcept {});
    }

    TEST_METHOD(TestRemoveAndWaitAfterClearStillWaitsForAnotherThreadsCall)
    {
        // Clear() drops every target without waiting, and leaves no list at all.
        AssertRemoveAndWaitWaitsForTheCall([](NotifyTargetSet<Counter>& set, Counter&) noexcept { set.Clear(); });
    }

    TEST_METHOD(TestRemoveAndWaitForATargetNoLongerListedStillWaits)
    {
        // The list exists but no longer holds the target: returning early on
        // "not found" would skip the wait.
        AssertRemoveAndWaitWaitsForTheCall([](NotifyTargetSet<Counter>& set, Counter& oOther) noexcept {
            set.Clear();
            set.Add(oOther);
        });
    }

    TEST_METHOD(TestRemoveDoesNotWaitForAnotherThreadsCall)
    {
        // Remove is also how a target is muted - removed and added back around a
        // change - sometimes while holding a lock its own handler takes
        // (TriggerViewModel::DoFrame and its ConditionsMonitor). Waiting there
        // would deadlock, so only RemoveAndWait waits.
        auto* pHeld = new HeldCall();
        pHeld->Start();
        const bool bEntered = pHeld->WaitUntilEntered();

        ra::tests::DetachedCall oRemove([pHeld]() noexcept { pHeld->set.Remove(pHeld->oTarget); });
        const bool bRemovedDuringTheCall = oRemove.FinishedWithin(std::chrono::seconds(5));

        pHeld->Release();
        const bool bNotifyFinished = pHeld->Finished();

        Assert::IsTrue(bEntered, L"the call never started");
        Assert::IsTrue(bRemovedDuringTheCall, L"Remove waited for another thread's call");
        Assert::IsTrue(bNotifyFinished, L"the pass never finished");
        Assert::IsTrue(pHeld->set.IsEmpty());

        delete pHeld; // reached only when every call finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestAHandlerMayRemoveAndWaitForItsOwnTarget)
    {
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oTarget;
        };
        auto* pState = new State();
        pState->set.Add(pState->oTarget);

        ra::tests::DetachedCall oNotify([pState]() {
            pState->set.ForEachTarget([pState](Counter& oTarget) noexcept {
                ++oTarget.nCalls;
                pState->set.RemoveAndWait(oTarget); // must not wait for its own call
            });
        });
        const bool bFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bFinished, L"a handler removing its own target waited on itself");
        Assert::AreEqual(1, pState->oTarget.nCalls.load());
        Assert::IsTrue(pState->set.IsEmpty());

        delete pState;
    }

    TEST_METHOD(TestATargetRemovedEarlierInThePassIsNotCalled)
    {
        NotifyTargetSet<Counter> set;
        Counter oFirst, oSecond;
        set.Add(oFirst);
        set.Add(oSecond);

        // A handler that removes (and, in real code, may then destroy) a target
        // later in the same pass: the pass must not call it.
        set.ForEachTarget([&set, &oFirst, &oSecond](Counter& oTarget) noexcept {
            ++oTarget.nCalls;
            if (&oTarget == &oFirst)
                set.Remove(oSecond);
        });

        Assert::AreEqual(1, oFirst.nCalls.load());
        Assert::AreEqual(0, oSecond.nCalls.load());
    }

    TEST_METHOD(TestATargetAddedDuringAPassIsCalledFromTheNext)
    {
        NotifyTargetSet<Counter> set;
        Counter oFirst, oSecond;
        set.Add(oFirst);

        set.ForEachTarget([&set, &oSecond](Counter& oTarget) noexcept {
            ++oTarget.nCalls;
            set.Add(oSecond);
        });
        Assert::AreEqual(1, oFirst.nCalls.load());
        Assert::AreEqual(0, oSecond.nCalls.load());

        set.ForEachTarget([](Counter& oTarget) noexcept { ++oTarget.nCalls; });
        Assert::AreEqual(2, oFirst.nCalls.load());
        Assert::AreEqual(1, oSecond.nCalls.load());
    }

    TEST_METHOD(TestAThrowingHandlerStillEndsItsCall)
    {
        // The throwing pass runs on this thread, which stays alive, so the
        // RemoveAndWait below runs on a thread whose id cannot be a recycled copy
        // of it: on glibc a new thread often reuses an exited thread's id, and a
        // leaked call record would then look like RemoveAndWait's own call and
        // hide the bug.
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oTarget;
        };
        auto* pState = new State();
        pState->set.Add(pState->oTarget);

        bool bThrew = false;
        try
        {
            pState->set.ForEachTarget([](Counter&) { throw std::runtime_error("handler failed"); });
        }
        catch (const std::runtime_error&)
        {
            bThrew = true;
        }

        // Another thread's RemoveAndWait must not wait for a call that ended in an exception.
        ra::tests::DetachedCall oRemove([pState]() noexcept { pState->set.RemoveAndWait(pState->oTarget); });
        const bool bRemoved = oRemove.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bThrew, L"the handler's exception did not reach the caller");
        Assert::IsTrue(bRemoved, L"RemoveAndWait waited for a call that had thrown");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }
};

} // namespace tests
} // namespace data
} // namespace ra
