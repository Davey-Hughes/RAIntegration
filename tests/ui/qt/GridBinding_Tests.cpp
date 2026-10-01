#ifndef _WIN32

#include "ui/qt/bindings/GridBinding.hh"

#include "ui/qt/bindings/GridBooleanColumnBinding.hh"
#include "ui/qt/bindings/GridCheckBoxColumnBinding.hh"
#include "ui/qt/bindings/GridTextColumnBinding.hh"
#include "ui/viewmodels/LookupItemViewModel.hh"

#include "tests/ui/qt/ItemViewInput.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QAbstractItemModel>
#include <QHeaderView>
#include <QProxyStyle>
#include <QTableView>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <thread>
#include <vector>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {
namespace tests {

using ra::ui::qt::tests::QtTestHost;
using ra::ui::viewmodels::LookupItemViewModel;

namespace {

class OwnerViewModel : public ViewModelBase
{
};

// Report a Problem's shape: a label, a description and a done flag; LookupItemViewModel brings IsSelected.
class RowViewModel : public LookupItemViewModel
{
public:
    static const StringModelProperty DescriptionProperty;
    static const BoolModelProperty IsDoneProperty;

    void SetDescription(const std::wstring& sValue) { SetValue(DescriptionProperty, sValue); }
    void SetDone(bool bValue) { SetValue(IsDoneProperty, bValue); }
};

const StringModelProperty RowViewModel::DescriptionProperty("GridBindingTests", "Description", L"");
const BoolModelProperty RowViewModel::IsDoneProperty("GridBindingTests", "IsDone", false);

using Rows = ViewModelCollection<RowViewModel>;

// Title n, Description n, done when n is odd.
void AddRows(Rows& vmItems, int nCount)
{
    for (int nIndex = 0; nIndex < nCount; ++nIndex)
    {
        auto& vmRow = vmItems.Add();
        vmRow.SetLabel(L"Title " + std::to_wstring(nIndex));
        vmRow.SetDescription(L"Description " + std::to_wstring(nIndex));
        vmRow.SetDone(nIndex % 2 == 1);
    }
}

// A style whose tick is 40 px wide: wider than the column's 20.
class WideTickStyle : public QProxyStyle
{
public:
    WideTickStyle() : QProxyStyle(QStringLiteral("Fusion")) {}

    int pixelMetric(PixelMetric nMetric, const QStyleOption* pOption = nullptr,
                    const QWidget* pWidget = nullptr) const override
    {
        if (nMetric == PM_IndicatorWidth || nMetric == PM_IndicatorHeight)
            return 40;
        return QProxyStyle::pixelMetric(nMetric, pOption, pWidget);
    }
};

struct BoundGrid
{
    explicit BoundGrid(ViewModelBase& vmOwner) : oBinding(vmOwner) {}

