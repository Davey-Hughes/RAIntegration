#ifndef RA_UI_QT_BINDINGS_GRIDBOOLEANCOLUMNBINDING_HH
#define RA_UI_QT_BINDINGS_GRIDBOOLEANCOLUMNBINDING_HH
#pragma once

#include "ui/qt/bindings/GridColumnBinding.hh"

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Shows a bool property as one of two texts: the Qt counterpart of ui/win32/bindings/GridBooleanColumnBinding.
/// </summary>
class GridBooleanColumnBinding : public GridColumnBinding
{
public:
    GridBooleanColumnBinding(const BoolModelProperty& pBoundProperty, const std::wstring& sTrueText,
                             const std::wstring& sFalseText)
        : m_pBoundProperty(&pBoundProperty), m_sTrueText(QString::fromStdWString(sTrueText)),
          m_sFalseText(QString::fromStdWString(sFalseText))
    {
    }

    GridCell GetCell(const ra::ui::ViewModelCollectionBase& vmItems, gsl::index nIndex) const override
    {
        GridCell oCell;
        oCell.sText = vmItems.GetItemValue(nIndex, *m_pBoundProperty) ? m_sTrueText : m_sFalseText;
        return oCell;
    }

    bool DependsOn(const ra::ui::BoolModelProperty& pProperty) const noexcept override
    {
        return pProperty == *m_pBoundProperty;
    }
    using GridColumnBinding::DependsOn; // the other overloads

private:
    const BoolModelProperty* m_pBoundProperty;
    QString m_sTrueText;
    QString m_sFalseText;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_GRIDBOOLEANCOLUMNBINDING_HH
