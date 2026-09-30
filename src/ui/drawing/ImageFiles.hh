#ifndef RA_UI_DRAWING_IMAGEFILES_HH
#define RA_UI_DRAWING_IMAGEFILES_HH
#pragma once

#include "ui/ImageReference.hh"

#include <ctime>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace ra {
namespace ui {
namespace drawing {

/// <summary>
/// The files behind badges, avatars, game icons and local images: where each one lives under the RACache, whether it
/// is on disk, downloading it, and storing a local badge. The file half of gdi::ImageRepository, copied so that
/// Windows compiles nothing new; no decoding, no Qt and no platform code. Any thread.
/// </summary>
class ImageFiles
{
public:
    /// <summary>
    /// Called on the pool worker that finished a download, whether it worked or not.
    /// </summary>
    using DownloadedCallback = std::function<void(ImageType nType, const std::string& sName)>;

    explicit ImageFiles(DownloadedCallback&& fDownloaded) noexcept : m_fDownloaded(std::move(fDownloaded)) {}

    /// <summary>
    /// The full path of the file for the image. A local badge keeps "local\" in its name, which is how it is stored
    /// in the user's files; only the path uses this platform's separator.
    /// </summary>
    std::wstring GetFilename(ImageType nType, const std::string& sName) const;

    /// <summary>
    /// True while the image is requested: a download is on its way, failed this session, or could not start.
    /// </summary>
    bool IsRequested(ImageType nType, const std::string& sName) const;

    /// <summary>
    /// True when the file is on disk, not empty, and not requested: gdi::ImageRepository::IsImageAvailable's file
    /// half.
    /// </summary>
    bool IsAvailable(ImageType nType, const std::string& sName) const;

    /// <summary>
    /// Reads the whole file into sBytes. False if it is missing or empty.
    /// </summary>
    bool Read(ImageType nType, const std::string& sName, std::string& sBytes) const;

    /// <summary>
    /// Ensures the image will be on disk: downloads it unless it is there already (and not older than
    /// tLastUpdated), already requested, or offline. An empty sSourceUrl builds the URL from the type and name.
    /// </summary>
    void Fetch(ImageType nType, const std::string& sName, const std::string& sSourceUrl, time_t tLastUpdated);

    /// <summary>
    /// Copies a file into the local badges as "local\&lt;game id&gt;-&lt;md5&gt;.&lt;extension&gt;".
    /// </summary>
    /// <returns>The new badge's name, or an empty string if the file could not be read or copied.</returns>
    std::string Store(ImageType nType, const std::wstring& sPath);

private:
    DownloadedCallback m_fDownloaded;

    mutable std::mutex m_mtxRequested;
    std::set<std::wstring> m_vRequested; // by file name, as gdi::ImageRepository keeps them
};

} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_IMAGEFILES_HH
