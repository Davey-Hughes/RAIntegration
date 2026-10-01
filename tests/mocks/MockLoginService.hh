#ifndef RA_SERVICES_MOCK_LOGINSERVICE_HH
#define RA_SERVICES_MOCK_LOGINSERVICE_HH
#pragma once

#include "services/ILoginService.hh"

#include "context/UserContext.hh"

#include "services/ServiceLocator.hh"

namespace ra {
namespace services {
namespace mocks {

class MockLoginService : public ILoginService
{
public:
    MockLoginService() noexcept
        : m_Override(this)
    {
    }

    bool IsLoggedIn() const noexcept override { return m_bIsLoggedIn; }

    void SetLoggedIn(bool bIsLoggedIn) noexcept { m_bIsLoggedIn = bIsLoggedIn; }

    /// <summary>
    /// Makes Login() fail with the given reason; an empty reason is an abandoned login (shutdown began).
    /// </summary>
    void MockLoginFailure(bool bIsFailure, const std::wstring& sErrorMessage = L"")
    {
        m_bFailLogin = bIsFailure;
        m_sFailureMessage = sErrorMessage;
    }

    bool Login(const std::string& sUsername, const std::string&, std::wstring& sErrorMessage) override
    {
        if (m_bFailLogin)
        {
            sErrorMessage = m_sFailureMessage;
            return false;
        }

        sErrorMessage.clear();

        if (ra::services::ServiceLocator::Exists<ra::context::UserContext>())
            ra::services::ServiceLocator::GetMutable<ra::context::UserContext>().Initialize(sUsername, sUsername + "_", "APITOKEN");

        m_bIsLoggedIn = true;
        return true;
    }

    /// <summary>
    /// For tests that only want a logged-in user and have no use for the failure reason.
    /// </summary>
    bool Login(const std::string& sUsername, const std::string& sPassword)
    {
        std::wstring sErrorMessage;
        return Login(sUsername, sPassword, sErrorMessage);
    }

    void Logout() override
    {
        if (ra::services::ServiceLocator::Exists<ra::context::UserContext>())
            ra::services::ServiceLocator::GetMutable<ra::context::UserContext>().Initialize("", "", "");

        m_bIsLoggedIn = false;
    }

private:
    ra::services::ServiceLocator::ServiceOverride<ra::services::ILoginService> m_Override;

    bool m_bIsLoggedIn = false;
    bool m_bFailLogin = false;
    std::wstring m_sFailureMessage;
};

} // namespace mocks
} // namespace service
} // namespace ra

#endif // !RA_SERVICES_MOCK_LOGINSERVICE_HH
