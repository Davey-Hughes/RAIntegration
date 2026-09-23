#include "WindowsHttpRequester.hh"

#include "RA_Defs.h"

#include "util/Log.hh"
#include "util/Strings.hh"

#include "services/HttpErrorCodes.hh"
#include "services/impl/StringTextWriter.hh"

#include <winhttp.h>

//#define ALLOW_INVALID_SSL_CERTIFICATES

// The shared table is WinINet's numbering. Assert the agreement rather than
// trusting it - the ERROR_INTERNET_* names come from <wininet.h>, which cannot
// be included here alongside <winhttp.h>, so the WinHTTP aliases stand in.
//
// These have to stay at file scope: inside namespace ra::services::impl the
// qualified names would resolve to a nested ra::services::impl::ra, which both
// declares a second set of constants and hides the global ::ra.
//
// Four of the fifteen shared codes have no WinHTTP name to assert against -
// RA_HTTP_NOT_ATTEMPTED (0, not an error code at all),
// RA_HTTP_ERROR_ITEM_NOT_FOUND (12028), RA_HTTP_ERROR_CONNECTION_RESET (12031)
// and RA_HTTP_ERROR_DISCONNECTED (12163). <winhttp.h> skips 12028, 12031 and
// 12163; the remaining eleven are checked below.
static_assert(ERROR_WINHTTP_TIMEOUT == ra::services::RA_HTTP_ERROR_TIMEOUT);
static_assert(ERROR_WINHTTP_INTERNAL_ERROR == ra::services::RA_HTTP_ERROR_INTERNAL);
static_assert(ERROR_WINHTTP_NAME_NOT_RESOLVED == ra::services::RA_HTTP_ERROR_NAME_NOT_RESOLVED);
static_assert(ERROR_WINHTTP_OPERATION_CANCELLED == ra::services::RA_HTTP_ERROR_OPERATION_CANCELLED);
static_assert(ERROR_WINHTTP_INCORRECT_HANDLE_STATE == ra::services::RA_HTTP_ERROR_HANDLE_STATE);
static_assert(ERROR_WINHTTP_CANNOT_CONNECT == ra::services::RA_HTTP_ERROR_CANNOT_CONNECT);
static_assert(ERROR_WINHTTP_CONNECTION_ERROR == ra::services::RA_HTTP_ERROR_CONNECTION_ABORTED);
static_assert(ERROR_WINHTTP_RESEND_REQUEST == ra::services::RA_HTTP_ERROR_FORCE_RETRY);
static_assert(ERROR_WINHTTP_INVALID_SERVER_RESPONSE == ra::services::RA_HTTP_ERROR_INVALID_RESPONSE);
static_assert(WSATRY_AGAIN == ra::services::RA_HTTP_ERROR_SOCKET_TRY_AGAIN);
static_assert(WSAECONNRESET == ra::services::RA_HTTP_ERROR_SOCKET_CONN_RESET);

// the shared table hardcodes 200 where this file used the constant
static_assert(HTTP_STATUS_OK == 200);

namespace ra {
namespace services {
namespace impl {

static bool ReadIntoString(const HINTERNET hRequest, std::string& sBuffer, DWORD& nStatusCode)
{
    DWORD dwSize = sizeof(DWORD);
    DWORD nContentLength = 0;

#pragma warning(push)
#pragma warning(disable: 26477)
    GSL_SUPPRESS_ES47
    if (!WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &nContentLength, &dwSize, WINHTTP_NO_HEADER_INDEX))
        return false;
#pragma warning(pop)

    // allocate enough space in the string for the whole content
    sBuffer.resize(gsl::narrow_cast<size_t>(nContentLength) + 1); // reserve space for null terminator
    sBuffer.at(nContentLength) = '\0'; // set null terminator, will fill remaining portion of buffer
    sBuffer.resize(nContentLength);

    DWORD nAvailableBytes = 0U;
    WinHttpQueryDataAvailable(hRequest, &nAvailableBytes);

    DWORD nInsertAt = 0U;
    while (nAvailableBytes > 0)
    {
        DWORD nBytesFetched = 0U;
        const DWORD nBytesToRead = gsl::narrow_cast<DWORD>(sBuffer.capacity()) - nInsertAt;
        if (WinHttpReadData(hRequest, &sBuffer.at(nInsertAt), nBytesToRead, &nBytesFetched))
        {
            nInsertAt += nBytesFetched;
        }
        else
        {
            if (nStatusCode == 200)
                nStatusCode = GetLastError();

            break;
        }

        WinHttpQueryDataAvailable(hRequest, &nAvailableBytes);
    }

    return true;
}

static void ReadIntoWriter(const HINTERNET hRequest, ra::services::TextWriter& pContentWriter, DWORD& nStatusCode)
{
    DWORD nAvailableBytes = 0;
    WinHttpQueryDataAvailable(hRequest, &nAvailableBytes);

    // writing to non string buffer - do up to 4K at a time
    std::string sBuffer;

    while (nAvailableBytes > 0)
    {
        const DWORD nBytesToRead = (nAvailableBytes > 4096) ? 4096 : nAvailableBytes;
        sBuffer.resize(nBytesToRead);

        DWORD nBytesFetched = 0U;
        if (WinHttpReadData(hRequest, sBuffer.data(), nBytesToRead, &nBytesFetched))
        {
            sBuffer.resize(nBytesFetched);
            pContentWriter.Write(sBuffer);
        }
        else
        {
            if (nStatusCode == 200)
                nStatusCode = GetLastError();

            break;
        }

        WinHttpQueryDataAvailable(hRequest, &nAvailableBytes);
    }
}

unsigned int WindowsHttpRequester::Request(const Http::Request& pRequest, TextWriter& pContentWriter) const
{
    DWORD nStatusCode = 0;

    // obtain a session handle.
#pragma warning(push)
#pragma warning(disable: 26477)
    GSL_SUPPRESS_ES47 HINTERNET hSession = WinHttpOpen(m_sUserAgent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
#pragma warning(pop)

    if (hSession == nullptr)
    {
        nStatusCode = GetLastError();
    }
    else
    {
        INTERNET_PORT nPort = INTERNET_DEFAULT_HTTP_PORT;

        auto sUrl = pRequest.GetUrl();
        if (_strnicmp(sUrl.c_str(), "http://", 7) == 0)
        {
            sUrl.erase(0, 7);
        }
        else if (_strnicmp(sUrl.c_str(), "https://", 8) == 0)
        {
            sUrl.erase(0, 8);
            nPort = INTERNET_DEFAULT_HTTPS_PORT;
        }

        std::string sPath;
        const auto nIndex = sUrl.find('/');
        if (nIndex != std::string::npos)
        {
            sPath.assign(sUrl, nIndex + 1, std::string::npos);
            sUrl.resize(nIndex);
        }

        const auto nPortIndex = sUrl.find(':');
        if (nPortIndex != std::string::npos)
        {
            nPort = gsl::narrow_cast<INTERNET_PORT>(atoi(&sUrl.at(nPortIndex + 1)));
            sUrl.resize(nPortIndex);
        }

        // specify the server
        auto sHostName = ra::util::String::Widen(sUrl);
        HINTERNET hConnect = WinHttpConnect(hSession, sHostName.c_str(), nPort, 0);

        if (hConnect == nullptr)
        {
            nStatusCode = GetLastError();
        }
        else
        {
            // merge query parameters onto sPath.
            std::string sQueryString = pRequest.GetQueryString();
            if (!sQueryString.empty())
            {
                sPath.push_back('?');
                sPath += sQueryString;
            }

            auto sPostData = pRequest.GetPostData();

            // open the connection
            auto sPathWide = ra::util::String::Widen(sPath);
            GSL_SUPPRESS_ES47
            HINTERNET hRequest = WinHttpOpenRequest(hConnect,
                sPostData.empty() ? L"GET" : L"POST",
                sPathWide.c_str(),
                nullptr,
                WINHTTP_NO_REFERER,
                WINHTTP_DEFAULT_ACCEPT_TYPES,
                (nPort == INTERNET_DEFAULT_HTTPS_PORT) ? WINHTTP_FLAG_SECURE : 0);

            if (hRequest == nullptr)
            {
                nStatusCode = GetLastError();
            }
            else
            {
                std::wstring sHeaders;
                sHeaders += L"Content-Type: ";
                sHeaders += ra::util::String::Widen(pRequest.GetContentType());

                BOOL bResults{};
                bool retry;

                do
                {
                    retry = false;

                    // send the request
                    if (sPostData.empty())
                    {
                        GSL_SUPPRESS_ES47
                        bResults = WinHttpSendRequest(hRequest,
                            sHeaders.c_str(), gsl::narrow_cast<int>(sHeaders.length()),
                            WINHTTP_NO_REQUEST_DATA,
                            0, 0,
                            0);
                    }
                    else
                    {
                        bResults = WinHttpSendRequest(hRequest,
                            sHeaders.c_str(), gsl::narrow_cast<int>(sHeaders.length()),
                            sPostData.data(),
                            gsl::narrow_cast<int>(sPostData.length()), gsl::narrow_cast<int>(sPostData.length()),
                            0);
                    }

                    if (!bResults)
                    {
                        nStatusCode = GetLastError();

                        if (nStatusCode == ERROR_WINHTTP_RESEND_REQUEST)
                        {
                            retry = true;
                        }
#ifdef ALLOW_INVALID_SSL_CERTIFICATES
                        else if (nStatusCode == ERROR_WINHTTP_SECURE_FAILURE)
                        {
                            // https://stackoverflow.com/questions/19338395/how-do-you-use-winhttp-to-do-ssl-with-a-self-signed-cert
                            DWORD dwFlags =
                                SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                                SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE |
                                SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                                SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;

                            if (WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &dwFlags, sizeof(dwFlags)))
                                retry = true;
                        }
#endif
                    }
                } while (retry);

                if (!bResults || !WinHttpReceiveResponse(hRequest, nullptr))
                {
                    nStatusCode = GetLastError();
                }
                else
                {
                    // get the http status code
                    DWORD dwSize = sizeof(DWORD);
                    
                    GSL_SUPPRESS_ES47 WinHttpQueryHeaders(
                        hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &nStatusCode, &dwSize, WINHTTP_NO_HEADER_INDEX);

                    // read the response
                    auto* pStringWriter = dynamic_cast<StringTextWriter*>(&pContentWriter);
                    if (pStringWriter != nullptr)
                    {
                        // optimized path for writing to string buffer
                        if (!ReadIntoString(hRequest, pStringWriter->GetString(), nStatusCode))
                        {
                            // could not use optimization, fall back to buffered reader
                            ReadIntoWriter(hRequest, pContentWriter, nStatusCode);
                        }
                    }
                    else
                    {
                        ReadIntoWriter(hRequest, pContentWriter, nStatusCode);
                    }
                }

                WinHttpCloseHandle(hRequest);
            }

            WinHttpCloseHandle(hConnect);
        }

        WinHttpCloseHandle(hSession);
    }

    return nStatusCode;
}

bool WindowsHttpRequester::IsRetryable(unsigned int nStatusCode) const noexcept
{
    return ra::services::IsRetryableStatusCode(nStatusCode);
}

std::string WindowsHttpRequester::GetStatusCodeText(unsigned int nStatusCode) const
{
    std::string message;

    // winhttp.h only defines *some* of the wininet errors; the file-scope
    // static_asserts above already confirm the two numberings agree.

    if (nStatusCode >= WINHTTP_ERROR_BASE && nStatusCode <= WINHTTP_ERROR_LAST)
    {
        GSL_SUPPRESS_CON4 TCHAR szMessageBuffer[256] = TEXT("");
        const DWORD nResult = ::FormatMessage(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
            ::GetModuleHandle(TEXT("winhttp.dll")),
            nStatusCode, 0, (LPTSTR)szMessageBuffer,
            sizeof(szMessageBuffer)/sizeof(szMessageBuffer[0]), nullptr);

        if (nResult > 0)
        {
            message = ra::util::String::Narrow(szMessageBuffer);

            // trim trailing whitespace
            while (ra::util::String::IsSpace(message.back()))
                message.pop_back();
        }
    }
    else
    {
        switch (nStatusCode)
        {
            case HTTP_STATUS_NO_CONTENT: message = "No Content"; break;
            case HTTP_STATUS_MOVED: message = "Moved Permanently"; break;
            case HTTP_STATUS_REDIRECT: message = "Moved Temporarily"; break;
            case HTTP_STATUS_BAD_REQUEST: message = "Bad Request"; break;
            case HTTP_STATUS_DENIED: message = "Unauthorized"; break;
            case HTTP_STATUS_FORBIDDEN: message = "Forbidden"; break;
            case HTTP_STATUS_NOT_FOUND: message = "Not Found"; break;
            case HTTP_STATUS_REQUEST_TIMEOUT: message = "Request Time-out"; break;
            case HTTP_STATUS_REQUEST_TOO_LARGE: message = "Request Entity Too Large"; break;
            case HTTP_STATUS_URI_TOO_LONG: message = "Request-URI Too Large"; break;
            case HTTP_STATUS_SERVER_ERROR: message = "Internal Server Error"; break;
            case HTTP_STATUS_NOT_SUPPORTED: message = "Not Implemented"; break;
            case HTTP_STATUS_BAD_GATEWAY: message = "Bad Gateway"; break;
            case HTTP_STATUS_SERVICE_UNAVAIL: message = "Service Unavailable"; break;
            case 429: message = "Too Many Requests"; break;
        }
    }

    return message;
}


} // namespace impl
} // namespace services
} // namespace ra
