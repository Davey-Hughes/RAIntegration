#ifndef _WIN32

#include "CppUnitTest.h"

#include "services/AchievementRuntime.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/services/mocks/MockClock.hh"
#include "tests/mocks/MockAchievementRuntime.hh"
#include "tests/mocks/MockHostThread.hh"

#include <atomic>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace tests {

// AchievementRuntime::QueueMemoryRead off Windows: a read made off the
// emulator's frame thread runs on the frame thread, never inline on its caller.
// The frame thread is whichever thread last called DoFrame(); the runtime is
// kept paused so that DoFrame() needs no game - it then only records its
// thread and runs rc_client_idle, which is what runs rc_client's scheduled
// reads.
TEST_CLASS(MemoryReadRouting_Tests)
{
private:
    class RoutingHarness
    {
    public:
        // order matters: the runtime needs the client, and must go before it
        ra::context::mocks::MockRcClient mockRcClient;
        ra::services::mocks::MockClock mockClock;
        ra::services::mocks::MockAchievementRuntime mockRuntime;

        RoutingHarness() { mockRuntime.SetPaused(true); }

        void FrameHere() { mockRuntime.DoFrame(); }
    };

    struct Probe
    {
        std::atomic<bool> bRan{false};
        std::thread::id nRanOn{};

        std::function<void()> Callback()
        {
            return [this]() {
                nRanOn = std::this_thread::get_id();
                bRan = true;
            };
        }
    };

public:
    TEST_METHOD(TestReadOnTheFrameThreadRunsInline)
    {
        ra::services::mocks::MockHostThread mockHostThread;
        RoutingHarness harness;
        harness.FrameHere();

        Probe oProbe;
        harness.mockRuntime.QueueMemoryRead(oProbe.Callback());

        Assert::IsTrue(oProbe.bRan.load(), L"a read on the frame thread did not run inline");
        Assert::AreEqual(size_t(0), mockHostThread.PendingCount());
    }

    // TestReadOnTheFrameThreadRunsInline makes the frame thread the host thread, so it
    // cannot tell the inline-on-the-frame-thread branch apart from falling through to
    // HostThreadDispatcher::Invoke, which also runs inline when called on the host
    // thread. Here the frame thread and the host thread are different threads, so only
    // the frame-thread branch can produce an inline run.
    TEST_METHOD(TestReadOnTheFrameThreadRunsInlineWhenTheFrameThreadIsNotTheHostThread)
    {
        ra::services::mocks::MockHostThread mockHostThread; // the test thread is the host thread
        RoutingHarness harness;

        Probe oProbe;
        std::thread::id nWorkerThread{};
        mockHostThread.RunElsewhere([&]() {
            harness.FrameHere(); // this worker becomes the frame thread, not the host thread
            nWorkerThread = std::this_thread::get_id();
            harness.mockRuntime.QueueMemoryRead(oProbe.Callback());
        });

        Assert::IsTrue(oProbe.bRan.load(), L"a read on the frame thread did not run inline");
        Assert::IsTrue(oProbe.nRanOn == nWorkerThread, L"the read did not run on the frame thread");
        Assert::AreEqual(size_t(0), mockHostThread.PendingCount());
    }

    TEST_METHOD(TestReadOffTheFrameThreadWaitsForTheHostThread)
    {
        ra::services::mocks::MockHostThread mockHostThread;
        RoutingHarness harness;
        harness.FrameHere(); // the frame thread is the host thread

        Probe oProbe;
        mockHostThread.RunElsewhere([&]() { harness.mockRuntime.QueueMemoryRead(oProbe.Callback()); });

        Assert::IsFalse(oProbe.bRan.load(), L"a read off the frame thread ran inline on its caller");
        Assert::AreEqual(size_t(1), mockHostThread.PendingCount());
        Assert::IsTrue(mockHostThread.Dispatcher().HasWarnedNoPostFunction(),
                       L"a read waiting with no post function did not warn");

        mockHostThread.Drain();
        Assert::IsTrue(oProbe.bRan.load(), L"draining did not run the read");
        Assert::IsTrue(oProbe.nRanOn == std::this_thread::get_id(), L"the read ran off the host thread");
    }

    TEST_METHOD(TestReadBeforeAnyFrameGoesToTheHostThread)
    {
        ra::services::mocks::MockHostThread mockHostThread;
        RoutingHarness harness; // no DoFrame yet

        Probe oProbe;
        mockHostThread.RunElsewhere([&]() { harness.mockRuntime.QueueMemoryRead(oProbe.Callback()); });

        Assert::IsFalse(oProbe.bRan.load(), L"a read before the first frame ran inline on its caller");
        Assert::AreEqual(size_t(1), mockHostThread.PendingCount());

        mockHostThread.Drain();
        Assert::IsTrue(oProbe.bRan.load());
        Assert::IsTrue(oProbe.nRanOn == std::this_thread::get_id());
    }

    TEST_METHOD(TestReadWhenFramesRunElsewhereWaitsForTheNextFrame)
    {
        ra::services::mocks::MockHostThread mockHostThread;
        RoutingHarness harness;
        mockHostThread.RunElsewhere([&]() { harness.FrameHere(); }); // frames run on another thread

        // from the host thread, which is not the frame thread here
        Probe oProbe;
        harness.mockRuntime.QueueMemoryRead(oProbe.Callback());

        Assert::IsFalse(oProbe.bRan.load(), L"a read off the frame thread ran inline on its caller");
        Assert::AreEqual(size_t(0), mockHostThread.PendingCount(),
                         L"sent to the host thread, which would race frames running on another thread");

        std::thread::id nFrameThread{};
        mockHostThread.RunElsewhere([&]() {
            nFrameThread = std::this_thread::get_id();
            harness.FrameHere();
        });
        Assert::IsTrue(oProbe.bRan.load(), L"the next frame did not run the read");
        Assert::IsTrue(oProbe.nRanOn == nFrameThread, L"the read did not run on the frame thread");
    }

    TEST_METHOD(TestWithoutARegisteredDispatcherTheReadStaysInline)
    {
        // No HostThreadDispatcher: only a unit test gets here (every Linux
        // _RA_Init registers one). The pre-existing path then runs, which is
        // inline because rc_client allows background reads by default.
        RoutingHarness harness;
        harness.FrameHere();

        Probe oProbe;
        ra::services::mocks::MockHostThread::RunElsewhere(
            [&]() { harness.mockRuntime.QueueMemoryRead(oProbe.Callback()); });
        Assert::IsTrue(oProbe.bRan.load());
        Assert::IsTrue(oProbe.nRanOn != std::this_thread::get_id());
    }
};

} // namespace tests
} // namespace services
} // namespace ra

#endif // !_WIN32
