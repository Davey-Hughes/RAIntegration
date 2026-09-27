#include "data/NotifyTargetSet.hh"

#include "testutil/CppUnitTest.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <array>
#include <atomic>
#include <chrono>
#include <future>
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
        // checks/tsan-notify.sh, which reports one.
        NotifyTargetSet<Counter> set;
        Counter oPermanent;
        std::array<Counter, 8> vTransient;
        set.Add(oPermanent);

        std::atomic<bool> bStop{ false };
        std::vector<std::thread> vMutators;
        for (int nThread = 0; nThread < 2; ++nThread)
        {
            vMutators.emplace_back([&set, &vTransient, nThread]() {
                for (int i = 0; i < 2000; ++i)
                {
                    set.Add(vTransient.at((i + nThread) % 8));
                    set.Remove(vTransient.at((i + nThread + 3) % 8));
                }
            });
        }

        std::vector<std::thread> vWalkers;
        for (int nThread = 0; nThread < 2; ++nThread)
        {
            vWalkers.emplace_back([&set, &bStop]() {
                do
                {
                    if (set.LockIfNotEmpty())
                    {
                        for (auto& oTarget : set.Targets())
                            ++oTarget.nCalls;

                        set.Unlock();
                    }
                } while (!bStop);
            });
        }

        for (auto& tMutator : vMutators)
            tMutator.join();
        bStop = true;
        for (auto& tWalker : vWalkers)
            tWalker.join();

        Assert::IsTrue(oPermanent.nCalls > 0, L"no pass ever ran");

        bool bPermanentFound = false;
        for (auto& oTarget : set.Targets())
            bPermanentFound |= (&oTarget == &oPermanent);
        Assert::IsTrue(bPermanentFound, L"a target nothing removed went missing");
    }

    TEST_METHOD(TestConcurrentChangesDuringForEachTargetPasses)
    {
        // As above, through ForEachTarget. A Remove here also waits for any
        // walker inside the target it removed.
        NotifyTargetSet<Counter> set;
        Counter oPermanent;
        std::array<Counter, 8> vTransient;
        set.Add(oPermanent);

        std::atomic<bool> bStop{ false };
        std::vector<std::thread> vMutators;
        for (int nThread = 0; nThread < 2; ++nThread)
        {
            vMutators.emplace_back([&set, &vTransient, nThread]() {
                for (int i = 0; i < 2000; ++i)
                {
                    set.Add(vTransient.at((i + nThread) % 8));
                    set.Remove(vTransient.at((i + nThread + 3) % 8));
                }
            });
        }

        std::vector<std::thread> vWalkers;
        for (int nThread = 0; nThread < 2; ++nThread)
        {
            vWalkers.emplace_back([&set, &bStop]() {
                do
                {
                    set.ForEachTarget([](Counter& oTarget) { ++oTarget.nCalls; });
                } while (!bStop);
            });
        }

        for (auto& tMutator : vMutators)
            tMutator.join();
        bStop = true;
        for (auto& tWalker : vWalkers)
            tWalker.join();

        Assert::IsTrue(oPermanent.nCalls > 0, L"no pass ever ran");
        Assert::IsFalse(set.IsEmpty());
    }

    TEST_METHOD(TestRemoveWaitsForAnotherThreadsCallToThatTarget)
    {
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oTarget;
            std::promise<void> oEntered;
            std::promise<void> oRelease;
        };
        auto* pState = new State();
        pState->set.Add(pState->oTarget);
        auto fEntered = pState->oEntered.get_future();
        std::shared_future<void> fRelease = pState->oRelease.get_future().share();

        ra::tests::DetachedCall oNotify([pState, fRelease]() {
            pState->set.ForEachTarget([pState, fRelease](Counter& oTarget) {
                ++oTarget.nCalls;
                pState->oEntered.set_value();
                fRelease.wait();
            });
        });
        const bool bEntered = (fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        ra::tests::DetachedCall oRemove([pState]() { pState->set.Remove(pState->oTarget); });
        const bool bRemovedDuringTheCall = oRemove.FinishedWithin(std::chrono::milliseconds(200));

        pState->oRelease.set_value();
        const bool bRemovedAfterIt = oRemove.FinishedWithin(std::chrono::seconds(5));
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bEntered, L"the call never started");
        Assert::IsFalse(bRemovedDuringTheCall, L"Remove returned while another thread was inside the target");
        Assert::IsTrue(bRemovedAfterIt, L"Remove never returned after the call ended");
        Assert::IsTrue(bNotifyFinished, L"the pass never finished");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestAHandlerMayRemoveItsOwnTarget)
    {
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oTarget;
        };
        auto* pState = new State();
        pState->set.Add(pState->oTarget);

        ra::tests::DetachedCall oNotify([pState]() {
            pState->set.ForEachTarget([pState](Counter& oTarget) {
                ++oTarget.nCalls;
                pState->set.Remove(oTarget); // must not wait for its own call
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
        set.ForEachTarget([&set, &oFirst, &oSecond](Counter& oTarget) {
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

        set.ForEachTarget([&set, &oSecond](Counter& oTarget) {
            ++oTarget.nCalls;
            set.Add(oSecond);
        });
        Assert::AreEqual(1, oFirst.nCalls.load());
        Assert::AreEqual(0, oSecond.nCalls.load());

        set.ForEachTarget([](Counter& oTarget) { ++oTarget.nCalls; });
        Assert::AreEqual(2, oFirst.nCalls.load());
        Assert::AreEqual(1, oSecond.nCalls.load());
    }

    TEST_METHOD(TestAThrowingHandlerStillEndsItsCall)
    {
        struct State
        {
            NotifyTargetSet<Counter> set;
            Counter oTarget;
        };
        auto* pState = new State();
        pState->set.Add(pState->oTarget);

        ra::tests::DetachedCall oNotify([pState]() {
            try
            {
                pState->set.ForEachTarget([](Counter&) { throw std::runtime_error("handler failed"); });
            }
            catch (const std::runtime_error&)
            {
            }
        });
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        // Another thread's Remove must not wait for a call that ended in an exception.
        ra::tests::DetachedCall oRemove([pState]() { pState->set.Remove(pState->oTarget); });
        const bool bRemoved = oRemove.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bNotifyFinished, L"the pass never finished");
        Assert::IsTrue(bRemoved, L"Remove waited for a call that had thrown");

        delete pState;
    }
};

} // namespace tests
} // namespace data
} // namespace ra
