#include "LinuxFileSystem.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include "services/impl/FileTextReader.hh"
#include "services/impl/FileTextWriter.hh"
#include "services/ServiceLocator.hh"

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

    std::string sDirectory = oError ? std::string(".") : oExe.parent_path().string();
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
    for (const auto& oEntry : oIterator)
    {
        if (oEntry.is_directory(oError))
            continue;

        // FindFirstFileW reports cFileName, not the full path
        vResults.emplace_back(ra::util::String::Widen(oEntry.path().filename().string()));
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
    struct stat oStat{};
    if (stat(MakeAbsolute(sPath).c_str(), &oStat) != 0)
    {
        if (errno != ENOENT && ra::services::ServiceLocator::Exists<ra::services::ILogger>())
        {
            RA_LOG_ERR("Error %d getting file size: %s", errno, ra::util::String::Narrow(sPath).c_str());
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
    struct stat oStat{};
    if (stat(MakeAbsolute(sPath).c_str(), &oStat) != 0)
    {
        RA_LOG_ERR("Error %d getting last modified for file: %s", errno,
                   ra::util::String::Narrow(sPath).c_str());
        return std::chrono::system_clock::time_point();
    }

    return std::chrono::system_clock::from_time_t(oStat.st_mtime);
}

std::unique_ptr<TextReader> LinuxFileSystem::OpenTextFile(const std::wstring& sPath) const
{
    const std::wstring sAbsolutePath = ra::util::String::Widen(MakeAbsolute(sPath));

    auto pReader = std::make_unique<FileTextReader>(sAbsolutePath);
    if (!pReader->GetFStream().is_open())
    {
        RA_LOG_INFO("Failed to open \"%s\": %d", ra::util::String::Narrow(sPath).c_str(), errno);
        return std::unique_ptr<TextReader>();
    }

    return std::unique_ptr<TextReader>(pReader.release());
}

std::unique_ptr<TextWriter> LinuxFileSystem::CreateTextFile(const std::wstring& sPath) const
{
    const std::wstring sAbsolutePath = ra::util::String::Widen(MakeAbsolute(sPath));

    auto pWriter = std::make_unique<FileTextWriter>(sAbsolutePath);
    if (!pWriter->GetFStream().is_open())
    {
        RA_LOG_WARN("Failed to create \"%s\": %d", ra::util::String::Narrow(sPath).c_str(), errno);
        return std::unique_ptr<TextWriter>();
    }

    return std::unique_ptr<TextWriter>(pWriter.release());
}

std::unique_ptr<TextWriter> LinuxFileSystem::AppendTextFile(const std::wstring& sPath) const
{
    const std::wstring sAbsolutePath = ra::util::String::Widen(MakeAbsolute(sPath));

    // cannot use std::ios::app, or the SetPosition method doesn't work
    // have to specify std::ios::in or the previous contents are lost
    auto pWriter = std::make_unique<FileTextWriter>(sAbsolutePath,
                                                    std::ios::ate | std::ios::in | std::ios::out);
    if (!pWriter->GetFStream().is_open())
    {
        // failed to open the file - try creating it
        std::ofstream oFile(MakeAbsolute(sPath), std::ios::out);
        if (!oFile.is_open())
        {
            if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
            {
                RA_LOG_WARN("Failed to open \"%s\" for append: %d",
                            ra::util::String::Narrow(sPath).c_str(), errno);
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
