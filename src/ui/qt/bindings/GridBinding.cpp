#include "ui/qt/bindings/GridBinding.hh"

#include "ui/qt/bindings/GridCheckBoxColumnBinding.hh"
#include "ui/qt/bindings/Post.hh"

#include "services/ServiceLocator.hh"

#include <QAbstractTableModel>
#include <QEvent>
#include <QHeaderView>
#include <QHelpEvent>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QTableView>
#include <QToolTip>

#include <algorithm>
#include <cassert>
#include <functional>
#include <utility>

namespace ra {
namespace ui {
namespace qt {
namespace bindings {

namespace detail {

/// <summary>
/// The view's side of a GridBinding, on the Qt thread: the rows shown, the header and widths, and the user's clicks
/// and keys, which it hands to the binding through its write function.
/// </summary>
/// <remarks>
/// A child of the view, so posted work whose context it is is dropped once the view is gone. The binding clears the
/// write function when it detaches; nothing here reaches the binding after that.
/// </remarks>
class GridModel final : public QAbstractTableModel
{
public:
    struct Column
    {
        QString sHeader;
        GridColumnBinding::WidthType nWidthType;
        int nWidth;
        Qt::Alignment nAlignment;
        bool bCheckBox;
    };

    GridModel(QTableView& oView, std::vector<Column> vColumns);

    int rowCount(const QModelIndex& oParent = QModelIndex()) const override;
    int columnCount(const QModelIndex& oParent = QModelIndex()) const override;
    QVariant data(const QModelIndex& oIndex, int nRole) const override;
    QVariant headerData(int nSection, Qt::Orientation nOrientation, int nRole) const override;
    Qt::ItemFlags flags(const QModelIndex& oIndex) const override;
    bool setData(const QModelIndex& oIndex, const QVariant& vValue, int nRole) override;

    /// <summary>Shows <paramref name="vRows" />, read when the binding's structure count was <paramref name="nStructure" />.</summary>
    void ShowRows(std::vector<GridRow> vRows, unsigned int nStructure);

    /// <summary>Shows one row, unless the rows shown were read at another structure count: a refresh is queued then.</summary>
    void ShowRow(gsl::index nRow, GridRow oRow, unsigned int nStructure);

    /// <summary>Sets every column's width from the viewport's: Win32's UpdateLayout.</summary>
    void UpdateLayout();

    std::function<void(gsl::index nRow, gsl::index nColumn, bool bChecked, unsigned int nStructure)> fWriteCheck;

protected:
    bool eventFilter(QObject* pWatched, QEvent* pEvent) override;

private:
    int CheckBoxWidth() const;

