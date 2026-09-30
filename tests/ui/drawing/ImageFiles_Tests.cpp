#ifndef _WIN32

#include "ui/drawing/ImageFiles.hh"

#include "services/Http.hh"

#include "tests/devkit/context/mocks/MockRcClient.hh"
#include "tests/devkit/services/mocks/MockConfiguration.hh"
#include "tests/devkit/services/mocks/MockFileSystem.hh"
#include "tests/devkit/services/mocks/MockHttpRequester.hh"
#include "tests/devkit/services/mocks/MockThreadPool.hh"
#include "tests/mocks/MockGameContext.hh"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace drawing {
namespace tests {

namespace {

// Counts the files opened for reading: LinuxFileSystem logs every open that fails.
class OpenCountingFileSystem : public ra::services::mocks::MockFileSystem
{
public:
    mutable int nOpens = 0;

    std::unique_ptr<ra::services::TextReader> OpenTextFile(const std::wstring& sPath) const override
    {
        ++nOpens;
        return MockFileSystem::OpenTextFile(sPath);
    }
};

class ImageFilesHarness
{
public:
    ra::context::mocks::MockRcClient mockRcClient;
    ra::data::context::mocks::MockGameContext mockGameContext;
    ra::services::mocks::MockConfiguration mockConfiguration;
    OpenCountingFileSystem mockFileSystem;
    ra::services::mocks::MockThreadPool mockThreadPool;
    ra::services::mocks::MockHttpRequester mockHttpRequester;

    // what the server answers, and what was asked of it
    ra::services::Http::StatusCode nStatus = ra::services::Http::StatusCode::OK;
    std::string sContent = "image bytes";
    std::vector<std::string> vUrls;

    // what the files told their owner
    std::vector<std::pair<ImageType, std::string>> vDownloaded;

    ImageFiles files;

    ImageFilesHarness()
        : mockHttpRequester([this](const ra::services::Http::Request& pRequest) {
              vUrls.push_back(pRequest.GetUrl());
              return ra::services::Http::Response(nStatus, sContent);
          }),
          files([this](ImageType nType, const std::string& sName) { vDownloaded.emplace_back(nType, sName); })
    {
        mockFileSystem.SetBaseDirectory(L"/base/");
    }

