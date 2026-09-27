#ifndef RA_UI_BINDINGBASE_H
#define RA_UI_BINDINGBASE_H
#pragma once

#include "services/ServiceLocator.hh"
#include "ui/ViewModelBase.hh"

#include <thread>

namespace ra {
namespace ui {

class BindingBase : protected ViewModelBase::NotifyTarget
{
public:
    ~BindingBase() noexcept
    {
        if (m_bAttached && ra::services::ServiceLocator::IsInitialized())
            m_vmViewModel.RemoveNotifyTarget(*this);
    }
    BindingBase(const BindingBase&) noexcept = delete;
    BindingBase& operator=(const BindingBase&) noexcept = delete;
    BindingBase(BindingBase&&) noexcept = delete;
    BindingBase& operator=(BindingBase&&) noexcept = delete;

protected:
    explicit BindingBase(_Inout_ ViewModelBase& vmViewModel) noexcept :
        m_vmViewModel{ vmViewModel }, m_bAttached{ true }
    {
        vmViewModel.AddNotifyTarget(*this);
    }

    /// <summary>
    /// Selects the constructor that leaves the binding out of the view model's notify targets until
    /// <see cref="AttachToViewModel" />.
    /// </summary>
    struct AttachLater {};

    /// <summary>
    /// Constructs the binding without joining the view model's notify targets. For a binding whose change handlers
    /// read its own members and can run on another thread: joining in this base constructor would let a handler run
    /// before those members exist.
    /// </summary>
    BindingBase(_Inout_ ViewModelBase& vmViewModel, AttachLater) noexcept :
        m_vmViewModel{ vmViewModel }
    {
    }

    /// <summary>
    /// Joins the view model's notify targets, if not already joined. Call it once everything the change handlers
    /// read is ready.
    /// </summary>
    void AttachToViewModel() noexcept
    {
        if (!m_bAttached)
        {
            m_bAttached = true;
            m_vmViewModel.AddNotifyTarget(*this);
        }
    }

    /// <summary>
    /// Leaves the view model's notify targets, if joined. When it returns, no other thread is inside one of this
    /// binding's change handlers, and none will enter one (RemoveNotifyTargetAndWait waits). Call it first in the
    /// destructor of a binding whose handlers read its own members: ~BindingBase runs only after they are gone.
    /// Never call it while holding a lock this binding's handlers take.
    /// </summary>
    void DetachFromViewModel() noexcept
    {
        if (m_bAttached)
        {
            m_bAttached = false;
            if (ra::services::ServiceLocator::IsInitialized())
                m_vmViewModel.RemoveNotifyTargetAndWait(*this);
        }
    }

    /// <summary>
    /// Sets <paramref name="pProperty" /> from the control this binding drives, without the change echoing back into
    /// the control: while it runs, <see cref="IsEchoOfOwnChange" /> is true for this property, on this thread only.
    /// Unlike <see cref="SetValue" />, the binding stays among the view model's notify targets, so a change another
    /// thread makes meanwhile - to this property or any other - still reaches it, as does a property the view model
    /// derives from this one.
    /// </summary>
    /// <remarks>
    /// Call it on the thread that constructed the binding. On any other it sets the value with nothing suppressed, and
    /// leaves the owner thread's echo state alone.
    /// </remarks>
    void SetValueFromControl(const BoolModelProperty& pProperty, bool bValue)
    {
        const EchoScope oScope(*this, pProperty);
        m_vmViewModel.SetValue(pProperty, bValue);
    }

    /// <summary>The string form of <see cref="SetValueFromControl(const BoolModelProperty&amp;, bool)" />.</summary>
    void SetValueFromControl(const StringModelProperty& pProperty, const std::wstring& sValue)
    {
        const EchoScope oScope(*this, pProperty);
        m_vmViewModel.SetValue(pProperty, sValue);
    }

    /// <summary>The integer form of <see cref="SetValueFromControl(const BoolModelProperty&amp;, bool)" />.</summary>
    void SetValueFromControl(const IntModelProperty& pProperty, int nValue)
    {
        const EchoScope oScope(*this, pProperty);
        m_vmViewModel.SetValue(pProperty, nValue);
    }

    /// <summary>
    /// Whether a change of <paramref name="pProperty" /> is this binding's own <see cref="SetValueFromControl" />
    /// coming back: true only on the thread that constructed the binding, while that call is setting this property.
    /// A view model that sets the same property again from inside that call - normalising it - is suppressed too, as
    /// <see cref="SetValue" />'s remove/re-add suppresses it.
    /// </summary>
    bool IsEchoOfOwnChange(const ra::data::ModelPropertyBase& pProperty) const noexcept
    {
        // Checked first, so no other thread ever reads m_pSettingProperty: only the owner thread touches it.
        if (std::this_thread::get_id() != m_nOwnerThread)
            return false;

        return m_pSettingProperty != nullptr && *m_pSettingProperty == pProperty;
    }

