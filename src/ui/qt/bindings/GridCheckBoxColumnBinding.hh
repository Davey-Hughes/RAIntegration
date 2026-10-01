#ifndef RA_UI_QT_BINDINGS_GRIDCHECKBOXCOLUMNBINDING_HH
#define RA_UI_QT_BINDINGS_GRIDCHECKBOXCOLUMNBINDING_HH
#pragma once

#include "ui/qt/bindings/GridColumnBinding.hh"

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Shows a bool property as a tick the user can click: the Qt counterpart of
/// ui/win32/bindings/GridCheckBoxColumnBinding. A click writes the property back (see GridBinding).
/// </summary>
class GridCheckBoxColumnBinding : public GridColumnBinding
{
public:
    explicit GridCheckBoxColumnBinding(const BoolModelProperty& pBoundProperty) noexcept : m_pBoundProperty(&pBoundProperty)
    {
    }

    GridCell GetCell(const ra::ui::ViewModelCollectionBase& vmItems, gsl::index nIndex) const override
    {
        GridCell oCell;
        oCell.bChecked = vmItems.GetItemValue(nIndex, *m_pBoundProperty);
        return oCell;
    }

    bool DependsOn(const ra::ui::BoolModelProperty& pProperty) const noexcept override
    {
        return pProperty == *m_pBoundProperty;
    }
    using GridColumnBinding::DependsOn; // the other overloads

    const BoolModelProperty& GetBoundProperty() const noexcept { return *m_pBoundProperty; }

private:
    const BoolModelProperty* m_pBoundProperty;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_GRIDCHECKBOXCOLUMNBINDING_HH
