#ifndef _WIN32

#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/StringTextWriter.hh"

#include "services/HttpErrorCodes.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <tuple>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

namespace {

// The two tests that open a socket have to be independent of whatever proxy the
// developer (or CI container) happens to export. With http_proxy/all_proxy set,
// TestClosedPortMapsToCannotConnect never reaches 127.0.0.1:1 and fails for an
// environmental reason, and - worse - TestUnresolvableHostMapsToNameNotResolved
// passes for the wrong one, because a proxy hostname that fails to resolve
// yields CURLE_COULDNT_RESOLVE_PROXY, which maps to the same 12007 it asserts.
//
// no_proxy=* is the environment spelling of CURLOPT_NOPROXY, "*"; the proxy
// variables are cleared as well so the test does not depend on libcurl
// preferring one spelling over another.
class ProxyFreeEnvironment
{
public:
    ProxyFreeEnvironment()
    {
        for (const char* sName : s_sVariables)
        {
            const char* sValue = std::getenv(sName);
            m_vRestore.emplace_back(std::string(sName), sValue ? std::string(sValue) : std::string(),
                                    sValue != nullptr);
            ::unsetenv(sName);
        }

        ::setenv("no_proxy", "*", 1);
        ::setenv("NO_PROXY", "*", 1);
    }

    ~ProxyFreeEnvironment()
    {
        for (const auto& pRestore : m_vRestore)
        {
            if (std::get<2>(pRestore))
                ::setenv(std::get<0>(pRestore).c_str(), std::get<1>(pRestore).c_str(), 1);
            else
                ::unsetenv(std::get<0>(pRestore).c_str());
        }
    }

    ProxyFreeEnvironment(const ProxyFreeEnvironment&) = delete;
    ProxyFreeEnvironment& operator=(const ProxyFreeEnvironment&) = delete;
    ProxyFreeEnvironment(ProxyFreeEnvironment&&) = delete;
    ProxyFreeEnvironment& operator=(ProxyFreeEnvironment&&) = delete;

private:
    static constexpr const char* s_sVariables[] = {
        "http_proxy", "HTTP_PROXY", "https_proxy", "HTTPS_PROXY",
        "all_proxy",  "ALL_PROXY",  "no_proxy",    "NO_PROXY"
    };

    std::vector<std::tuple<std::string, std::string, bool>> m_vRestore;
};

static bool SupportsFileProtocol()
{
    const curl_version_info_data* pVersion = curl_version_info(CURLVERSION_NOW);
    for (const char* const* pProtocol = pVersion->protocols; *pProtocol != nullptr; ++pProtocol)
    {
        if (std::strcmp(*pProtocol, "file") == 0)
            return true;
    }

    return false;
}

} // namespace

