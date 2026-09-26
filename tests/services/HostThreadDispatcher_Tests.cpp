#include "services/impl/HostThreadDispatcher.hh"

#include "services/ServiceLocator.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(HostThreadDispatcher_Tests)
{
private:
    // An emulator's post function, as a test double: it only records what it
    // was asked to run, and the test runs it later "on the emulator's thread".
    static std::mutex& PostedMutex()
    {
        static std::mutex s_oMutex;
        return s_oMutex;
    }

    static std::vector<std::pair<HostThreadDispatcher::WorkFunction, void*>>& Posted()
    {
        static std::vector<std::pair<HostThreadDispatcher::WorkFunction, void*>> s_vPosted;
        return s_vPosted;
    }

    static void RecordPost(HostThreadDispatcher::WorkFunction fpWork, void* pContext)
    {
        std::lock_guard<std::mutex> oLock(PostedMutex());
        Posted().emplace_back(fpWork, pContext);
    }

    static size_t PostedCount()
    {
        std::lock_guard<std::mutex> oLock(PostedMutex());
        return Posted().size();
    }

public:
    TEST_METHOD(TestInlineOnTheHostThread)
    {
        HostThreadDispatcher oDispatcher;
        bool bRan = false;
        oDispatcher.Invoke([&bRan]() { bRan = true; });
        Assert::IsTrue(bRan);
        Assert::AreEqual(size_t(0), oDispatcher.PendingCount());
    }

    TEST_METHOD(TestQueuedFromAnotherThreadUntilDrainedOnTheHostThread)
    {
        HostThreadDispatcher oDispatcher;
        std::atomic<bool> bRan{false};
        std::thread([&]() { oDispatcher.Invoke([&bRan]() { bRan = true; }); }).join();
        Assert::IsFalse(bRan.load(), L"ran on the worker");
        Assert::AreEqual(size_t(1), oDispatcher.PendingCount());

        std::thread([&]() { oDispatcher.DrainIfOnHostThread(); }).join();
        Assert::IsFalse(bRan.load(), L"drained off the host thread");

        oDispatcher.DrainIfOnHostThread();
        Assert::IsTrue(bRan.load());
        Assert::AreEqual(size_t(0), oDispatcher.PendingCount());
    }

    TEST_METHOD(TestPostedThroughTheEmulatorsPostFunction)
    {
        {
            std::lock_guard<std::mutex> oLock(PostedMutex());
            Posted().clear();
        }
        HostThreadDispatcher oDispatcher;
        ra::services::ServiceLocator::ServiceOverride<HostThreadDispatcher> oOverride(&oDispatcher);
        oDispatcher.SetPostFunction(&RecordPost);

        std::atomic<bool> bRan{false};
        std::thread([&]() { oDispatcher.Invoke([&bRan]() { bRan = true; }); }).join();
        Assert::AreEqual(size_t(1), PostedCount());
        Assert::IsFalse(bRan.load());

        const auto oPost = Posted().front();
        oPost.first(oPost.second); // the emulator runs it on its own thread - this one
        Assert::IsTrue(bRan.load());
    }

    TEST_METHOD(TestInstallingThePostFunctionFlushesQueuedWork)
    {
        {
            std::lock_guard<std::mutex> oLock(PostedMutex());
            Posted().clear();
        }
        HostThreadDispatcher oDispatcher;
        std::thread([&]() { oDispatcher.Invoke([]() {}); }).join();
        Assert::AreEqual(size_t(0), PostedCount());

        oDispatcher.SetPostFunction(&RecordPost);
        Assert::AreEqual(size_t(1), PostedCount(), L"queued work was not posted when the post function arrived");
    }

    TEST_METHOD(TestShutdownDiscardsAndRefusesWorkFromOtherThreads)
    {
        HostThreadDispatcher oDispatcher;
        std::atomic<int> nRan{0};
        std::thread([&]() { oDispatcher.Invoke([&nRan]() { ++nRan; }); }).join();
        Assert::AreEqual(size_t(1), oDispatcher.PendingCount());

        oDispatcher.Shutdown();
        Assert::AreEqual(size_t(0), oDispatcher.PendingCount());

        std::thread([&]() { oDispatcher.Invoke([&nRan]() { ++nRan; }); }).join();
        oDispatcher.DrainIfOnHostThread();
        Assert::AreEqual(0, nRan.load(), L"work from another thread ran after Shutdown()");

        // on the host thread itself there is nothing to move, even now
        oDispatcher.Invoke([&nRan]() { ++nRan; });
        Assert::AreEqual(1, nRan.load());
    }

    TEST_METHOD(TestRunPostedWithNoDispatcherRegisteredDoesNothing)
    {
        // A post that outlives its dispatcher (see RunPosted) arrives with no
        // dispatcher registered. ServiceLocator::Get on an empty slot throws,
        // and a test that throws fails, so this fails if the Exists check goes.
        Assert::IsFalse(ra::services::ServiceLocator::Exists<HostThreadDispatcher>(), L"test setup: a dispatcher is registered");
        HostThreadDispatcher::RunPosted(nullptr);
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra
