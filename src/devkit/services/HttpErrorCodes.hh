#ifndef RA_SERVICES_HTTPERRORCODES_HH
#define RA_SERVICES_HTTPERRORCODES_HH
#pragma once

namespace ra {
namespace services {

// IHttpRequester::Request returns either an HTTP status code or one of these.
// The numbering is WinINet's, preserved on every platform so that a stored
// status code, a log line and IsRetryable all mean the same thing everywhere.
constexpr unsigned int RA_HTTP_NOT_ATTEMPTED = 0;
constexpr unsigned int RA_HTTP_ERROR_TIMEOUT = 12002;              // ERROR_INTERNET_TIMEOUT
constexpr unsigned int RA_HTTP_ERROR_INTERNAL = 12004;             // ERROR_INTERNET_INTERNAL_ERROR
constexpr unsigned int RA_HTTP_ERROR_NAME_NOT_RESOLVED = 12007;    // ERROR_INTERNET_NAME_NOT_RESOLVED
constexpr unsigned int RA_HTTP_ERROR_OPERATION_CANCELLED = 12017;  // ERROR_INTERNET_OPERATION_CANCELLED
constexpr unsigned int RA_HTTP_ERROR_HANDLE_STATE = 12019;         // ERROR_INTERNET_INCORRECT_HANDLE_STATE
constexpr unsigned int RA_HTTP_ERROR_ITEM_NOT_FOUND = 12028;       // ERROR_INTERNET_ITEM_NOT_FOUND
constexpr unsigned int RA_HTTP_ERROR_CANNOT_CONNECT = 12029;       // ERROR_INTERNET_CANNOT_CONNECT
constexpr unsigned int RA_HTTP_ERROR_CONNECTION_ABORTED = 12030;   // ERROR_INTERNET_CONNECTION_ABORTED
constexpr unsigned int RA_HTTP_ERROR_CONNECTION_RESET = 12031;     // ERROR_INTERNET_CONNECTION_RESET
constexpr unsigned int RA_HTTP_ERROR_FORCE_RETRY = 12032;          // ERROR_INTERNET_FORCE_RETRY
constexpr unsigned int RA_HTTP_ERROR_INVALID_RESPONSE = 12152;     // ERROR_HTTP_INVALID_SERVER_RESPONSE
constexpr unsigned int RA_HTTP_ERROR_DISCONNECTED = 12163;         // ERROR_INTERNET_DISCONNECTED

// socket-level errors WinHTTP can surface directly
constexpr unsigned int RA_HTTP_ERROR_SOCKET_TRY_AGAIN = 11002;     // WSATRY_AGAIN
constexpr unsigned int RA_HTTP_ERROR_SOCKET_CONN_RESET = 10054;    // WSAECONNRESET

/// <summary>
/// Determines whether it would be reasonable to retry a request that returned
/// the provided status code.
/// </summary>
constexpr bool IsRetryableStatusCode(unsigned int nStatusCode) noexcept
{
    switch (nStatusCode)
    {
        case RA_HTTP_NOT_ATTEMPTED:              // not attempted
        case 200:                                // success
        case RA_HTTP_ERROR_TIMEOUT:
        case RA_HTTP_ERROR_NAME_NOT_RESOLVED:
        case RA_HTTP_ERROR_SOCKET_TRY_AGAIN:
        case RA_HTTP_ERROR_OPERATION_CANCELLED:
        case RA_HTTP_ERROR_HANDLE_STATE:
        case RA_HTTP_ERROR_ITEM_NOT_FOUND:
        case RA_HTTP_ERROR_CANNOT_CONNECT:
        case RA_HTTP_ERROR_CONNECTION_ABORTED:
        case RA_HTTP_ERROR_CONNECTION_RESET:
        case RA_HTTP_ERROR_SOCKET_CONN_RESET:
        case RA_HTTP_ERROR_FORCE_RETRY:
        case RA_HTTP_ERROR_INVALID_RESPONSE:
        case RA_HTTP_ERROR_DISCONNECTED:
            return true;

        default:
            return false;
    }
}

} // namespace services
} // namespace ra

#endif // !RA_SERVICES_HTTPERRORCODES_HH
