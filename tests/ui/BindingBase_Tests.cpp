#include "ui/BindingBase.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace tests {

TEST_CLASS(BindingBase_Tests)
{
    class ViewModelHarness : public ViewModelBase
    {
    public:
        static const StringModelProperty StringProperty;
        void SetString(const std::wstring& sValue) { SetValue(StringProperty, sValue); }
        const std::wstring& GetString() const { return GetValue(StringProperty); }

        // A second property: a binding sets it while a worker is inside its handler for the first.
        static const StringModelProperty OtherStringProperty;

        static const BoolModelProperty BoolProperty;
        void SetBool(bool bValue) { SetValue(BoolProperty, bValue); }
        static const IntModelProperty IntProperty;
        void SetInt(int nValue) { SetValue(IntProperty, nValue); }
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
        explicit BlockingBindingHarness(ViewModelBase& vmViewModel) : BindingBase(vmViewModel) {}

        using BindingBase::DetachFromViewModel;

        void SetString(const StringModelProperty& pProperty, const std::wstring& sValue)
        {
            SetValue(pProperty, sValue);
        }

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

    // Derives OtherStringProperty whenever StringProperty changes, and runs a hook once - on the setting thread,
    // inside the notification - the next time StringProperty changes.
    class DerivingViewModel : public ViewModelHarness
    {
    public:
        std::function<void()> fOnStringChanged;

    protected:
        using ViewModelHarness::OnValueChanged;

        void OnValueChanged(const StringModelProperty::ChangeArgs& args) override
        {
            ViewModelHarness::OnValueChanged(args); // tells the notify targets

            if (args.Property == StringProperty)
            {
                SetValue(OtherStringProperty, L"derived");

                if (fOnStringChanged)
                {
                    // once: a change the hook makes on another thread comes through here too
                    auto fHook = std::move(fOnStringChanged);
                    fOnStringChanged = nullptr;
                    fHook();
                }
            }
        }
    };

    // Counts each string property's changes, leaving out its own echoes as a Qt control binding does.
    class RecordingBindingHarness : public BindingBase
    {
    public:
        explicit RecordingBindingHarness(ViewModelBase& vmViewModel) noexcept : BindingBase(vmViewModel) {}

        using BindingBase::IsEchoOfOwnChange;

        void SetStringFromControl(const StringModelProperty& pProperty, const std::wstring& sValue)
        {
            SetValueFromControl(pProperty, sValue);
        }

        void SetBoolFromControl(const BoolModelProperty& pProperty, bool bValue) { SetValueFromControl(pProperty, bValue); }
        void SetIntFromControl(const IntModelProperty& pProperty, int nValue) { SetValueFromControl(pProperty, nValue); }

        std::atomic<int> nStringChanges{0};
        std::atomic<int> nOtherStringChanges{0};
        std::atomic<int> nBoolChanges{0};
        std::atomic<int> nIntChanges{0};

    protected:
        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args) noexcept override
        {
            if (IsEchoOfOwnChange(args.Property))
                return;

            if (args.Property == ViewModelHarness::StringProperty)
                ++nStringChanges;
            else if (args.Property == ViewModelHarness::OtherStringProperty)
                ++nOtherStringChanges;
        }

        void OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args) noexcept override
        {
            if (!IsEchoOfOwnChange(args.Property) && args.Property == ViewModelHarness::BoolProperty)
                ++nBoolChanges;
        }

        void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) noexcept override
        {
            if (!IsEchoOfOwnChange(args.Property) && args.Property == ViewModelHarness::IntProperty)
                ++nIntChanges;
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

        oBinding.SetString(ViewModelHarness::StringProperty, L"from the view");
        Assert::AreEqual(0, oBinding.nChanges);

        vmViewModel.SetString(L"from elsewhere");
        Assert::AreEqual(1, oBinding.nChanges);
    }

    TEST_METHOD(TestSetValueOnADetachedBindingDoesNotAttachIt)
    {
        ViewModelHarness vmViewModel;
        DeferredBindingHarness oBinding(vmViewModel);

        oBinding.SetString(ViewModelHarness::StringProperty, L"from the view");
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

        ra::tests::DetachedCall oDetach([pState]() noexcept { pState->oBinding.DetachFromViewModel(); });
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

    TEST_METHOD(TestSetValueDoesNotWaitForAnotherThreadsCall)
    {
        // SetValue leaves the notify targets around its change, so the change
        // does not echo back into the view. That removal only mutes the binding,
        // as TriggerViewModel::DoFrame mutes its monitor, sometimes while holding
        // a lock the handler takes: it must not wait for a worker inside one of
        // the binding's handlers.
        struct State
        {
            ViewModelHarness vmViewModel;
            BlockingBindingHarness oBinding{ vmViewModel };
        };
        auto* pState = new State();
        auto fEntered = pState->oBinding.oEntered.get_future();

        ra::tests::DetachedCall oNotify([pState]() { pState->vmViewModel.SetString(L"from a worker"); });
        const bool bEntered = (fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        ra::tests::DetachedCall oSet([pState]() {
            pState->oBinding.SetString(ViewModelHarness::OtherStringProperty, L"from the view");
        });
        const bool bSetDuringTheCall = oSet.FinishedWithin(std::chrono::seconds(5));

        pState->oBinding.oRelease.set_value();
        const bool bSetAfterIt = oSet.FinishedWithin(std::chrono::seconds(5));
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bEntered, L"the notification never started");
        Assert::IsTrue(bSetDuringTheCall, L"SetValue waited for another thread's call into the binding");
        Assert::IsTrue(bSetAfterIt, L"SetValue never returned");
        Assert::IsTrue(bNotifyFinished, L"the notification never finished");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestSetValueFromControlDoesNotEchoItsOwnProperty)
    {
        ViewModelHarness vmViewModel;
        RecordingBindingHarness oBinding(vmViewModel);

        oBinding.SetStringFromControl(ViewModelHarness::StringProperty, L"typed");
        Assert::AreEqual(0, oBinding.nStringChanges.load());
        Assert::AreEqual(std::wstring(L"typed"), vmViewModel.GetString());

        // outside the call, a change on the same thread arrives as usual
        vmViewModel.SetString(L"elsewhere");
        Assert::AreEqual(1, oBinding.nStringChanges.load());
    }

    TEST_METHOD(TestSetValueFromControlStaysAttachedForAPropertyTheViewModelDerives)
    {
        // SetValue's remove/re-add would miss this: the binding is unregistered while the view model derives it.
        DerivingViewModel vmViewModel;
        RecordingBindingHarness oBinding(vmViewModel);

        oBinding.SetStringFromControl(ViewModelHarness::StringProperty, L"typed");

        Assert::AreEqual(0, oBinding.nStringChanges.load());
        Assert::AreEqual(1, oBinding.nOtherStringChanges.load());
    }

    TEST_METHOD(TestSetValueFromControlStillDeliversAnotherThreadsChangeToTheSameProperty)
    {
        struct State
        {
            DerivingViewModel vmViewModel;
            RecordingBindingHarness oBinding{ vmViewModel };
            bool bWorkerFinished = false;
        };
        auto* pState = new State();

        // Inside the binding's own set, on this thread, a worker sets the same property and is waited for.
        pState->vmViewModel.fOnStringChanged = [pState]() {
            ra::tests::DetachedCall oWorker([pState]() { pState->vmViewModel.SetString(L"from a worker"); });
            pState->bWorkerFinished = oWorker.FinishedWithin(std::chrono::seconds(5));
        };

        pState->oBinding.SetStringFromControl(ViewModelHarness::StringProperty, L"typed");

        Assert::IsTrue(pState->bWorkerFinished, L"the worker's change never finished");
        Assert::AreEqual(1, pState->oBinding.nStringChanges.load(), L"the worker's change was dropped, or the echo was not");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestIsEchoOfOwnChangeHoldsOnlyOnTheOwnerThreadDuringTheSet)
    {
        struct State
        {
            DerivingViewModel vmViewModel;
            RecordingBindingHarness oBinding{ vmViewModel };
            bool bEchoHere = false;
            bool bEchoElsewhere = true;
            bool bWorkerFinished = false;
        };
        auto* pState = new State();

        pState->vmViewModel.fOnStringChanged = [pState]() {
            pState->bEchoHere = pState->oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty);
            ra::tests::DetachedCall oWorker([pState]() noexcept {
                pState->bEchoElsewhere = pState->oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty);
            });
            pState->bWorkerFinished = oWorker.FinishedWithin(std::chrono::seconds(5));
        };

        pState->oBinding.SetStringFromControl(ViewModelHarness::StringProperty, L"typed");

        Assert::IsTrue(pState->bWorkerFinished, L"the worker never finished");
        Assert::IsTrue(pState->bEchoHere, L"not an echo on the owner thread inside its own set");
        Assert::IsFalse(pState->bEchoElsewhere, L"an echo on another thread");
        Assert::IsFalse(pState->oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty), L"an echo after the set");

        delete pState;
    }

    TEST_METHOD(TestANestedSetValueFromControlRestoresTheOuterMark)
    {
        DerivingViewModel vmViewModel;
        RecordingBindingHarness oBinding(vmViewModel);
        bool bOuterStillMarked = false;
        bool bInnerCleared = false;

        // Inside the outer set of StringProperty, the binding sets OtherStringProperty too.
        vmViewModel.fOnStringChanged = [&oBinding, &bOuterStillMarked, &bInnerCleared]() {
            oBinding.SetStringFromControl(ViewModelHarness::OtherStringProperty, L"nested");
            bOuterStillMarked = oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty);
            bInnerCleared = !oBinding.IsEchoOfOwnChange(ViewModelHarness::OtherStringProperty);
        };

        oBinding.SetStringFromControl(ViewModelHarness::StringProperty, L"typed");

        Assert::IsTrue(bOuterStillMarked, L"the nested set did not restore the outer mark");
        Assert::IsTrue(bInnerCleared, L"the nested set left its own mark");
        Assert::IsFalse(oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty));
    }

    TEST_METHOD(TestSetValueFromControlSuppressesTheEchoForBoolAndInt)
    {
        ViewModelHarness vmViewModel;
        RecordingBindingHarness oBinding(vmViewModel);

        oBinding.SetBoolFromControl(ViewModelHarness::BoolProperty, true);
        oBinding.SetIntFromControl(ViewModelHarness::IntProperty, 7);
        Assert::AreEqual(0, oBinding.nBoolChanges.load());
        Assert::AreEqual(0, oBinding.nIntChanges.load());

        vmViewModel.SetBool(false);
        vmViewModel.SetInt(8);
        Assert::AreEqual(1, oBinding.nBoolChanges.load());
        Assert::AreEqual(1, oBinding.nIntChanges.load());
    }

    TEST_METHOD(TestSetValueFromControlOffTheOwnerThreadLeavesTheMarkAlone)
    {
        // Another thread's sets are all delivered, and never mark: the owner thread, reading its mark meanwhile, never
        // sees one. Under the TSan gate, a mark written there would also be a data race with the reads below.
        struct State
        {
            ViewModelHarness vmViewModel;
            RecordingBindingHarness oBinding{ vmViewModel };
            std::atomic<bool> bDone{false};
        };
        auto* pState = new State();

        ra::tests::DetachedCall oWorker([pState]() {
            for (int i = 1; i <= 2000; ++i)
                pState->oBinding.SetStringFromControl(ViewModelHarness::StringProperty, std::to_wstring(i));
            pState->bDone = true;
        });

        int nEchoesSeen = 0;
        const auto tDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!pState->bDone && std::chrono::steady_clock::now() < tDeadline)
        {
            if (pState->oBinding.IsEchoOfOwnChange(ViewModelHarness::StringProperty))
                ++nEchoesSeen;
        }
        const bool bFinished = oWorker.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bFinished, L"the worker never finished");
        Assert::AreEqual(0, nEchoesSeen, L"the owner thread saw another thread's set as its own");
        Assert::AreEqual(2000, pState->oBinding.nStringChanges.load(), L"a set on another thread was suppressed");

        delete pState; // reached only when the worker finished: on a failure above it is leaked on purpose
    }
};

const StringModelProperty BindingBase_Tests::ViewModelHarness::StringProperty("BindingBaseHarness", "String", L"");
const StringModelProperty BindingBase_Tests::ViewModelHarness::OtherStringProperty(
    "BindingBaseHarness", "OtherString", L"");
const BoolModelProperty BindingBase_Tests::ViewModelHarness::BoolProperty("BindingBaseHarness", "Bool", false);
const IntModelProperty BindingBase_Tests::ViewModelHarness::IntProperty("BindingBaseHarness", "Int", 0);

} // namespace tests
} // namespace ui
} // namespace ra
