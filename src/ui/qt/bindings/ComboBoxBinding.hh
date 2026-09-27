#ifndef RA_UI_QT_BINDINGS_COMBOBOXBINDING_HH
#define RA_UI_QT_BINDINGS_COMBOBOXBINDING_HH
#pragma once

#include "ui/qt/bindings/ControlBinding.hh"

#include "ui/ViewModelCollection.hh"
#include "ui/viewmodels/LookupItemViewModel.hh"

#include <QMetaObject>
#include <QStringList>

#include <atomic>

class QComboBox;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Binds a QComboBox to a list of items and an int property holding the selected item's id: the Qt counterpart of
/// ui/win32/bindings/ComboBoxBinding.
/// </summary>
class ComboBoxBinding : public ControlBinding, protected ViewModelCollectionBase::NotifyTarget
{
public:
    explicit ComboBoxBinding(ViewModelBase& vmViewModel) noexcept : ControlBinding(vmViewModel) {}
    ~ComboBoxBinding() noexcept override;

    ComboBoxBinding(const ComboBoxBinding&) noexcept = delete;
    ComboBoxBinding& operator=(const ComboBoxBinding&) noexcept = delete;
    ComboBoxBinding(ComboBoxBinding&&) noexcept = delete;
    ComboBoxBinding& operator=(ComboBoxBinding&&) noexcept = delete;

    /// <summary>
    /// Shows <paramref name="vmItems" /> by label and selects by id. Not tracked: as on Win32, a const collection is
    /// taken as fixed. Call before <see cref="SetControl" />.
    /// </summary>
    void BindItems(const ra::ui::viewmodels::LookupItemViewModelCollection& vmItems) noexcept;

    /// <summary>
    /// The same, tracked unless frozen: an item added, removed or relabelled on any thread reaches the control.
    /// Call before SetControl.
    /// </summary>
    void BindItems(ra::ui::viewmodels::LookupItemViewModelCollection& vmItems) noexcept;

    /// <summary>
    /// Selects the item whose id <paramref name="pProperty" /> holds, and writes the user's choice back. Call before
    /// SetControl.
    /// </summary>
    void BindSelectedItem(const IntModelProperty& pProperty) noexcept { m_pSelectedProperty = &pProperty; }

    /// <summary>
    /// Attaches the control: joins the notify targets (and the items', if tracked), fills it, selects, and connects
    /// the user's choice. Qt thread, once, last.
    /// </summary>
    void SetControl(QComboBox& oComboBox);

    void Detach() noexcept override;

protected:
    // the view model
    void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) override;

    // the items, if tracked
    void OnViewModelStringValueChanged(gsl::index nIndex, const StringModelProperty::ChangeArgs& args) override;
    void OnViewModelAdded(gsl::index nIndex) override;
    void OnViewModelRemoved(gsl::index nIndex) override;
    void OnEndViewModelCollectionUpdate() override;

private:
    void OnActivated(int nIndex);

    // Any thread, while attached: what the control shows.
    QStringList ReadLabels() const;
    int FindIndex(int nId) const;
    void PostRefresh();

    std::atomic<QComboBox*> m_pComboBox{nullptr}; // written once, by SetControl

    // Set before SetControl and never changed after it.
    const ViewModelCollectionBase* m_pItems = nullptr;
    ViewModelCollectionBase* m_pTrackedItems = nullptr;
    const IntModelProperty* m_pSelectedProperty = nullptr;

    // Qt thread only.
    bool m_bTracking = false;
    QMetaObject::Connection m_oActivated;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_COMBOBOXBINDING_HH
