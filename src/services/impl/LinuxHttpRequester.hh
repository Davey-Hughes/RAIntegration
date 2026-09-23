#ifndef RA_SERVICES_LINUX_HTTPREQUESTER_HH
#define RA_SERVICES_LINUX_HTTPREQUESTER_HH
#pragma once

#include "services/IHttpRequester.hh"

#include <string>

namespace ra {
namespace services {
namespace impl {

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

#endif // !RA_SERVICES_LINUX_HTTPREQUESTER_HH
