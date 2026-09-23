#ifndef _WIN32

#include "LinuxHttpRequester.hh"

#include "services/HttpErrorCodes.hh"
#include "services/TextWriter.hh"

#include "util/Log.hh"

#include <curl/curl.h>

#include <mutex>

namespace ra {
namespace services {
namespace impl {

static size_t WriteCallback(char* pData, size_t nSize, size_t nCount, void* pUserData) noexcept
{
    const size_t nBytes = nSize * nCount;

    // Neither the std::string nor TextWriter::Write (FileTextWriter's overload
    // can throw from the stream) is noexcept, and letting an exception unwind
    // through libcurl's C frames is undefined behaviour - it would also skip
    // the curl_slist_free_all/curl_easy_cleanup below. Abort the transfer
    // instead: a short return makes curl_easy_perform fail with
    // CURLE_WRITE_ERROR, which unwinds through Request's own cleanup.
    try
    {
        auto* pWriter = static_cast<ra::services::TextWriter*>(pUserData);
        pWriter->Write(std::string(pData, nBytes));
        return nBytes;
    }
    catch (...)
    {
        return 0;
    }
}

namespace detail {

unsigned int MapCurlError(int nCurlCode) noexcept
{
    switch (nCurlCode)
    {
        case CURLE_OPERATION_TIMEDOUT:
            return RA_HTTP_ERROR_TIMEOUT;

        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return RA_HTTP_ERROR_NAME_NOT_RESOLVED;

        case CURLE_COULDNT_CONNECT:
            return RA_HTTP_ERROR_CANNOT_CONNECT;

        case CURLE_SEND_ERROR:
            return RA_HTTP_ERROR_CONNECTION_ABORTED;

        case CURLE_RECV_ERROR:
        case CURLE_PARTIAL_FILE:
            return RA_HTTP_ERROR_CONNECTION_RESET;

        case CURLE_GOT_NOTHING:
        case CURLE_WEIRD_SERVER_REPLY:
            return RA_HTTP_ERROR_INVALID_RESPONSE;

        default:
            // deliberately not retryable: an unmapped failure that retries
            // forever is worse than one that surfaces. CURLE_WRITE_ERROR (the
            // aborted WriteCallback above) lands here too.
            return RA_HTTP_ERROR_INTERNAL;
    }
}

} // namespace detail

LinuxHttpRequester::LinuxHttpRequester()
{
    // curl_global_init is not thread-safe and must run before any easy handle
    // exists. std::call_once is what serialises it - the requester is created
    // through ServiceLocator, which nothing guarantees to be a single thread.
    //
    // There is deliberately no matching curl_global_cleanup. RAIntegration
    // ships as a DLL the emulator can unload at any point, and tearing down
    // libcurl's global state while another service still holds a handle is
    // worse than leaking it for the life of the process.
    static std::once_flag s_oInitialised;
    std::call_once(s_oInitialised, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

unsigned int LinuxHttpRequester::Request(const Http::Request& pRequest, TextWriter& pContentWriter) const
{
    // One handle per call: Request is const and the thread pool calls it
    // concurrently, so a shared handle would be a data race.
    CURL* pCurl = curl_easy_init();
    if (pCurl == nullptr)
        return RA_HTTP_ERROR_INTERNAL;

    std::string sUrl = pRequest.GetUrl();
    if (!pRequest.GetQueryString().empty())
    {
        sUrl.push_back('?');
        sUrl += pRequest.GetQueryString();
    }

    curl_easy_setopt(pCurl, CURLOPT_URL, sUrl.c_str());
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &pContentWriter);

    // WinHTTP follows redirects by default; curl does not. Parity, not preference.
    curl_easy_setopt(pCurl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(pCurl, CURLOPT_CONNECTTIMEOUT, 30L);

    // no CURLOPT_TIMEOUT: image downloads are unbounded in size, so a total
    // deadline would cut off a slow but healthy transfer. WinHTTP does not have
    // one either - it has a 30s receive-data timeout, which bounds a *stall*
    // rather than the transfer. This is the curl equivalent: fail once the
    // transfer has averaged under one byte per second for 30s. curl reports it
    // as CURLE_OPERATION_TIMEDOUT, which maps to the same RA_HTTP_ERROR_TIMEOUT
    // WinHTTP would return.
    curl_easy_setopt(pCurl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(pCurl, CURLOPT_LOW_SPEED_TIME, 30L);

    if (!m_sUserAgent.empty())
        curl_easy_setopt(pCurl, CURLOPT_USERAGENT, m_sUserAgent.c_str());

    struct curl_slist* pHeaders = nullptr;

    if (!pRequest.GetPostData().empty())
    {
        curl_easy_setopt(pCurl, CURLOPT_POSTFIELDS, pRequest.GetPostData().c_str());
        curl_easy_setopt(pCurl, CURLOPT_POSTFIELDSIZE,
                         static_cast<long>(pRequest.GetPostData().length()));

        const std::string sContentType = "Content-Type: " + pRequest.GetContentType();
        pHeaders = curl_slist_append(pHeaders, sContentType.c_str());
        curl_easy_setopt(pCurl, CURLOPT_HTTPHEADER, pHeaders);
    }

    const CURLcode nResult = curl_easy_perform(pCurl);

    unsigned int nStatusCode;
    if (nResult == CURLE_OK)
    {
        long nResponseCode = 0;
        curl_easy_getinfo(pCurl, CURLINFO_RESPONSE_CODE, &nResponseCode);

        // CURLINFO_RESPONSE_CODE stays 0 when the transfer completed without an
        // HTTP status line (a file:// URL, or a protocol curl handled but this
        // caller cannot interpret). 0 is RA_HTTP_NOT_ATTEMPTED, which
        // IsRetryableStatusCode calls retryable, so returning it would retry a
        // request that did in fact happen - forever, in ConnectedServer and
        // RcClient. Report it as an unusable response instead.
        nStatusCode = (nResponseCode == 0) ? RA_HTTP_ERROR_INVALID_RESPONSE
                                           : static_cast<unsigned int>(nResponseCode);
    }
    else
    {
        nStatusCode = detail::MapCurlError(nResult);

        // Deliberately GetUrl() and not sUrl: Http::Request splits the query
        // string off at construction, so this is the scheme, host and path
        // without it. A query string can carry a credential - RcClient redacts
        // p= and t= out of a request's parameters before it logs them - and
        // this is a transport: it cannot know which parameter of some future
        // caller's request is the secret one, so a denylist here would leak
        // everything not on the list. Dropping the query string is the
        // fail-closed choice, and costs nothing diagnostically: what curl
        // failed at (resolve, connect, TLS, stalled transfer) is decided by
        // the scheme, host and port rather than by the parameters.
        RA_LOG_WARN("curl error %d (%s) requesting %s", static_cast<int>(nResult),
                    curl_easy_strerror(nResult), pRequest.GetUrl().c_str());
    }

    if (pHeaders != nullptr)
        curl_slist_free_all(pHeaders);

    curl_easy_cleanup(pCurl);

    return nStatusCode;
}

bool LinuxHttpRequester::IsRetryable(unsigned int nStatusCode) const noexcept
{
    return ra::services::IsRetryableStatusCode(nStatusCode);
}

std::string LinuxHttpRequester::GetStatusCodeText(unsigned int nStatusCode) const
{
    switch (nStatusCode)
    {
        case 204: return "No Content";
        case 301: return "Moved Permanently";
        case 302: return "Moved Temporarily";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 408: return "Request Time-out";
        case 413: return "Request Entity Too Large";
        case 414: return "Request-URI Too Large";
        case 429: return "Too Many Requests";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";

        // every unmapped CURLcode lands on RA_HTTP_ERROR_INTERNAL, so it is the
        // one that most needs text. WinHTTP's FormatMessage supplies
        // "An internal error has occurred."
        case RA_HTTP_ERROR_INTERNAL: return "An internal error has occurred";
        case RA_HTTP_ERROR_TIMEOUT: return "The operation timed out";
        case RA_HTTP_ERROR_NAME_NOT_RESOLVED: return "The server name could not be resolved";
        case RA_HTTP_ERROR_CANNOT_CONNECT: return "A connection could not be established";
        case RA_HTTP_ERROR_CONNECTION_ABORTED: return "The connection was aborted";
        case RA_HTTP_ERROR_CONNECTION_RESET: return "The connection was reset";
        case RA_HTTP_ERROR_INVALID_RESPONSE: return "The server response could not be parsed";

        default: return std::string();
    }
}

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
