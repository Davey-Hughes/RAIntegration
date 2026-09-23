#ifndef _WIN32

#include "services/impl/LinuxHttpRequester.hh"
#include "services/impl/StringTextWriter.hh"

#include "services/HttpErrorCodes.hh"

#include "tests/RA_UnitTestHelpers.h"

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace services {
namespace impl {
namespace tests {

TEST_CLASS(LinuxHttpRequester_Tests)
{
public:
    TEST_METHOD(TestRetryTableMatchesTheWindowsOne)
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

    TEST_METHOD(TestClosedPortMapsToCannotConnect)
    {
        // The negative control. A wrong CURLcode map is indistinguishable from a
        // correct one until the network fails, which is exactly when the retry
        // logic matters. Port 1 on loopback refuses immediately and needs no
        // internet connection.
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
        const LinuxHttpRequester oRequester;
        Http::Request oRequest("http://invalid.invalid./");
        StringTextWriter oWriter;

        const unsigned int nStatus = oRequester.Request(oRequest, oWriter);

        Assert::AreEqual(RA_HTTP_ERROR_NAME_NOT_RESOLVED, nStatus);
    }

    TEST_METHOD(TestStatusCodeTextForKnownCodes)
    {
        const LinuxHttpRequester oRequester;

        Assert::AreEqual(std::string("Not Found"), oRequester.GetStatusCodeText(404));
        Assert::AreEqual(std::string("Too Many Requests"), oRequester.GetStatusCodeText(429));
        Assert::AreEqual(std::string(""), oRequester.GetStatusCodeText(200));
    }
};

} // namespace tests
} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32
