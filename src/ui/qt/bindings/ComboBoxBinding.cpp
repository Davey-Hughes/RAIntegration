#include "ui/qt/bindings/ComboBoxBinding.hh"

#include "ui/qt/bindings/Post.hh"

#include "services/ServiceLocator.hh"

#include <QComboBox>
#include <QString>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

using ra::ui::viewmodels::LookupItemViewModel;
using ra::ui::viewmodels::LookupItemViewModelCollection;

ComboBoxBinding::~ComboBoxBinding() noexcept
{
    Detach();
}

void ComboBoxBinding::BindItems(const LookupItemViewModelCollection& vmItems) noexcept
{
    m_pItems = &vmItems;
    m_pTrackedItems = nullptr;
}

void ComboBoxBinding::BindItems(LookupItemViewModelCollection& vmItems) noexcept
{
    m_pItems = &vmItems;
    m_pTrackedItems = vmItems.IsFrozen() ? nullptr : &vmItems; // a frozen collection never changes
}

void ComboBoxBinding::SetControl(QComboBox& oComboBox)
{
    // Joined first - the items, then the view model - then published, then read: a change from here on either
    // reaches a handler or is read below.
    if (m_pTrackedItems != nullptr && !IsDetached())
    {
        m_pTrackedItems->AddNotifyTarget(static_cast<ViewModelCollectionBase::NotifyTarget&>(*this));
        m_bTracking = true;
    }
    Attach();
    m_pComboBox.store(&oComboBox);

    oComboBox.clear();
    oComboBox.addItems(ReadLabels());
    oComboBox.setCurrentIndex(m_pSelectedProperty != nullptr ? FindIndex(GetValue(*m_pSelectedProperty)) : -1);

    // activated, not currentIndexChanged: setCurrentIndex - a posted update - emits the latter too
    m_oActivated =
        QObject::connect(&oComboBox, &QComboBox::activated, &oComboBox, [this](int nIndex) { OnActivated(nIndex); });
}

void ComboBoxBinding::Detach() noexcept
{
    QObject::disconnect(m_oActivated);

    if (m_bTracking)
    {
        m_bTracking = false;

        // waits for an item handler running on another thread, as the view model's detach below does
        if (ra::services::ServiceLocator::IsInitialized())
            m_pTrackedItems->RemoveNotifyTargetAndWait(static_cast<ViewModelCollectionBase::NotifyTarget&>(*this));
    }

    ControlBinding::Detach();
}

void ComboBoxBinding::OnActivated(int nIndex)
{
    if (IsDetached() || m_pItems == nullptr || m_pSelectedProperty == nullptr || nIndex < 0)
        return;

    SetValueFromControl(*m_pSelectedProperty, m_pItems->GetItemValue(nIndex, LookupItemViewModel::IdProperty));
}

QStringList ComboBoxBinding::ReadLabels() const
{
    QStringList vLabels;
    if (m_pItems != nullptr)
    {
        const auto nCount = gsl::narrow_cast<gsl::index>(m_pItems->Count());
        for (gsl::index nIndex = 0; nIndex < nCount; ++nIndex)
            vLabels.append(QString::fromStdWString(m_pItems->GetItemValue(nIndex, LookupItemViewModel::LabelProperty)));
    }

    return vLabels;
}

int ComboBoxBinding::FindIndex(int nId) const
{
    if (m_pItems == nullptr)
        return -1;

    return gsl::narrow_cast<int>(m_pItems->FindItemIndex(LookupItemViewModel::IdProperty, nId));
}

void ComboBoxBinding::PostRefresh()
{
    // Any thread, while attached: everything is read now, and the posted work touches only the control.
    QComboBox* pComboBox = m_pComboBox.load();
    if (pComboBox == nullptr)
        return; // SetControl reads it itself
    if (m_pItems != nullptr && m_pItems->IsUpdating())
        return; // the batch's end refreshes

    const int nSelected = (m_pSelectedProperty != nullptr) ? FindIndex(GetValue(*m_pSelectedProperty)) : -1;
    Post(*pComboBox, [pComboBox, vLabels = ReadLabels(), nSelected]() {
        pComboBox->clear();
        pComboBox->addItems(vLabels);
        pComboBox->setCurrentIndex(nSelected);
    });
}

void ComboBoxBinding::OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args)
{
    // Any thread, while attached.
    if (m_pSelectedProperty == nullptr || !(args.Property == *m_pSelectedProperty) || IsEchoOfOwnChange(args.Property))
        return;

    QComboBox* pComboBox = m_pComboBox.load();
    if (pComboBox == nullptr)
        return;

    const int nIndex = FindIndex(args.tNewValue);
    Post(*pComboBox, [pComboBox, nIndex]() { pComboBox->setCurrentIndex(nIndex); });
}

void ComboBoxBinding::OnViewModelStringValueChanged(gsl::index, const StringModelProperty::ChangeArgs& args)
{
    if (args.Property == LookupItemViewModel::LabelProperty)
        PostRefresh();
}

void ComboBoxBinding::OnViewModelAdded(gsl::index)
{
    PostRefresh();
}

void ComboBoxBinding::OnViewModelRemoved(gsl::index)
{
    PostRefresh();
}

void ComboBoxBinding::OnEndViewModelCollectionUpdate()
{
    PostRefresh();
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra
