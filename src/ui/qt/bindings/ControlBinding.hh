#ifndef RA_UI_QT_BINDINGS_CONTROLBINDING_HH
#define RA_UI_QT_BINDINGS_CONTROLBINDING_HH
#pragma once

#include "ui/BindingBase.hh"

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// The base of a two-way binding between one Qt control and a view model: the Qt counterpart of
/// ui/win32/bindings/ControlBinding.
/// </summary>
/// <remarks>
/// Constructed, attached, detached and destroyed on the Qt thread. That is the binding's owner thread, on which
/// SetValueFromControl suppresses the control's own echo. The change handlers run on whatever thread set the
/// property, only while the binding is attached, and only ever Post work that captures the values it needs: posted
/// work never touches the view model or its collections, which may be gone by the time it runs. A subclass joins the
/// notify targets (<see cref="Attach" />) in its SetControl, once everything its handlers read is set, and leaves them
/// first thing in its destructor (<see cref="Detach" />). It writes back only on user signals - textEdited,
/// editingFinished, clicked, activated - never on those a programmatic update also emits.
/// </remarks>
class ControlBinding : protected BindingBase
{
public:
    ~ControlBinding() noexcept override = default;

    ControlBinding(const ControlBinding&) noexcept = delete;
    ControlBinding& operator=(const ControlBinding&) noexcept = delete;
    ControlBinding(ControlBinding&&) noexcept = delete;
    ControlBinding& operator=(ControlBinding&&) noexcept = delete;

    /// <summary>
    /// Leaves the view model's notify targets, waiting for a handler running on another thread, and ignores the
    /// control from then on: the binding never touches the view model again. A modal dialog calls it in done(),
    /// before finished(), because its caller may destroy the view model as soon as it wakes. Idempotent. Qt thread.
    /// </summary>
    virtual void Detach() noexcept
    {
        m_bDetached = true;
        DetachFromViewModel();
    }

protected:
    explicit ControlBinding(ViewModelBase& vmViewModel) noexcept : BindingBase(vmViewModel, AttachLater{}) {}

    /// <summary>Joins the view model's notify targets - not after <see cref="Detach" />. Qt thread.</summary>
    void Attach() noexcept
    {
        if (!m_bDetached)
            AttachToViewModel();
    }

    /// <summary>Whether <see cref="Detach" /> ran. Qt thread.</summary>
    bool IsDetached() const noexcept { return m_bDetached; }

private:
    bool m_bDetached = false; // Qt thread only
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_CONTROLBINDING_HH