    // runs every queued download
    void RunDownloads()
    {
        while (mockThreadPool.PendingTasks() > 0)
            mockThreadPool.ExecuteNextTask();
    }
};

} // namespace

TEST_CLASS(ImageFiles_Tests)
{
public:
    TEST_METHOD(TestGetFilenameForEachType)
    {
        ImageFilesHarness harness;

        Assert::AreEqual(std::wstring(L"/base/RACache/Badge/12345.png"), harness.files.GetFilename(ImageType::Badge, "12345"));
        Assert::AreEqual(std::wstring(L"/base/RACache/Badge/i000001.png"), harness.files.GetFilename(ImageType::Icon, "000001"));
        Assert::AreEqual(std::wstring(L"/base/RACache/UserPic/User.png"), harness.files.GetFilename(ImageType::UserPic, "User"));
        Assert::AreEqual(std::wstring(L"/base/Overlay/overlayBG.png"),
                         harness.files.GetFilename(ImageType::Local, "Overlay/overlayBG.png"));
    }

    TEST_METHOD(TestGetFilenameMapsALocalBadgesBackslash)
    {
        // the name keeps "local\", as the user's files store it; only the path uses this platform's separator
        ImageFilesHarness harness;

        Assert::AreEqual(std::wstring(L"/base/RACache/Badge/local/22-0123abcd.png"),
                         harness.files.GetFilename(ImageType::Badge, "local\\22-0123abcd.png"));
    }

    TEST_METHOD(TestGetFilenameKeepsASupportedExtension)
    {
        ImageFilesHarness harness;

        Assert::AreEqual(std::wstring(L"/base/RACache/Badge/picture.JPG"),
                         harness.files.GetFilename(ImageType::Badge, "picture.JPG"));
        Assert::AreEqual(std::wstring(L"/base/RACache/Badge/picture.bmp.png"),
                         harness.files.GetFilename(ImageType::Badge, "picture.bmp"));
    }

    TEST_METHOD(TestFetchSkipsAFileOnDisk)
    {
        ImageFilesHarness harness;
        harness.mockFileSystem.MockFile(L"/base/RACache/Badge/12345.png", "on disk");

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);

        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks());
        Assert::IsFalse(harness.files.IsRequested(ImageType::Badge, "12345"));
        Assert::IsTrue(harness.files.IsAvailable(ImageType::Badge, "12345"));
    }

    TEST_METHOD(TestFetchSkipsAnImageAlreadyRequested)
    {
        ImageFilesHarness harness;

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);
        harness.files.Fetch(ImageType::Badge, "12345", "", 0);

        Assert::AreEqual({1U}, harness.mockThreadPool.PendingTasks());
        Assert::IsTrue(harness.files.IsRequested(ImageType::Badge, "12345"));
        Assert::IsFalse(harness.files.IsAvailable(ImageType::Badge, "12345"));
    }

    TEST_METHOD(TestFetchOfflineRecordsTheRequestAndDownloadsNothing)
    {
        ImageFilesHarness harness;
        harness.mockConfiguration.SetFeatureEnabled(ra::services::Feature::Offline, true);

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);

        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks());
        Assert::IsTrue(harness.files.IsRequested(ImageType::Badge, "12345"));
    }

    TEST_METHOD(TestFetchSkipsAnEmptyName)
    {
        ImageFilesHarness harness;

        harness.files.Fetch(ImageType::Badge, "", "", 0);

        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks());
        Assert::IsFalse(harness.files.IsAvailable(ImageType::Badge, ""));
    }

    TEST_METHOD(TestFetchWithANewerLastUpdatedDownloadsAgain)
    {
        ImageFilesHarness harness;
        const auto tFile = std::chrono::system_clock::from_time_t(1700000000);
        harness.mockFileSystem.MockFile(L"/base/RACache/UserPic/Friend.png", "old avatar");
        harness.mockFileSystem.MockLastModified(L"/base/RACache/UserPic/Friend.png", tFile);

        harness.files.Fetch(ImageType::UserPic, "Friend", "https://example.test/Friend.png", 1700000000 - 60);
        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks()); // older than the file: kept

        harness.files.Fetch(ImageType::UserPic, "Friend", "https://example.test/Friend.png", 1700000000 + 60);
        Assert::AreEqual({1U}, harness.mockThreadPool.PendingTasks()); // newer: downloaded again

        harness.sContent = "new avatar";
        harness.RunDownloads();
        Assert::AreEqual(std::string("new avatar"),
                         harness.mockFileSystem.GetFileContents(L"/base/RACache/UserPic/Friend.png"));
    }

    TEST_METHOD(TestFetchBuildsTheUrlWhenNoneIsGiven)
    {
        ImageFilesHarness harness;

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);
        harness.files.Fetch(ImageType::Badge, "12345_lock", "", 0);
        harness.files.Fetch(ImageType::UserPic, "User", "", 0);
        harness.files.Fetch(ImageType::Icon, "000001", "", 0);
        harness.RunDownloads();

        Assert::AreEqual({4U}, harness.vUrls.size());
        Assert::AreEqual(std::string("https://media.retroachievements.org/Badge/12345.png"), harness.vUrls.at(0));
        Assert::AreEqual(std::string("https://media.retroachievements.org/Badge/12345_lock.png"), harness.vUrls.at(1));
        Assert::AreEqual(std::string("https://media.retroachievements.org/UserPic/User.png"), harness.vUrls.at(2));
        Assert::AreEqual(std::string("https://media.retroachievements.org/Images/000001.png"), harness.vUrls.at(3));
    }

    TEST_METHOD(TestFetchUsesTheGivenUrl)
    {
        ImageFilesHarness harness;

        harness.files.Fetch(ImageType::UserPic, "Friend", "https://example.test/avatars/Friend.png", 0);
        harness.RunDownloads();

        Assert::AreEqual({1U}, harness.vUrls.size());
        Assert::AreEqual(std::string("https://example.test/avatars/Friend.png"), harness.vUrls.at(0));
    }

    TEST_METHOD(TestALocalImageIsNeverDownloaded)
    {
        // rcheevos has no image type for it, so there is no URL (rc_api_init_fetch_image_request_hosted)
        ImageFilesHarness harness;

        harness.files.Fetch(ImageType::Local, "Overlay/overlayBG.png", "", 0);

        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks());
        Assert::IsTrue(harness.files.IsRequested(ImageType::Local, "Overlay/overlayBG.png"));
    }

    TEST_METHOD(TestASuccessfulDownloadWritesTheFileClearsTheRequestAndCallsBack)
    {
        ImageFilesHarness harness;
        harness.sContent = std::string("\x89PNG\r\n\x1A\n\0bytes", 14);

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);
        Assert::IsTrue(harness.vDownloaded.empty(), L"called back before the download ran");
        harness.RunDownloads();

        Assert::AreEqual(harness.sContent, harness.mockFileSystem.GetFileContents(L"/base/RACache/Badge/12345.png"));
        Assert::IsFalse(harness.files.IsRequested(ImageType::Badge, "12345"));
        Assert::IsTrue(harness.files.IsAvailable(ImageType::Badge, "12345"));
        Assert::AreEqual({1U}, harness.vDownloaded.size());
        Assert::IsTrue(harness.vDownloaded.at(0) == std::make_pair(ImageType::Badge, std::string("12345")));
    }

    TEST_METHOD(TestAFailedDownloadDeletesTheFileKeepsTheRequestAndCallsBack)
    {
        ImageFilesHarness harness;
        harness.nStatus = ra::services::Http::StatusCode::NotFound;
        harness.sContent = "<html>Not Found</html>";

        harness.files.Fetch(ImageType::Badge, "12345", "", 0);
        harness.RunDownloads();

        Assert::AreEqual({-1}, harness.mockFileSystem.GetFileSize(L"/base/RACache/Badge/12345.png"));
        Assert::IsTrue(harness.files.IsRequested(ImageType::Badge, "12345"));
        Assert::IsFalse(harness.files.IsAvailable(ImageType::Badge, "12345"));
        Assert::AreEqual({1U}, harness.vDownloaded.size());

        // no retry this session
        harness.files.Fetch(ImageType::Badge, "12345", "", 0);
        Assert::AreEqual({0U}, harness.mockThreadPool.PendingTasks());
    }

    TEST_METHOD(TestReadReturnsTheFilesExactBytes)
    {
        ImageFilesHarness harness;
        const std::string sBytes("\x89PNG\r\n\x1A\n\0\n\r\xFF", 12);
        harness.mockFileSystem.MockFile(L"/base/RACache/Badge/12345.png", sBytes);
        harness.mockFileSystem.MockFile(L"/base/RACache/Badge/empty.png", "");

        std::string sRead;
        Assert::IsTrue(harness.files.Read(ImageType::Badge, "12345", sRead));
        Assert::AreEqual(sBytes, sRead);

        Assert::IsFalse(harness.files.Read(ImageType::Badge, "empty", sRead));
        Assert::IsFalse(harness.files.Read(ImageType::Badge, "missing", sRead));
        Assert::IsTrue(sRead.empty());
    }

    TEST_METHOD(TestReadDoesNotOpenAMissingFile)
    {
        // a missing image is asked for on every draw until it arrives, and every failed open is a log line
        ImageFilesHarness harness;

        std::string sRead;
        Assert::IsFalse(harness.files.Read(ImageType::Badge, "missing", sRead));
        Assert::AreEqual(0, harness.mockFileSystem.nOpens);
    }

    TEST_METHOD(TestStoreCopiesIntoTheLocalBadges)
    {
        ImageFilesHarness harness;
        harness.mockGameContext.SetGameId(22);
        harness.mockFileSystem.MockFile(L"/pictures/Badge.PNG", "abc");

        const std::string sName = harness.files.Store(ImageType::Badge, L"/pictures/Badge.PNG");

        // md5("abc"); the name keeps "local\", the file goes in local/
        Assert::AreEqual(std::string("local\\22-900150983cd24fb0d6963f7d28e17f72.png"), sName);
        Assert::AreEqual(std::string("abc"),
                         harness.mockFileSystem.GetFileContents(
                             L"/base/RACache/Badge/local/22-900150983cd24fb0d6963f7d28e17f72.png"));
        Assert::IsTrue(harness.mockFileSystem.DirectoryExists(L"/base/RACache/Badge/local/"));
    }

    TEST_METHOD(TestStoreOfAMissingFileGivesNoName)
    {
        ImageFilesHarness harness;

        Assert::AreEqual(std::string(), harness.files.Store(ImageType::Badge, L"/pictures/missing.png"));
    }
};

} // namespace tests
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !_WIN32
