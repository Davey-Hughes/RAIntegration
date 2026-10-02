#ifndef RA_UI_MOCK_IMAGEREPOSITORY_HH
#define RA_UI_MOCK_IMAGEREPOSITORY_HH
#pragma once

#include "services/ServiceLocator.hh"

#include "ui/IImageRepository.hh"

#include "util/Strings.hh"

#include <functional>
#include <map>
#include <set>
#include <string>

namespace ra {
namespace ui {
namespace mocks {

class MockImageRepository : public IImageRepository
{
public:
    MockImageRepository() noexcept : m_Override(this)
    {

    }

    bool IsImageAvailable(ImageType nType, const std::string& sName) const override
    {
        const auto vAvailableImages = m_vAvailableImages.find(nType);
        if (vAvailableImages == m_vAvailableImages.end())
            return false;

        return (vAvailableImages->second.find(sName) != vAvailableImages->second.end());
    }

    void SetImageAvailable(ImageType nType, const std::string& sName)
    {
        auto pIter = m_vAvailableImages.find(nType);
        if (pIter == m_vAvailableImages.end())
        {
            m_vAvailableImages.emplace(nType, std::set<std::string>());
            pIter = m_vAvailableImages.find(nType);
        }

        pIter->second.insert(sName);
    }

    void FetchImage(ImageType nType, const std::string& sName, _UNUSED const std::string& sSourceUrl, _UNUSED time_t tLastModified = 0) override
    {
        if (m_fFetchImageHandler)
            m_fFetchImageHandler(nType, sName);
    }

    /// <summary>
    /// Sets a function to call whenever an image is fetched.
    /// </summary>
    void SetFetchImageHandler(std::function<void(ImageType, const std::string&)>&& fHandler) noexcept
    {
        m_fFetchImageHandler = std::move(fHandler);
    }

    std::string StoreImage(_UNUSED ImageType nType, _UNUSED const std::wstring& sPath) override
    {
        return "REPO:" + ra::util::String::Narrow(sPath);
    }

    std::wstring GetFilename(_UNUSED ImageType nType, const std::string& sName) const override
    {
        return L"RACache\\Badges\\" + ra::util::String::Widen(sName);
    }

    void AddReference(_UNUSED const ImageReference& pImage) noexcept override
    {

    }

    void ReleaseReference(_UNUSED ImageReference& pImage) noexcept override
    {

    }

    bool HasReferencedImageChanged(_UNUSED ImageReference& pImage) const noexcept override
    {
        return false;
    }

private:
    std::map<ImageType, std::set<std::string>> m_vAvailableImages;
    std::function<void(ImageType, const std::string&)> m_fFetchImageHandler;

    ra::services::ServiceLocator::ServiceOverride<ra::ui::IImageRepository> m_Override;
};

} // namespace mocks
} // namespace ui
} // namespace ra

#endif // !RA_SERVICES_UI_IMAGEREPOSITORY_HH
