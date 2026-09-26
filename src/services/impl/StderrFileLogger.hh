#ifndef RA_SERVICES_STDERR_FILELOGGER_HH
#define RA_SERVICES_STDERR_FILELOGGER_HH
#pragma once

#include "services/impl/FileLogger.hh"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ra {
namespace services {
namespace impl {

// The Linux counterpart to WindowsDebuggerFileLogger: the file log, plus an
// optional copy to stderr. The copy is not the equivalent of the Windows one.
// OutputDebugStringA only reaches a debugger (or a listener such as DebugView)
// and is otherwise discarded, so on Windows the copy costs a release build
// nothing. stderr is always somewhere: loaded into a libretro core it is the
// frontend's log, and with it any log a user attaches to a bug report. Every
// RA_LOG_* line would land there, whole API responses among them.
//
// So the copy is off unless RA_LOG_STDERR is set to a non-empty value other
// than "0". RACache/RALog.txt gets every line either way. The variable is read
// once, by the constructor, which runs before the logger is registered; after
// that the setting is a const member, so concurrent LogMessage calls only
// read it.
class StderrFileLogger : public FileLogger
{
public:
    explicit StderrFileLogger(const ra::services::IFileSystem& pFileSystem)
        : FileLogger(pFileSystem), m_bMirrorToStderr(IsMirrorRequested())
    {
    }

    void LogMessage(LogLevel level, const std::string& sMessage) const override
    {
        FileLogger::LogMessage(level, sMessage);

        if (m_bMirrorToStderr)
        {
            std::fputs(sMessage.c_str(), stderr);
            std::fputc('\n', stderr);
        }
    }

private:
    static bool IsMirrorRequested()
    {
        const char* sValue = std::getenv("RA_LOG_STDERR");
        return sValue != nullptr && sValue[0] != '\0' && std::strcmp(sValue, "0") != 0;
    }

    const bool m_bMirrorToStderr;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_STDERR_FILELOGGER_HH
