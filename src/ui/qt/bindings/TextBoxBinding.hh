#ifndef RA_UI_QT_BINDINGS_TEXTBOXBINDING_HH
#define RA_UI_QT_BINDINGS_TEXTBOXBINDING_HH
#pragma once

#include "ui/qt/bindings/ControlBinding.hh"

#include <QMetaObject>
#include <QPointer>

#include <atomic>
#include <vector>

class QLineEdit;
class QTimer;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Binds a QLineEdit's text to a string property: the Qt counterpart of ui/win32/bindings/TextBoxBinding.
/// </summary>
class TextBoxBinding : public ControlBinding
{
public:
    /// <summary>When the control's text is written to the view model: Win32's modes.</summary>
    enum class UpdateMode
    {
        None,      // one way, from the view model
        LostFocus, // when editing finishes: Return, or leaving the control
        KeyPress,  // after each edit
        Typing,    // 300 ms after the last edit
    };

    explicit TextBoxBinding(ViewModelBase& vmViewModel) noexcept : ControlBinding(vmViewModel) {}
    ~TextBoxBinding() noexcept override;

    TextBoxBinding(const TextBoxBinding&) noexcept = delete;
    TextBoxBinding& operator=(const TextBoxBinding&) noexcept = delete;
    TextBoxBinding(TextBoxBinding&&) noexcept = delete;
    TextBoxBinding& operator=(TextBoxBinding&&) noexcept = delete;

    /// <summary>Binds the text. Call before <see cref="SetControl" />.</summary>
    void BindText(const StringModelProperty& pProperty, UpdateMode nMode = UpdateMode::LostFocus) noexcept;

    /// <summary>Makes the control read-only while <paramref name="pProperty" /> is true. Call before SetControl.</summary>
    void BindReadOnly(const BoolModelProperty& pProperty) noexcept;

    /// <summary>
    /// Attaches the control: joins the notify targets, shows the current values and connects the user's edits. Call
    /// once, last, on the Qt thread. The control must outlive the binding, or the binding be detached first.
    /// </summary>
    void SetControl(QLineEdit& oLineEdit);

    /// <summary>Writes the control's text to the view model now, whatever the mode, and marks it written. Qt thread.</summary>
    void UpdateSource();

    /// <summary>
    /// Writes an edit the mode has not written yet: text typed without leaving the control, or still waiting for the
    /// typing pause. Nothing else - an unedited control, or one whose edit was written, leaves the view model alone,
    /// so a change still queued for the control is not overwritten with what it showed before. Never in None mode. A
    /// modal dialog calls it on OK. Qt thread.
    /// </summary>
    void FlushPendingEdit();

    void Detach() noexcept override;

protected:
    void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args) override;
    void OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args) override;

private:
    // Written once, by SetControl; read by the change handlers on any thread.
    std::atomic<QLineEdit*> m_pLineEdit{nullptr};

    // Set before SetControl and never changed after it: the handlers read them.
    const StringModelProperty* m_pTextProperty = nullptr;
    const BoolModelProperty* m_pReadOnlyProperty = nullptr;
    UpdateMode m_nUpdateMode = UpdateMode::LostFocus;

    // Qt thread only.
    QPointer<QTimer> m_pTypingTimer; // a child of the control: gone with it
    std::vector<QMetaObject::Connection> m_vConnections;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_TEXTBOXBINDING_HH
