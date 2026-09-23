#ifndef RA_UI_DRAWING_NULL_SURFACE_HH
#define RA_UI_DRAWING_NULL_SURFACE_HH
#pragma once

#include "ui/drawing/ISurface.hh"

namespace ra {
namespace ui {
namespace drawing {
namespace null {

// Accepts every draw call and keeps only its dimensions. The overlay phase
// replaces this; until then the overlay view models can run without rendering.
class NullSurface : public ISurface
{
public:
    NullSurface(int nWidth, int nHeight) noexcept
        : m_nWidth(nWidth < 0 ? 0u : static_cast<unsigned int>(nWidth)),
          m_nHeight(nHeight < 0 ? 0u : static_cast<unsigned int>(nHeight))
    {
    }

    unsigned int GetWidth() const override { return m_nWidth; }
    unsigned int GetHeight() const override { return m_nHeight; }

    void FillRectangle(int, int, int, int, Color) override {}
    int LoadFont(const std::string&, int nFontSize, FontStyles) override
    {
        m_nFontSize = nFontSize;
        return 1;
    }

    ra::ui::Size MeasureText(int, const std::wstring& sText) const override
    {
        // a plausible monospace estimate, so layout code does not divide by zero
        return {static_cast<int>(sText.length()) * (m_nFontSize / 2), m_nFontSize};
    }

    void WriteText(int, int, int, Color, const std::wstring&) override {}
    void DrawImage(int, int, int, int, const ImageReference&) override {}
    void DrawImageStretched(int, int, int, int, const ImageReference&) override {}
    void DrawSurface(int, int, const ISurface&) override {}
    void DrawSurface(int, int, const ISurface&, int, int, int, int) override {}
    void SetOpacity(double) override {}
    void SetPixels(int, int, int, int, uint32_t*) override {}

private:
    unsigned int m_nWidth;
    unsigned int m_nHeight;
    int m_nFontSize = 12;
};

class NullSurfaceFactory : public ISurfaceFactory
{
public:
    std::unique_ptr<ISurface> CreateSurface(int nWidth, int nHeight) const override
    {
        return std::make_unique<NullSurface>(nWidth, nHeight);
    }

    std::unique_ptr<ISurface> CreateTransparentSurface(int nWidth, int nHeight) const override
    {
        return std::make_unique<NullSurface>(nWidth, nHeight);
    }

    bool SaveImage(const ISurface&, const std::wstring&) const override { return false; }
};

} // namespace null
} // namespace drawing
} // namespace ui
} // namespace ra

#endif // !RA_UI_DRAWING_NULL_SURFACE_HH
