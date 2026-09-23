#ifndef RA_SERVICES_QT_CLIPBOARD_HH
#define RA_SERVICES_QT_CLIPBOARD_HH
#pragma once

#include "services/IClipboard.hh"

namespace ra {
namespace services {
namespace impl {

class QtClipboard : public ra::services::IClipboard
{
public:
    void SetText(const std::wstring& sValue) const override;
    std::wstring GetText() const override;
};

} // namespace impl
} // namespace services
} // namespace ra

#endif // !RA_SERVICES_QT_CLIPBOARD_HH
