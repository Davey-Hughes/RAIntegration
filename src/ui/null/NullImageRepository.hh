#ifndef RA_UI_NULL_IMAGEREPOSITORY_HH
#define RA_UI_NULL_IMAGEREPOSITORY_HH
#pragma once

#include "ui/IImageRepository.hh"

namespace ra {
namespace ui {
namespace null {

// Reports every image as unavailable and holds no references. Badge downloads
// still happen through ILocalStorage; this only declines to decode them.
class NullImageRepository : public ra::ui::IImageRepository
{
public:
    bool IsImageAvailable(ImageType, const std::string&) const override { return false; }
    void FetchImage(ImageType, const std::string&, const std::string&, time_t) override {}
    std::string StoreImage(ImageType, const std::wstring&) override { return std::string(); }
    std::wstring GetFilename(ImageType, const std::string&) const override { return std::wstring(); }
    void AddReference(const ImageReference&) override {}
    void ReleaseReference(ImageReference&) noexcept(false) override {}
    bool HasReferencedImageChanged(ImageReference&) const override { return false; }
};

} // namespace null
} // namespace ui
} // namespace ra

#endif // !RA_UI_NULL_IMAGEREPOSITORY_HH
