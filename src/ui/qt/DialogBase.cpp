#include "ui/qt/DialogBase.hh"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QShowEvent>

namespace ra {
namespace ui {
namespace qt {

DialogBase::DialogBase(ra::ui::WindowViewModelBase& vmWindow)
    : QWidget(nullptr, Qt::Window), m_bindWindow(vmWindow)
{
    setAttribute(Qt::WA_DeleteOnClose);
}

void DialogBase::showEvent(QShowEvent* pEvent)
{
    QWidget::showEvent(pEvent);
    m_bindWindow.OnShown();
}

void DialogBase::closeEvent(QCloseEvent* pEvent)
{
    QWidget::closeEvent(pEvent);
    if (pEvent->isAccepted())
        m_bindWindow.OnClosed();
}

void DialogBase::resizeEvent(QResizeEvent* pEvent)
{
    QWidget::resizeEvent(pEvent);
    m_bindWindow.OnResized(pEvent->size().width(), pEvent->size().height());
}

} // namespace qt
} // namespace ui
} // namespace ra