TEST_CLASS(LinuxHttpRequester_Tests)
{
public:
    TEST_METHOD(TestIsRetryableUsesTheSharedTable)
    {
        const LinuxHttpRequester oRequester;

        Assert::IsTrue(oRequester.IsRetryable(0), L"not attempted");
        Assert::IsTrue(oRequester.IsRetryable(200));
        Assert::IsTrue(oRequester.IsRetryable(RA_HTTP_ERROR_TIMEOUT));
        Assert::IsTrue(oRequester.IsRetryable(RA_HTTP_ERROR_CANNOT_CONNECT));
        Assert::IsTrue(oRequester.IsRetryable(RA_HTTP_ERROR_CONNECTION_RESET));

        Assert::IsFalse(oRequester.IsRetryable(401));
        Assert::IsFalse(oRequester.IsRetryable(404));
        Assert::IsFalse(oRequester.IsRetryable(429));
        Assert::IsFalse(oRequester.IsRetryable(RA_HTTP_ERROR_INTERNAL));
    }

    TEST_METHOD(TestMapCurlError)
    {
        // Every branch of the map, with no network. The socket-based tests
        // below can only reach two of them, so a mis-mapped CURLcode anywhere
        // else would otherwise ship unnoticed.
        struct Mapping
        {
            int nCurlCode;
            unsigned int nExpected;
            const wchar_t* sName;
        };

        const Mapping vMappings[] = {
            {CURLE_OPERATION_TIMEDOUT, RA_HTTP_ERROR_TIMEOUT, L"CURLE_OPERATION_TIMEDOUT"},
            {CURLE_COULDNT_RESOLVE_HOST, RA_HTTP_ERROR_NAME_NOT_RESOLVED, L"CURLE_COULDNT_RESOLVE_HOST"},
            {CURLE_COULDNT_RESOLVE_PROXY, RA_HTTP_ERROR_NAME_NOT_RESOLVED, L"CURLE_COULDNT_RESOLVE_PROXY"},
            {CURLE_COULDNT_CONNECT, RA_HTTP_ERROR_CANNOT_CONNECT, L"CURLE_COULDNT_CONNECT"},
            {CURLE_SEND_ERROR, RA_HTTP_ERROR_CONNECTION_ABORTED, L"CURLE_SEND_ERROR"},
            {CURLE_RECV_ERROR, RA_HTTP_ERROR_CONNECTION_RESET, L"CURLE_RECV_ERROR"},
            {CURLE_PARTIAL_FILE, RA_HTTP_ERROR_CONNECTION_RESET, L"CURLE_PARTIAL_FILE"},
            {CURLE_GOT_NOTHING, RA_HTTP_ERROR_INVALID_RESPONSE, L"CURLE_GOT_NOTHING"},
            {CURLE_WEIRD_SERVER_REPLY, RA_HTTP_ERROR_INVALID_RESPONSE, L"CURLE_WEIRD_SERVER_REPLY"},

            // the default arm. CURLE_WRITE_ERROR is what the write callback
            // provokes when TextWriter::Write throws.
            {CURLE_WRITE_ERROR, RA_HTTP_ERROR_INTERNAL, L"CURLE_WRITE_ERROR (default)"},
            {CURLE_UNSUPPORTED_PROTOCOL, RA_HTTP_ERROR_INTERNAL, L"CURLE_UNSUPPORTED_PROTOCOL (default)"},
            {CURLE_OUT_OF_MEMORY, RA_HTTP_ERROR_INTERNAL, L"CURLE_OUT_OF_MEMORY (default)"},
        };

        for (const auto& pMapping : vMappings)
            Assert::AreEqual(pMapping.nExpected, detail::MapCurlError(pMapping.nCurlCode), pMapping.sName);

        // the default arm must not be retryable - an unmapped failure that
        // retries forever is worse than one that surfaces
        const LinuxHttpRequester oRequester;
        Assert::IsFalse(oRequester.IsRetryable(detail::MapCurlError(CURLE_WRITE_ERROR)));
    }

    TEST_METHOD(TestClosedPortMapsToCannotConnect)
    {
        // The negative control. A wrong CURLcode map is indistinguishable from a
        // correct one until the network fails, which is exactly when the retry
        // logic matters. Port 1 on loopback refuses immediately and needs no
        // internet connection.
        const ProxyFreeEnvironment oProxyFree;
        const LinuxHttpRequester oRequester;
        Http::Request oRequest("http://127.0.0.1:1/");
        StringTextWriter oWriter;

        const unsigned int nStatus = oRequester.Request(oRequest, oWriter);

        Assert::AreEqual(RA_HTTP_ERROR_CANNOT_CONNECT, nStatus,
                         L"a refused connection must map to ERROR_INTERNET_CANNOT_CONNECT");
        Assert::IsTrue(oRequester.IsRetryable(nStatus), L"and must be retryable");
    }

    TEST_METHOD(TestUnresolvableHostMapsToNameNotResolved)
    {
        const ProxyFreeEnvironment oProxyFree;
        const LinuxHttpRequester oRequester;
        Http::Request oRequest("http://invalid.invalid./");
        StringTextWriter oWriter;

        const unsigned int nStatus = oRequester.Request(oRequest, oWriter);

        Assert::AreEqual(RA_HTTP_ERROR_NAME_NOT_RESOLVED, nStatus);
    }

    TEST_METHOD(TestSuccessfulTransferWithoutHttpStatusIsNotReportedAsNotAttempted)
    {
        // A transfer curl calls successful but that carries no HTTP status line
        // leaves CURLINFO_RESPONSE_CODE at 0 - and 0 is RA_HTTP_NOT_ATTEMPTED,
        // which claims the request never happened, logs as "HTTP error code: 0"
        // with no text at all, and is retryable for that reason rather than on
        // its merits. Report the unusable response instead. file:// is the
        // cheapest way to reach that state without a server; libcurl can be
        // built without it, in which case there is nothing to check here.
        if (!SupportsFileProtocol())
            return;

        const LinuxHttpRequester oRequester;
        Http::Request oRequest("file:///dev/null");
        StringTextWriter oWriter;

        const unsigned int nStatus = oRequester.Request(oRequest, oWriter);

        Assert::AreEqual(RA_HTTP_ERROR_INVALID_RESPONSE, nStatus);
        Assert::IsFalse(nStatus == RA_HTTP_NOT_ATTEMPTED, L"the request was attempted");
        Assert::IsFalse(oRequester.GetStatusCodeText(nStatus).empty(),
                        L"and must carry text the caller can log");
    }

    TEST_METHOD(TestStatusCodeTextForKnownCodes)
    {
        const LinuxHttpRequester oRequester;

        Assert::AreEqual(std::string("Not Found"), oRequester.GetStatusCodeText(404));
        Assert::AreEqual(std::string("Too Many Requests"), oRequester.GetStatusCodeText(429));
        Assert::AreEqual(std::string(""), oRequester.GetStatusCodeText(200));

        // every unmapped CURLcode becomes RA_HTTP_ERROR_INTERNAL, so it is the
        // code most likely to reach a user with no text at all
        Assert::AreEqual(std::string("An internal error has occurred"),
                         oRequester.GetStatusCodeText(RA_HTTP_ERROR_INTERNAL));
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