    QTableView& m_oView;
    std::vector<Column> m_vColumns;
    std::vector<GridRow> m_vRows;
    unsigned int m_nShownStructure = 0;
};

/// <summary>Shows a cell's whole text as its tooltip when the cell cuts it short: Win32's LVS_EX_LABELTIP.</summary>
class GridDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    bool helpEvent(QHelpEvent* pEvent, QAbstractItemView* pView, const QStyleOptionViewItem& oOption,
                   const QModelIndex& oIndex) override
    {
        if (pEvent == nullptr || pView == nullptr || pEvent->type() != QEvent::ToolTip)
            return QStyledItemDelegate::helpEvent(pEvent, pView, oOption, oIndex);

        QStyleOptionViewItem oItem(oOption);
        initStyleOption(&oItem, oIndex);

        // QCommonStyle elides within the text rect less a margin on each side (viewItemDrawText)
        const QStyle* pStyle = pView->style();
        const int nMargin = pStyle->pixelMetric(QStyle::PM_FocusFrameHMargin, &oItem, pView) + 1;
        const QRect rcText = pStyle->subElementRect(QStyle::SE_ItemViewItemText, &oItem, pView);
        if (!oItem.text.isEmpty() && oItem.fontMetrics.horizontalAdvance(oItem.text) > rcText.width() - 2 * nMargin)
            QToolTip::showText(pEvent->globalPos(), oItem.text, pView, pView->visualRect(oIndex));
        else
            QToolTip::hideText();

        return true;
    }
};

GridModel::GridModel(QTableView& oView, std::vector<Column> vColumns)
    : QAbstractTableModel(&oView), m_oView(oView), m_vColumns(std::move(vColumns))
{
    oView.setModel(this);
    oView.setItemDelegate(new GridDelegate(&oView));
    oView.setSelectionBehavior(QAbstractItemView::SelectRows); // Win32's LVS_EX_FULLROWSELECT
    oView.setShowGrid(false);                                  // Win32 draws no grid lines unless asked
    oView.setWordWrap(false);
    oView.verticalHeader()->hide();

    auto* pHeader = oView.horizontalHeader();
    pHeader->setSectionResizeMode(QHeaderView::Fixed); // Win32's LVCFMT_FIXED_WIDTH: the widths are the binding's
    pHeader->setMinimumSectionSize(0);                 // or a style's minimum (38 px Fusion, 46 Breeze) overrides them
    pHeader->setStretchLastSection(false);
    pHeader->setSectionsClickable(false);              // nothing sorts yet (Win32: LVS_NOSORTHEADER here)
    pHeader->setHighlightSections(false);

    oView.installEventFilter(this);             // Space
    oView.viewport()->installEventFilter(this); // its width

    UpdateLayout();
}

int GridModel::rowCount(const QModelIndex& oParent) const
{
    return oParent.isValid() ? 0 : gsl::narrow_cast<int>(m_vRows.size());
}

int GridModel::columnCount(const QModelIndex& oParent) const
{
    return oParent.isValid() ? 0 : gsl::narrow_cast<int>(m_vColumns.size());
}

QVariant GridModel::data(const QModelIndex& oIndex, int nRole) const
{
    if (!oIndex.isValid() || ra::to_unsigned(oIndex.row()) >= m_vRows.size() ||
        ra::to_unsigned(oIndex.column()) >= m_vColumns.size())
    {
        return QVariant();
    }

    const auto& oCell = m_vRows.at(oIndex.row()).vCells.at(oIndex.column());
    switch (nRole)
    {
        case Qt::DisplayRole:
            return oCell.sText.isEmpty() ? QVariant() : QVariant(oCell.sText);

        case Qt::CheckStateRole:
            if (!oCell.bChecked.has_value())
                return QVariant();
            return *oCell.bChecked ? Qt::Checked : Qt::Unchecked;

        case Qt::TextAlignmentRole:
            return QVariant::fromValue(m_vColumns.at(oIndex.column()).nAlignment);

        default:
            return QVariant();
    }
}

QVariant GridModel::headerData(int nSection, Qt::Orientation nOrientation, int nRole) const
{
    if (nOrientation != Qt::Horizontal || nSection < 0 || ra::to_unsigned(nSection) >= m_vColumns.size())
        return QVariant();

    if (nRole == Qt::DisplayRole)
        return m_vColumns.at(nSection).sHeader;
    if (nRole == Qt::TextAlignmentRole)
        return QVariant::fromValue(m_vColumns.at(nSection).nAlignment);

    return QVariant();
}

Qt::ItemFlags GridModel::flags(const QModelIndex& oIndex) const
{
    Qt::ItemFlags nFlags = Qt::ItemIsEnabled | Qt::ItemIsSelectable;
    if (oIndex.isValid() && ra::to_unsigned(oIndex.column()) < m_vColumns.size() && m_vColumns.at(oIndex.column()).bCheckBox)
        nFlags |= Qt::ItemIsUserCheckable;

    return nFlags;
}

bool GridModel::setData(const QModelIndex& oIndex, const QVariant& vValue, int nRole)
{
    // Only the user's tick: the delegate takes the press on the box, so a click is one change (measured). The rows are
    // not changed here: the view model's answer comes back as a queued row refresh.
    if (nRole != Qt::CheckStateRole || !oIndex.isValid() || ra::to_unsigned(oIndex.column()) >= m_vColumns.size() ||
        !m_vColumns.at(oIndex.column()).bCheckBox || !fWriteCheck)
    {
        return false;
    }

    fWriteCheck(oIndex.row(), oIndex.column(), vValue.toInt() == Qt::Checked, m_nShownStructure);
    return true;
}

void GridModel::ShowRows(std::vector<GridRow> vRows, unsigned int nStructure)
{
    beginResetModel();
    m_vRows = std::move(vRows);
    m_nShownStructure = nStructure;
    endResetModel();
}

void GridModel::ShowRow(gsl::index nRow, GridRow oRow, unsigned int nStructure)
{
    if (nStructure != m_nShownStructure || nRow < 0 || ra::to_unsigned(nRow) >= m_vRows.size())
        return;

    m_vRows.at(nRow) = std::move(oRow);
    emit dataChanged(index(gsl::narrow_cast<int>(nRow), 0),
                     index(gsl::narrow_cast<int>(nRow), gsl::narrow_cast<int>(m_vColumns.size()) - 1));
}

bool GridModel::eventFilter(QObject* pWatched, QEvent* pEvent)
{
    if (pWatched == m_oView.viewport() && pEvent->type() == QEvent::Resize)
    {
        UpdateLayout();
    }
    else if (pWatched == &m_oView && pEvent->type() == QEvent::KeyPress)
    {
        // Space toggles the current row's tick wherever the cursor is, as on Win32 (Qt toggles it only when the tick's
        // own cell is current).
        const auto* pKeyEvent = static_cast<const QKeyEvent*>(pEvent);
        if (pKeyEvent->key() == Qt::Key_Space && pKeyEvent->modifiers() == Qt::NoModifier)
        {
            const auto pColumn = std::find_if(m_vColumns.begin(), m_vColumns.end(),
                                              [](const Column& oColumn) { return oColumn.bCheckBox; });
            const int nRow = m_oView.currentIndex().row();
            if (pColumn != m_vColumns.end() && nRow >= 0 && ra::to_unsigned(nRow) < m_vRows.size())
            {
                const auto nColumn = std::distance(m_vColumns.begin(), pColumn);
                if (fWriteCheck)
                {
                    const bool bChecked = m_vRows.at(nRow).vCells.at(nColumn).bChecked.value_or(false);
                    fWriteCheck(nRow, nColumn, !bChecked, m_nShownStructure);
                }
                return true;
            }
        }
    }

    return QAbstractTableModel::eventFilter(pWatched, pEvent);
}

int GridModel::CheckBoxWidth() const
{
    // The narrowest cell that holds the style's tick: 20 px for Fusion, 34 for Breeze (measured).
    QStyleOptionViewItem oOption;
    oOption.initFrom(m_oView.viewport());
    oOption.widget = &m_oView;
    oOption.font = m_oView.font();
    oOption.fontMetrics = QFontMetrics(m_oView.font());
    oOption.features = QStyleOptionViewItem::HasCheckIndicator;
    oOption.checkState = Qt::Checked;
    return m_oView.style()->sizeFromContents(QStyle::CT_ItemViewItem, &oOption, QSize(), &m_oView).width();
}

void GridModel::UpdateLayout()
{
    const int nWidth = m_oView.viewport()->width();
    int nRemaining = nWidth;
    int nFillParts = 0;
    std::vector<int> vWidths(m_vColumns.size(), 0);

    for (size_t nColumn = 0; nColumn < m_vColumns.size(); ++nColumn)
    {
        const auto& oColumn = m_vColumns.at(nColumn);
        switch (oColumn.nWidthType)
        {
            case GridColumnBinding::WidthType::Pixels:
                // Qt's logical pixels already absorb the display's scale; a tick column never cuts its tick
                vWidths.at(nColumn) = oColumn.bCheckBox ? std::max(oColumn.nWidth, CheckBoxWidth()) : oColumn.nWidth;
                break;

            case GridColumnBinding::WidthType::Percentage:
                vWidths.at(nColumn) = nWidth * oColumn.nWidth / 100;
                break;

            default:
                nFillParts += oColumn.nWidth;
                break;
        }

        nRemaining -= vWidths.at(nColumn);
    }

    if (nFillParts > 0)
    {
        nRemaining = std::max(nRemaining, 0);
        for (size_t nColumn = 0; nColumn < m_vColumns.size(); ++nColumn)
        {
            const auto& oColumn = m_vColumns.at(nColumn);
            if (oColumn.nWidthType == GridColumnBinding::WidthType::Fill)
                vWidths.at(nColumn) = nRemaining * oColumn.nWidth / nFillParts;
        }
    }

    auto* pHeader = m_oView.horizontalHeader();
    for (size_t nColumn = 0; nColumn < vWidths.size(); ++nColumn)
        pHeader->resizeSection(gsl::narrow_cast<int>(nColumn), vWidths.at(nColumn));
}

} // namespace detail

namespace {

Qt::Alignment ToQtAlignment(ra::ui::RelativePosition nAlignment) noexcept
{
    switch (nAlignment)
    {
        case ra::ui::RelativePosition::Center:
            return Qt::AlignHCenter | Qt::AlignVCenter;
        case ra::ui::RelativePosition::Far:
            return Qt::AlignRight | Qt::AlignVCenter;
        default:
            return Qt::AlignLeft | Qt::AlignVCenter;
    }
}

} // namespace

GridBinding::~GridBinding() noexcept
{
    Detach();
}

void GridBinding::BindColumn(gsl::index nColumn, std::unique_ptr<GridColumnBinding> pColumnBinding)
{
    assert(m_pModel.load() == nullptr); // before SetControl: the handlers read the columns
    if (m_vColumns.size() <= ra::to_unsigned(nColumn))
        m_vColumns.resize(ra::to_unsigned(nColumn) + 1);

    m_vColumns.at(nColumn) = std::move(pColumnBinding);
}

void GridBinding::BindItems(ViewModelCollectionBase& vmItems) noexcept
{
    assert(m_pModel.load() == nullptr); // before SetControl: the handlers read it
    m_pItems = &vmItems;
}

void GridBinding::SetControl(QTableView& oView)
{
    assert(m_pModel.load() == nullptr); // once

    std::vector<detail::GridModel::Column> vColumns;
    vColumns.reserve(m_vColumns.size());
    for (const auto& pColumn : m_vColumns)
    {
        assert(pColumn != nullptr); // numbered with no gaps
        vColumns.push_back({QString::fromStdWString(pColumn->GetHeader()), pColumn->GetWidthType(), pColumn->GetWidth(),
                            ToQtAlignment(pColumn->GetAlignment()),
                            dynamic_cast<const GridCheckBoxColumnBinding*>(pColumn.get()) != nullptr});
    }

    auto* pModel = new detail::GridModel(oView, std::move(vColumns));
    m_pQtModel = pModel;
    if (!IsDetached())
    {
        pModel->fWriteCheck = [this](gsl::index nRow, gsl::index nColumn, bool bChecked, unsigned int nStructure) {
            WriteCheck(nRow, nColumn, bChecked, nStructure);
        };
    }

    // Joined first - the items, then the view model - then published, then read: a change from here on either reaches
    // a handler or is read below. A handler bumps the structure count before it looks for the model.
    if (m_pItems != nullptr && !m_pItems->IsFrozen() && !IsDetached())
    {
        m_pItems->AddNotifyTarget(static_cast<ViewModelCollectionBase::NotifyTarget&>(*this));
        m_bTracking = true;
    }
    Attach();
    m_pModel.store(pModel);

    const unsigned int nStructure = m_nStructure.load();
    pModel->ShowRows(ReadRows(), nStructure);
}

void GridBinding::Detach() noexcept
{
    if (m_pQtModel != nullptr)
    {
        // Qt thread: the user's clicks no longer reach the binding, which may be destroyed before the view. The one
        // thing that stops a write after Detach.
        m_pQtModel->fWriteCheck = nullptr;
    }

    if (m_bTracking)
    {
        m_bTracking = false;

        // waits for an item handler running on another thread, as the view model's detach below does
        if (ra::services::ServiceLocator::IsInitialized())
            m_pItems->RemoveNotifyTargetAndWait(static_cast<ViewModelCollectionBase::NotifyTarget&>(*this));
    }

    ControlBinding::Detach();
}

detail::GridRow GridBinding::ReadRow(gsl::index nIndex) const
{
    detail::GridRow oRow;
    oRow.vCells.reserve(m_vColumns.size());
    for (const auto& pColumn : m_vColumns)
        oRow.vCells.push_back(pColumn->GetCell(*m_pItems, nIndex));

    return oRow;
}

std::vector<detail::GridRow> GridBinding::ReadRows() const
{
    std::vector<detail::GridRow> vRows;
    if (m_pItems != nullptr)
    {
        const auto nCount = gsl::narrow_cast<gsl::index>(m_pItems->Count());
        vRows.reserve(gsl::narrow_cast<size_t>(nCount));
        for (gsl::index nIndex = 0; nIndex < nCount; ++nIndex)
            vRows.push_back(ReadRow(nIndex));
    }

    return vRows;
}

void GridBinding::PostRow(gsl::index nIndex)
{
    // The thread changing the item, while attached: the row is read now, and the queued work touches only the model.
    const unsigned int nStructure = m_nStructure.load();
    auto* pModel = m_pModel.load();
    if (pModel == nullptr || m_pItems->IsUpdating())
        return; // SetControl reads it itself / the batch's end refreshes every row

    PostQueued(*pModel, [pModel, nIndex, oRow = ReadRow(nIndex), nStructure]() mutable {
        pModel->ShowRow(nIndex, std::move(oRow), nStructure);
    });
}

void GridBinding::PostAllRows()
{
    // Bumped before the rows are read, so the count a refresh carries is never older than what it read.
    const unsigned int nStructure = ++m_nStructure;
    auto* pModel = m_pModel.load();
    if (pModel == nullptr || m_pItems->IsUpdating())
        return; // SetControl reads them itself / the batch's end refreshes

    PostQueued(*pModel, [pModel, vRows = ReadRows(), nStructure]() mutable {
        pModel->ShowRows(std::move(vRows), nStructure);
    });
}

void GridBinding::WriteCheck(gsl::index nRow, gsl::index nColumn, bool bChecked, unsigned int nShownStructure)
{
    // The row number is an index into the items only if nothing has been added, removed or moved since the rows shown
    // were read: otherwise drop the click rather than tick another item. The refresh is on its way.
    if (nShownStructure != m_nStructure.load())
        return;

    const auto* pColumn = dynamic_cast<const GridCheckBoxColumnBinding*>(m_vColumns.at(nColumn).get());
    if (pColumn != nullptr)
        m_pItems->SetItemValue(nRow, pColumn->GetBoundProperty(), bChecked);
}

void GridBinding::OnViewModelBoolValueChanged(gsl::index nIndex, const BoolModelProperty::ChangeArgs& args)
{
    if (std::any_of(m_vColumns.begin(), m_vColumns.end(),
                    [&args](const auto& pColumn) { return pColumn->DependsOn(args.Property); }))
    {
        PostRow(nIndex);
    }
}

void GridBinding::OnViewModelIntValueChanged(gsl::index nIndex, const IntModelProperty::ChangeArgs& args)
{
    if (std::any_of(m_vColumns.begin(), m_vColumns.end(),
                    [&args](const auto& pColumn) { return pColumn->DependsOn(args.Property); }))
    {
        PostRow(nIndex);
    }
}

void GridBinding::OnViewModelStringValueChanged(gsl::index nIndex, const StringModelProperty::ChangeArgs& args)
{
    if (std::any_of(m_vColumns.begin(), m_vColumns.end(),
                    [&args](const auto& pColumn) { return pColumn->DependsOn(args.Property); }))
    {
        PostRow(nIndex);
    }
}

void GridBinding::OnViewModelAdded(gsl::index)
{
    PostAllRows();
}

void GridBinding::OnViewModelRemoved(gsl::index)
{
    PostAllRows();
}

void GridBinding::OnViewModelChanged(gsl::index)
{
    // raised for the items a move displaced
    PostAllRows();
}

void GridBinding::OnBeginViewModelCollectionUpdate()
{
    // the batch may add, remove or move: a click meanwhile is dropped
    ++m_nStructure;
}

void GridBinding::OnEndViewModelCollectionUpdate()
{
    PostAllRows();
}

} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra
