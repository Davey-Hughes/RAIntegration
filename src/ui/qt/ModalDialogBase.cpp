#include "ui/qt/ModalDialogBase.hh"

#include "services/ServiceLocator.hh"

#include "ui/IDesktop.hh"
#include "ui/qt/QtDesktop.hh"
#include "ui/qt/bindings/ControlBinding.hh"
#include "ui/qt/bindings/TextBoxBinding.hh"

#include <QDialogButtonBox>

namespace ra {
namespace ui {
namespace qt {

namespace {

// Whether the registered desktop has closed its windows for shutdown. Not a QtDesktop - a test's mock - never has.
bool IsDesktopClosedForShutdown()
{
    if (!ra::services::ServiceLocator::Exists<ra::ui::IDesktop>())
        return false;

    const auto* pDesktop = dynamic_cast<const QtDesktop*>(&ra::services::ServiceLocator::Get<ra::ui::IDesktop>());
    return pDesktop != nullptr && pDesktop->IsClosedForShutdown();
}

} // namespace

ModalDialogBase::ModalDialogBase(ra::ui::WindowViewModelBase& vmWindow)
    : QDialog(nullptr), m_bindWindow(vmWindow), m_pViewModel(&vmWindow)
{
    // A result left from an earlier showing would read as this one's: done() keeps a result the view model set.
    vmWindow.SetDialogResult(ra::ui::DialogResult::None);
}

QDialogButtonBox* ModalDialogBase::CreateButtons()
{
    auto* pButtons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(pButtons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(pButtons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    return pButtons;
}

void ModalDialogBase::RegisterBinding(ra::ui::qt::bindings::ControlBinding& oBinding)
{
    m_vBindings.push_back(&oBinding);
}

void ModalDialogBase::RegisterTextBox(ra::ui::qt::bindings::TextBoxBinding& oBinding)
{
    m_vBindings.push_back(&oBinding);
    m_vTextBoxes.push_back(&oBinding);
}

void ModalDialogBase::done(int nResult)
{
    if (m_pViewModel == nullptr)
    {
        QDialog::done(nResult); // answered already
        return;
    }

    if (m_bInCanAccept)
    {
        // A reject while CanAccept runs - QtDesktop::CloseAll at shutdown, while CanAccept is inside a nested modal -
        // must not finish now: that releases the caller, who may destroy the view model while CanAccept, a view model
        // method, is still on the stack. It finishes when CanAccept returns.
        if (nResult != QDialog::Accepted)
            m_bRejectPending = true;
        return;
    }

    if (nResult == QDialog::Accepted && IsDesktopClosedForShutdown())
    {
        // Shutting down: OK must not start CanAccept's work - Login's server call, which the thread pool may refuse
        // and never answer - while a caller waits to be released. It answers Cancel, as CloseAll's reject would.
        nResult = QDialog::Rejected;
    }

    if (nResult == QDialog::Accepted)
    {
        // An edit made without leaving its field (LostFocus), or still waiting for the typing pause, has not been
        // written yet. Only such an edit: an unedited box would overwrite a view-model change still queued for it.
        for (auto* pTextBox : m_vTextBoxes)
            pTextBox->FlushPendingEdit();

        m_bInCanAccept = true;
        const bool bCanAccept = CanAccept();
        m_bInCanAccept = false;

        if (m_bRejectPending)
            nResult = QDialog::Rejected;
        else if (!bCanAccept)
            return; // stays open
    }

    Answer(nResult);
    QDialog::done(nResult);
}

void ModalDialogBase::Answer(int nResult)
{
    // Every notify target first: the WindowBinding closes the window when DialogResult changes, so the
    // SetDialogResult below would queue a second close for a dialog that is closing already.
    for (auto* pBinding : m_vBindings)
        pBinding->Detach();
    m_bindWindow.Detach();

    // A result the view model set itself - it closed the dialog - stands.
    if (m_pViewModel->GetDialogResult() == ra::ui::DialogResult::None)
    {
        m_pViewModel->SetDialogResult(nResult == QDialog::Accepted ? ra::ui::DialogResult::OK
                                                                   : ra::ui::DialogResult::Cancel);
    }

    // Before finished(), which QDialog::done emits: the caller reads the result as soon as it wakes, and may destroy
    // the view model then.
    m_pViewModel = nullptr;
}

} // namespace qt
} // namespace ui
} // namespace ra
