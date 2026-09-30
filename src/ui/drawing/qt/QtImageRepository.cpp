#include "QtImageRepository.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

namespace {

inline constexpr const char* DefaultBadge{"00000"};
inline constexpr const char* DefaultUserPic{"_User"};

} // namespace

QtImageRepository::QtImageRepository()
    : m_oFiles([this](ImageType nType, const std::string& sName) { OnDownloaded(nType, sName); })
{
}

void QtImageRepository::Initialize()
{
    // pre-fetch the default images
    FetchImage(ImageType::Badge, DefaultBadge, "");
    FetchImage(ImageType::UserPic, DefaultUserPic, "");
}

QtImageRepository::EntryMap* QtImageRepository::GetMap(ImageType nType) const noexcept
{
    switch (nType)
    {
        case ImageType::Badge:
            return &m_mBadges;
        case ImageType::UserPic:
            return &m_mUserPics;
        case ImageType::Local:
            return &m_mLocal;
        case ImageType::Icon:
            return &m_mIcons;
        default:
            return nullptr;
    }
}

bool QtImageRepository::IsImageAvailable(ImageType nType, const std::string& sName) const
{
    if (sName.empty())
        return false;

    const auto* pMap = GetMap(nType);
    if (pMap != nullptr)
    {
        std::lock_guard<std::mutex> oLock(m_mtxImages);
        if (pMap->find(sName) != pMap->end())
            return true;
    }

    return m_oFiles.IsAvailable(nType, sName);
}

void QtImageRepository::FetchImage(ImageType nType, const std::string& sName, const std::string& sSourceUrl,
                                   time_t tLastUpdated)
{
    m_oFiles.Fetch(nType, sName, sSourceUrl, tLastUpdated);
}

std::string QtImageRepository::StoreImage(ImageType nType, const std::wstring& sPath)
{
    return m_oFiles.Store(nType, sPath);
}

std::wstring QtImageRepository::GetFilename(ImageType nType, const std::string& sName) const
{
    return m_oFiles.GetFilename(nType, sName);
}

void QtImageRepository::TakeReference(Entry& pEntry, const ImageReference* pReference) const
{
    if (pReference != nullptr && pReference->GetData() != pEntry.nId)
    {
        ++pEntry.nReferences;
        pReference->SetData(pEntry.nId);
    }
}

QImage QtImageRepository::Resolve(ImageType nType, const std::string& sName, const ImageReference* pReference) const
{
    if (sName.empty())
        return QImage();

    auto* pMap = GetMap(nType);
    if (pMap == nullptr)
        return QImage();

    {
        std::lock_guard<std::mutex> oLock(m_mtxImages);
        auto pIter = pMap->find(sName);
        if (pIter != pMap->end())
        {
            TakeReference(pIter->second, pReference);
            return pIter->second.oImage;
        }

        if (m_vUndecodable.find({nType, sName}) != m_vUndecodable.end())
            return QImage();
    }

    // a download that finishes from here on may have replaced the file this decodes (OnDownloaded)
    const unsigned int nDownloadsBefore = m_nDownloads.load();

    std::string sBytes;
    if (!m_oFiles.Read(nType, sName, sBytes))
    {
        m_oFiles.Fetch(nType, sName, "", 0);
        return QImage();
    }

    // decoded outside the lock: another thread may decode the same file meanwhile, and the first to insert wins
    ++m_nDecodes;
    QImage oImage;
    if (!oImage.loadFromData(reinterpret_cast<const uchar*>(sBytes.data()), static_cast<int>(sBytes.size())) ||
        oImage.isNull())
    {
        // Remembered for the session, so a bad file is not decoded again on every draw - unless a download finished
        // meanwhile: what was read may be the old or a half-written file, and OnDownloaded has already forgotten this
        // name, so remembering it now would hide the new file for the rest of the session.
        std::lock_guard<std::mutex> oLock(m_mtxImages);
        if (m_nDownloads.load() == nDownloadsBefore && m_vUndecodable.insert({nType, sName}).second)
            RA_LOG_WARN("Could not decode %s", ra::util::String::Narrow(m_oFiles.GetFilename(nType, sName)).c_str());
        return QImage();
    }
    oImage = oImage.convertToFormat(QImage::Format_ARGB32_Premultiplied);

    std::lock_guard<std::mutex> oLock(m_mtxImages);
    auto pIter = pMap->find(sName);
    if (pIter == pMap->end())
    {
        Entry oEntry;
        oEntry.oImage = std::move(oImage);
        oEntry.nId = ++m_nLastId;
        pIter = pMap->emplace(sName, std::move(oEntry)).first;
    }

    TakeReference(pIter->second, pReference);

    return pIter->second.oImage;
}

QImage QtImageRepository::GetDefaultImage(ImageType nType) const
{
    switch (nType)
    {
        case ImageType::Badge:
        case ImageType::Icon:
            return Resolve(ImageType::Badge, DefaultBadge, nullptr);

        case ImageType::UserPic:
            return Resolve(ImageType::UserPic, DefaultUserPic, nullptr);

        default:
            return QImage();
    }
}

QImage QtImageRepository::GetImage(const ImageReference& pImage) const
{
    if (pImage.Type() == ImageType::None)
        return QImage();

    QImage oImage = Resolve(pImage.Type(), pImage.Name(), &pImage);
    if (oImage.isNull())
        return GetDefaultImage(pImage.Type());

    return oImage;
}

void QtImageRepository::AddReference(const ImageReference& pImage)
{
    auto* pMap = GetMap(pImage.Type());
    if (pMap == nullptr || pImage.Name().empty())
        return;

    std::lock_guard<std::mutex> oLock(m_mtxImages);
    const auto pIter = pMap->find(pImage.Name());
    if (pIter != pMap->end())
        ++pIter->second.nReferences;
}

void QtImageRepository::ReleaseReference(ImageReference& pImage) noexcept
{
    // if data isn't set, we don't have a reference to release.
    if (pImage.GetData() == 0)
        return;

    auto* pMap = GetMap(pImage.Type());
    if (pMap != nullptr)
    {
        std::lock_guard<std::mutex> oLock(m_mtxImages);
        const auto pIter = pMap->find(pImage.Name());
        if (pIter != pMap->end() && pIter->second.nId == pImage.GetData())
        {
            // Defensive only: each holder of this id counted itself in (TakeReference), and the entry goes at 0. A
            // copied ImageReference carries the id without a count; the id check above and the erase below keep that
            // to a re-decode at worst.
            if (pIter->second.nReferences > 0)
                --pIter->second.nReferences;

            if (pIter->second.nReferences == 0)
                pMap->erase(pIter);
        }
    }

    pImage.SetData(0);
}

bool QtImageRepository::HasReferencedImageChanged(ImageReference& pImage) const
{
    if (pImage.Type() == ImageType::None)
        return false;

    const auto nBefore = pImage.GetData();
    GetImage(pImage);
    return (pImage.GetData() != nBefore);
}

void QtImageRepository::OnDownloaded(ImageType nType, const std::string& sName)
{
    {
        std::lock_guard<std::mutex> oLock(m_mtxImages);
        ++m_nDownloads;
        m_vUndecodable.erase({nType, sName});
    }

    OnImageChanged(nType, sName);
}

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra
