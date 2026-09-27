#include "GSL.hh"

#include "services/ILogger.hh"
#include "services/IMessageDispatcher.hh"
#include "services/ServiceLocator.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

static inline constexpr const char* __gsl_filename(const char* const str)
{
    if (str == nullptr)
        return str;

    if (str[0] == 's' && str[1] == 'r' && str[2] == 'c' && str[3] == '\\')
        return str;

    const char* scan = str;
    if (scan == nullptr)
        return str;

    while (*scan != '\\')
    {
        if (!*scan)
            return str;
        scan++;
    }

    return __gsl_filename(scan + 1);
}

#ifdef NDEBUG

void __gsl_contract_handler(const char* const file, unsigned int line)
{
    static char buffer[128];
    snprintf(buffer, sizeof(buffer), "Assertion failure at %s: %u", __gsl_filename(file), line);

    if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        RA_LOG_ERR(buffer);
    }

#ifdef _WIN32
    if (ra::services::ServiceLocator::Exists<ra::services::IMessageDispatcher>())
    {
        ra::services::ServiceLocator::Get<ra::services::IMessageDispatcher>()
            .ReportErrorMessage(L"Unexpected error", ra::util::String::Widen(buffer));
    }
#else
    // No message box off Windows. This runs on whichever thread failed the
    // check, under whatever that thread holds - rc_client's state mutex inside
    // DoFrame, a view model's lock - and the Qt views' thread may itself be
    // waiting on one of those (IQtApplicationHost), so a box that waits for an
    // answer could deadlock both. The log line above is the report.
#endif

    gsl::details::throw_exception(gsl::fail_fast(buffer));
}

#else

void __gsl_contract_handler(const char* const file, unsigned int line, const char* const error)
{
    const char* const filename = __gsl_filename(file);
    const auto sError = ra::util::String::Printf("Assertion failure at %s: %d: %s", filename, line, error);

    if (ra::services::ServiceLocator::Exists<ra::services::ILogger>())
    {
        RA_LOG_ERR("%s", sError.c_str());
    }

    _wassert(ra::util::String::Widen(error).c_str(), ra::util::String::Widen(filename).c_str(), line);
}

#endif
