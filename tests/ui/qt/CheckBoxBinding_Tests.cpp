#ifndef _WIN32

#include "ui/qt/bindings/CheckBoxBinding.hh"

#include "tests/ui/qt/QtTestHost.hh"

#include <QCheckBox>

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

namespace {

class CheckViewModel : public ViewModelBase
{
public:
    static const BoolModelProperty CheckedProperty;

    bool IsChecked() const { return GetValue(CheckedProperty); }
    void SetChecked(bool bValue) { SetValue(CheckedProperty, bValue); }
};

const BoolModelProperty CheckViewModel::CheckedProperty("CheckBoxBindingTests", "Checked", false);

class CheckChangeCounter : public ViewModelBase::NotifyTarget
{
public:
    void OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args) override
    {
        if (args.Property == CheckViewModel::CheckedProperty)
            ++nChanges;
    }

    std::atomic<int> nChanges{0};
};

struct BoundCheckBox
{
    explicit BoundCheckBox(CheckViewModel& vmCheck) : oBinding(vmCheck) {}

    QCheckBox oCheckBox;
    CheckBoxBinding oBinding;
};

BoundCheckBox* Create(QtTestHost& oQt, CheckViewModel& vmCheck)
{
    BoundCheckBox* pBound = nullptr;
    oQt.RunOnQt([&vmCheck, &pBound]() {
        pBound = new BoundCheckBox(vmCheck);
        pBound->oBinding.BindCheck(CheckViewModel::CheckedProperty);
        pBound->oBinding.SetControl(pBound->oCheckBox);
    });
    return pBound;
}

void Delete(QtTestHost& oQt, BoundCheckBox* pBound)
{
    oQt.RunOnQt([pBound]() { delete pBound; });
}

} // namespace

TEST_CLASS(CheckBoxBinding_Tests)
{
public:
    TEST_METHOD(TestSetControlShowsTheValue)
    {
        CheckViewModel vmCheck;
        vmCheck.SetChecked(true);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmCheck);

        bool bChecked = false;
        oQt.RunOnQt([pBound, &bChecked]() { bChecked = pBound->oCheckBox.isChecked(); });
        Delete(oQt, pBound);

        Assert::IsTrue(bChecked);
    }

    TEST_METHOD(TestAClickWritesTheViewModel)
    {
        CheckViewModel vmCheck;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmCheck);

        oQt.RunOnQt([pBound]() { pBound->oCheckBox.click(); });
        Delete(oQt, pBound);

        Assert::IsTrue(vmCheck.IsChecked());
    }

    TEST_METHOD(TestAChangeFromAWorkerReachesTheControl)
    {
        CheckViewModel vmCheck;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmCheck);

        std::thread([&vmCheck]() { vmCheck.SetChecked(true); }).join();
        const bool bShown = oQt.WaitOnQt([pBound]() { return pBound->oCheckBox.isChecked(); });
        Delete(oQt, pBound);

        Assert::IsTrue(bShown, L"the control never showed the worker's change");
    }

    TEST_METHOD(TestAnUpdateFromTheViewModelIsNotWrittenBack)
    {
        // As TextBoxBinding_Tests' twin: two changes queued while the Qt thread is held.
        CheckViewModel vmCheck;
        CheckChangeCounter oCounter;
        vmCheck.AddNotifyTarget(oCounter);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmCheck);

        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        std::thread([&vmCheck]() {
            vmCheck.SetChecked(true);
            vmCheck.SetChecked(false);
        }).join();
        oRelease.set_value();

        oQt.RunOnQt([]() {}); // both posts have run: they were queued before this call
        Delete(oQt, pBound);
        vmCheck.RemoveNotifyTarget(oCounter);

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::AreEqual(2, oCounter.nChanges.load(), L"the control wrote a view-model change back");
        Assert::IsFalse(vmCheck.IsChecked());
    }

    TEST_METHOD(TestDetachStopsBothDirections)
    {
        CheckViewModel vmCheck;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmCheck);
        oQt.RunOnQt([pBound]() { pBound->oBinding.Detach(); });

        std::thread([&vmCheck]() { vmCheck.SetChecked(true); }).join();

        bool bShown = true;
        oQt.RunOnQt([pBound, &bShown]() {
            bShown = pBound->oCheckBox.isChecked();
            pBound->oCheckBox.setChecked(true); // as the view model holds - setChecked emits no clicked
            pBound->oCheckBox.click();          // unchecks the control: a leaked write would store false
        });
        Delete(oQt, pBound);

        Assert::IsFalse(bShown, L"a change reached the control after Detach");
        Assert::IsTrue(vmCheck.IsChecked(), L"the control wrote after Detach");
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
