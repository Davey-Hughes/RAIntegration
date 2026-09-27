#include "ViewModelBase.hh"

namespace ra {
namespace ui {

// Every notification goes through ForEachTarget, not Targets(): a target removed
// during a pass is skipped, and RemoveNotifyTargetAndWait - a binding's
// destructor, on the UI thread - can wait until no other thread is inside it.

void ViewModelBase::OnValueChanged(const BoolModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([&args](NotifyTarget& target) { target.OnViewModelBoolValueChanged(args); });

    ModelBase::OnValueChanged(args);
}

void ViewModelBase::OnValueChanged(const StringModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([&args](NotifyTarget& target) { target.OnViewModelStringValueChanged(args); });

    ModelBase::OnValueChanged(args);
}

void ViewModelBase::OnValueChanged(const IntModelProperty::ChangeArgs& args)
{
    m_vNotifyTargets.ForEachTarget([&args](NotifyTarget& target) { target.OnViewModelIntValueChanged(args); });

    ModelBase::OnValueChanged(args);
}

} // namespace ui
} // namespace ra
