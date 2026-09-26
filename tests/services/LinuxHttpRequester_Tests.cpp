#ifndef _WIN32

#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/StringTextWriter.hh"

#include "services/HttpErrorCodes.hh"

#include "tests/RA_UnitTestHelpers.h"

#include <curl/curl.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <tuple>
#include <unistd.h>
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

// A file with content, for a file:// transfer that has to reach the write
// callback: file:///dev/null completes without ever calling it.
class ScopedTempFile
{
public:
    explicit ScopedTempFile(const std::string& sContent)
    {
        const std::string sTemplate =
            std::filesystem::absolute(std::filesystem::temp_directory_path() / "ra-http-XXXXXX").string();
        std::vector<char> vBuffer(sTemplate.begin(), sTemplate.end());
        vBuffer.push_back('\0');

        const int nFile = ::mkstemp(vBuffer.data());
        Assert::IsTrue(nFile != -1, L"mkstemp failed");
        m_sPath = std::string(vBuffer.data());

        const bool bWritten =
            ::write(nFile, sContent.data(), sContent.length()) == static_cast<ssize_t>(sContent.length());
        ::close(nFile);

        // the destructor does not run if the constructor throws
        if (!bWritten)
            ::unlink(m_sPath.c_str());
        Assert::IsTrue(bWritten, L"write failed");
    }

    ~ScopedTempFile() noexcept { ::unlink(m_sPath.c_str()); }

    ScopedTempFile(const ScopedTempFile&) = delete;
    ScopedTempFile& operator=(const ScopedTempFile&) = delete;
    ScopedTempFile(ScopedTempFile&&) = delete;
    ScopedTempFile& operator=(ScopedTempFile&&) = delete;

    std::string GetUrl() const { return "file://" + m_sPath; }

private:
    std::string m_sPath;
};

// Installs a recognisable SIGPIPE handler for the life of the object, and puts
// back whatever was there before - including when an Assert throws. The
// handler never runs; it only has to be told apart from anything libcurl
// would install.
class ScopedSigPipeSentinel
{
public:
    ScopedSigPipeSentinel() noexcept
    {
        struct sigaction oAction {};
        oAction.sa_handler = Handler;
        sigemptyset(&oAction.sa_mask);
        ::sigaction(SIGPIPE, &oAction, &m_oPrevious);
    }

    ~ScopedSigPipeSentinel() noexcept { ::sigaction(SIGPIPE, &m_oPrevious, nullptr); }

    ScopedSigPipeSentinel(const ScopedSigPipeSentinel&) = delete;
    ScopedSigPipeSentinel& operator=(const ScopedSigPipeSentinel&) = delete;
    ScopedSigPipeSentinel(ScopedSigPipeSentinel&&) = delete;
    ScopedSigPipeSentinel& operator=(ScopedSigPipeSentinel&&) = delete;

    static bool IsInForce() noexcept
    {
        struct sigaction oAction {};
        ::sigaction(SIGPIPE, nullptr, &oAction);
        return (oAction.sa_flags & SA_SIGINFO) == 0 && oAction.sa_handler == Handler;
    }

private:
    static void Handler(int) noexcept {}

    struct sigaction m_oPrevious {};
};

// Samples SIGPIPE's disposition each time curl hands it data. That is from
// inside curl_easy_perform, the only window in which a change libcurl makes
// is visible: it restores the disposition before returning.
class SigPipeProbeWriter : public TextWriter
{
public:
    void Write(const std::string&) override { Sample(); }
    void Write(const std::wstring&) override { Sample(); }
    void WriteLine() override { Sample(); }
    std::streampos GetPosition() const noexcept override { return 0; }
    void SetPosition(std::streampos) noexcept override {}

    void Sample() noexcept
    {
        ++m_nSamples;
        if (ScopedSigPipeSentinel::IsInForce())
            ++m_nSentinelSamples;
    }

