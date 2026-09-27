#ifndef RA_UI_QT_MODALDIALOGBASE_HH
#define RA_UI_QT_MODALDIALOGBASE_HH
#pragma once

#include "ui/qt/bindings/WindowBinding.hh"

#include <QDialog>

#include <vector>

class QDialogButtonBox;

namespace ra {
namespace ui {
namespace qt {

namespace bindings {
class ControlBinding;
class TextBoxBinding;
} // namespace bindings

/// <summary>
/// A modal dialog bound to a view model: the Qt counterpart of a Win32 DialogBase shown with CreateModalWindow. A
/// presenter's CreateModal returns one; QtDesktop shows it and waits for finished().
/// </summary>
/// <remarks>
/// A subclass builds its widgets, binds them, registers each control binding, and calls
/// <c>m_bindWindow.SetWidget(*this)</c> last in its constructor. Qt thread only. <see cref="done" /> answers the view
/// model and leaves every notify target before finished() is emitted, so the caller may destroy the view model as
/// soon as it wakes: nothing in the dialog touches it after that, and destroying the dialog later leaves it alone.
/// </remarks>
class ModalDialogBase : public QDialog
{
public:
    explicit ModalDialogBase(ra::ui::WindowViewModelBase& vmWindow);
    ~ModalDialogBase() noexcept override = default;

    ModalDialogBase(const ModalDialogBase&) noexcept = delete;
    ModalDialogBase& operator=(const ModalDialogBase&) noexcept = delete;
    ModalDialogBase(ModalDialogBase&&) noexcept = delete;
    ModalDialogBase& operator=(ModalDialogBase&&) noexcept = delete;

    /// <summary>
    /// On OK, writes pending text-box edits and asks <see cref="CanAccept" />, staying open if it says no. Then, for
    /// OK or not: leaves every notify target, answers the view model (OK or Cancel, unless it answered itself), and
    /// closes as QDialog does. Once the desktop has closed for shutdown, OK cancels instead.
    /// </summary>
    void done(int nResult) override;

protected:
    /// <summary>Whether OK may close the dialog. False keeps it open; the view model has shown why. Qt thread.</summary>
    virtual bool CanAccept() { return true; }

    /// <summary>OK and Cancel, wired to accept() and reject(); OK is the default button.</summary>
    QDialogButtonBox* CreateButtons();

    /// <summary>A control binding done() detaches. Register each once, in the constructor.</summary>
    void RegisterBinding(ra::ui::qt::bindings::ControlBinding& oBinding);

    /// <summary>
    /// A text-box binding whose pending edit done() also writes on OK (FlushPendingEdit). Register each once, in the
    /// constructor.
    /// </summary>
    void RegisterTextBox(ra::ui::qt::bindings::TextBoxBinding& oBinding);

    ra::ui::qt::bindings::WindowBinding m_bindWindow;

private:
    void Answer(int nResult);

    ra::ui::WindowViewModelBase* m_pViewModel; // cleared once answered: the caller may destroy it then
    std::vector<ra::ui::qt::bindings::ControlBinding*> m_vBindings;
    std::vector<ra::ui::qt::bindings::TextBoxBinding*> m_vTextBoxes;
    bool m_bInCanAccept = false;
    bool m_bRejectPending = false;
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_MODALDIALOGBASE_HH
