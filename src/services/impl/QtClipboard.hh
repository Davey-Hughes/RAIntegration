#ifndef RA_SERVICES_QT_CLIPBOARD_HH
#define RA_SERVICES_QT_CLIPBOARD_HH
#pragma once

#include "services/IClipboard.hh"
#include "services/IQtApplicationHost.hh"

namespace ra {
namespace services {
namespace impl {

class QtClipboard : public ra::services::IClipboard
{
public:
    explicit QtClipboard(ra::services::IQtApplicationHost& pHost) noexcept : m_pHost(pHost) {}

    void SetText(const std::wstring& sValue) const override;
    std::wstring GetText() const override;

private:
    ra::services::IQtApplicationHost& m_pHost;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_QT_CLIPBOARD_HH
