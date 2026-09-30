#ifndef _WIN32

#include "ui/drawing/qt/QtImageRepository.hh"

#include "services/Http.hh"
#include "services/impl/PlatformServices.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockFileSystem.hh"
#include "tests/devkit/services/mocks/MockHttpRequester.hh"
#include "tests/devkit/services/mocks/MockThreadPool.hh"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QImage>

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

// No QtTestHost anywhere in this class: Qt decodes and paints images without a Qt application, and
// checks/tsan-notify.sh runs only classes that never start Qt.

namespace ra {
namespace ui {
namespace drawing {
namespace qt {
namespace tests {

namespace {

// A PNG of the size, every pixel the (straight) ARGB colour.
std::string Png(int nWidth, int nHeight, QRgb nColor)
{
    QImage oImage(nWidth, nHeight, QImage::Format_ARGB32);
    oImage.fill(QColor::fromRgba(nColor));
    QByteArray oBytes;
    QBuffer oBuffer(&oBytes);
    oBuffer.open(QIODevice::WriteOnly);
    oImage.save(&oBuffer, "PNG");
    return std::string(oBytes.constData(), static_cast<size_t>(oBytes.size()));
}

// the pixel as the repository hands it out: premultiplied ARGB32; 0 outside the image, or for no image, so a
// regression fails the assertion rather than crashing the class
uint32_t PixelAt(const QImage& oImage, int nX, int nY)
{
    if (nX < 0 || nY < 0 || nX >= oImage.width() || nY >= oImage.height())
        return 0;
    return reinterpret_cast<const uint32_t*>(oImage.constScanLine(nY))[nX];
}

// A MockFileSystem that runs a hook once while a file is being opened for reading, after the reader holds the old
// contents: a download finishing while the repository decodes.
class ReadInterceptingFileSystem : public ra::services::mocks::MockFileSystem
{
public:
    mutable std::function<void()> fDuringRead; // OpenTextFile is const

    std::unique_ptr<ra::services::TextReader> OpenTextFile(const std::wstring& sPath) const override
    {
        auto pReader = MockFileSystem::OpenTextFile(sPath);
        if (fDuringRead)
        {
            auto fHook = std::move(fDuringRead);
            fDuringRead = nullptr;
            fHook();
        }
        return pReader;
    }
};

class ImageChangedRecorder : public IImageRepository::NotifyTarget
{
public:
    std::vector<std::pair<ImageType, std::string>> vChanged;

    void OnImageChanged(ImageType nType, const std::string& sName) override { vChanged.emplace_back(nType, sName); }
};

class QtImageRepositoryHarness
{
public:
    ra::context::mocks::MockRcClient mockRcClient;
    ra::services::mocks::MockConfiguration mockConfiguration;
    ReadInterceptingFileSystem mockFileSystem;
    ra::services::mocks::MockThreadPool mockThreadPool;
    ra::services::mocks::MockHttpRequester mockHttpRequester;

    // what the server answers
    ra::services::Http::StatusCode nStatus = ra::services::Http::StatusCode::OK;
    std::string sContent;

    QtImageRepository repository;

    QtImageRepositoryHarness()
        : mockHttpRequester([this](const ra::services::Http::Request&) {
              return ra::services::Http::Response(nStatus, sContent);
          }),
          m_oRepositoryOverride(&repository)
    {
        mockFileSystem.SetBaseDirectory(L"/base/");
    }

    void MockBadge(const std::string& sName, const std::string& sBytes)
    {
        mockFileSystem.MockFile(L"/base/RACache/Badge/" + ra::util::String::Widen(sName) + L".png", sBytes);
    }

    void RunDownloads()
    {
        while (mockThreadPool.PendingTasks() > 0)
            mockThreadPool.ExecuteNextTask();
    }

private:
    // ImageReference releases through the registered repository
    ra::services::ServiceLocator::ServiceOverride<IImageRepository> m_oRepositoryOverride;
};

constexpr QRgb BADGE_COLOR = 0xFF102030;
constexpr QRgb DEFAULT_BADGE_COLOR = 0xFF00FF00;
constexpr QRgb DEFAULT_USERPIC_COLOR = 0xFF0000FF;

} // namespace

TEST_CLASS(QtImageRepository_Tests)
{
public:
    TEST_METHOD(TestAFileOnDiskComesBackWithItsPixels)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(64, 64, BADGE_COLOR));
        const ImageReference pImage(ImageType::Badge, "12345");

