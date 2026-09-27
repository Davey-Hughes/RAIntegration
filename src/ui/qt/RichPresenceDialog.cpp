#include "ui/qt/RichPresenceDialog.hh"

#include <QLabel>
#include <QVBoxLayout>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::viewmodels::RichPresenceMonitorViewModel;

bool RichPresenceDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const RichPresenceMonitorViewModel*>(&vmWindow) != nullptr;
}

void RichPresenceDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    // One window, as Win32 keeps one: a second ShowWindow raises it.
    if (m_pDialog.isNull())
        m_pDialog = new RichPresenceDialog(dynamic_cast<RichPresenceMonitorViewModel&>(vmWindow));

    m_pDialog->show();
    m_pDialog->raise();
    m_pDialog->activateWindow();
}

std::unique_ptr<QDialog> RichPresenceDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    // Win32's presenter shows the monitor non-modally even when asked for a
    // modal, and ShowModal returns at once.
    ShowWindow(vmWindow);
    return nullptr;
}

RichPresenceDialog::RichPresenceDialog(RichPresenceMonitorViewModel& vmRichPresence) : DialogBase(vmRichPresence)
{
    setMinimumSize(240, 90);

    m_pText = new QLabel(this);
    m_pText->setObjectName(QStringLiteral("RichPresenceText"));
    m_pText->setWordWrap(true);
    m_pText->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_pText->setTextFormat(Qt::PlainText); // the game's text, not markup

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(m_pText);

    m_bindWindow.SetSizeKey("Rich Presence Monitor");
    m_bindWindow.BindLabel(*m_pText, RichPresenceMonitorViewModel::DisplayStringProperty);
    m_bindWindow.SetWidget(*this);
}

} // namespace qt
} // namespace ui
} // namespace ra
