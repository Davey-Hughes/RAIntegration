#ifndef _WIN32

#include "ui/qt/bindings/Post.hh"

#include "tests/ui/qt/QtTestHost.hh"

#include <QObject>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {
namespace tests {

using ra::ui::qt::tests::QtTestHost;

TEST_CLASS(Post_Tests)
{
public:
    TEST_METHOD(TestPostOnTheContextsThreadRunsInline)
    {
        QtTestHost oQt;
        bool bRanInline = false;
        oQt.RunOnQt([&bRanInline]() {
            QObject oContext;
            bool bRan = false;
            Post(oContext, [&bRan]() { bRan = true; });
            bRanInline = bRan;
        });

        Assert::IsTrue(bRanInline);
    }

    TEST_METHOD(TestPostFromAnotherThreadRunsOnTheContextsThread)
    {
        QtTestHost oQt;
        QObject* pContext = nullptr;
        std::thread::id nQtThread;
        oQt.RunOnQt([&pContext, &nQtThread]() {
            pContext = new QObject();
            nQtThread = std::this_thread::get_id();
        });

        std::atomic<bool> bRan{false};
        std::thread::id nRanOn;
        std::thread([pContext, &bRan, &nRanOn]() {
            Post(*pContext, [&bRan, &nRanOn]() {
                nRanOn = std::this_thread::get_id();
                bRan = true;
            });
        }).join();

        const bool bArrived = oQt.WaitOnQt([&bRan]() { return bRan.load(); });
        oQt.RunOnQt([pContext]() { delete pContext; });

        Assert::IsTrue(bArrived, L"the posted call never ran");
        Assert::IsTrue(nRanOn == nQtThread, L"the posted call ran off the Qt thread");
    }

    TEST_METHOD(TestPostToADeletedContextNeverRuns)
    {
        QtTestHost oQt;
        QObject* pContext = nullptr;
        oQt.RunOnQt([&pContext]() { pContext = new QObject(); });

        // Hold the Qt thread, so the post below waits in the queue behind the
        // held call - which then deletes the context before the queue reaches it.
        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease, pContext]() {
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
            delete pContext;
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        std::atomic<int> nRan{0};
        Post(*pContext, [&nRan]() { ++nRan; });
        oRelease.set_value();

        oQt.RunOnQt([]() {}); // queued after the post: the post has been delivered or discarded

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::AreEqual(0, nRan.load());
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