    std::unique_ptr<QStyle> pStyle; // outlives the view
    QTableView oView;
    GridBinding oBinding; // detaches before the view goes
};

constexpr int TickColumn = 0;
constexpr int TitleColumn = 1;
constexpr int DescriptionColumn = 2;
constexpr int DoneColumn = 3;

struct GridOptions
{
    bool bWideTick = false;                          // WideTickStyle
    std::function<GridColumnBinding*()> fExtraColumn; // a fifth column
};

// Report a Problem's columns over vmItems, in a shown 500x300 view.
BoundGrid* Create(QtTestHost& oQt, OwnerViewModel& vmOwner, Rows& vmItems, const GridOptions& oOptions = {})
{
    BoundGrid* pGrid = nullptr;
    oQt.RunOnQt([&vmOwner, &vmItems, &oOptions, &pGrid]() {
        pGrid = new BoundGrid(vmOwner);
        if (oOptions.bWideTick)
        {
            pGrid->pStyle = std::make_unique<WideTickStyle>();
            pGrid->oView.setStyle(pGrid->pStyle.get());
        }
        pGrid->oView.setSelectionMode(QAbstractItemView::SingleSelection);

        auto pTick = std::make_unique<GridCheckBoxColumnBinding>(LookupItemViewModel::IsSelectedProperty);
        pTick->SetWidth(GridColumnBinding::WidthType::Pixels, 20);
        pGrid->oBinding.BindColumn(TickColumn, std::move(pTick));

        auto pTitle = std::make_unique<GridTextColumnBinding>(LookupItemViewModel::LabelProperty);
        pTitle->SetHeader(L"Title");
        pTitle->SetWidth(GridColumnBinding::WidthType::Fill, 20);
        pGrid->oBinding.BindColumn(TitleColumn, std::move(pTitle));

        auto pDescription = std::make_unique<GridTextColumnBinding>(RowViewModel::DescriptionProperty);
        pDescription->SetHeader(L"Description");
        pDescription->SetWidth(GridColumnBinding::WidthType::Fill, 40);
        pGrid->oBinding.BindColumn(DescriptionColumn, std::move(pDescription));

        auto pDone = std::make_unique<GridBooleanColumnBinding>(RowViewModel::IsDoneProperty, L"Yes", L"No");
        pDone->SetHeader(L"Done");
        pDone->SetWidth(GridColumnBinding::WidthType::Pixels, 64);
        pDone->SetAlignment(ra::ui::RelativePosition::Far);
        pGrid->oBinding.BindColumn(DoneColumn, std::move(pDone));

        if (oOptions.fExtraColumn)
            pGrid->oBinding.BindColumn(DoneColumn + 1, std::unique_ptr<GridColumnBinding>(oOptions.fExtraColumn()));

        pGrid->oBinding.BindItems(vmItems);
        pGrid->oBinding.SetControl(pGrid->oView);

        pGrid->oView.resize(500, 300);
        pGrid->oView.show();
    });
    return pGrid;
}

void Delete(QtTestHost& oQt, BoundGrid* pGrid)
{
    oQt.RunOnQt([pGrid]() { delete pGrid; });
}

// Qt thread only, as every helper below.
QString Text(const QTableView& oView, int nRow, int nColumn)
{
    return oView.model()->index(nRow, nColumn).data(Qt::DisplayRole).toString();
}

int CheckState(const QTableView& oView, int nRow)
{
    const QVariant vState = oView.model()->index(nRow, TickColumn).data(Qt::CheckStateRole);
    return vState.isValid() ? vState.toInt() : -1;
}

QPoint TickCentre(const QTableView& oView, int nRow)
{
    return ra::ui::qt::tests::TickCentre(oView, nRow, TickColumn);
}

// A user's click, with the Qt thread's queue run between the press and the release, and after: where a refresh
// queued by the press lands.
void Click(QtTestHost& oQt, BoundGrid* pGrid, std::function<QPoint(const QTableView&)> fWhere,
           Qt::KeyboardModifiers nModifiers = Qt::NoModifier)
{
    QPoint ptWhere;
    oQt.RunOnQt([pGrid, &fWhere, &ptWhere, nModifiers]() {
        ptWhere = fWhere(pGrid->oView);
        ra::ui::qt::tests::PressMouse(pGrid->oView, ptWhere, nModifiers);
    });
    oQt.RunOnQt([pGrid, ptWhere, nModifiers]() { ra::ui::qt::tests::ReleaseMouse(pGrid->oView, ptWhere, nModifiers); });
    oQt.RunOnQt([]() {});
}

void PressSpace(QtTestHost& oQt, BoundGrid* pGrid)
{
    oQt.RunOnQt([pGrid]() { ra::ui::qt::tests::PressKey(pGrid->oView, Qt::Key_Space, QStringLiteral(" ")); });
    oQt.RunOnQt([]() {});
}

bool IsSelected(Rows& vmItems, gsl::index nIndex)
{
    return vmItems.GetItemAt(nIndex)->IsSelected();
}

} // namespace

TEST_CLASS(GridBinding_Tests)
{
public:
    // --- what the view shows ---

    TEST_METHOD(TestEachColumnShowsItsValue)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        vmItems.GetItemAt(1)->SetSelected(true);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        int nRows = 0, nColumns = 0, nCheck0 = 0, nCheck1 = 0;
        QString sTitle, sDescription, sDone0, sDone1, sTickText;
        QVariant vTitleCheck;
        oQt.RunOnQt([pGrid, &nRows, &nColumns, &nCheck0, &nCheck1, &sTitle, &sDescription, &sDone0, &sDone1, &sTickText,
                     &vTitleCheck]() {
            const auto& oView = pGrid->oView;
            nRows = oView.model()->rowCount();
            nColumns = oView.model()->columnCount();
            nCheck0 = CheckState(oView, 0);
            nCheck1 = CheckState(oView, 1);
            sTitle = Text(oView, 2, TitleColumn);
            sDescription = Text(oView, 2, DescriptionColumn);
            sDone0 = Text(oView, 0, DoneColumn);
            sDone1 = Text(oView, 1, DoneColumn);
            sTickText = Text(oView, 0, TickColumn);
            vTitleCheck = oView.model()->index(0, TitleColumn).data(Qt::CheckStateRole);
        });
        Delete(oQt, pGrid);

        Assert::AreEqual(3, nRows);
        Assert::AreEqual(4, nColumns);
        Assert::AreEqual(static_cast<int>(Qt::Unchecked), nCheck0);
        Assert::AreEqual(static_cast<int>(Qt::Checked), nCheck1);
        Assert::AreEqual(std::wstring(L"Title 2"), sTitle.toStdWString());
        Assert::AreEqual(std::wstring(L"Description 2"), sDescription.toStdWString());
        Assert::AreEqual(std::wstring(L"No"), sDone0.toStdWString());
        Assert::AreEqual(std::wstring(L"Yes"), sDone1.toStdWString());
        Assert::IsTrue(sTickText.isEmpty(), L"the tick column shows text");
        Assert::IsFalse(vTitleCheck.isValid(), L"a text column shows a tick");
    }

