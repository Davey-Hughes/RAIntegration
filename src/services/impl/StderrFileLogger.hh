#ifndef RA_SERVICES_STDERR_FILELOGGER_HH
#define RA_SERVICES_STDERR_FILELOGGER_HH
#pragma once

#include "services/impl/FileLogger.hh"

#include <cstdio>

namespace ra {
namespace services {
namespace impl {

// The counterpart to WindowsDebuggerFileLogger: the file log, plus a copy to
// the place a developer is already watching. OutputDebugStringA goes to the
// attached debugger on Windows; stderr is the equivalent here.
class StderrFileLogger : public FileLogger
{
public:
    explicit StderrFileLogger(const ra::services::IFileSystem& pFileSystem)
        : FileLogger(pFileSystem)
    {
    }

    void LogMessage(LogLevel level, const std::string& sMessage) const override
    {
        FileLogger::LogMessage(level, sMessage);

        std::fputs(sMessage.c_str(), stderr);
        std::fputc('\n', stderr);
    }
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_STDERR_FILELOGGER_HH