    /// <summary>
    /// Gets the value associated to the requested boolean property from the view-model.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for the bound view model.</returns>
    bool GetValue(const BoolModelProperty& pProperty) const
    {
        return m_vmViewModel.GetValue(pProperty);
    }

    /// <summary>
    /// Sets the specified boolean property of the view-model to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="bValue">The value to set.</param>
    void SetValue(const BoolModelProperty& pProperty, bool bValue)
    {
        // Leave the notify targets while setting, so the change does not echo back into the control that made it -
        // unless detached, when there is nothing to leave and nothing may re-join.
        if (m_nSetDepth++ == 0 && m_bAttached)
            m_vmViewModel.RemoveNotifyTarget(*this);

        m_vmViewModel.SetValue(pProperty, bValue);

        if (--m_nSetDepth == 0 && m_bAttached)
            m_vmViewModel.AddNotifyTarget(*this);
    }

    /// <summary>
    /// Gets the value associated to the requested string property from the view-model.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for the bound view model.</returns>
    const std::wstring& GetValue(const StringModelProperty& pProperty) const
    {
        return m_vmViewModel.GetValue(pProperty);
    }

    /// <summary>
    /// Gets a copy of the requested string property from the view-model, safe to take while another thread sets it.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>A copy of the current value of the property for the bound view model.</returns>
    std::wstring CopyValue(const StringModelProperty& pProperty) const
    {
        return m_vmViewModel.CopyValue(pProperty);
    }

    /// <summary>
    /// Sets the specified string property of the view-model to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="sValue">The value to set.</param>
    void SetValue(const StringModelProperty& pProperty, const std::wstring& sValue)
    {
        if (m_nSetDepth++ == 0 && m_bAttached)
            m_vmViewModel.RemoveNotifyTarget(*this);

        m_vmViewModel.SetValue(pProperty, sValue);

        if (--m_nSetDepth == 0 && m_bAttached)
            m_vmViewModel.AddNotifyTarget(*this);
    }

    /// <summary>
    /// Gets the value associated to the requested integer property from the view-model.
    /// </summary>
    /// <param name="pProperty">The property to query.</param>
    /// <returns>The current value of the property for the bound view model.</returns>
    int GetValue(const IntModelProperty& pProperty) const
    {
        return m_vmViewModel.GetValue(pProperty);
    }

    /// <summary>
    /// Sets the specified integer property of the view-model to the specified value.
    /// </summary>
    /// <param name="pProperty">The property to set.</param>
    /// <param name="nValue">The value to set.</param>
    void SetValue(const IntModelProperty& pProperty, int nValue)
    {
        if (m_nSetDepth++ == 0 && m_bAttached)
            m_vmViewModel.RemoveNotifyTarget(*this);

        m_vmViewModel.SetValue(pProperty, nValue);

        if (--m_nSetDepth == 0 && m_bAttached)
            m_vmViewModel.AddNotifyTarget(*this);
    }

protected:
    template <class T>
    T& GetViewModel() const noexcept { return dynamic_cast<T&>(m_vmViewModel); }

private:
    ra::ui::ViewModelBase& m_vmViewModel;
    int m_nSetDepth = 0;

    // Whether the binding is (meant to be) one of the view model's notify targets. Changed only on the thread that
    // owns the binding - the UI thread - so not atomic.
    bool m_bAttached = false;

    // Marks a property as being set from the control until destroyed, then restores the previous mark, so a nested
    // SetValueFromControl - a handler setting another property - unwinds correctly. Only on the owner thread: off it,
    // nothing is marked, and the mark - which only the owner thread touches - is neither read nor written.
    class EchoScope
    {
    public:
        EchoScope(BindingBase& oBinding, const ra::data::ModelPropertyBase& pProperty) noexcept
            : m_oBinding(oBinding), m_bMarked(std::this_thread::get_id() == oBinding.m_nOwnerThread)
        {
            if (m_bMarked)
            {
                m_pPrevious = m_oBinding.m_pSettingProperty;
                m_oBinding.m_pSettingProperty = &pProperty;
            }
        }

        ~EchoScope() noexcept
        {
            if (m_bMarked)
                m_oBinding.m_pSettingProperty = m_pPrevious;
        }

        EchoScope(const EchoScope&) noexcept = delete;
        EchoScope& operator=(const EchoScope&) noexcept = delete;
        EchoScope(EchoScope&&) noexcept = delete;
        EchoScope& operator=(EchoScope&&) noexcept = delete;

    private:
        BindingBase& m_oBinding;
        const ra::data::ModelPropertyBase* m_pPrevious = nullptr;
        bool m_bMarked;
    };

    // The thread that constructed the binding: the only one on which an echo is suppressed.
    const std::thread::id m_nOwnerThread = std::this_thread::get_id();

    // The property SetValueFromControl is setting, or null. Owner thread only (see IsEchoOfOwnChange).
    const ra::data::ModelPropertyBase* m_pSettingProperty = nullptr;
};

} // namespace ui
} // namespace ra

#endif // !RA_UI_BINDINGBASE_H
