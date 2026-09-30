#ifndef RA_TESTS_UI_QT_RIGHTALIGNINGSTYLE_HH
#define RA_TESTS_UI_QT_RIGHTALIGNINGSTYLE_HH
#pragma once

#include <QProxyStyle>

namespace ra {
namespace ui {
namespace qt {
namespace tests {

// What KDE's Breeze answers, and so what a form that does not choose its label alignment gets there: labels right
// aligned. Win32's dialogs put them on the left. Set on a dialog before it is first shown: QFormLayout caches the
// style's answer on first use.
class RightAligningStyle : public QProxyStyle
{
public:
    int styleHint(StyleHint nHint, const QStyleOption* pOption, const QWidget* pWidget,
                  QStyleHintReturn* pReturn) const override
    {
        if (nHint == QStyle::SH_FormLayoutLabelAlignment)
            return static_cast<int>(Qt::AlignRight | Qt::AlignVCenter);
        return QProxyStyle::styleHint(nHint, pOption, pWidget, pReturn);
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif /* !RA_TESTS_UI_QT_RIGHTALIGNINGSTYLE_HH */
