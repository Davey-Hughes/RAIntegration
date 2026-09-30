#include "QtSurface.hh"

#include "QtImageRepository.hh"

#include "services/ServiceLocator.hh"

#include "util/EnumOps.hh"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QString>

#include <algorithm>
#include <cmath>

namespace ra {
namespace ui {
namespace drawing {
namespace qt {

namespace {

// Qt makes fonts only while a QGuiApplication runs: without one it aborts.
bool CanMakeFonts()
{
    return qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
}

QFont StyledFont(const std::string& sFont, FontStyles nStyle, int nPixelSize)
{
    using namespace ra::bitwise_ops;
    QFont oFont(QString::fromStdString(sFont));
    oFont.setPixelSize(nPixelSize);
    oFont.setBold((nStyle & FontStyles::Bold) == FontStyles::Bold);
    oFont.setItalic((nStyle & FontStyles::Italic) == FontStyles::Italic);
    oFont.setUnderline((nStyle & FontStyles::Underline) == FontStyles::Underline);
    oFont.setStrikeOut((nStyle & FontStyles::Strikethrough) == FontStyles::Strikethrough);
    return oFont;
}

// GDI's CreateFont with a positive height asks for the cell height - ascent plus descent - where Qt's pixel size is
// the em. The largest pixel size whose ascent plus descent fits the cell height, so text takes the room the layout
// gives it (measured: "Tahoma" is Noto Sans here, whose pixel size 26 is 36 pixels tall; 19 is 26). Resolved once,
// under the registry's lock, and stored as a plain int: the QFont it sizes is built fresh on every later use.
int CellHeightPixelSize(const std::string& sFont, int nCellHeight, FontStyles nStyle)
{
    const auto fHeight = [&sFont, nStyle](int nPixelSize) {
        return QFontMetrics(StyledFont(sFont, nStyle, nPixelSize)).height();
    };

    const int nHeight100 = std::max(1, fHeight(100));
    int nPixelSize = std::max(1, (nCellHeight * 100 + nHeight100 / 2) / nHeight100);
    while (nPixelSize > 1 && fHeight(nPixelSize) > nCellHeight)
        --nPixelSize;
    while (fHeight(nPixelSize + 1) <= nCellHeight)
        ++nPixelSize;

    return nPixelSize;
}

QColor ToQColor(Color nColor)
{
    return QColor::fromRgba(nColor.ARGB); // straight ARGB, as Color holds it
}

} // namespace

int QtFontRegistry::Load(const std::string& sFont, int nCellHeight, FontStyles nStyle)
{
    if (nCellHeight <= 0 || !CanMakeFonts())
        return 0;

    std::lock_guard<std::mutex> oLock(m_mtxFonts);
    for (size_t nIndex = 0; nIndex < m_vFonts.size(); ++nIndex)
    {
        const auto& pEntry = m_vFonts.at(nIndex);
        if (pEntry.nCellHeight == nCellHeight && pEntry.nStyle == nStyle && pEntry.sFont == sFont)
            return static_cast<int>(nIndex) + 1;
    }

    Entry oEntry;
    oEntry.sFont = sFont;
    oEntry.nCellHeight = nCellHeight;
    oEntry.nStyle = nStyle;
    oEntry.nPixelSize = CellHeightPixelSize(sFont, nCellHeight, nStyle);
    m_vFonts.push_back(std::move(oEntry));
    return static_cast<int>(m_vFonts.size());
}

bool QtFontRegistry::Get(int nFont, QFont& oFont, int& nCellHeight) const
{
    std::string sFont;
    FontStyles nStyle = FontStyles::Normal;
    int nPixelSize = 0;
    int nEntryCellHeight = 0;
    {
        std::lock_guard<std::mutex> oLock(m_mtxFonts);
        if (nFont <= 0 || static_cast<size_t>(nFont) > m_vFonts.size())
            return false;

        const auto& pEntry = m_vFonts.at(static_cast<size_t>(nFont) - 1);
        sFont = pEntry.sFont;
        nStyle = pEntry.nStyle;
        nPixelSize = pEntry.nPixelSize;
        nEntryCellHeight = pEntry.nCellHeight;
    }

    // Load's same check, re-run: a surface (and so its font ids) can outlive the QGuiApplication that made them.
    if (!CanMakeFonts())
        return false;

    // built fresh, outside the lock, from plain data: no QFont crosses threads, so no two threads ever touch one
    // QFont's shared private at once, however this Get() and another Get() elsewhere overlap.
    oFont = StyledFont(sFont, nStyle, nPixelSize);
    nCellHeight = nEntryCellHeight;
    return true;
}

QtSurface::QtSurface(int nWidth, int nHeight, int nDeviceWidth, int nDeviceHeight, double fScale,
                     std::shared_ptr<QtFontRegistry> pFonts)
    : m_nWidth(nWidth < 0 ? 0U : static_cast<unsigned int>(nWidth)),
      m_nHeight(nHeight < 0 ? 0U : static_cast<unsigned int>(nHeight)),
      m_pFonts(pFonts != nullptr ? std::move(pFonts) : std::make_shared<QtFontRegistry>())
{
    if (nWidth > 0 && nHeight > 0 && nDeviceWidth > 0 && nDeviceHeight > 0)
    {
        m_oImage = QImage(nDeviceWidth, nDeviceHeight, QImage::Format_ARGB32_Premultiplied);
        m_oImage.setDevicePixelRatio(fScale > 0.0 ? fScale : 1.0);
        m_oImage.fill(Qt::transparent);
    }
}

void QtSurface::Clear()
{
    if (!m_oImage.isNull())
        m_oImage.fill(Qt::transparent);
}

void QtSurface::FillRectangle(int nX, int nY, int nWidth, int nHeight, Color nColor)
{
    if (m_oImage.isNull() || nWidth <= 0 || nHeight <= 0)
        return;

    QPainter oPainter(&m_oImage);
    oPainter.setCompositionMode(QPainter::CompositionMode_Source);
    oPainter.fillRect(nX, nY, nWidth, nHeight, ToQColor(nColor));
}

int QtSurface::LoadFont(const std::string& sFont, int nFontSize, FontStyles nStyle)
{
    return m_pFonts->Load(sFont, nFontSize, nStyle);
}

ra::ui::Size QtSurface::MeasureText(int nFont, const std::wstring& sText) const
{
    QFont oFont;
    int nCellHeight = 0;
    if (!m_pFonts->Get(nFont, oFont, nCellHeight))
        return {0, 0};

    // the advance, and the cell height asked for: GDI's GetTextExtentPoint32 answers both the same way
    const QFontMetrics oMetrics(oFont);
    return {oMetrics.horizontalAdvance(QString::fromStdWString(sText)), nCellHeight};
}

void QtSurface::WriteText(int nX, int nY, int nFont, Color nColor, const std::wstring& sText)
{
    if (m_oImage.isNull() || sText.empty())
        return;

    QFont oFont;
    int nCellHeight = 0;
    if (!m_pFonts->Get(nFont, oFont, nCellHeight))
        return;

    const QString sQText = QString::fromStdWString(sText);
    const QFontMetrics oMetrics(oFont);

    // the cell's top-left at (nX, nY), as GDI's TextOut draws it: the baseline an ascent below. Not clipped, as
    // TextOut is not.
    QPainter oPainter(&m_oImage);
    oPainter.setFont(oFont);
    oPainter.setPen(ToQColor(nColor));
    oPainter.drawText(QPointF(nX, nY + oMetrics.ascent()), sQText);
}

void QtSurface::DrawImage(int nX, int nY, int nWidth, int nHeight, const ImageReference& pImage)
{
    DrawImageStretched(nX, nY, nWidth, nHeight, pImage);
}

void QtSurface::DrawImageStretched(int nX, int nY, int nWidth, int nHeight, const ImageReference& pImage)
{
    if (m_oImage.isNull() || nWidth <= 0 || nHeight <= 0 ||
        !ra::services::ServiceLocator::Exists<ra::ui::IImageRepository>())
        return;

    const auto* pRepository =
        dynamic_cast<const QtImageRepository*>(&ra::services::ServiceLocator::Get<ra::ui::IImageRepository>());
    if (pRepository == nullptr)
        return;

    const QImage oImage = pRepository->GetImage(pImage);
    if (oImage.isNull())
        return;

    QPainter oPainter(&m_oImage);
    oPainter.setRenderHint(QPainter::SmoothPixmapTransform);
    oPainter.drawImage(QRectF(nX, nY, nWidth, nHeight), oImage);
}

void QtSurface::DrawSurface(int nX, int nY, const ISurface& pSurface)
{
    const auto* pQtSurface = dynamic_cast<const QtSurface*>(&pSurface);
    if (m_oImage.isNull() || pQtSurface == nullptr || pQtSurface->m_oImage.isNull())
        return; // a NullSurface (NullDesktop::CaptureClientArea) has no pixels

    // source-over: the source's alpha shows what is under it. At its own device pixel ratio, so a surface made at
    // this one's scale lands pixel for pixel.
    QPainter oPainter(&m_oImage);
    oPainter.drawImage(QPointF(nX, nY), pQtSurface->m_oImage);
}

void QtSurface::DrawSurface(int nX, int nY, const ISurface& pSurface, int nSurfaceX, int nSurfaceY, int nWidth,
                            int nHeight)
{
    const auto* pQtSurface = dynamic_cast<const QtSurface*>(&pSurface);
    if (m_oImage.isNull() || pQtSurface == nullptr || pQtSurface->m_oImage.isNull() || nWidth <= 0 || nHeight <= 0)
        return;

    // the source rectangle is in the source image's own device pixels
    const double fSourceScale = pQtSurface->m_oImage.devicePixelRatio();
    QPainter oPainter(&m_oImage);
    oPainter.drawImage(QRectF(nX, nY, nWidth, nHeight), pQtSurface->m_oImage,
                       QRectF(nSurfaceX * fSourceScale, nSurfaceY * fSourceScale, nWidth * fSourceScale,
                              nHeight * fSourceScale));
}

void QtSurface::SetOpacity(double fAlpha)
{
    if (m_oImage.isNull())
        return;

    const int nAlpha = static_cast<int>(std::lround(std::clamp(fAlpha, 0.0, 1.0) * 255.0));
    QPainter oPainter(&m_oImage);
    oPainter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    oPainter.fillRect(QRectF(QPointF(0, 0), m_oImage.deviceIndependentSize()), QColor(0, 0, 0, nAlpha));
}

void QtSurface::SetPixels(int nX, int nY, int nWidth, int nHeight, uint32_t* pARGB)
{
    if (m_oImage.isNull() || pARGB == nullptr || nWidth <= 0 || nHeight <= 0)
        return;

    // borrowed, not copied: drawn before this returns
    const QImage oPixels(reinterpret_cast<const uchar*>(pARGB), nWidth, nHeight, nWidth * 4, QImage::Format_ARGB32);
    QPainter oPainter(&m_oImage);
    oPainter.setCompositionMode(QPainter::CompositionMode_Source);
    oPainter.drawImage(QRectF(nX, nY, nWidth, nHeight), oPixels);
}

std::unique_ptr<ISurface> QtSurfaceFactory::CreateSurface(int nWidth, int nHeight) const
{
    const double fScale = m_fScale.load();
    const int nDeviceWidth = static_cast<int>(std::ceil(nWidth * fScale));
    const int nDeviceHeight = static_cast<int>(std::ceil(nHeight * fScale));
    return std::make_unique<QtSurface>(nWidth, nHeight, nDeviceWidth, nDeviceHeight, fScale, m_pFonts);
}

std::unique_ptr<ISurface> QtSurfaceFactory::CreateTransparentSurface(int nWidth, int nHeight) const
{
    return CreateSurface(nWidth, nHeight);
}

std::unique_ptr<QtSurface> QtSurfaceFactory::CreateDeviceSurface(int nDeviceWidth, int nDeviceHeight,
                                                                 double fScale) const
{
    const double fSafeScale = (fScale > 0.0 && std::isfinite(fScale)) ? fScale : 1.0;
    const int nWidth = std::max(1, static_cast<int>(std::floor(nDeviceWidth / fSafeScale)));
    const int nHeight = std::max(1, static_cast<int>(std::floor(nDeviceHeight / fSafeScale)));
    return std::make_unique<QtSurface>(nWidth, nHeight, nDeviceWidth, nDeviceHeight, fSafeScale, m_pFonts);
}

void QtSurfaceFactory::SetScale(double fScale) noexcept
{
    m_fScale.store((fScale > 0.0 && std::isfinite(fScale)) ? fScale : 1.0);
}

} // namespace qt
} // namespace drawing
} // namespace ui
} // namespace ra
