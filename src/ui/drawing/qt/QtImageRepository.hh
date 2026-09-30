#ifndef RA_UI_DRAWING_QT_QTIMAGEREPOSITORY_HH
#define RA_UI_DRAWING_QT_QTIMAGEREPOSITORY_HH
#pragma once

#include "ui/IImageRepository.hh"
#include "ui/drawing/ImageFiles.hh"

#include <QImage>

#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

/// <summary>
/// The Linux IImageRepository: gdi::ImageRepository's behaviour, with the files kept by ImageFiles and the images
/// decoded by Qt. Any thread; images cross threads only as QImage copies taken under the lock, and a decoded image is
/// never written again, so its shared pixels are only ever read.
/// </summary>
class QtImageRepository : public IImageRepository
{
public:
    QtImageRepository();
    ~QtImageRepository() noexcept override = default;
    QtImageRepository(const QtImageRepository&) noexcept = delete;
    QtImageRepository& operator=(const QtImageRepository&) noexcept = delete;
    QtImageRepository(QtImageRepository&&) noexcept = delete;
    QtImageRepository& operator=(QtImageRepository&&) noexcept = delete;

    /// <summary>
    /// Fetches the default images (the badge "00000" and the user pic "_User"), as gdi::ImageRepository::Initialize
    /// does.
    /// </summary>
    void Initialize();

    bool IsImageAvailable(ImageType nType, const std::string& sName) const override;
    void FetchImage(ImageType nType, const std::string& sName, const std::string& sSourceUrl,
                    time_t tLastUpdated = 0) override;
    std::string StoreImage(ImageType nType, const std::wstring& sPath) override;
    std::wstring GetFilename(ImageType nType, const std::string& sName) const override;

    void AddReference(const ImageReference& pImage) override;
    void ReleaseReference(ImageReference& pImage) noexcept override;
    bool HasReferencedImageChanged(ImageReference& pImage) const override;

    /// <summary>
    /// The image to draw for the reference, premultiplied ARGB32 at the file's own size: decoded on first use, when
    /// it also takes a reference (ImageReference releases it). While the file is missing it asks for it, and gives
    /// the type's default (the badge "00000" for a badge or an icon, "_User" for a user pic); an empty image when
    /// there is neither. gdi::ImageRepository::GetHBitmap's counterpart.
    /// </summary>
    QImage GetImage(const ImageReference& pImage) const;

    /// <summary>How many times a file has been decoded, whether it worked or not.</summary>
    unsigned int GetDecodeCount() const noexcept { return m_nDecodes.load(); }

private:
    struct Entry
    {
        QImage oImage;
        unsigned int nReferences = 0;
        unsigned long long nId = 0; // what a reference holding this image keeps as its data; never 0
    };
    using EntryMap = std::unordered_map<std::string, Entry>;

    EntryMap* GetMap(ImageType nType) const noexcept;

    // The image for (nType, sName): cached, or decoded now. With pReference, the reference takes a reference to it.
    QImage Resolve(ImageType nType, const std::string& sName, const ImageReference* pReference) const;

    // With pReference, the reference takes a reference to the entry - once: drawing the same reference every frame
    // does not count it again. Called under m_mtxImages.
    void TakeReference(Entry& pEntry, const ImageReference* pReference) const;
    QImage GetDefaultImage(ImageType nType) const;

    // A download finished (a pool worker): a file that would not decode may be good now.
    void OnDownloaded(ImageType nType, const std::string& sName);

    // mutable: GetImage, a const lookup, asks for a missing file, as gdi::ImageRepository::GetHBitmap does
    mutable ImageFiles m_oFiles;

    mutable std::mutex m_mtxImages;
    mutable EntryMap m_mBadges;
    mutable EntryMap m_mUserPics;
    mutable EntryMap m_mLocal;
    mutable EntryMap m_mIcons;
    mutable std::set<std::pair<ImageType, std::string>> m_vUndecodable;
    mutable unsigned long long m_nLastId = 0;

    std::atomic<unsigned int> m_nDownloads{0}; // finished downloads; raised under m_mtxImages
    mutable std::atomic<unsigned int> m_nDecodes{0};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_QTIMAGEREPOSITORY_HH
