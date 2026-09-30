#ifndef RA_UI_DRAWING_QT_QTSURFACE_HH
#define RA_UI_DRAWING_QT_QTSURFACE_HH
#pragma once

#include "ui/drawing/ISurface.hh"

#include <QFont>
#include <QImage>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

/// <summary>
/// The fonts QtSurfaces draw with, shared by every surface of one factory: a font id from LoadFont on one surface
/// is good on the others, as GDI's ResourceRepository makes it on Windows (PopupMessageViewModel measures on one
/// surface and writes with the same id on another). Any thread.
/// </summary>
class QtFontRegistry
{
public:
    /// <summary>
    /// The id (1 or more) for the font; the same request gives the same id. 0 when fonts cannot be made: Qt needs
    /// a running QGuiApplication for them.
    /// </summary>
    /// <param name="nCellHeight">The height GDI's CreateFont takes when positive: ascent plus descent, in logical
    /// pixels, not the em size.</param>
    int Load(const std::string& sFont, int nCellHeight, FontStyles nStyle);

    /// <summary>The font for the id, and its cell height; false for an id Load never gave.</summary>
    bool Get(int nFont, QFont& oFont, int& nCellHeight) const;

private:
    struct Entry
    {
        std::string sFont;
        int nCellHeight = 0;
        FontStyles nStyle = FontStyles::Normal;
        QFont oFont;
    };

    mutable std::mutex m_mtxFonts;
    std::vector<Entry> m_vFonts;
};

/// <summary>
/// An ISurface backed by a premultiplied ARGB32 QImage and drawn with QPainter: the Linux counterpart of
/// GDIBitmapSurface. It lays out in logical pixels and paints at device resolution: the image holds the logical
/// size times the scale, with that device pixel ratio, so every draw is scaled and text is drawn at full resolution.
/// </summary>
class QtSurface : public ISurface
{
public:
    /// <param name="nWidth">The logical width, what GetWidth returns.</param>
    /// <param name="nHeight">The logical height, what GetHeight returns.</param>
    /// <param name="nDeviceWidth">The image's width in device pixels.</param>
    /// <param name="nDeviceHeight">The image's height in device pixels.</param>
    /// <param name="fScale">Device pixels per logical pixel.</param>
    QtSurface(int nWidth, int nHeight, int nDeviceWidth, int nDeviceHeight, double fScale,
              std::shared_ptr<QtFontRegistry> pFonts);

    unsigned int GetWidth() const noexcept override { return m_nWidth; }
    unsigned int GetHeight() const noexcept override { return m_nHeight; }

    /// <summary>Writes the colour, alpha included, as GDIBitmapSurface does: it replaces, it does not blend.</summary>
    void FillRectangle(int nX, int nY, int nWidth, int nHeight, Color nColor) override;
    int LoadFont(const std::string& sFont, int nFontSize, FontStyles nStyle) override;
    ra::ui::Size MeasureText(int nFont, const std::wstring& sText) const override;
    void WriteText(int nX, int nY, int nFont, Color nColor, const std::wstring& sText) override;

    // Badges and avatars arrive in O1b: until then there is no image to draw.
    void DrawImage(int, int, int, int, const ImageReference&) noexcept override {}
    void DrawImageStretched(int, int, int, int, const ImageReference&) noexcept override {}

    void DrawSurface(int nX, int nY, const ISurface& pSurface) override;
    void DrawSurface(int nX, int nY, const ISurface& pSurface, int nSurfaceX, int nSurfaceY, int nWidth,
                     int nHeight) override;

    /// <summary>Scales every pixel's alpha (and so its premultiplied colour) by fAlpha, 0 to 1.</summary>
    void SetOpacity(double fAlpha) override;

    /// <summary>Writes straight (not premultiplied) ARGB pixels, rows top-down, as GDIBitmapSurface does.</summary>
    void SetPixels(int nX, int nY, int nWidth, int nHeight, uint32_t* pARGB) override;

    /// <summary>Every pixel transparent.</summary>
    void Clear();

    /// <summary>The pixels: device size, premultiplied ARGB32, rows top-down.</summary>
    const QImage& GetImage() const noexcept { return m_oImage; }

private:
    unsigned int m_nWidth;
    unsigned int m_nHeight;
    QImage m_oImage;
    std::shared_ptr<QtFontRegistry> m_pFonts;
};

/// <summary>
/// Makes QtSurfaces, at the scale OverlayImage last set: the Linux ISurfaceFactory when a Qt application is running
/// (PlatformServices_Linux.cpp). CreateSurface and CreateTransparentSurface make the same kind of surface: every
/// QtSurface has an alpha channel.
/// </summary>
class QtSurfaceFactory : public ISurfaceFactory
{
public:
    std::unique_ptr<ISurface> CreateSurface(int nWidth, int nHeight) const override;
    std::unique_ptr<ISurface> CreateTransparentSurface(int nWidth, int nHeight) const override;

    // Achievement screenshots arrive in O1c.
    bool SaveImage(const ISurface&, const std::wstring&) const noexcept override { return false; }

    /// <summary>
    /// A surface of exactly nDeviceWidth x nDeviceHeight device pixels at fScale: the image OverlayImage hands the
    /// emulator. Its logical size is the device size divided by the scale, rounded down, and at least 1.
    /// </summary>
    std::unique_ptr<QtSurface> CreateDeviceSurface(int nDeviceWidth, int nDeviceHeight, double fScale) const;

    /// <summary>
    /// The scale (device pixels per logical pixel) of the surfaces CreateSurface makes from now on. Surfaces made
    /// before keep theirs, and are drawn scaled until they are made again.
    /// </summary>
    void SetScale(double fScale) noexcept;
    double GetScale() const noexcept { return m_fScale.load(); }

private:
    std::shared_ptr<QtFontRegistry> m_pFonts = std::make_shared<QtFontRegistry>();
    std::atomic<double> m_fScale{1.0};
};

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_QT_QTSURFACE_HH
