#ifndef RA_UI_QT_BINDINGS_GRIDCOLUMNBINDING_HH
#define RA_UI_QT_BINDINGS_GRIDCOLUMNBINDING_HH
#pragma once

#include "ui/Types.hh"
#include "ui/ViewModelCollection.hh"

#include <QString>

#include <optional>
#include <string>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// What one cell of a grid shows: a copy, read on whatever thread changed the items and handed to the Qt thread. The
/// Qt thread reads the items itself only in SetControl, and in the row refresh that follows the user's own tick or
/// selection, where the view model's reaction also runs on the Qt thread. Later column types add their own fields (a
/// colour, a tooltip, an icon).
/// </summary>
struct GridCell
{
    QString sText;
    std::optional<bool> bChecked; // a tick, for a check box column
};

/// <summary>
/// Describes one column of a <see cref="GridBinding" />: the Qt counterpart of ui/win32/bindings/GridColumnBinding.
/// </summary>
/// <remarks>
/// Set up before the grid's SetControl and never changed after it: <see cref="GetCell" /> runs on whatever thread
/// changed the items.
/// </remarks>
class GridColumnBinding
{
public:
    GridColumnBinding() noexcept = default;
    virtual ~GridColumnBinding() noexcept = default;

    GridColumnBinding(const GridColumnBinding&) noexcept = delete;
    GridColumnBinding& operator=(const GridColumnBinding&) noexcept = delete;
    GridColumnBinding(GridColumnBinding&&) noexcept = delete;
    GridColumnBinding& operator=(GridColumnBinding&&) noexcept = delete;

    void SetHeader(const std::wstring& sHeader) { m_sHeader = sHeader; }
    const std::wstring& GetHeader() const noexcept { return m_sHeader; }

    /// <summary>Win32's three width rules.</summary>
    enum class WidthType
    {
        Pixels,     // a fixed width, in Qt's logical pixels
        Percentage, // a share of the grid's width
        Fill        // a weighted share of what Pixels and Percentage columns leave
    };

    void SetWidth(WidthType nType, int nAmount) noexcept
    {
        m_nWidthType = nType;
        m_nWidth = nAmount;
    }

    WidthType GetWidthType() const noexcept { return m_nWidthType; }
    int GetWidth() const noexcept { return m_nWidth; }

    ra::ui::RelativePosition GetAlignment() const noexcept { return m_nAlignment; }
    void SetAlignment(ra::ui::RelativePosition nValue) noexcept { m_nAlignment = nValue; }

    /// <summary>
    /// Whether the cells cannot be edited in place. Always true until a column type supports in-cell editing (Win32's
    /// CreateInPlaceEditor), which arrives with the asset editor.
    /// </summary>
    bool IsReadOnly() const noexcept { return true; }

    /// <summary>
    /// What the cell for the item at <paramref name="nIndex" /> shows. Any thread. It must never wait on the Qt thread:
    /// Detach waits, from the Qt thread, for a GetCell running on another thread.
    /// </summary>
    virtual GridCell GetCell(const ra::ui::ViewModelCollectionBase& vmItems, gsl::index nIndex) const = 0;

    /// <summary>Whether a change of <paramref name="pProperty" /> changes what this column shows.</summary>
    virtual bool DependsOn(const ra::ui::BoolModelProperty&) const noexcept { return false; }
    virtual bool DependsOn(const ra::ui::IntModelProperty&) const noexcept { return false; }
    virtual bool DependsOn(const ra::ui::StringModelProperty&) const noexcept { return false; }

protected:
    std::wstring m_sHeader;
    WidthType m_nWidthType = WidthType::Fill;
    int m_nWidth = 1;
    ra::ui::RelativePosition m_nAlignment = ra::ui::RelativePosition::Near;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_GRIDCOLUMNBINDING_HH