    TEST_METHOD(TestTheHeaderShowsTheColumnsTitlesAndAlignment)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 1);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        QString sTick, sTitle, sDone;
        int nDoneAlignment = 0, nDoneCellAlignment = 0;
        bool bClickable = true, bFixed = false, bRowHeader = true;
        oQt.RunOnQt([pGrid, &sTick, &sTitle, &sDone, &nDoneAlignment, &nDoneCellAlignment, &bClickable, &bFixed,
                     &bRowHeader]() {
            const auto* pModel = pGrid->oView.model();
            sTick = pModel->headerData(TickColumn, Qt::Horizontal, Qt::DisplayRole).toString();
            sTitle = pModel->headerData(TitleColumn, Qt::Horizontal, Qt::DisplayRole).toString();
            sDone = pModel->headerData(DoneColumn, Qt::Horizontal, Qt::DisplayRole).toString();
            nDoneAlignment = pModel->headerData(DoneColumn, Qt::Horizontal, Qt::TextAlignmentRole).toInt();
            nDoneCellAlignment = pModel->index(0, DoneColumn).data(Qt::TextAlignmentRole).toInt();
            bClickable = pGrid->oView.horizontalHeader()->sectionsClickable();
            bFixed = pGrid->oView.horizontalHeader()->sectionResizeMode(TitleColumn) == QHeaderView::Fixed;
            bRowHeader = pGrid->oView.verticalHeader()->isVisible();
        });
        Delete(oQt, pGrid);

        Assert::IsTrue(sTick.isEmpty());
        Assert::AreEqual(std::wstring(L"Title"), sTitle.toStdWString());
        Assert::AreEqual(std::wstring(L"Done"), sDone.toStdWString());
        Assert::AreEqual(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter), nDoneAlignment);
        Assert::AreEqual(static_cast<int>(Qt::AlignRight | Qt::AlignVCenter), nDoneCellAlignment);
        Assert::IsFalse(bClickable, L"a click on the header would sort, which nothing does yet");
        Assert::IsTrue(bFixed, L"Win32's columns are LVCFMT_FIXED_WIDTH");
        Assert::IsFalse(bRowHeader, L"Win32's list has no row header");
    }

    TEST_METHOD(TestPixelsWidthsStayAndFillWidthsShareTheRest)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 2);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        struct Widths
        {
            int nViewport, nTick, nTitle, nDescription, nDone;
        };
        const auto fRead = [pGrid]() {
            const auto& oView = pGrid->oView;
            return Widths{oView.viewport()->width(), oView.columnWidth(TickColumn), oView.columnWidth(TitleColumn),
                          oView.columnWidth(DescriptionColumn), oView.columnWidth(DoneColumn)};
        };
        Widths oNarrow{}, oWide{};
        oQt.RunOnQt([&fRead, &oNarrow]() { oNarrow = fRead(); });
        oQt.RunOnQt([pGrid]() { pGrid->oView.resize(800, 300); });
        oQt.RunOnQt([&fRead, &oWide]() { oWide = fRead(); });
        Delete(oQt, pGrid);

        for (const auto& oWidths : {oNarrow, oWide})
        {
            Assert::AreEqual(20, oWidths.nTick, L"offscreen's Fusion tick fits 20 px");
            Assert::AreEqual(64, oWidths.nDone);
            const int nRemaining = oWidths.nViewport - 20 - 64;
            Assert::AreEqual(nRemaining * 20 / 60, oWidths.nTitle);
            Assert::AreEqual(nRemaining * 40 / 60, oWidths.nDescription);
        }
        Assert::IsTrue(oWide.nViewport > oNarrow.nViewport + 250, L"the resize did not reach the viewport");
    }

    TEST_METHOD(TestATickColumnIsNeverNarrowerThanTheStylesTick)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 1);
        QtTestHost oQt;
        GridOptions oOptions;
        oOptions.bWideTick = true;
        auto* pGrid = Create(oQt, vmOwner, vmItems, oOptions);

        int nTick = 0;
        oQt.RunOnQt([pGrid, &nTick]() { nTick = pGrid->oView.columnWidth(TickColumn); });
        Delete(oQt, pGrid);

        Assert::IsTrue(nTick >= 40, (L"tick column " + std::to_wstring(nTick) + L" px").c_str());
    }

    TEST_METHOD(TestACellThatCutsItsTextShortShowsItAsATooltip)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 2);
        const std::wstring sLong(200, L'x');
        vmItems.GetItemAt(1)->SetDescription(sLong);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        QString sShort, sCut;
        oQt.RunOnQt([pGrid, &sShort, &sCut]() {
            sShort = ra::ui::qt::tests::HoverTooltip(pGrid->oView, 0, DescriptionColumn); // first: none shown yet
            sCut = ra::ui::qt::tests::HoverTooltip(pGrid->oView, 1, DescriptionColumn);
        });
        Delete(oQt, pGrid);

        Assert::IsTrue(sShort.isEmpty(), (L"a cell that fits showed " + sShort.toStdWString()).c_str());
        Assert::AreEqual(sLong, sCut.toStdWString());
    }

    // --- following the items ---

    TEST_METHOD(TestAChangeOnAWorkerReachesItsRow)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        std::thread([&vmItems]() {
            vmItems.GetItemAt(1)->SetLabel(L"Changed");
            vmItems.GetItemAt(2)->SetDone(true);
            vmItems.GetItemAt(0)->SetSelected(true);
        }).join();
        const bool bReached = oQt.WaitOnQt([pGrid]() {
            return Text(pGrid->oView, 1, TitleColumn) == QStringLiteral("Changed") &&
                   Text(pGrid->oView, 2, DoneColumn) == QStringLiteral("Yes") &&
                   CheckState(pGrid->oView, 0) == Qt::Checked;
        });
        Delete(oQt, pGrid);

        Assert::IsTrue(bReached, L"a worker's change never reached its row");
    }

    TEST_METHOD(TestAnAddAndARemoveReachTheView)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        std::thread([&vmItems]() { vmItems.Add().SetLabel(L"Added"); }).join();
        const bool bAdded = oQt.WaitOnQt([pGrid]() {
            return pGrid->oView.model()->rowCount() == 4 && Text(pGrid->oView, 3, TitleColumn) == QStringLiteral("Added");
        });
        std::thread([&vmItems]() { vmItems.RemoveAt(0); }).join();
        const bool bRemoved = oQt.WaitOnQt([pGrid]() {
            return pGrid->oView.model()->rowCount() == 3 && Text(pGrid->oView, 0, TitleColumn) == QStringLiteral("Title 1");
        });
        Delete(oQt, pGrid);

        Assert::IsTrue(bAdded, L"the add never reached the view");
        Assert::IsTrue(bRemoved, L"the remove never reached the view");
    }

    TEST_METHOD(TestAMoveReachesTheView)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        std::thread([&vmItems]() { vmItems.MoveItem(0, 2); }).join();
        const bool bMoved = oQt.WaitOnQt([pGrid]() {
            return Text(pGrid->oView, 0, TitleColumn) == QStringLiteral("Title 1") &&
                   Text(pGrid->oView, 1, TitleColumn) == QStringLiteral("Title 2") &&
                   Text(pGrid->oView, 2, TitleColumn) == QStringLiteral("Title 0");
        });
        Delete(oQt, pGrid);

        Assert::IsTrue(bMoved, L"the move never reached the view");
    }

    TEST_METHOD(TestABatchRefreshesTheViewOnce)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);
        std::atomic<int> nResets{0}, nRowChanges{0};
        oQt.RunOnQt([pGrid, &nResets, &nRowChanges]() {
            QObject::connect(pGrid->oView.model(), &QAbstractItemModel::modelReset, &pGrid->oView,
                             [&nResets]() { ++nResets; });
            QObject::connect(pGrid->oView.model(), &QAbstractItemModel::dataChanged, &pGrid->oView,
                             [&nRowChanges]() { ++nRowChanges; });
        });

        std::thread([&vmItems]() {
            vmItems.BeginUpdate();
            vmItems.GetItemAt(0)->SetLabel(L"Zero");
            vmItems.RemoveAt(2);
            vmItems.Add().SetLabel(L"Three");
            vmItems.EndUpdate();
        }).join();
        const bool bReached = oQt.WaitOnQt([pGrid]() {
            return Text(pGrid->oView, 0, TitleColumn) == QStringLiteral("Zero") &&
                   Text(pGrid->oView, 2, TitleColumn) == QStringLiteral("Three");
        });
        oQt.RunOnQt([]() {});
        Delete(oQt, pGrid);

        Assert::IsTrue(bReached, L"the batch never reached the view");
        Assert::AreEqual(1, nResets.load());
        Assert::AreEqual(0, nRowChanges.load(), L"a change inside the batch was posted on its own");
    }

    TEST_METHOD(TestDetachWaitsForAHandlerOnAnotherThread)
    {
        // a column whose cells the test holds: the worker's handler is inside it when Detach is called
        struct Gate
        {
            std::atomic<bool> bArmed{false};
            std::atomic<bool> bEntered{false};
            std::promise<void> oRelease;
            std::shared_future<void> fRelease{oRelease.get_future().share()};
        };
        class HeldColumn : public GridColumnBinding
        {
        public:
            explicit HeldColumn(Gate& oGate) noexcept : m_oGate(oGate) {}
            GridCell GetCell(const ViewModelCollectionBase&, gsl::index) const override
            {
                if (m_oGate.bArmed)
                {
                    m_oGate.bEntered = true;
                    m_oGate.fRelease.wait_for(std::chrono::seconds(5));
                }
                return {};
            }
            bool DependsOn(const StringModelProperty&) const noexcept override { return true; }
            using GridColumnBinding::DependsOn;

        private:
            Gate& m_oGate;
        };

        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 2);
        Gate oGate;
        QtTestHost oQt;
        GridOptions oOptions;
        oOptions.fExtraColumn = [&oGate]() { return new HeldColumn(oGate); };
        auto* pGrid = Create(oQt, vmOwner, vmItems, oOptions);

        oGate.bArmed = true;
        std::thread oWorker([&vmItems]() { vmItems.GetItemAt(0)->SetLabel(L"Changed"); });
        const bool bEntered = QtTestHost::WaitFor([&oGate]() { return oGate.bEntered.load(); });

        std::atomic<bool> bDetached{false};
        oQt.Host().Invoke([pGrid, &bDetached]() {
            pGrid->oBinding.Detach();
            bDetached = true;
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const bool bDetachedWhileHeld = bDetached.load();

        oGate.oRelease.set_value();
        oWorker.join();
        const bool bDetachedAfter = QtTestHost::WaitFor([&bDetached]() { return bDetached.load(); });
        Delete(oQt, pGrid);

        Assert::IsTrue(bEntered, L"the worker's handler never reached the column");
        Assert::IsFalse(bDetachedWhileHeld, L"Detach returned while a handler was still running on another thread");
        Assert::IsTrue(bDetachedAfter, L"Detach never returned");
    }

    TEST_METHOD(TestWorkQueuedForADeletedViewIsDropped)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        // the refresh is queued while the Qt thread is held, and the view is deleted before it can run
        const bool bWasHeld = oQt.HoldQtWhile([]() {}, [&vmItems]() { vmItems.GetItemAt(0)->SetLabel(L"Changed"); },
                                              [pGrid]() { delete pGrid; });

        Assert::IsTrue(bWasHeld);
    }

    // --- the user's ticks ---

    TEST_METHOD(TestClickingATickWritesIt)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        Click(oQt, pGrid, [](const QTableView& oView) { return TickCentre(oView, 1); });
        const bool bTicked = IsSelected(vmItems, 1);
        int nShown = 0;
        oQt.RunOnQt([pGrid, &nShown]() { nShown = CheckState(pGrid->oView, 1); });
        Click(oQt, pGrid, [](const QTableView& oView) { return TickCentre(oView, 1); });
        const bool bUnticked = !IsSelected(vmItems, 1);
        Delete(oQt, pGrid);

        Assert::IsTrue(bTicked, L"the first click did not tick the item");
        Assert::AreEqual(static_cast<int>(Qt::Checked), nShown, L"the view did not show the tick");
        Assert::IsTrue(bUnticked, L"the second click did not untick it");
        Assert::IsFalse(IsSelected(vmItems, 0));
        Assert::IsFalse(IsSelected(vmItems, 2));
    }

    TEST_METHOD(TestSpaceTogglesTheCurrentRowsTickWhereverTheCursorIs)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        oQt.RunOnQt([pGrid]() {
            auto& oView = pGrid->oView;
            oView.setFocus();
            oView.selectionModel()->setCurrentIndex(oView.model()->index(1, DescriptionColumn),
                                                    QItemSelectionModel::NoUpdate);
        });
        PressSpace(oQt, pGrid);
        const bool bTicked = IsSelected(vmItems, 1);
        PressSpace(oQt, pGrid);
        const bool bUnticked = !IsSelected(vmItems, 1);
        Delete(oQt, pGrid);

        Assert::IsTrue(bTicked, L"Space did not tick the current row");
        Assert::IsTrue(bUnticked, L"Space again did not untick it");
    }

    TEST_METHOD(TestATickWhileTheRowsAreStaleIsDropped)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);

        // A worker removes the first item; before its refresh arrives the user ticks the second row they see, Title 1.
        // That row number is now Title 2's.
        const bool bWasHeld = oQt.HoldQtWhile([]() {}, [&vmItems]() { vmItems.RemoveAt(0); },
                                              [pGrid]() {
                                                  const QPoint ptTick = TickCentre(pGrid->oView, 1);
                                                  ra::ui::qt::tests::PressMouse(pGrid->oView, ptTick);
                                                  ra::ui::qt::tests::ReleaseMouse(pGrid->oView, ptTick);
                                              });
        oQt.RunOnQt([]() {});
        Delete(oQt, pGrid);

        Assert::IsTrue(bWasHeld);
        Assert::IsFalse(IsSelected(vmItems, 1), L"the click ticked Title 2, a row the user never saw there");
        Assert::IsFalse(IsSelected(vmItems, 0));
    }

    TEST_METHOD(TestDetachStopsTheViewAndTheTicks)
    {
        OwnerViewModel vmOwner;
        Rows vmItems;
        AddRows(vmItems, 3);
        QtTestHost oQt;
        auto* pGrid = Create(oQt, vmOwner, vmItems);
        oQt.RunOnQt([pGrid]() { pGrid->oBinding.Detach(); });

        std::thread([&vmItems]() { vmItems.GetItemAt(0)->SetLabel(L"Changed"); }).join();
        Click(oQt, pGrid, [](const QTableView& oView) { return TickCentre(oView, 1); });
        QString sTitle;
        oQt.RunOnQt([pGrid, &sTitle]() { sTitle = Text(pGrid->oView, 0, TitleColumn); });
        Delete(oQt, pGrid);

        Assert::AreEqual(std::wstring(L"Title 0"), sTitle.toStdWString(), L"an item change reached the view after Detach");
        Assert::IsFalse(IsSelected(vmItems, 1), L"a tick was written after Detach");
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
