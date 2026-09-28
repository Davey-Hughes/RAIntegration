#include "ui/qt/bindings/ComboBoxBinding.hh"

#include "ui/qt/bindings/Post.hh"

#include "services/ServiceLocator.hh"

#include <QComboBox>
#include <QVariant>

#include <cassert>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

using ra::ui::viewmodels::LookupItemViewModel;
using ra::ui::viewmodels::LookupItemViewModelCollection;

namespace {

// The id to select, kept on the control rather than read from the view model when a refresh runs: posted work never
// touches the view model, and a refresh queued before a newer selection must not bring the older one back.
constexpr const char* SelectedIdProperty = "raSelectedId";

} // namespace

ComboBoxBinding::~ComboBoxBinding() noexcept
{
    Detach();
}

void ComboBoxBinding::BindItems(const LookupItemViewModelCollection& vmItems) noexcept
{
    assert(m_pComboBox.load() == nullptr); // before SetControl: the handlers read it
    m_pItems = &vmItems;
    m_pTrackedItems = nullptr;
}

void ComboBoxBinding::BindItems(LookupItemViewModelCollection& vmItems) noexcept
{
    assert(m_pComboBox.load() == nullptr); // before SetControl: the handlers read it
    m_pItems = &vmItems;
    m_pTrackedItems = vmItems.IsFrozen() ? nullptr : &vmItems; // a frozen collection never changes
}

void ComboBoxBinding::BindSelectedItem(const IntModelProperty& pProperty) noexcept
{
    assert(m_pComboBox.load() == nullptr); // before SetControl: the handlers read it
    m_pSelectedProperty = &pProperty;
}

void ComboBoxBinding::SetControl(QComboBox& oComboBox)
{
    assert(m_pComboBox.load() == nullptr); // once

    // Joined first - the items, then the view model - then published, then read: a change from here on either
    // reaches a handler or is read below.
    if (m_pTrackedItems != nullptr && !IsDetached())
    {
        m_pTrackedItems->AddNotifyTarget(static_cast<ViewModelCollectionBase::NotifyTarget&>(*this));
        m_bTracking = true;
    }
    Attach();
    m_pComboBox.store(&oComboBox);

    if (m_pSelectedProperty != nullptr)
        oComboBox.setProperty(SelectedIdProperty, GetValue(*m_pSelectedProperty));
    Fill(oComboBox, ReadItems());

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
    // The user chose an item the control shows, so the id is the control's: the collection may have changed since.
    QComboBox* pComboBox = m_pComboBox.load();
    if (IsDetached() || pComboBox == nullptr || m_pSelectedProperty == nullptr || nIndex < 0)
        return;

    const QVariant vId = pComboBox->itemData(nIndex);
    if (!vId.isValid())
        return;

    // Stored here too: SetValueFromControl suppresses the echo that would otherwise store it, and a refresh already
    // queued re-selects whatever is stored.
    const int nId = vId.toInt();
    pComboBox->setProperty(SelectedIdProperty, nId);
    SetValueFromControl(*m_pSelectedProperty, nId);
}

ComboBoxBinding::Items ComboBoxBinding::ReadItems() const
{
    Items vItems;
    if (m_pItems != nullptr)
    {
        const auto nCount = gsl::narrow_cast<gsl::index>(m_pItems->Count());
        vItems.reserve(gsl::narrow_cast<size_t>(nCount));
        for (gsl::index nIndex = 0; nIndex < nCount; ++nIndex)
        {
            vItems.emplace_back(
                QString::fromStdWString(m_pItems->GetItemValue(nIndex, LookupItemViewModel::LabelProperty)),
                m_pItems->GetItemValue(nIndex, LookupItemViewModel::IdProperty));
        }
    }

    return vItems;
}

void ComboBoxBinding::PostRefresh()
{
    // The thread changing the items, while attached: they are read now, and the posted work touches only the control.
    QComboBox* pComboBox = m_pComboBox.load();
    if (pComboBox == nullptr)
        return; // SetControl reads them itself
    if (m_pItems != nullptr && m_pItems->IsUpdating())
        return; // the batch's end refreshes

    Post(*pComboBox, [pComboBox, vItems = ReadItems()]() { Fill(*pComboBox, vItems); });
}

void ComboBoxBinding::Fill(QComboBox& oComboBox, const Items& vItems)
{
    oComboBox.clear();
    for (const auto& pItem : vItems)
        oComboBox.addItem(pItem.first, pItem.second);

    SelectStoredId(oComboBox);
}

void ComboBoxBinding::SelectStoredId(QComboBox& oComboBox)
{
    // No id stored (no selected property) selects nothing, as does an id no item has.
    const QVariant vId = oComboBox.property(SelectedIdProperty);
    oComboBox.setCurrentIndex(vId.isValid() ? oComboBox.findData(vId) : -1);
}

void ComboBoxBinding::OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args)
{
    // Any thread, while attached. Only the id is posted: it is looked up among the items the control shows when the
    // post runs, not in the collection now.
    if (m_pSelectedProperty == nullptr || !(args.Property == *m_pSelectedProperty) || IsEchoOfOwnChange(args.Property))
        return;

    QComboBox* pComboBox = m_pComboBox.load();
    if (pComboBox == nullptr)
        return;

    Post(*pComboBox, [pComboBox, nId = args.tNewValue]() {
        pComboBox->setProperty(SelectedIdProperty, nId);
        SelectStoredId(*pComboBox);
    });
}

void ComboBoxBinding::OnViewModelIntValueChanged(gsl::index, const IntModelProperty::ChangeArgs& args)
{
    if (args.Property == LookupItemViewModel::IdProperty)
        PostRefresh();
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

void ComboBoxBinding::OnViewModelChanged(gsl::index)
{
    // raised only for the items a move displaced: UpdateIndices renumbers the items an add or remove shifted without
    // raising events
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