        const QImage oImage = harness.repository.GetImage(pImage);

        Assert::AreEqual(64, oImage.width());
        Assert::AreEqual(64, oImage.height());
        Assert::IsTrue(oImage.format() == QImage::Format_ARGB32_Premultiplied);
        Assert::AreEqual(static_cast<uint32_t>(BADGE_COLOR), PixelAt(oImage, 0, 0));
        Assert::AreEqual(static_cast<uint32_t>(BADGE_COLOR), PixelAt(oImage, 63, 63));
        Assert::AreNotEqual(0ULL, pImage.GetData());
    }

    TEST_METHOD(TestAnImageKeepsItsAlpha)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(4, 4, 0x80FF00FF)); // 50% magenta

        const QImage oImage = harness.repository.GetImage(ImageReference(ImageType::Badge, "12345"));

        Assert::AreEqual(0x80800080U, PixelAt(oImage, 1, 1)); // premultiplied
    }

    TEST_METHOD(TestAMissingBadgeGivesTheDefaultAndAsksForIt)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR));
        const ImageReference pImage(ImageType::Badge, "777");

        const QImage oImage = harness.repository.GetImage(pImage);

        Assert::AreEqual(static_cast<uint32_t>(DEFAULT_BADGE_COLOR), PixelAt(oImage, 0, 0));
        Assert::AreEqual(0ULL, pImage.GetData(), L"the default is not the reference's own image");
        Assert::AreEqual({1U}, harness.mockThreadPool.PendingTasks(), L"the badge was not asked for");
        Assert::IsFalse(harness.repository.IsImageAvailable(ImageType::Badge, "777"));
    }

    TEST_METHOD(TestEachTypeHasItsDefault)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR));
        harness.mockFileSystem.MockFile(L"/base/RACache/UserPic/_User.png", Png(64, 64, DEFAULT_USERPIC_COLOR));

        const QImage oIcon = harness.repository.GetImage(ImageReference(ImageType::Icon, "000001"));
        const QImage oUserPic = harness.repository.GetImage(ImageReference(ImageType::UserPic, "Friend"));
        const QImage oLocal = harness.repository.GetImage(ImageReference(ImageType::Local, "Overlay/missing.png"));
        const QImage oNone = harness.repository.GetImage(ImageReference(ImageType::None, "12345"));

        Assert::AreEqual(static_cast<uint32_t>(DEFAULT_BADGE_COLOR), PixelAt(oIcon, 0, 0));
        Assert::AreEqual(static_cast<uint32_t>(DEFAULT_USERPIC_COLOR), PixelAt(oUserPic, 0, 0));
        Assert::IsTrue(oLocal.isNull(), L"a local image has no default");
        Assert::IsTrue(oNone.isNull());
    }

    TEST_METHOD(TestNoDefaultOnDiskGivesAnEmptyImage)
    {
        QtImageRepositoryHarness harness;

        Assert::IsTrue(harness.repository.GetImage(ImageReference(ImageType::Badge, "777")).isNull());
    }

    TEST_METHOD(TestTheChangedFlagTurnsOnWhenTheFileArrives)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR)); // so only 777 is downloaded
        ImageChangedRecorder oRecorder;
        harness.repository.AddNotifyTarget(oRecorder);
        ImageReference pImage(ImageType::Badge, "777");

        Assert::IsFalse(harness.repository.HasReferencedImageChanged(pImage)); // missing: asked for, default drawn
        Assert::IsFalse(harness.repository.HasReferencedImageChanged(pImage)); // still missing

        harness.sContent = Png(64, 64, BADGE_COLOR);
        harness.RunDownloads();

        Assert::AreEqual({1U}, oRecorder.vChanged.size(), L"the download was not announced");
        Assert::IsTrue(oRecorder.vChanged.at(0) == std::make_pair(ImageType::Badge, std::string("777")));
        Assert::IsTrue(harness.repository.HasReferencedImageChanged(pImage));
        Assert::IsFalse(harness.repository.HasReferencedImageChanged(pImage)); // and only once
        Assert::AreEqual(static_cast<uint32_t>(BADGE_COLOR), PixelAt(harness.repository.GetImage(pImage), 0, 0));

        harness.repository.RemoveNotifyTarget(oRecorder);
    }

    TEST_METHOD(TestReleasingTheLastReferenceDropsTheImage)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(64, 64, BADGE_COLOR));
        {
            const ImageReference pFirst(ImageType::Badge, "12345");
            const ImageReference pSecond(ImageType::Badge, "12345");
            harness.repository.GetImage(pFirst);
            harness.repository.GetImage(pSecond);
            Assert::AreEqual(1U, harness.repository.GetDecodeCount());

            {
                ImageReference pThird(ImageType::Badge, "12345");
                harness.repository.GetImage(pThird);
                pThird.Release();
            }
            harness.repository.GetImage(ImageReference(ImageType::Badge, "12345"));
            Assert::AreEqual(1U, harness.repository.GetDecodeCount(), L"dropped while references remained");
            Assert::IsTrue(harness.repository.IsImageAvailable(ImageType::Badge, "12345"));
        }

        // every reference is gone: the next one decodes it again
        harness.repository.GetImage(ImageReference(ImageType::Badge, "12345"));
        Assert::AreEqual(2U, harness.repository.GetDecodeCount());
    }

    TEST_METHOD(TestDrawingOneReferenceEveryFrameHoldsOneReference)
    {
        // a popup draws its badge on every frame: one reference, however many draws
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(64, 64, BADGE_COLOR));
        {
            const ImageReference pImage(ImageType::Badge, "12345");
            for (int nFrame = 0; nFrame < 3; ++nFrame)
                harness.repository.GetImage(pImage);
            Assert::AreEqual(1U, harness.repository.GetDecodeCount());
        }

        // its one reference is released, so the image is dropped: the next draw decodes it again
        harness.repository.GetImage(ImageReference(ImageType::Badge, "12345"));
        Assert::AreEqual(2U, harness.repository.GetDecodeCount(), L"a reference drawn three times held more than one");
    }

    TEST_METHOD(TestAnUndecodableFileIsDecodedOnce)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("666", "<html>Not Found</html>");

        for (int i = 0; i < 3; ++i)
        {
            const ImageReference pImage(ImageType::Badge, "666");
            Assert::IsTrue(harness.repository.GetImage(pImage).isNull()); // no default on disk either
            Assert::AreEqual(0ULL, pImage.GetData());
        }

        Assert::AreEqual(1U, harness.repository.GetDecodeCount());
    }

    TEST_METHOD(TestAnUndecodableFileIsTriedAgainAfterADownload)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR)); // on disk, so only 666 is downloaded
        harness.MockBadge("666", "<html>Not Found</html>");
        harness.mockFileSystem.MockLastModified(L"/base/RACache/Badge/666.png",
                                                std::chrono::system_clock::from_time_t(1700000000));
        const ImageReference pImage(ImageType::Badge, "666");
        Assert::AreEqual(static_cast<uint32_t>(DEFAULT_BADGE_COLOR), PixelAt(harness.repository.GetImage(pImage), 0, 0));

        // a newer copy is downloaded over it
        harness.sContent = Png(64, 64, BADGE_COLOR);
        harness.repository.FetchImage(ImageType::Badge, "666", "https://example.test/666.png", 1700000000 + 60);
        harness.RunDownloads();

        Assert::AreEqual(static_cast<uint32_t>(BADGE_COLOR), PixelAt(harness.repository.GetImage(pImage), 0, 0));
    }

    TEST_METHOD(TestADownloadFinishingDuringADecodeIsNotHidden)
    {
        // The draw reads the old, bad file; before it decodes, the new file lands and its download is announced.
        // Remembering the bad read as undecodable now would hide the good file for the rest of the session.
        QtImageRepositoryHarness harness;
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR)); // on disk, so only 555 is downloaded
        harness.MockBadge("555", "<html>half written");
        harness.mockFileSystem.MockLastModified(L"/base/RACache/Badge/555.png",
                                                std::chrono::system_clock::from_time_t(1700000000));
        harness.sContent = Png(64, 64, BADGE_COLOR);
        harness.repository.FetchImage(ImageType::Badge, "555", "https://example.test/555.png", 1700000000 + 60);
        harness.mockFileSystem.fDuringRead = [&harness]() { harness.RunDownloads(); };

        const ImageReference pImage(ImageType::Badge, "555");
        Assert::AreEqual(static_cast<uint32_t>(DEFAULT_BADGE_COLOR), PixelAt(harness.repository.GetImage(pImage), 0, 0),
                         L"the draw did not read the old file");

        Assert::AreEqual(static_cast<uint32_t>(BADGE_COLOR), PixelAt(harness.repository.GetImage(pImage), 0, 0),
                         L"the downloaded file was hidden");
    }

    TEST_METHOD(TestIsImageAvailable)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(64, 64, BADGE_COLOR));

        Assert::IsTrue(harness.repository.IsImageAvailable(ImageType::Badge, "12345")); // on disk
        Assert::IsFalse(harness.repository.IsImageAvailable(ImageType::Badge, "777"));  // missing
        Assert::IsFalse(harness.repository.IsImageAvailable(ImageType::Badge, ""));

        const ImageReference pImage(ImageType::Badge, "12345");
        harness.repository.GetImage(pImage);
        harness.mockFileSystem.DeleteFile(L"/base/RACache/Badge/12345.png");
        Assert::IsTrue(harness.repository.IsImageAvailable(ImageType::Badge, "12345")); // decoded
    }

    TEST_METHOD(TestInitializeFetchesTheDefaults)
    {
        QtImageRepositoryHarness harness;
        std::vector<std::string> vUrls;
        harness.mockHttpRequester.SetHandler([&vUrls](const ra::services::Http::Request& pRequest) {
            vUrls.push_back(pRequest.GetUrl());
            return ra::services::Http::Response(ra::services::Http::StatusCode::OK, "x");
        });

        harness.repository.Initialize();
        harness.RunDownloads();

        Assert::AreEqual({2U}, vUrls.size());
        Assert::AreEqual(std::string("https://media.retroachievements.org/Badge/00000.png"), vUrls.at(0));
        Assert::AreEqual(std::string("https://media.retroachievements.org/UserPic/_User.png"), vUrls.at(1));
    }

    TEST_METHOD(TestThePlatformImageRepositoryIsThisOne)
    {
        QtImageRepositoryHarness harness; // the services the default images' fetches use

        const auto pRepository = ra::services::impl::CreatePlatformImageRepository();

        Assert::IsNotNull(dynamic_cast<const QtImageRepository*>(pRepository.get()));
        Assert::AreEqual({2U}, harness.mockThreadPool.PendingTasks(), L"the default images were not fetched");
    }

    TEST_METHOD(TestTheRealOverlayBackgroundDecodes)
    {
        // RAInterface/overlay/overlayBG.png, which RALibretro copies to Overlay/ beside itself
        std::string sPath(__FILE__);
        sPath.resize(sPath.rfind("/tests/"));
        std::ifstream oFile(sPath + "/RAInterface/overlay/overlayBG.png", std::ios::binary);
        Assert::IsTrue(oFile.is_open(), L"RAInterface/overlay/overlayBG.png not found");
        std::stringstream oBytes;
        oBytes << oFile.rdbuf();

        QtImageRepositoryHarness harness;
        harness.mockFileSystem.MockFile(L"/base/Overlay/overlayBG.png", oBytes.str());
        const ImageReference pImage(ImageType::Local, "Overlay/overlayBG.png");

        Assert::IsTrue(harness.repository.IsImageAvailable(ImageType::Local, "Overlay/overlayBG.png"));
        const QImage oImage = harness.repository.GetImage(pImage);
        Assert::AreEqual(1024, oImage.width());
        Assert::AreEqual(1024, oImage.height());
        Assert::AreEqual(0xFFu, PixelAt(oImage, 512, 512) >> 24); // an RGB file: opaque
    }

    // Two threads resolving, drawing and releasing one name at once: insert, erase and decode race. Built with TSan
    // by checks/tsan-notify.sh, which fails on any report; in a plain build it checks every thread saw the pixels.
    TEST_METHOD(TestTwoThreadsSharingOneImageAgree)
    {
        QtImageRepositoryHarness harness;
        harness.MockBadge("12345", Png(64, 64, BADGE_COLOR));
        harness.MockBadge("00000", Png(64, 64, DEFAULT_BADGE_COLOR));

        std::atomic<int> nWrong{0};
        auto fWork = [&harness, &nWrong]() {
            for (int i = 0; i < 200; ++i)
            {
                const ImageReference pImage(ImageType::Badge, (i % 3 == 0) ? "777" : "12345"); // 777: the default
                const QImage oImage = harness.repository.GetImage(pImage);
                const uint32_t nWant = (i % 3 == 0) ? DEFAULT_BADGE_COLOR : BADGE_COLOR;
                if (oImage.isNull() || PixelAt(oImage, 32, 32) != nWant)
                    ++nWrong;
            }
        };

        std::thread oFirst(fWork);
        std::thread oSecond(fWork);
        oFirst.join();
        oSecond.join();

        Assert::AreEqual(0, nWrong.load());
    }
};

} // namespace tests
} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !_WIN32
