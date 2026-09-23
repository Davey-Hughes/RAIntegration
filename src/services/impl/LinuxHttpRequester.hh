#ifndef RA_SERVICES_LINUX_HTTPREQUESTER_HH
#define RA_SERVICES_LINUX_HTTPREQUESTER_HH
#pragma once

#ifndef _WIN32

#include "services/IHttpRequester.hh"

#include <string>

namespace ra {
namespace services {
namespace impl {

namespace detail {

/// <summary>
/// Maps a libcurl result to the shared RA_HTTP_ERROR_* numbering.
/// </summary>
/// <param name="nCurlCode">A <c>CURLcode</c>. Declared as <c>int</c> so this
/// header does not drag &lt;curl/curl.h&gt; into everything that constructs the
/// requester.</param>
/// <remarks>Exposed only so the mapping can be covered without a socket. A test
/// that has to provoke a real network failure can reach two of the nine
/// CURLcodes it names; the rest would otherwise never be exercised.</remarks>
unsigned int MapCurlError(int nCurlCode) noexcept;

} // namespace detail

class LinuxHttpRequester : public IHttpRequester
{
public:
    LinuxHttpRequester();

    void SetUserAgent(const std::string& sUserAgent) override { m_sUserAgent = sUserAgent; }

    unsigned int Request(const Http::Request& pRequest, TextWriter& pContentWriter) const override;

    bool IsRetryable(unsigned int nStatusCode) const noexcept override;

    std::string GetStatusCodeText(unsigned int nStatusCode) const override;

private:
    std::string m_sUserAgent;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !_WIN32

#endif // !RA_SERVICES_LINUX_HTTPREQUESTER_HH
