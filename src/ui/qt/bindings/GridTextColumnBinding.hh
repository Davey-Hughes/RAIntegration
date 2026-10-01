#ifndef RA_UI_QT_BINDINGS_GRIDTEXTCOLUMNBINDING_HH
#define RA_UI_QT_BINDINGS_GRIDTEXTCOLUMNBINDING_HH
#pragma once

#include "ui/qt/bindings/GridColumnBinding.hh"

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Shows a string property: the Qt counterpart of ui/win32/bindings/GridTextColumnBinding.
/// </summary>
class GridTextColumnBinding : public GridColumnBinding
{
public:
    explicit GridTextColumnBinding(const StringModelProperty& pBoundProperty) noexcept : m_pBoundProperty(&pBoundProperty)
    {
    }

    GridCell GetCell(const ra::ui::ViewModelCollectionBase& vmItems, gsl::index nIndex) const override
    {
        GridCell oCell;
        oCell.sText = QString::fromStdWString(vmItems.GetItemValue(nIndex, *m_pBoundProperty));
        return oCell;
    }

    bool DependsOn(const ra::ui::StringModelProperty& pProperty) const noexcept override
    {
        return pProperty == *m_pBoundProperty;
    }
    using GridColumnBinding::DependsOn; // the other overloads

private:
    const StringModelProperty* m_pBoundProperty;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_GRIDTEXTCOLUMNBINDING_HH
