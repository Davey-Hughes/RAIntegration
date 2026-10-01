#ifndef RA_TESTS_UI_QT_ITEMVIEWINPUT_HH
#define RA_TESTS_UI_QT_ITEMVIEWINPUT_HH
#pragma once

#ifndef _WIN32

#include <QAbstractItemModel>
#include <QApplication>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTableView>
#include <QToolTip>

namespace ra {
namespace ui {
namespace qt {
namespace tests {

// What a user does to a table view, sent as Qt would deliver it. Qt thread only.

// The centre of the tick the style draws in the cell: what the delegate tests a press against.
inline QPoint TickCentre(const QTableView& oView, int nRow, int nColumn)
{
    QStyleOptionViewItem oOption;
    oOption.initFrom(oView.viewport());
    oOption.rect = oView.visualRect(oView.model()->index(nRow, nColumn));
    oOption.widget = &oView;
    oOption.features = QStyleOptionViewItem::HasCheckIndicator;
    return oView.style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &oOption, &oView).center();
}

inline QPoint CellCentre(const QTableView& oView, int nRow, int nColumn)
{
    return oView.visualRect(oView.model()->index(nRow, nColumn)).center();
}

inline void PressMouse(QTableView& oView, QPoint ptWhere, Qt::KeyboardModifiers nModifiers = Qt::NoModifier)
{
    QMouseEvent oEvent(QEvent::MouseButtonPress, ptWhere, oView.viewport()->mapToGlobal(ptWhere), Qt::LeftButton,
                       Qt::LeftButton, nModifiers);
    QApplication::sendEvent(oView.viewport(), &oEvent);
}

inline void ReleaseMouse(QTableView& oView, QPoint ptWhere, Qt::KeyboardModifiers nModifiers = Qt::NoModifier)
{
    QMouseEvent oEvent(QEvent::MouseButtonRelease, ptWhere, oView.viewport()->mapToGlobal(ptWhere), Qt::LeftButton,
                       Qt::NoButton, nModifiers);
    QApplication::sendEvent(oView.viewport(), &oEvent);
}

// A key press sent to the view; one it ignores goes on to its parents, as a real one does (QApplication::notify).
inline void PressKey(QTableView& oView, int nKey, const QString& sText = QString())
{
    QKeyEvent oEvent(QEvent::KeyPress, nKey, Qt::NoModifier, sText);
    QApplication::sendEvent(&oView, &oEvent);
}

// The tooltip a hover over the cell shows, or an empty string.
inline QString HoverTooltip(QTableView& oView, int nRow, int nColumn)
{
    const QPoint ptWhere = CellCentre(oView, nRow, nColumn);
    QHelpEvent oEvent(QEvent::ToolTip, ptWhere, oView.viewport()->mapToGlobal(ptWhere));
    QApplication::sendEvent(oView.viewport(), &oEvent);
    return QToolTip::isVisible() ? QToolTip::text() : QString();
}

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32

#endif // !RA_TESTS_UI_QT_ITEMVIEWINPUT_HH
