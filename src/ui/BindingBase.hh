#ifndef RA_UI_BINDINGBASE_H
#define RA_UI_BINDINGBASE_H
#pragma once

#include "services/ServiceLocator.hh"
#include "ui/ViewModelBase.hh"

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
};

} // namespace ui
} // namespace ra

#endif // !RA_UI_BINDINGBASE_H
