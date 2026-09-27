#include "ViewModelCollection.hh"

namespace ra {
namespace ui {

void ViewModelCollectionBase::OnFrozen() noexcept
{
    m_vNotifyTargets.Clear();
}

// Every notification goes through ForEachTarget, not Targets(), as in
// ViewModelBase.cpp: a target removed during a pass is skipped, and a waiting
// removal becomes possible once collection bindings need one (slice 2).

void ViewModelCollectionBase::OnModelValueChanged(gsl::index nIndex,
    const BoolModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([nIndex, &args](NotifyTarget& target) {
        target.OnViewModelBoolValueChanged(nIndex, args);
    });
}

void ViewModelCollectionBase::OnModelValueChanged(gsl::index nIndex,
    const StringModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([nIndex, &args](NotifyTarget& target) {
        target.OnViewModelStringValueChanged(nIndex, args);
    });
}

void ViewModelCollectionBase::OnModelValueChanged(gsl::index nIndex,
    const IntModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([nIndex, &args](NotifyTarget& target) {
        target.OnViewModelIntValueChanged(nIndex, args);
    });
}

void ViewModelCollectionBase::OnBeginUpdate()
{
    m_vNotifyTargets.ForEachTarget([](NotifyTarget& target) { target.OnBeginViewModelCollectionUpdate(); });
}

void ViewModelCollectionBase::OnEndUpdate()
{
    m_vNotifyTargets.ForEachTarget([](NotifyTarget& target) { target.OnEndViewModelCollectionUpdate(); });
}

void ViewModelCollectionBase::OnItemsRemoved(const std::vector<gsl::index>& vDeletedIndices)
{
    m_vNotifyTargets.ForEachTarget([&vDeletedIndices](NotifyTarget& target) {
        for (auto nDeletedIndex : vDeletedIndices)
            target.OnViewModelRemoved(nDeletedIndex);
    });
}

void ViewModelCollectionBase::OnItemsAdded(const std::vector<gsl::index>& vNewIndices)
{
    m_vNotifyTargets.ForEachTarget([&vNewIndices](NotifyTarget& target) {
        for (auto vNewIndex : vNewIndices)
            target.OnViewModelAdded(vNewIndex);
    });
}

void ViewModelCollectionBase::OnItemsChanged(const std::vector<gsl::index>& vChangedIndices)
{
    m_vNotifyTargets.ForEachTarget([&vChangedIndices](NotifyTarget& target) {
        for (auto vChangedIndex : vChangedIndices)
            target.OnViewModelChanged(vChangedIndex);
    });
}

} // namespace ui
} // namespace ra
