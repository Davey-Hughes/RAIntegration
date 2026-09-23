#include "LinuxHttpRequester.hh"

#include "services/HttpErrorCodes.hh"
#include "services/TextWriter.hh"

#include "util/Log.hh"

#include <curl/curl.h>

#include <mutex>

namespace ra {
namespace services {
namespace impl {

static size_t WriteCallback(char* pData, size_t nSize, size_t nCount, void* pUserData)
{
    const size_t nBytes = nSize * nCount;
    auto* pWriter = static_cast<ra::services::TextWriter*>(pUserData);
    pWriter->Write(std::string(pData, nBytes));
    return nBytes;
}

static unsigned int MapCurlError(CURLcode nResult) noexcept
{
    switch (nResult)
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
            // forever is worse than one that surfaces
            return RA_HTTP_ERROR_INTERNAL;
    }
}

LinuxHttpRequester::LinuxHttpRequester()
{
    // curl_global_init is not thread-safe and must run before any easy handle
    // exists. The constructor runs on the initialisation thread.
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
    // no CURLOPT_TIMEOUT: image downloads are unbounded in size

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
        nStatusCode = static_cast<unsigned int>(nResponseCode);
    }
    else
    {
        nStatusCode = MapCurlError(nResult);
        RA_LOG_WARN("curl error %d (%s) requesting %s", static_cast<int>(nResult),
                    curl_easy_strerror(nResult), sUrl.c_str());
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
