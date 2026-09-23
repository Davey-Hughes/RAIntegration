#ifndef _WIN32

#include "services/impl/LinuxFileSystem.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <cstdlib>
#include <filesystem>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(LinuxFileSystem_Tests)
{
private:
    // Each test gets its own directory so the suite can run in any order.
    class ScopedTempDirectory
    {
    public:
        ScopedTempDirectory()
        {
            std::string sTemplate = (std::filesystem::temp_directory_path() / "ra-fs-XXXXXX").string();
            std::vector<char> vBuffer(sTemplate.begin(), sTemplate.end());
            vBuffer.push_back('\0');
            Assert::IsNotNull(mkdtemp(vBuffer.data()), L"mkdtemp failed");
            m_sPath = std::string(vBuffer.data());
        }

        ~ScopedTempDirectory() { std::filesystem::remove_all(m_sPath); }

        std::wstring Path(const char* sLeaf) const
        {
            return ra::util::String::Widen(m_sPath + "/" + sLeaf);
        }

    private:
        std::string m_sPath;
    };

public:
    TEST_METHOD(TestBaseDirectoryIsTheExecutableDirectory)
    {
        const LinuxFileSystem oFileSystem;
        const std::wstring& sBase = oFileSystem.BaseDirectory();

        Assert::IsFalse(sBase.empty());
        Assert::AreEqual(L'/', sBase.front(), L"BaseDirectory must be absolute");
        Assert::AreEqual(L'/', sBase.back(), L"BaseDirectory must end with a separator");
        Assert::IsTrue(oFileSystem.DirectoryExists(sBase), L"BaseDirectory must exist");
    }

    TEST_METHOD(TestPathHelpers)
    {
        const LinuxFileSystem oFileSystem;

        Assert::AreEqual(std::wstring(L"/a/b"), oFileSystem.GetDirectory(L"/a/b/c.txt"));
        Assert::AreEqual(std::wstring(L"c.txt"), oFileSystem.GetFileName(L"/a/b/c.txt"));
        Assert::AreEqual(std::wstring(L"txt"), oFileSystem.GetExtension(L"/a/b/c.txt"));
        Assert::AreEqual(std::wstring(L"/a/b/c"), oFileSystem.RemoveExtension(L"/a/b/c.txt"));

        // a dot in a directory name is not an extension
        Assert::AreEqual(std::wstring(L""), oFileSystem.GetExtension(L"/a.b/c"));
        Assert::AreEqual(std::wstring(L"/a.b/c"), oFileSystem.RemoveExtension(L"/a.b/c"));
    }

    TEST_METHOD(TestCreateWriteReadDelete)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("hello.txt");

        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(sPath));

        {
            auto pWriter = oFileSystem.CreateTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("hello"));
        }

        Assert::AreEqual(static_cast<int64_t>(5), oFileSystem.GetFileSize(sPath));
        Assert::IsTrue(oFileSystem.DeleteFile(sPath));
        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(sPath));
    }

    TEST_METHOD(TestGetFileSizeOfDirectoryIsNegativeOne)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sDir = oTemp.Path("subdir");

        Assert::IsTrue(oFileSystem.CreateDirectory(sDir));
        Assert::IsTrue(oFileSystem.DirectoryExists(sDir));
        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(sDir),
                         L"a directory has no file size");
    }

    TEST_METHOD(TestMoveFileFailsWhenDestinationExists)
    {
        // POSIX rename() silently overwrites; MoveFileW does not. FileLogger's
        // rotation deletes the old log before moving, and depends on the
        // Windows contract.
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sFrom = oTemp.Path("from.txt");
        const std::wstring sTo = oTemp.Path("to.txt");

        oFileSystem.CreateTextFile(sFrom)->Write(std::string("from"));
        oFileSystem.CreateTextFile(sTo)->Write(std::string("to"));

        Assert::IsFalse(oFileSystem.MoveFile(sFrom, sTo), L"must not overwrite");
        Assert::AreEqual(static_cast<int64_t>(4), oFileSystem.GetFileSize(sFrom),
                         L"source must survive a failed move");

        Assert::IsTrue(oFileSystem.DeleteFile(sTo));
        Assert::IsTrue(oFileSystem.MoveFile(sFrom, sTo));
        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(sFrom));
    }

    TEST_METHOD(TestCopyFileFailsWhenDestinationExists)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sFrom = oTemp.Path("from.txt");
        const std::wstring sTo = oTemp.Path("to.txt");

        oFileSystem.CreateTextFile(sFrom)->Write(std::string("from"));
        Assert::IsTrue(oFileSystem.CopyFile(sFrom, sTo));
        Assert::IsFalse(oFileSystem.CopyFile(sFrom, sTo), L"CopyFileW(FALSE) does not overwrite");
    }

    TEST_METHOD(TestGetFilesInDirectoryReturnsBareNamesAndSkipsDirectories)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;

        oFileSystem.CreateTextFile(oTemp.Path("a.txt"))->Write(std::string("a"));
        oFileSystem.CreateTextFile(oTemp.Path("b.txt"))->Write(std::string("b"));
        Assert::IsTrue(oFileSystem.CreateDirectory(oTemp.Path("sub")));

        std::vector<std::wstring> vResults;
        Assert::AreEqual(static_cast<size_t>(2), oFileSystem.GetFilesInDirectory(oTemp.Path(""), vResults));

        std::sort(vResults.begin(), vResults.end());
        Assert::AreEqual(std::wstring(L"a.txt"), vResults.at(0));
        Assert::AreEqual(std::wstring(L"b.txt"), vResults.at(1));
    }

    TEST_METHOD(TestNonAsciiFilename)
    {
        // exercises Widen/Narrow on a real path, which is the reason Task 2
        // comes first
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("caf\xC3\xA9.txt");

        oFileSystem.CreateTextFile(sPath)->Write(std::string("x"));
        Assert::AreEqual(static_cast<int64_t>(1), oFileSystem.GetFileSize(sPath));
    }

    TEST_METHOD(TestRelativePathsResolveAgainstBaseDirectory)
    {
        const LinuxFileSystem oFileSystem;
        Assert::IsTrue(oFileSystem.DirectoryExists(L"."), L"'.' resolves under BaseDirectory");
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
