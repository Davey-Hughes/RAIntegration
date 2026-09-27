#ifndef _WIN32

#include "ui/qt/bindings/ComboBoxBinding.hh"

#include "ui/viewmodels/LookupItemViewModel.hh"

#include "tests/ui/qt/QtTestHost.hh"

#include <QComboBox>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {
namespace tests {

using ra::ui::qt::tests::QtTestHost;
using ra::ui::viewmodels::LookupItemViewModelCollection;

namespace {

class SelectionViewModel : public ViewModelBase
{
public:
    static const IntModelProperty SelectedProperty;

    int GetSelected() const { return GetValue(SelectedProperty); }
    void SetSelected(int nValue) { SetValue(SelectedProperty, nValue); }
};

const IntModelProperty SelectionViewModel::SelectedProperty("ComboBoxBindingTests", "Selected", 0);

class SelectionChangeCounter : public ViewModelBase::NotifyTarget
{
public:
    void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) override
    {
        if (args.Property == SelectionViewModel::SelectedProperty)
            ++nChanges;
    }

    std::atomic<int> nChanges{0};
};

void AddThreeItems(LookupItemViewModelCollection& vmItems)
{
    vmItems.Add(1, L"One");
    vmItems.Add(2, L"Two");
    vmItems.Add(3, L"Three");
}

struct BoundComboBox
{
    explicit BoundComboBox(SelectionViewModel& vmSelection) : oBinding(vmSelection) {}

    QComboBox oComboBox;
    ComboBoxBinding oBinding;
};

// bTracked: bind the collection as non-const, so its changes are followed.
BoundComboBox* Create(QtTestHost& oQt, SelectionViewModel& vmSelection, LookupItemViewModelCollection& vmItems,
                      bool bTracked)
{
    BoundComboBox* pBound = nullptr;
    oQt.RunOnQt([&vmSelection, &vmItems, &pBound, bTracked]() {
        pBound = new BoundComboBox(vmSelection);
        if (bTracked)
            pBound->oBinding.BindItems(vmItems);
        else
            pBound->oBinding.BindItems(static_cast<const LookupItemViewModelCollection&>(vmItems));
        pBound->oBinding.BindSelectedItem(SelectionViewModel::SelectedProperty);
        pBound->oBinding.SetControl(pBound->oComboBox);
    });
    return pBound;
}

void Delete(QtTestHost& oQt, BoundComboBox* pBound)
{
    oQt.RunOnQt([pBound]() { delete pBound; });
}

// What a user does: Qt emits activated for the user's choice only, never for setCurrentIndex.
void Choose(QComboBox& oComboBox, int nIndex)
{
    oComboBox.setCurrentIndex(nIndex);
    Q_EMIT oComboBox.activated(nIndex);
}

QStringList Labels(const QComboBox& oComboBox)
{
    QStringList vLabels;
    for (int nIndex = 0; nIndex < oComboBox.count(); ++nIndex)
        vLabels.append(oComboBox.itemText(nIndex));
    return vLabels;
}

} // namespace