    unsigned int GetSampleCount() const noexcept { return m_nSamples; }
    unsigned int GetSentinelSampleCount() const noexcept { return m_nSentinelSamples; }

private:
    unsigned int m_nSamples = 0;
    unsigned int m_nSentinelSamples = 0;
};

static size_t SampleSigPipe(char*, size_t nSize, size_t nCount, void* pUserData) noexcept
{
    static_cast<SigPipeProbeWriter*>(pUserData)->Sample();
    return nSize * nCount;
}

// The positive control for TestTransferLeavesSigPipeAlone: whether this
// libcurl, left to its defaults, changes SIGPIPE's disposition during a
// transfer at all. libcurl 8.22 does wherever it has sigaction and no
// SO_NOSIGPIPE (lib/sigpipe.h), which includes Linux; one that did not would
// let a requester without CURLOPT_NOSIGNAL pass that test.
static bool CurlDefaultsChangeSigPipe(const std::string& sUrl)
{
    CURL* pCurl = curl_easy_init();
    if (pCurl == nullptr)
        return false;

    SigPipeProbeWriter oProbe;
    curl_easy_setopt(pCurl, CURLOPT_URL, sUrl.c_str());
    curl_easy_setopt(pCurl, CURLOPT_WRITEFUNCTION, SampleSigPipe);
    curl_easy_setopt(pCurl, CURLOPT_WRITEDATA, &oProbe);

    const CURLcode nResult = curl_easy_perform(pCurl);
    curl_easy_cleanup(pCurl);

    return nResult == CURLE_OK && oProbe.GetSampleCount() > 0 && oProbe.GetSentinelSampleCount() == 0;
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
        // its merits. Whatever replaces it must not be retryable either:
        // sending the request again cannot produce a status line, and a
        // retryable code is re-sent with no attempt limit. file:// is the
        // cheapest way to reach that state without a server; libcurl can be
        // built without it, in which case there is nothing to check here.
        if (!SupportsFileProtocol())
            return;

        const LinuxHttpRequester oRequester;
        Http::Request oRequest("file:///dev/null");
        StringTextWriter oWriter;

        const unsigned int nStatus = oRequester.Request(oRequest, oWriter);

        Assert::IsFalse(nStatus == RA_HTTP_NOT_ATTEMPTED, L"the request was attempted");
        Assert::IsFalse(oRequester.IsRetryable(nStatus), L"and must not be retried");
        Assert::AreEqual(RA_HTTP_ERROR_INTERNAL, nStatus);
        Assert::IsFalse(oRequester.GetStatusCodeText(nStatus).empty(),
                        L"and must carry text the caller can log");
    }

    TEST_METHOD(TestTransferLeavesSigPipeAlone)
    {
        // The requester runs on thread pool threads inside the emulator's
        // process. A handle without CURLOPT_NOSIGNAL sets SIGPIPE to SIG_IGN
        // process-wide for the length of its transfer, so overlapping transfers
        // can leave the emulator's own disposition replaced. The content writer
        // is called from inside the transfer, which makes it the one place a
        // test can see what disposition libcurl has in force.
        if (!SupportsFileProtocol())
            return;

        // constructed first: it is what runs curl_global_init, which has to
        // precede the control's raw handle
        const LinuxHttpRequester oRequester;
        const ScopedTempFile oFile("content");
        const ScopedSigPipeSentinel oSentinel;

        // a libcurl that never touches SIGPIPE cannot tell a requester that
        // sets CURLOPT_NOSIGNAL from one that does not
        if (!CurlDefaultsChangeSigPipe(oFile.GetUrl()))
            return;

        Http::Request oRequest(oFile.GetUrl());
        SigPipeProbeWriter oWriter;
        oRequester.Request(oRequest, oWriter);

        Assert::IsTrue(oWriter.GetSampleCount() > 0, L"the transfer must reach the writer");
        Assert::AreEqual(oWriter.GetSampleCount(), oWriter.GetSentinelSampleCount(),
                         L"SIGPIPE's disposition must be the process's own throughout the transfer");
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
