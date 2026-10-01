#ifndef RA_UI_QT_BINDINGS_GRIDBINDING_HH
#define RA_UI_QT_BINDINGS_GRIDBINDING_HH
#pragma once

#include "ui/qt/bindings/ControlBinding.hh"
#include "ui/qt/bindings/GridColumnBinding.hh"

#include "ui/ViewModelCollection.hh"

#include <QPointer>

#include <atomic>
#include <memory>
#include <vector>

class QTableView;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

namespace detail {

/// <summary>What one row of a grid shows: a copy, as GridCell is.</summary>
struct GridRow
{
    std::vector<GridCell> vCells;
    bool bSelected = false; // the bound IsSelected property; false when none is bound
};

class GridModel;

} // namespace detail

/// <summary>
/// Binds a QTableView to a collection of view models, one row per item: the Qt counterpart of
/// ui/win32/bindings/GridBinding.
/// </summary>
/// <remarks>
/// The view shows copies. A change to the items, on any thread, reads what the changed row (or every row) shows on
/// that thread and queues the copy to the Qt thread - queued even there, because the user's own changes are written
/// from inside Qt's handlers and the view model reacts inline. Posted work touches only the view's model, never the
/// view model or its collections. A user's change is written to the items on the Qt thread, by row number, and only
/// while the view shows the items' current order: an add, remove, move or batch makes the rows shown stale until its
/// refresh arrives, and a write meanwhile is dropped rather than landing on another item. That is best-effort: the
/// check and the write are not atomic against a structural change on a worker, and collections have no lock (as with
/// Win32's writes by index), so it closes the gap only for changes ordered before the click. The binding's own write
/// comes back as a queued row refresh, so the view converges on the view model while one thread writes the items. A
/// worker's queued refresh of the same row, read before the user's change, can land after it and show a stale tick or
/// selection until the row next changes. Report a Problem is safe: nothing else writes while it is open. A grid whose
/// items a worker also changes (the D-slices) is not.
/// </remarks>
class GridBinding : public ControlBinding, protected ViewModelCollectionBase::NotifyTarget
{
public:
    explicit GridBinding(ViewModelBase& vmViewModel) noexcept : ControlBinding(vmViewModel) {}
    ~GridBinding() noexcept override;

    GridBinding(const GridBinding&) noexcept = delete;
    GridBinding& operator=(const GridBinding&) noexcept = delete;
    GridBinding(GridBinding&&) noexcept = delete;
    GridBinding& operator=(GridBinding&&) noexcept = delete;

    /// <summary>Sets column <paramref name="nColumn" />. Columns are numbered from 0, with no gaps. Before SetControl.</summary>
    void BindColumn(gsl::index nColumn, std::unique_ptr<GridColumnBinding> pColumnBinding);

    /// <summary>
    /// The rows: one per item, tracked unless the collection is frozen. Before SetControl. Collections have no lock
    /// and SetControl reads the items, so nothing may change them on another thread while it runs. The collection
    /// must outlive the binding, or the binding be detached first.
    /// </summary>
    void BindItems(ViewModelCollectionBase& vmItems) noexcept;

    /// <summary>
    /// Ties the rows' selection to <paramref name="pProperty" /> on each item: selecting a row sets it, and setting it
    /// selects the row. Before SetControl.
    /// </summary>
    void BindIsSelected(const BoolModelProperty& pProperty) noexcept;

    /// <summary>
    /// Attaches the view: gives it its model, header and widths, joins the notify targets, and shows the items. Qt
    /// thread, once, last. The view's selection mode is the caller's. The view must outlive the binding, or the binding
    /// be detached first.
    /// </summary>
    void SetControl(QTableView& oView);

    void Detach() noexcept override;

protected:
    // the items
    void OnViewModelBoolValueChanged(gsl::index nIndex, const BoolModelProperty::ChangeArgs& args) override;
    void OnViewModelIntValueChanged(gsl::index nIndex, const IntModelProperty::ChangeArgs& args) override;
    void OnViewModelStringValueChanged(gsl::index nIndex, const StringModelProperty::ChangeArgs& args) override;
    void OnViewModelAdded(gsl::index nIndex) override;
    void OnViewModelRemoved(gsl::index nIndex) override;
    void OnViewModelChanged(gsl::index nIndex) override;
    void OnBeginViewModelCollectionUpdate() override;
    void OnEndViewModelCollectionUpdate() override;

    // the view model: nothing of it is bound yet
    using ViewModelBase::NotifyTarget::OnViewModelBoolValueChanged;
    using ViewModelBase::NotifyTarget::OnViewModelIntValueChanged;
    using ViewModelBase::NotifyTarget::OnViewModelStringValueChanged;

private:
    // The thread changing the items, or the Qt thread in SetControl: what the view shows.
    detail::GridRow ReadRow(gsl::index nIndex) const;
    std::vector<detail::GridRow> ReadRows() const;
    void PostRow(gsl::index nIndex);
    void PostAllRows();

    // Qt thread: the user's changes, from the view's model.
    void WriteCheck(gsl::index nRow, gsl::index nColumn, bool bChecked, unsigned int nShownStructure);
    void WriteSelected(gsl::index nRow, bool bSelected, unsigned int nShownStructure);

    // Set before SetControl and never changed after it.
    std::vector<std::unique_ptr<GridColumnBinding>> m_vColumns;
    ViewModelCollectionBase* m_pItems = nullptr;
    const BoolModelProperty* m_pIsSelectedProperty = nullptr;

    std::atomic<detail::GridModel*> m_pModel{nullptr}; // written once, by SetControl
    std::atomic<unsigned int> m_nStructure{0};         // bumped by every add, remove, move and batch

    // Qt thread only.
    QPointer<detail::GridModel> m_pQtModel;
    bool m_bTracking = false;
};

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_BINDINGS_GRIDBINDING_HH
