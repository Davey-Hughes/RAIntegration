#include "ui/BindingBase.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <chrono>
#include <future>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace tests {

TEST_CLASS(BindingBase_Tests)
{
    class ViewModelHarness : public ViewModelBase
    {
    public:
        StringModelProperty StringProperty{ "BindingBaseHarness", "String", L"" };
        void SetString(const std::wstring& sValue) { SetValue(StringProperty, sValue); }
    };

    // Counts the string changes it is told about.
    class BindingHarness : public BindingBase
    {
    public:
        explicit BindingHarness(ViewModelBase& vmViewModel) noexcept : BindingBase(vmViewModel) {}

        using BindingBase::AttachToViewModel;
        using BindingBase::DetachFromViewModel;

        void SetString(const StringModelProperty& pProperty, const std::wstring& sValue)
        {
            SetValue(pProperty, sValue);
        }

        int nChanges = 0;

    protected:
        BindingHarness(ViewModelBase& vmViewModel, AttachLater oAttachLater) noexcept
            : BindingBase(vmViewModel, oAttachLater)
        {
        }

        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs&) noexcept override { ++nChanges; }
    };

    // The same, constructed detached.
    class DeferredBindingHarness : public BindingHarness
    {
    public:
        explicit DeferredBindingHarness(ViewModelBase& vmViewModel) noexcept
            : BindingHarness(vmViewModel, AttachLater{})
        {
        }
    };

    // Attached from construction; stays inside its string-change handler until released.
    class BlockingBindingHarness : public BindingBase
    {
    public:
        explicit BlockingBindingHarness(ViewModelBase& vmViewModel) noexcept : BindingBase(vmViewModel) {}

        using BindingBase::DetachFromViewModel;

        std::promise<void> oEntered;
        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();

    protected:
        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs&) override
        {
            oEntered.set_value();
            fRelease.wait();
        }
    };

public:
    TEST_METHOD(TestABindingIsNotifiedFromConstruction)
    {
        ViewModelHarness vmViewModel;
        BindingHarness oBinding(vmViewModel);

        vmViewModel.SetString(L"changed");

        Assert::AreEqual(1, oBinding.nChanges);
    }

    TEST_METHOD(TestADeferredBindingIsNotifiedOnlyWhileAttached)
    {
        ViewModelHarness vmViewModel;
        DeferredBindingHarness oBinding(vmViewModel);

        vmViewModel.SetString(L"before attaching");
        Assert::AreEqual(0, oBinding.nChanges);

        oBinding.AttachToViewModel();
        oBinding.AttachToViewModel(); // idempotent
        vmViewModel.SetString(L"attached");
        Assert::AreEqual(1, oBinding.nChanges);

        oBinding.DetachFromViewModel();
        oBinding.DetachFromViewModel(); // idempotent
        vmViewModel.SetString(L"detached");
        Assert::AreEqual(1, oBinding.nChanges);
    }

    TEST_METHOD(TestSetValueSuppressesItsOwnChangeAndStaysAttached)
    {
        ViewModelHarness vmViewModel;
        BindingHarness oBinding(vmViewModel);

        oBinding.SetString(vmViewModel.StringProperty, L"from the view");
        Assert::AreEqual(0, oBinding.nChanges);

        vmViewModel.SetString(L"from elsewhere");
        Assert::AreEqual(1, oBinding.nChanges);
    }

    TEST_METHOD(TestSetValueOnADetachedBindingDoesNotAttachIt)
    {
        ViewModelHarness vmViewModel;
        DeferredBindingHarness oBinding(vmViewModel);

        oBinding.SetString(vmViewModel.StringProperty, L"from the view");
        vmViewModel.SetString(L"from elsewhere");

        Assert::AreEqual(0, oBinding.nChanges);
    }

    TEST_METHOD(TestDetachFromViewModelWaitsForAnotherThreadsCall)
    {
        // The Qt WindowBinding detaches first in its destructor: that must not
        // return while a worker is still inside one of its handlers.
        struct State
        {
            ViewModelHarness vmViewModel;
            BlockingBindingHarness oBinding{ vmViewModel };
        };
        auto* pState = new State();
        auto fEntered = pState->oBinding.oEntered.get_future();

        ra::tests::DetachedCall oNotify([pState]() { pState->vmViewModel.SetString(L"from a worker"); });
        const bool bEntered = (fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        ra::tests::DetachedCall oDetach([pState]() { pState->oBinding.DetachFromViewModel(); });
        const bool bDetachedDuringTheCall = oDetach.FinishedWithin(std::chrono::milliseconds(200));

        pState->oBinding.oRelease.set_value();
        const bool bDetachedAfterIt = oDetach.FinishedWithin(std::chrono::seconds(5));
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bEntered, L"the notification never started");
        Assert::IsFalse(bDetachedDuringTheCall, L"DetachFromViewModel returned while another thread was inside the binding");
        Assert::IsTrue(bDetachedAfterIt, L"DetachFromViewModel never returned after the call ended");
        Assert::IsTrue(bNotifyFinished, L"the notification never finished");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }
};

} // namespace tests
} // namespace ui
} // namespace ra
