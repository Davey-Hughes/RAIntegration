#include "ui/qt/BrokenAchievementsDialog.hh"

#include "ui/qt/bindings/GridBooleanColumnBinding.hh"
#include "ui/qt/bindings/GridCheckBoxColumnBinding.hh"
#include "ui/qt/bindings/GridTextColumnBinding.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QDialogButtonBox>
#include <QLabel>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::qt::bindings::GridColumnBinding;
using ra::ui::viewmodels::BrokenAchievementsViewModel;

bool BrokenAchievementsDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const BrokenAchievementsViewModel*>(&vmWindow) != nullptr;
}

void BrokenAchievementsDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    RA_LOG_WARN("Report achievement problem \"%s\" shown without waiting for an answer - not shown; use ShowModal",
                ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
}

std::unique_ptr<QDialog> BrokenAchievementsDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    return std::make_unique<BrokenAchievementsDialog>(dynamic_cast<BrokenAchievementsViewModel&>(vmWindow));
}

BrokenAchievementsDialog::BrokenAchievementsDialog(BrokenAchievementsViewModel& vmBrokenAchievements)
    : ModalDialogBase(vmBrokenAchievements),
      m_vmBrokenAchievements(vmBrokenAchievements),
      m_bindAchievements(vmBrokenAchievements)
{
    // Win32's text, columns and widths (RA_Shared.rc IDD_RA_REPORTBROKENACHIEVEMENTS, BrokenAchievementsDialog.cpp)
    auto* pPrompt = new QLabel(QStringLiteral("Which achievement would you like to report?"), this);

    auto* pList = new QTableView(this);
    pList->setObjectName(QStringLiteral("Achievements"));
    pList->setSelectionMode(QAbstractItemView::SingleSelection); // LVS_SINGLESEL

    using Item = BrokenAchievementsViewModel::BrokenAchievementViewModel;

    auto pSelectedColumn = std::make_unique<ra::ui::qt::bindings::GridCheckBoxColumnBinding>(Item::IsSelectedProperty);
    pSelectedColumn->SetWidth(GridColumnBinding::WidthType::Pixels, 20);
    m_bindAchievements.BindColumn(0, std::move(pSelectedColumn));

    auto pTitleColumn = std::make_unique<ra::ui::qt::bindings::GridTextColumnBinding>(Item::LabelProperty);
    pTitleColumn->SetHeader(L"Title");
    pTitleColumn->SetWidth(GridColumnBinding::WidthType::Fill, 20);
    m_bindAchievements.BindColumn(1, std::move(pTitleColumn));

    auto pDescriptionColumn = std::make_unique<ra::ui::qt::bindings::GridTextColumnBinding>(Item::DescriptionProperty);
    pDescriptionColumn->SetHeader(L"Description");
    pDescriptionColumn->SetWidth(GridColumnBinding::WidthType::Fill, 40);
    m_bindAchievements.BindColumn(2, std::move(pDescriptionColumn));

    auto pAchievedColumn =
        std::make_unique<ra::ui::qt::bindings::GridBooleanColumnBinding>(Item::IsAchievedProperty, L"Yes", L"No");
    pAchievedColumn->SetHeader(L"Achieved");
    pAchievedColumn->SetWidth(GridColumnBinding::WidthType::Pixels, 64);
    m_bindAchievements.BindColumn(3, std::move(pAchievedColumn));

    m_bindAchievements.BindItems(vmBrokenAchievements.Achievements());

    // Linux only (disclosed): a selected row is a ticked one. Win32 does not bind the selection.
    m_bindAchievements.BindIsSelected(Item::IsSelectedProperty);

    m_bindAchievements.SetControl(*pList);
    RegisterBinding(m_bindAchievements);

    auto* pButtons = CreateButtons();
    pButtons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Report Problem"));

    auto* pLayout = new QVBoxLayout(this);
    pLayout->addWidget(pPrompt);
    pLayout->addWidget(pList, 1);
    pLayout->addWidget(pButtons);

    setMinimumSize(416, 322); // Win32's SetMinimumSize
    resize(593, 416);         // the .rc's 339x222 dialog units at Segoe UI 9 (1.75 x 1.875 px each)

    m_bindWindow.SetWidget(*this);
}

bool BrokenAchievementsDialog::CanAccept()
{
    // Win32's OnCommand(IDOK): Submit shows why when there is nothing to report, and the dialog stays open
    return m_vmBrokenAchievements.Submit();
}

} // namespace qt
} // namespace ui
} // namespace ra
