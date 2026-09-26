#ifndef _WIN32

#include "LinuxFileSystem.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include "services/impl/FileTextReader.hh"
#include "services/impl/FileTextWriter.hh"
#include "services/ServiceLocator.hh"

#include <cerrno>
#include <climits>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <system_error>

namespace ra {
namespace services {
namespace impl {

LinuxFileSystem::LinuxFileSystem() noexcept
{
    // /proc/self/exe is the running executable, which is what
    // GetModuleFileNameW(nullptr) returns on Windows - the host process, not
    // the shared library this code lives in. RALibretro resolves its root
    // folder the same way (commit 15aa33b).
    std::error_code oError;
    const auto oExe = std::filesystem::read_symlink("/proc/self/exe", oError);

    std::string sDirectory;
    if (!oError)
    {
        sDirectory = oExe.parent_path().string();
    }
    else
    {
        // No /proc (e.g. a sandboxed environment without procfs mounted).
        // BaseDirectory() is documented - and relied on by every MakeAbsolute
        // call - to be absolute, so falling back to a bare "./" would be
        // silently wrong; fall back to the current working directory instead,
        // and to "/" if even that isn't available.
        char sCwd[PATH_MAX]{};
        sDirectory = (getcwd(sCwd, sizeof(sCwd)) != nullptr) ? std::string(sCwd) : std::string("/");
    }

    if (sDirectory.empty() || sDirectory.back() != '/')
        sDirectory.push_back('/');

    m_sBaseDirectory = ra::util::String::Widen(sDirectory);
}

std::string LinuxFileSystem::MakeAbsolute(const std::wstring& sPath) const
{
    if (!sPath.empty() && sPath.front() == L'/')
        return ra::util::String::Narrow(sPath);

    return ra::util::String::Narrow(m_sBaseDirectory + sPath);
}

std::wstring LinuxFileSystem::GetDirectory(const std::wstring& sPath) const
{
    const auto nIndex = sPath.find_last_of(L"/\\");
    if (nIndex != std::wstring::npos)
        return std::wstring(sPath, 0, nIndex);

    return sPath;
}

std::wstring LinuxFileSystem::GetFileName(const std::wstring& sPath) const
{
    const auto nIndex = sPath.find_last_of(L"/\\");
    if (nIndex != std::wstring::npos)
        return std::wstring(sPath, nIndex + 1);

    return sPath;
}

std::wstring LinuxFileSystem::GetExtension(const std::wstring& sPath) const
{
    const auto nIndex = sPath.find_last_of(L"/\\.");
    if (nIndex != std::wstring::npos && sPath.at(nIndex) == L'.')
        return std::wstring(sPath, nIndex + 1);

    return L"";
}

std::wstring LinuxFileSystem::RemoveExtension(const std::wstring& sPath) const
{
    const auto nIndex = sPath.find_last_of(L"/\\.");
    if (nIndex != std::wstring::npos && sPath.at(nIndex) == L'.')
        return std::wstring(sPath, 0, nIndex);

    return sPath;
}

bool LinuxFileSystem::DirectoryExists(const std::wstring& sDirectory) const
{
    struct stat oStat{};
    if (stat(MakeAbsolute(sDirectory).c_str(), &oStat) != 0)
        return false;

    return S_ISDIR(oStat.st_mode);
}

bool LinuxFileSystem::CreateDirectory(const std::wstring& sDirectory) const
{
    // single level, matching CreateDirectoryW - not create_directories
    return mkdir(MakeAbsolute(sDirectory).c_str(), 0755) == 0;
}

size_t LinuxFileSystem::GetFilesInDirectory(const std::wstring& sDirectory,
                                            _Inout_ std::vector<std::wstring>& vResults) const
{
    std::error_code oError;
    std::filesystem::directory_iterator oIterator(MakeAbsolute(sDirectory), oError);
    if (oError)
        return 0U;

    const size_t nInitialSize = vResults.size();
    const std::filesystem::directory_iterator oEnd;

    // Manual loop with the non-throwing increment(error_code&), not a
    // range-for: a range-for over a directory_iterator calls the throwing
    // operator++, which can raise filesystem_error mid-enumeration (e.g.
    // ESTALE/EIO on a network mount). FindFirstFileW/FindNextFileW on
    // Windows simply end the loop on failure and return what was collected,
    // so end the loop here too instead of letting the exception propagate.
    while (oIterator != oEnd)
    {
        std::error_code oTypeError;
        const bool bIsDirectory = oIterator->is_directory(oTypeError);
        if (oTypeError)
        {
            // The entry's type couldn't be determined (e.g. a dangling
            // symlink, or the same class of stat failure as above racing an
            // individual entry). FindFirstFileW/FindNextFileW get their
            // attributes from the directory read itself and cannot fail this
            // way, so there's no Windows behaviour to match; skip the entry
            // rather than risk reporting a directory (or something we simply
            // don't know about) as a file.
        }
        else if (!bIsDirectory)
        {
            // FindFirstFileW reports cFileName, not the full path
            vResults.emplace_back(ra::util::String::Widen(oIterator->path().filename().string()));
        }

        oIterator.increment(oError);
        if (oError)
            break;
    }

    return vResults.size() - nInitialSize;
}

bool LinuxFileSystem::DeleteFile(const std::wstring& sPath) const
{
    return unlink(MakeAbsolute(sPath).c_str()) == 0;
}

bool LinuxFileSystem::MoveFile(const std::wstring& sOldPath, const std::wstring& sNewPath) const
{
    const std::string sTo = MakeAbsolute(sNewPath);

    // rename() overwrites silently; MoveFileW fails. Preserve the Windows
    // contract - FileLogger's rotation relies on it.
    //
    // This stat-then-rename is not atomic: a concurrent creator of sTo
    // between the two calls would be silently overwritten, which is the
    // exact thing this check exists to prevent. The portable atomic idiom is
    // link(old, new) (fails EEXIST) followed by unlink(old). It was not
    // applied here: link() cannot target a directory on Linux (EPERM),
    // while rename() can, and IFileSystem::MoveFile's contract doesn't rule
    // out directories - so link() would trade this race for a new failure
    // mode on an input rename() currently accepts. (Cross-filesystem use
    // isn't a reason either way: link() and rename() both fail with EXDEV
    // there.) The sole caller (FileLogger's log rotation) is single-process,
    // single-threaded, and only ever targets a private RACache path with no
    // other writer, so the window this leaves open is not reachable in
    // practice.
    struct stat oStat{};
    if (stat(sTo.c_str(), &oStat) == 0)
        return false;

    return rename(MakeAbsolute(sOldPath).c_str(), sTo.c_str()) == 0;
}

bool LinuxFileSystem::CopyFile(const std::wstring& sSourcePath, const std::wstring& sNewPath) const
{
    std::error_code oError;
    // matches CopyFileW(..., FALSE): fails rather than overwriting
    return std::filesystem::copy_file(MakeAbsolute(sSourcePath), MakeAbsolute(sNewPath),
                                      std::filesystem::copy_options::none, oError);
}

int64_t LinuxFileSystem::GetFileSize(const std::wstring& sPath) const
{
    // MakeAbsolute's result is captured in a named local, not passed inline
    // to stat(): a temporary would be destroyed at the end of the full
    // expression containing the stat() call - i.e. before the errno read
    // below runs at all - and freeing it isn't guaranteed to preserve errno.
    const std::string sAbsolutePath = MakeAbsolute(sPath);

    struct stat oStat{};
    if (stat(sAbsolutePath.c_str(), &oStat) != 0)
    {
        // Captured immediately, before any other call (including the
        // ServiceLocator/Narrow calls below) can clobber it, and used
        // throughout rather than re-reading errno - RA_LOG_ERR's argument
        // evaluation order is otherwise unspecified.
        const int nError = errno;
        if (nError != ENOENT && ra::services::ServiceLocator::Exists<ra::services::ILogger>())
        {
            RA_LOG_ERR("Error %d getting file size: %s", nError, ra::util::String::Narrow(sPath).c_str());
        }

        return -1;
    }

    if (S_ISDIR(oStat.st_mode))
    {
        if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
        {
            RA_LOG_ERR("File size requested for directory: %s", ra::util::String::Narrow(sPath).c_str());
        }

        return -1;
    }

    return static_cast<int64_t>(oStat.st_size);
}

std::chrono::system_clock::time_point LinuxFileSystem::GetLastModified(const std::wstring& sPath) const
{
    // stat(), not std::filesystem::last_write_time: C++17 has no portable
    // conversion from file_time_type to system_clock (clock_cast is C++20).
    // Do not "simplify" this to last_write_time() - see
    // LinuxFileSystem_Tests.cpp's TestGetLastModified for what such a change
    // would break (FileLocalStorage's 30-day cache expiry reads this value
    // on every launch).
    const std::string sAbsolutePath = MakeAbsolute(sPath);

    struct stat oStat{};
    if (stat(sAbsolutePath.c_str(), &oStat) != 0)
    {
        // See GetFileSize for why this is captured immediately into a named
        // local rather than read from a later, separate errno access.
        const int nError = errno;
        RA_LOG_ERR("Error %d getting last modified for file: %s", nError,
                   ra::util::String::Narrow(sPath).c_str());
        return std::chrono::system_clock::time_point();
    }

    return std::chrono::system_clock::from_time_t(oStat.st_mtime);
}

std::unique_ptr<TextReader> LinuxFileSystem::OpenTextFile(const std::wstring& sPath) const
{
    // Kept narrow for the same reason as in GetFileSize: FileTextReader's
    // wide-path constructor narrows it into a temporary of its own, which is
    // destroyed before the constructor even returns - after the failed open,
    // but before the errno read below.
    const std::string sAbsolutePath = MakeAbsolute(sPath);

    auto pReader = std::make_unique<FileTextReader>(sAbsolutePath);
    if (!pReader->GetFStream().is_open())
    {
        // See GetFileSize for why this is captured immediately into a named
        // local rather than read from a later, separate errno access.
        const int nError = errno;
        RA_LOG_INFO("Failed to open \"%s\": %d", ra::util::String::Narrow(sPath).c_str(), nError);
        return std::unique_ptr<TextReader>();
    }

    return std::unique_ptr<TextReader>(pReader.release());
}

std::unique_ptr<TextWriter> LinuxFileSystem::CreateTextFile(const std::wstring& sPath) const
{
    // Kept narrow - see OpenTextFile.
    const std::string sAbsolutePath = MakeAbsolute(sPath);

    auto pWriter = std::make_unique<FileTextWriter>(sAbsolutePath);
    if (!pWriter->GetFStream().is_open())
    {
        // See GetFileSize for why this is captured immediately into a named
        // local rather than read from a later, separate errno access.
        const int nError = errno;
        RA_LOG_WARN("Failed to create \"%s\": %d", ra::util::String::Narrow(sPath).c_str(), nError);
        return std::unique_ptr<TextWriter>();
    }

    return std::unique_ptr<TextWriter>(pWriter.release());
}

std::unique_ptr<TextWriter> LinuxFileSystem::AppendTextFile(const std::wstring& sPath) const
{
    // Kept narrow - see OpenTextFile - and also used for the std::ofstream
    // fallback below, rather than a fresh MakeAbsolute() temporary: that
    // would be destroyed at the end of the declaration, before the errno read.
    const std::string sAbsolutePath = MakeAbsolute(sPath);

    // cannot use std::ios::app, or the SetPosition method doesn't work
    // have to specify std::ios::in or the previous contents are lost
    auto pWriter = std::make_unique<FileTextWriter>(sAbsolutePath,
                                                    std::ios::ate | std::ios::in | std::ios::out);
    if (!pWriter->GetFStream().is_open())
    {
        // failed to open the file - try creating it
        std::ofstream oFile(sAbsolutePath, std::ios::out);
        if (!oFile.is_open())
        {
            // See GetFileSize for why this is captured immediately into a
            // named local rather than read from a later, separate errno
            // access.
            const int nError = errno;
            if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
            {
                RA_LOG_WARN("Failed to open \"%s\" for append: %d",
                            ra::util::String::Narrow(sPath).c_str(), nError);
            }

            return std::unique_ptr<TextWriter>();
        }

        oFile.close();
        pWriter = std::make_unique<FileTextWriter>(sAbsolutePath, std::ios::in | std::ios::out);
    }

    return std::unique_ptr<TextWriter>(pWriter.release());
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