TEST_CLASS(ComboBoxBinding_Tests)
{
public:
    TEST_METHOD(TestSetControlShowsTheItemsAndTheSelection)
    {
        SelectionViewModel vmSelection;
        vmSelection.SetSelected(2);
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        QStringList vLabels;
        int nIndex = -2;
        oQt.RunOnQt([pBound, &vLabels, &nIndex]() {
            vLabels = Labels(pBound->oComboBox);
            nIndex = pBound->oComboBox.currentIndex();
        });
        Delete(oQt, pBound);

        Assert::IsTrue(vLabels == QStringList({QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")}),
                       L"wrong labels");
        Assert::AreEqual(1, nIndex);
    }

    TEST_METHOD(TestAnUnknownIdSelectsNothing)
    {
        SelectionViewModel vmSelection;
        vmSelection.SetSelected(99);
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        int nIndex = -2;
        oQt.RunOnQt([pBound, &nIndex]() { nIndex = pBound->oComboBox.currentIndex(); });
        Delete(oQt, pBound);

        Assert::AreEqual(-1, nIndex);
    }

    TEST_METHOD(TestChoosingAnItemWritesItsId)
    {
        SelectionViewModel vmSelection;
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        oQt.RunOnQt([pBound]() { Choose(pBound->oComboBox, 2); });
        Delete(oQt, pBound);

        Assert::AreEqual(3, vmSelection.GetSelected());
    }

    TEST_METHOD(TestAChangeFromAWorkerSelectsTheItem)
    {
        SelectionViewModel vmSelection;
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        std::thread([&vmSelection]() { vmSelection.SetSelected(1); }).join();
        const bool bSelected = oQt.WaitOnQt([pBound]() { return pBound->oComboBox.currentIndex() == 0; });
        Delete(oQt, pBound);

        Assert::IsTrue(bSelected, L"the worker's selection never reached the control");
    }

    TEST_METHOD(TestAnUpdateFromTheViewModelIsNotWrittenBack)
    {
        SelectionViewModel vmSelection;
        SelectionChangeCounter oCounter;
        vmSelection.AddNotifyTarget(oCounter);
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        std::thread([&vmSelection]() {
            vmSelection.SetSelected(1);
            vmSelection.SetSelected(3);
        }).join();
        oRelease.set_value();

        oQt.RunOnQt([]() {}); // both posts have run
        Delete(oQt, pBound);
        vmSelection.RemoveNotifyTarget(oCounter);

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::AreEqual(2, oCounter.nChanges.load(), L"the control wrote a view-model change back");
        Assert::AreEqual(3, vmSelection.GetSelected());
    }

    TEST_METHOD(TestATrackedCollectionsChangesReachTheControl)
    {
        SelectionViewModel vmSelection;
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, true);

        std::thread([&vmItems]() { vmItems.Add(4, L"Four"); }).join();
        const bool bAdded = oQt.WaitOnQt([pBound]() {
            return pBound->oComboBox.count() == 4 && pBound->oComboBox.itemText(3) == QStringLiteral("Four");
        });

        std::thread([&vmItems]() { vmItems.GetItemAt(0)->SetLabel(L"Uno"); }).join();
        const bool bRelabelled =
            oQt.WaitOnQt([pBound]() { return pBound->oComboBox.itemText(0) == QStringLiteral("Uno"); });

        std::thread([&vmItems]() { vmItems.RemoveAt(1); }).join();
        const bool bRemoved = oQt.WaitOnQt([pBound]() {
            return pBound->oComboBox.count() == 3 && pBound->oComboBox.itemText(1) == QStringLiteral("Three");
        });
        Delete(oQt, pBound);

        Assert::IsTrue(bAdded, L"an added item never appeared");
        Assert::IsTrue(bRelabelled, L"a new label never appeared");
        Assert::IsTrue(bRemoved, L"a removed item never went");
    }

    TEST_METHOD(TestAConstCollectionIsNotTracked)
    {
        SelectionViewModel vmSelection;
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, false);

        vmItems.Add(4, L"Four");
        int nCount = 0;
        oQt.RunOnQt([pBound, &nCount]() { nCount = pBound->oComboBox.count(); }); // after any post from the add
        Delete(oQt, pBound);

        Assert::AreEqual(3, nCount);
    }

    TEST_METHOD(TestDetachStopsAllThreeDirections)
    {
        SelectionViewModel vmSelection;
        LookupItemViewModelCollection vmItems;
        AddThreeItems(vmItems);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmSelection, vmItems, true);
        oQt.RunOnQt([pBound]() { pBound->oBinding.Detach(); });

        std::thread([&vmSelection, &vmItems]() {
            vmSelection.SetSelected(2);
            vmItems.Add(4, L"Four");
        }).join();

        int nIndex = -2, nCount = 0;
        oQt.RunOnQt([pBound, &nIndex, &nCount]() {
            nIndex = pBound->oComboBox.currentIndex();
            nCount = pBound->oComboBox.count();
            Choose(pBound->oComboBox, 0); // must not write
        });
        Delete(oQt, pBound);

        Assert::AreEqual(-1, nIndex, L"a selection reached the control after Detach");
        Assert::AreEqual(3, nCount, L"an item change reached the control after Detach");
        Assert::AreEqual(2, vmSelection.GetSelected(), L"the control wrote after Detach");
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
