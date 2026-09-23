#ifndef RA_SERVICES_LINUX_FILESYSTEM_HH
#define RA_SERVICES_LINUX_FILESYSTEM_HH
#pragma once

#include "services/IFileSystem.hh"

namespace ra {
namespace services {
namespace impl {

#undef CreateDirectory
#undef DeleteFile
#undef MoveFile
#undef CopyFile

class LinuxFileSystem : public IFileSystem
{
public:
    LinuxFileSystem() noexcept;

    const std::wstring& BaseDirectory() const noexcept override { return m_sBaseDirectory; }
    bool DirectoryExists(const std::wstring& sDirectory) const override;
    bool CreateDirectory(const std::wstring& sDirectory) const override;
    size_t GetFilesInDirectory(const std::wstring& sDirectory,
                               _Inout_ std::vector<std::wstring>& vResults) const override;
    bool DeleteFile(const std::wstring& sPath) const override;
    bool MoveFile(const std::wstring& sOldPath, const std::wstring& sNewPath) const override;
    bool CopyFile(const std::wstring& sSourcePath, const std::wstring& sNewPath) const override;
    int64_t GetFileSize(const std::wstring& sPath) const override;
    std::chrono::system_clock::time_point GetLastModified(const std::wstring& sPath) const override;
    std::unique_ptr<TextReader> OpenTextFile(const std::wstring& sPath) const override;
    std::unique_ptr<TextWriter> CreateTextFile(const std::wstring& sPath) const override;
    std::unique_ptr<TextWriter> AppendTextFile(const std::wstring& sPath) const override;

    std::wstring GetDirectory(const std::wstring& sPath) const override;
    std::wstring GetFileName(const std::wstring& sPath) const override;
    std::wstring GetExtension(const std::wstring& sPath) const override;
    std::wstring RemoveExtension(const std::wstring& sPath) const override;

private:
    // Returns a narrow, absolute path suitable for passing to the C library.
    std::string MakeAbsolute(const std::wstring& sPath) const;

    std::wstring m_sBaseDirectory;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_LINUX_FILESYSTEM_HH
