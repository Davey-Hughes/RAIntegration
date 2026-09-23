#ifndef _WIN32

#include "services/impl/LinuxFileSystem.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <climits>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <unistd.h>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;
using namespace std::chrono_literals;

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

        ~ScopedTempDirectory()
        {
            // Destructors are implicitly noexcept; this one can run while
            // unwinding from a thrown AssertionFailure, so a throwing
            // cleanup failure here would turn a named test failure into an
            // unhandled-exception process crash with no message. Use the
            // std::error_code overload instead.
            std::error_code oError;
            std::filesystem::remove_all(m_sPath, oError);
        }

        std::wstring Path(const char* sLeaf) const
        {
            return ra::util::String::Widen(m_sPath + "/" + sLeaf);
        }

        // The raw narrow path, bypassing Widen/Narrow entirely - useful when
        // a test needs to avoid exercising those conversions itself (e.g. to
        // chdir(), or to check on-disk bytes directly).
        const std::string& RawPath() const { return m_sPath; }

    private:
        std::string m_sPath;
    };

    // Restores the process's working directory on scope exit - including
    // when an Assert::* throws - so one test's chdir() can never leak into
    // whichever test runs next.
    class ScopedWorkingDirectory
    {
    public:
        ScopedWorkingDirectory()
        {
            Assert::IsNotNull(getcwd(m_sOriginalCwd, sizeof(m_sOriginalCwd)), L"getcwd failed");
        }

        ~ScopedWorkingDirectory() { static_cast<void>(chdir(m_sOriginalCwd)); }

    private:
        char m_sOriginalCwd[PATH_MAX]{};
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

        // paths arrive from config files and from the emulator with
        // backslashes too - the helpers must accept both separators
        Assert::AreEqual(std::wstring(L"\\a\\b"), oFileSystem.GetDirectory(L"\\a\\b\\c.txt"));
        Assert::AreEqual(std::wstring(L"c.txt"), oFileSystem.GetFileName(L"\\a\\b\\c.txt"));
        Assert::AreEqual(std::wstring(L"txt"), oFileSystem.GetExtension(L"\\a\\b\\c.txt"));
        Assert::AreEqual(std::wstring(L"\\a\\b\\c"), oFileSystem.RemoveExtension(L"\\a\\b\\c.txt"));

        // a dot in a directory name is not an extension, backslash form
        Assert::AreEqual(std::wstring(L""), oFileSystem.GetExtension(L"\\a.b\\c"));
        Assert::AreEqual(std::wstring(L"\\a.b\\c"), oFileSystem.RemoveExtension(L"\\a.b\\c"));

        // concrete regression: a Windows-style absolute path with an
        // uppercase extension
        Assert::AreEqual(std::wstring(L"PNG"), oFileSystem.GetExtension(L"C:\\roms\\art\\badge.PNG"));
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

        {
            auto pWriter = oFileSystem.CreateTextFile(sFrom);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("from"));
        }
        {
            auto pWriter = oFileSystem.CreateTextFile(sTo);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("to"));
        }

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

        {
            auto pWriter = oFileSystem.CreateTextFile(sFrom);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("from"));
        }

        Assert::IsTrue(oFileSystem.CopyFile(sFrom, sTo));
        Assert::AreEqual(static_cast<int64_t>(4), oFileSystem.GetFileSize(sTo),
                         L"the first copy must actually copy the source's 4 bytes");

        // give the destination different, distinguishable content so a
        // truncate-then-fail or a silent overwrite would be detectable
        Assert::IsTrue(oFileSystem.DeleteFile(sTo));
        {
            auto pWriter = oFileSystem.CreateTextFile(sTo);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("preexisting"));
        }

        Assert::IsFalse(oFileSystem.CopyFile(sFrom, sTo), L"CopyFileW(FALSE) does not overwrite");

        Assert::AreEqual(static_cast<int64_t>(11), oFileSystem.GetFileSize(sTo),
                         L"a failed copy must not truncate the destination");

        auto pReader = oFileSystem.OpenTextFile(sTo);
        Assert::IsNotNull(pReader.get(), L"OpenTextFile returned nullptr");
        std::string sLine;
        Assert::IsTrue(pReader->GetLine(sLine));
        Assert::AreEqual(std::string("preexisting"), sLine,
                         L"a failed copy must leave the destination's content untouched");
    }

    TEST_METHOD(TestGetFilesInDirectoryReturnsBareNamesAndSkipsDirectories)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;

        {
            auto pWriter = oFileSystem.CreateTextFile(oTemp.Path("a.txt"));
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("a"));
        }
        {
            auto pWriter = oFileSystem.CreateTextFile(oTemp.Path("b.txt"));
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("b"));
        }
        Assert::IsTrue(oFileSystem.CreateDirectory(oTemp.Path("sub")));

        std::vector<std::wstring> vResults;
        Assert::AreEqual(static_cast<size_t>(2), oFileSystem.GetFilesInDirectory(oTemp.Path(""), vResults));

        std::sort(vResults.begin(), vResults.end());
        Assert::AreEqual(std::wstring(L"a.txt"), vResults.at(0));
        Assert::AreEqual(std::wstring(L"b.txt"), vResults.at(1));
    }

    TEST_METHOD(TestGetFilesInDirectoryDoesNotThrowAndSkipsUnstattableEntries)
    {
        // GetFilesInDirectory used to range-for over a directory_iterator,
        // calling the throwing operator++ - which can raise filesystem_error
        // mid-enumeration on a real failure (ESTALE/EIO on a network mount).
        // That specific OS-level failure can't be manufactured hermetically
        // in a unit test, but a dangling symlink exercises the closely
        // related bug in the same function: is_directory(error_code&) fails
        // (returns false, with the error_code set) for an entry it can't
        // stat, which the old code silently treated as "not a directory" -
        // i.e. counted it as a file. Windows can't produce this ambiguous
        // case at all (FindFirstFileW/FindNextFileW get attributes from the
        // directory read itself), so the closest match is to skip an entry
        // whose type is unknown rather than guess.
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;

        {
            auto pWriter = oFileSystem.CreateTextFile(oTemp.Path("real.txt"));
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("real"));
        }

        Assert::AreEqual(0, symlink("/nonexistent/target/for/ra-fs-test", (oTemp.RawPath() + "/dangling").c_str()),
                         L"symlink() failed");

        std::vector<std::wstring> vResults;
        const size_t nCount = oFileSystem.GetFilesInDirectory(oTemp.Path(""), vResults);

        Assert::AreEqual(static_cast<size_t>(1), nCount,
                         L"an unstattable entry must not be counted as a file");
        Assert::AreEqual(static_cast<size_t>(1), vResults.size());
        Assert::AreEqual(std::wstring(L"real.txt"), vResults.at(0));
    }

    TEST_METHOD(TestNonAsciiFilename)
    {
        // exercises Widen/Narrow on a real path, which is the reason Task 2
        // comes first.
        //
        // A pure Widen-then-Narrow round trip would still pass even if both
        // conversions were symmetrically broken, so assert against the raw
        // on-disk bytes (via std::filesystem, bypassing ra::util::String
        // entirely) rather than only checking that the file was found again
        // through the same pair of conversions that wrote it.
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("caf\xC3\xA9.txt");

        {
            auto pWriter = oFileSystem.CreateTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("x"));
        }

        Assert::IsTrue(std::filesystem::exists(oTemp.RawPath() + "/caf\xC3\xA9.txt"),
                       L"the UTF-8 filename must exist on disk under its exact byte sequence");
        Assert::AreEqual(static_cast<int64_t>(1), oFileSystem.GetFileSize(sPath));
    }

    TEST_METHOD(TestRelativePathsResolveAgainstBaseDirectory)
    {
        // The original version of this test asserted DirectoryExists(L"."),
        // which succeeds from any working directory - it passed against a
        // MakeAbsolute that ignored BaseDirectory() entirely. Pin resolution
        // to BaseDirectory() specifically, not just "a" directory: chdir into
        // a temp directory and prove a relative path does NOT resolve there,
        // then prove it DOES resolve under the real BaseDirectory().
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const ScopedWorkingDirectory oCwdGuard;

        Assert::AreEqual(0, chdir(oTemp.RawPath().c_str()), L"chdir into temp dir failed");

        {
            std::ofstream oCwdFile("leaf.txt");
            Assert::IsTrue(oCwdFile.is_open(), L"failed to create leaf.txt in the temp CWD");
            oCwdFile << "cwd";
        }

        // Proves resolution is NOT against the process CWD: this file only
        // exists relative to getcwd(), which LinuxFileSystem must never
        // consult.
        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(L"leaf.txt"),
                         L"a relative path must not resolve against the process CWD");

        // A same-named file placed under the real BaseDirectory() must be
        // the one a relative path finds.
        const std::wstring sRealPath = oFileSystem.BaseDirectory() + L"leaf.txt";
        {
            auto pWriter = oFileSystem.CreateTextFile(sRealPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("base"));
        }

        Assert::AreEqual(static_cast<int64_t>(4), oFileSystem.GetFileSize(L"leaf.txt"),
                         L"a relative path must resolve against BaseDirectory()");

        oFileSystem.DeleteFile(sRealPath);
    }

    TEST_METHOD(TestOpenTextFile)
    {
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("open.txt");

        Assert::IsNull(oFileSystem.OpenTextFile(sPath).get(), L"a missing file must return nullptr");

        {
            auto pWriter = oFileSystem.CreateTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("hello"));
        }

        auto pReader = oFileSystem.OpenTextFile(sPath);
        Assert::IsNotNull(pReader.get(), L"OpenTextFile returned nullptr");

        std::string sLine;
        Assert::IsTrue(pReader->GetLine(sLine));
        Assert::AreEqual(std::string("hello"), sLine);
    }

    TEST_METHOD(TestAppendTextFile)
    {
        // Two branches: appending to a file that already exists, and
        // appending to one that doesn't yet (AppendTextFile's own fallback
        // creates it).
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("append.txt");

        Assert::AreEqual(static_cast<int64_t>(-1), oFileSystem.GetFileSize(sPath),
                         L"sanity: the file must not exist yet");

        {
            // branch: the file does not exist yet
            auto pWriter = oFileSystem.AppendTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"AppendTextFile (new file) returned nullptr");
            pWriter->Write(std::string("first"));
        }

        Assert::AreEqual(static_cast<int64_t>(5), oFileSystem.GetFileSize(sPath));

        {
            // branch: the file already exists
            auto pWriter = oFileSystem.AppendTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"AppendTextFile (existing file) returned nullptr");
            pWriter->Write(std::string("second"));
        }

        Assert::AreEqual(static_cast<int64_t>(11), oFileSystem.GetFileSize(sPath),
                         L"append must not truncate the existing contents");

        auto pReader = oFileSystem.OpenTextFile(sPath);
        Assert::IsNotNull(pReader.get(), L"OpenTextFile returned nullptr");
        std::string sLine;
        Assert::IsTrue(pReader->GetLine(sLine));
        Assert::AreEqual(std::string("firstsecond"), sLine);
    }

    TEST_METHOD(TestGetLastModified)
    {
        // The one production consumer shape is FileLocalStorage.cpp's 30-day
        // cache expiry sweep, which runs on every launch: if this ever
        // returns the default time_point() for a file that DOES exist - its
        // failure return - the entire cache is silently deleted on startup.
        const ScopedTempDirectory oTemp;
        const LinuxFileSystem oFileSystem;
        const std::wstring sPath = oTemp.Path("touched.txt");

        // 1. Missing file -> the default-constructed sentinel. Pins the
        // Windows failure contract (WindowsFileSystem.cpp's GetLastModified).
        Assert::IsTrue(oFileSystem.GetLastModified(oTemp.Path("nope.txt")) ==
                            std::chrono::system_clock::time_point(),
                        L"a missing file must return the default sentinel");

        // 2. Existing file -> a real, epoch-based time. The +/-2s window
        // absorbs st_mtime's one-second truncation. The lower bound catches
        // a regression that returns the sentinel for a file that exists; the
        // upper bound catches a wrong epoch base - an unconverted
        // std::filesystem::file_time_type, or a raw FILETIME as on the
        // Windows side, lands roughly 50 years off. (See the comment on
        // LinuxFileSystem::GetLastModified for why stat()/from_time_t is
        // used instead of std::filesystem::last_write_time - this test is
        // what makes that warning enforceable.)
        const auto tBefore = std::chrono::system_clock::now();
        {
            auto pWriter = oFileSystem.CreateTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("x"));
        }
        const auto tAfter = std::chrono::system_clock::now();

        const auto tModified = oFileSystem.GetLastModified(sPath);
        Assert::IsTrue(tModified >= tBefore - 2s, L"last-modified time is too far in the past");
        Assert::IsTrue(tModified <= tAfter + 2s, L"last-modified time is too far in the future");

        // 3. Monotonicity across a rewrite.
        std::this_thread::sleep_for(1100ms);
        {
            auto pWriter = oFileSystem.CreateTextFile(sPath);
            Assert::IsNotNull(pWriter.get(), L"CreateTextFile returned nullptr");
            pWriter->Write(std::string("xy"));
        }

        const auto tModifiedAfterRewrite = oFileSystem.GetLastModified(sPath);
        Assert::IsTrue(tModifiedAfterRewrite >= tModified,
                       L"last-modified must not go backwards after a rewrite");
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
