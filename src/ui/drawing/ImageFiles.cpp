#include "ImageFiles.hh"

#include "RA_Defs.h"
#include "RA_md5factory.h"

#include "context/IRcClient.hh"

#include "data/context/GameContext.hh"

#include "services/Http.hh"
#include "services/IConfiguration.hh"
#include "services/IFileSystem.hh"
#include "services/ServiceLocator.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <rcheevos/include/rc_api_runtime.h>

#include <chrono>
#include <cstring>

namespace ra {
namespace ui {
namespace drawing {

namespace {

// how a local badge's name starts (AchievementModel, GameAssets, AssetUploadViewModel, AchievementRuntime)
constexpr const char* LOCAL_BADGE_PREFIX = "local\\";

// The name as it appears in a path: "local\" becomes "local" and this platform's separator. The name itself keeps
// the backslash - it is stored in the user's local achievement files, which Windows and Linux share.
std::wstring PathName(const std::string& sName)
{
    if (ra::util::String::StartsWith(sName, LOCAL_BADGE_PREFIX))
        return L"local" RA_DIR_SEP_L + ra::util::String::Widen(sName.substr(std::strlen(LOCAL_BADGE_PREFIX)));

    return ra::util::String::Widen(sName);
}

void RemoveRedundantFileExtension(std::wstring& sFilename, const std::string& sName)
{
    const auto nIndex = sName.rfind('.');
    if (nIndex == std::string::npos)
        return;

    auto sExtension = sName.substr(nIndex);
    ra::util::String::MakeLowercase(sExtension);

    if (sExtension == ".jpg" || sExtension == ".gif" || sExtension == ".jpeg" || sExtension == ".png")
    {
        // name contains a supported extension, remove ".png" appended by GetFilename
        sFilename.pop_back();
        sFilename.pop_back();
        sFilename.pop_back();
        sFilename.pop_back();
    }
}

} // namespace

std::wstring ImageFiles::GetFilename(ImageType nType, const std::string& sName) const
{
    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    std::wstring sFilename = pFileSystem.BaseDirectory();

    switch (nType)
    {
        case ImageType::Badge:
            sFilename += RA_DIR_BADGE + PathName(sName) + L".png";
            // if sName contains a supported file format extension, remove the ".png"
            RemoveRedundantFileExtension(sFilename, sName);
            break;
        case ImageType::Icon:
            sFilename += RA_DIR_BADGE + std::wstring(L"i") + PathName(sName) + L".png";
            break;
        case ImageType::UserPic:
            sFilename += RA_DIR_USERPIC + PathName(sName) + L".png";
            break;
        case ImageType::Local:
            sFilename += PathName(sName);
            break;
        default:
            Expects(!"Unsupported image type");
            break;
    }

    return sFilename;
}

bool ImageFiles::IsRequested(ImageType nType, const std::string& sName) const
{
    const std::wstring sFilename = GetFilename(nType, sName);

    std::lock_guard<std::mutex> oLock(m_mtxRequested);
    return m_vRequested.find(sFilename) != m_vRequested.end();
}

bool ImageFiles::IsAvailable(ImageType nType, const std::string& sName) const
{
    if (sName.empty())
        return false;

    const std::wstring sFilename = GetFilename(nType, sName);
    {
        std::lock_guard<std::mutex> oLock(m_mtxRequested);
        if (m_vRequested.find(sFilename) != m_vRequested.end())
            return false;
    }

    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    return (pFileSystem.GetFileSize(sFilename) > 0);
}

bool ImageFiles::Read(ImageType nType, const std::string& sName, std::string& sBytes) const
{
    sBytes.clear();
    if (sName.empty())
        return false;

    // Not opened unless it is there: LinuxFileSystem logs every open that fails, and a missing image is asked for on
    // every draw until it arrives. gdi::ImageRepository::GetImage checks the size before it decodes too.
    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    const std::wstring sFilename = GetFilename(nType, sName);
    if (pFileSystem.GetFileSize(sFilename) <= 0)
        return false;

    auto pFile = pFileSystem.OpenTextFile(sFilename);
    if (pFile == nullptr)
        return false;

    const size_t nSize = pFile->GetSize();
    if (nSize == 0)
        return false;

    sBytes.resize(nSize);
    const size_t nRead = pFile->GetBytes(reinterpret_cast<uint8_t*>(sBytes.data()), nSize);
    sBytes.resize(nRead);
    return nRead > 0;
}

void ImageFiles::Fetch(ImageType nType, const std::string& sName, const std::string& sSourceUrl, time_t tLastUpdated)
{
    if (sName.empty())
        return;

    std::wstring sFilename = GetFilename(nType, sName);
    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    if (pFileSystem.GetFileSize(sFilename) > 0)
    {
        if (tLastUpdated == 0 ||
            tLastUpdated < std::chrono::system_clock::to_time_t(pFileSystem.GetLastModified(sFilename)))
            return;
    }

    // check to see if it's already queued
    {
        std::lock_guard<std::mutex> oLock(m_mtxRequested);
        if (m_vRequested.find(sFilename) != m_vRequested.end())
            return;

        m_vRequested.emplace(sFilename);
    }

    const auto& pConfiguration = ra::services::ServiceLocator::Get<ra::services::IConfiguration>();
    if (pConfiguration.IsFeatureEnabled(ra::services::Feature::Offline))
        return;

    // fetch it
    std::string sUrl = sSourceUrl;
    if (sSourceUrl.empty())
    {
        const auto& pRcClient = ra::services::ServiceLocator::Get<ra::context::IRcClient>();
        rc_api_fetch_image_request_t api_params;
        memset(&api_params, 0, sizeof(api_params));
        api_params.image_name = sName.c_str();
        switch (nType)
        {
            case ImageType::Badge:
                api_params.image_type = RC_IMAGE_TYPE_ACHIEVEMENT;
                break;

            case ImageType::UserPic:
                api_params.image_type = RC_IMAGE_TYPE_USER;
                break;

            case ImageType::Icon:
                api_params.image_type = RC_IMAGE_TYPE_GAME;
                break;

            default: // Local: no URL
                break;
        }

        rc_api_request_t api_request;
        if (rc_api_init_fetch_image_request_hosted(&api_request, &api_params, pRcClient.GetHost()) == RC_OK)
            sUrl = api_request.url;

        rc_api_destroy_request(&api_request);
        if (sUrl.empty())
            return;
    }

    RA_LOG_INFO("Downloading %s", sUrl.c_str());

    ra::services::Http::Request request(sUrl);
    request.DownloadAsync(sFilename, [this, sFilename, sUrl, nType, sName](const ra::services::Http::Response& response)
    {
        if (response.StatusCode() == ra::services::Http::StatusCode::OK)
        {
            auto nFileSize = ra::services::ServiceLocator::Get<ra::services::IFileSystem>().GetFileSize(sFilename);
            RA_LOG_INFO("Wrote %lu bytes to %s", nFileSize, ra::util::String::Narrow(sFilename).c_str());

            // only remove the image from the request queue if successful. prevents repeated requests
            {
                std::lock_guard<std::mutex> oLock(m_mtxRequested);
                m_vRequested.erase(sFilename);
            }
        }
        else
        {
            RA_LOG_WARN("Error %u fetching %s", response.StatusCode(), sUrl.c_str());
            ra::services::ServiceLocator::Get<ra::services::IFileSystem>().DeleteFile(sFilename);
        }

        if (m_fDownloaded)
            m_fDownloaded(nType, sName);
    });
}

std::string ImageFiles::Store(ImageType nType, const std::wstring& sPath)
{
    auto sFileMD5 = RAGenerateFileMD5(sPath);
    if (sFileMD5.empty())
        return "";

    const auto& pFileSystem = ra::services::ServiceLocator::Get<ra::services::IFileSystem>();
    const auto sDirectory = pFileSystem.BaseDirectory() + RA_DIR_BADGE + L"local" RA_DIR_SEP_L;
    if (!pFileSystem.DirectoryExists(sDirectory))
        pFileSystem.CreateDirectory(sDirectory);

    auto sExtension = pFileSystem.GetExtension(sPath);
    ra::util::String::MakeLowercase(sExtension);

    const auto& pGameContext = ra::services::ServiceLocator::Get<ra::data::context::GameContext>();
    sFileMD5 = ra::util::String::Printf("local\\%u-%s.%s", pGameContext.GameId(), sFileMD5, sExtension);

    std::wstring sFilename = GetFilename(nType, sFileMD5);

    if (pFileSystem.GetFileSize(sFilename) < 0)
    {
        if (!pFileSystem.CopyFile(sPath, sFilename))
            return "";
    }

    return sFileMD5;
}

} // namespace drawing
} // namespace ui
} // namespace ra
