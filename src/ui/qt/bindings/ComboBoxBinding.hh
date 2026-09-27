#ifndef RA_UI_QT_BINDINGS_COMBOBOXBINDING_HH
#define RA_UI_QT_BINDINGS_COMBOBOXBINDING_HH
#pragma once

#include "ui/qt/bindings/ControlBinding.hh"

#include "ui/ViewModelCollection.hh"
#include "ui/viewmodels/LookupItemViewModel.hh"

#include <QMetaObject>
#include <QString>

#include <atomic>
#include <utility>
#include <vector>

class QComboBox;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

/// <summary>
/// Binds a QComboBox to a list of items and an int property holding the selected item's id: the Qt counterpart of
/// ui/win32/bindings/ComboBoxBinding.
/// </summary>
/// <remarks>
/// The control holds each item's id beside its label, and the id to select. So a refresh posted from another thread
/// re-selects the latest id when it runs, and a choice writes the id of the item the user saw. Neither reads the
/// items or the view model on the Qt thread.
/// </remarks>
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
    /// The same, tracked unless frozen: an item added, removed, moved, relabelled or given a new id, on any thread,
    /// reaches the control. Call before SetControl. Collections have no lock and SetControl reads the items, so
    /// nothing may change them on another thread while it runs. The collection must outlive the binding, or the
    /// binding be detached first.
    /// </summary>
    void BindItems(ra::ui::viewmodels::LookupItemViewModelCollection& vmItems) noexcept;

    /// <summary>
    /// Selects the item whose id <paramref name="pProperty" /> holds, and writes the user's choice back. Call before
    /// SetControl.
    /// </summary>
    void BindSelectedItem(const IntModelProperty& pProperty) noexcept;

    /// <summary>
    /// Attaches the control: joins the notify targets (and the items', if tracked), fills it, selects, and connects
    /// the user's choice. Qt thread, once, last. The control must outlive the binding, or the binding be detached
    /// first.
    /// </summary>
    void SetControl(QComboBox& oComboBox);

    void Detach() noexcept override;

protected:
    // the view model
    void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) override;

    // the items, if tracked
    void OnViewModelIntValueChanged(gsl::index nIndex, const IntModelProperty::ChangeArgs& args) override;
    void OnViewModelStringValueChanged(gsl::index nIndex, const StringModelProperty::ChangeArgs& args) override;
    void OnViewModelAdded(gsl::index nIndex) override;
    void OnViewModelRemoved(gsl::index nIndex) override;
    void OnViewModelChanged(gsl::index nIndex) override;
    void OnEndViewModelCollectionUpdate() override;

private:
    using Items = std::vector<std::pair<QString, int>>; // label, id

    void OnActivated(int nIndex);

    // The thread changing the items, or the Qt thread in SetControl: what the control shows.
    Items ReadItems() const;
    void PostRefresh();

    // Qt thread. Static, so posted work can call them without the binding, which may be gone by then.
    static void Fill(QComboBox& oComboBox, const Items& vItems);
    static void SelectStoredId(QComboBox& oComboBox);

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
