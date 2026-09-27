#include "ui/ViewModelBase.hh"

#include "tests/devkit/testutil/DetachedCall.hh"

#include <chrono>
#include <future>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace tests {

TEST_CLASS(ViewModelBase_Tests)
{
    class ViewModelHarness : public ViewModelBase
    {
    public:
        ViewModelHarness() noexcept(std::is_nothrow_default_constructible_v<ViewModelBase>) = default;

        StringModelProperty StringProperty{ "ViewModelHarness", "String", L"" };
        const std::wstring& GetString() const { return GetValue(StringProperty); }
        void SetString(const std::wstring& sValue) { SetValue(StringProperty, sValue); }

        IntModelProperty IntProperty{ "ViewModelHarness", "Int", 0 };
        int GetInt() const { return GetValue(IntProperty); }
        void SetInt(int nValue) { SetValue(IntProperty, nValue); }

        BoolModelProperty BoolProperty{ "ViewModelHarness", "Bool", false };
        bool GetBool() const { return GetValue(BoolProperty); }
        void SetBool(bool bValue) { SetValue(BoolProperty, bValue); }
    };

    class NotifyTargetHarness : public ViewModelBase::NotifyTarget
    {
    public:
        void AssertNotChanged()
        {
            Assert::AreEqual(std::string(), m_sLastPropertyChanged);

            m_sLastPropertyChanged.clear();
        }

        void OnViewModelBoolValueChanged(const BoolModelProperty::ChangeArgs& args) noexcept override
        {
            GSL_SUPPRESS_F6 m_sLastPropertyChanged = args.Property.GetPropertyName();

            m_bOldValue = args.tOldValue;
            m_bNewValue = args.tNewValue;
        }

        void AssertBoolChanged(const BoolModelProperty& pProperty, bool bOldValue, bool bNewValue)
        {
            Assert::AreEqual(pProperty.GetPropertyName(), m_sLastPropertyChanged.c_str());
            Assert::AreEqual(bOldValue, m_bOldValue);
            Assert::AreEqual(bNewValue, m_bNewValue);

            m_sLastPropertyChanged.clear();
        }
        
        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args) noexcept override
        {
           GSL_SUPPRESS_F6 m_sLastPropertyChanged = args.Property.GetPropertyName();

           GSL_SUPPRESS_F6 m_sOldValue = args.tOldValue;
           GSL_SUPPRESS_F6 m_sNewValue = args.tNewValue;
        }

        void AssertStringChanged(const StringModelProperty& pProperty, const std::wstring& sOldValue, const std::wstring& sNewValue)
        {
            Assert::AreEqual(pProperty.GetPropertyName(), m_sLastPropertyChanged.c_str());
            Assert::AreEqual(sOldValue, m_sOldValue);
            Assert::AreEqual(sNewValue, m_sNewValue);

            m_sLastPropertyChanged.clear();
        }

        void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) noexcept override
        {
            GSL_SUPPRESS_F6 m_sLastPropertyChanged = args.Property.GetPropertyName();

            m_nOldValue = args.tOldValue;
            m_nNewValue = args.tNewValue;
        }

        void AssertIntChanged(const IntModelProperty& pProperty, int nOldValue, int nNewValue)
        {
            Assert::AreEqual(pProperty.GetPropertyName(), m_sLastPropertyChanged.c_str());
            Assert::AreEqual(nOldValue, m_nOldValue);
            Assert::AreEqual(nNewValue, m_nNewValue);

            m_sLastPropertyChanged.clear();
        }

    private:
        std::string m_sLastPropertyChanged;
        std::wstring m_sOldValue, m_sNewValue;
        int m_nOldValue{}, m_nNewValue{};
        bool m_bOldValue{}, m_bNewValue{};
    };

    // Removes another target when it is told of a string change.
    class RemovingTarget : public ViewModelBase::NotifyTarget
    {
    public:
        RemovingTarget(ViewModelBase& vmViewModel, ViewModelBase::NotifyTarget& oVictim) noexcept
            : m_vmViewModel(vmViewModel), m_oVictim(oVictim)
        {
        }

        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs&) noexcept override
        {
            m_vmViewModel.RemoveNotifyTarget(m_oVictim);
        }

    private:
        ViewModelBase& m_vmViewModel;
        ViewModelBase::NotifyTarget& m_oVictim;
    };

    // Stays inside its string-change handler until released.
    class BlockingTarget : public ViewModelBase::NotifyTarget
    {
    public:
        void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs&) override
        {
            oEntered.set_value();
            fRelease.wait();
        }

        std::promise<void> oEntered;
        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
    };

public:
    TEST_METHOD(TestStringProperty)
    {
        ViewModelHarness vmViewModel;
        Assert::AreEqual(std::wstring(), vmViewModel.GetString());

        vmViewModel.SetString(L"Test");
        Assert::AreEqual(std::wstring(L"Test"), vmViewModel.GetString());

        NotifyTargetHarness oNotify;
        vmViewModel.AddNotifyTarget(oNotify);
        oNotify.AssertNotChanged();

        vmViewModel.SetString(L"Test2");
        oNotify.AssertStringChanged(vmViewModel.StringProperty, L"Test", L"Test2");

        vmViewModel.SetString(L"Test2");
        oNotify.AssertNotChanged();

        vmViewModel.SetString(vmViewModel.StringProperty.GetDefaultValue());
        oNotify.AssertStringChanged(vmViewModel.StringProperty, L"Test2", L"");

        vmViewModel.RemoveNotifyTarget(oNotify);
        vmViewModel.SetString(L"Test3");
        oNotify.AssertNotChanged();
    }

    TEST_METHOD(TestIntProperty)
    {
        ViewModelHarness vmViewModel;
        Assert::AreEqual(0, vmViewModel.GetInt());

        vmViewModel.SetInt(32);
        Assert::AreEqual(32, vmViewModel.GetInt());

        NotifyTargetHarness oNotify;
        vmViewModel.AddNotifyTarget(oNotify);
        oNotify.AssertNotChanged();

        vmViewModel.SetInt(64);
        oNotify.AssertIntChanged(vmViewModel.IntProperty, 32, 64);

        vmViewModel.SetInt(64);
        oNotify.AssertNotChanged();

        vmViewModel.SetInt(vmViewModel.IntProperty.GetDefaultValue());
        oNotify.AssertIntChanged(vmViewModel.IntProperty, 64, 0);

        vmViewModel.RemoveNotifyTarget(oNotify);
        vmViewModel.SetInt(96);
        oNotify.AssertNotChanged();
    }

    TEST_METHOD(TestBoolProperty)
    {
        ViewModelHarness vmViewModel;
        Assert::AreEqual(false, vmViewModel.GetBool());

        vmViewModel.SetBool(true);
        Assert::AreEqual(true, vmViewModel.GetBool());

        NotifyTargetHarness oNotify;
        vmViewModel.AddNotifyTarget(oNotify);
        oNotify.AssertNotChanged();

        vmViewModel.SetBool(false);
        oNotify.AssertBoolChanged(vmViewModel.BoolProperty, true, false);

        vmViewModel.SetBool(false);
        oNotify.AssertNotChanged();

        vmViewModel.RemoveNotifyTarget(oNotify);
        vmViewModel.SetBool(true);
        oNotify.AssertNotChanged();
    }

    TEST_METHOD(TestATargetRemovedByAnEarlierTargetIsNotNotified)
    {
        ViewModelHarness vmViewModel;
        NotifyTargetHarness oSecond;
        RemovingTarget oFirst(vmViewModel, oSecond);
        vmViewModel.AddNotifyTarget(oFirst);
        vmViewModel.AddNotifyTarget(oSecond);

        vmViewModel.SetString(L"Test");

        oSecond.AssertNotChanged();
    }

    TEST_METHOD(TestRemoveNotifyTargetAndWaitWaitsForAnotherThreadsCall)
    {
        // A binding's destructor removes it on the UI thread while a worker may
        // be inside its handler; the binding must not be destroyed under it.
        struct State
        {
            ViewModelHarness vmViewModel;
            BlockingTarget oTarget;
        };
        auto* pState = new State();
        pState->vmViewModel.AddNotifyTarget(pState->oTarget);
        auto fEntered = pState->oTarget.oEntered.get_future();

        ra::tests::DetachedCall oNotify([pState]() { pState->vmViewModel.SetString(L"from a worker"); });
        const bool bEntered = (fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        ra::tests::DetachedCall oRemove(
            [pState]() noexcept { pState->vmViewModel.RemoveNotifyTargetAndWait(pState->oTarget); });
        const bool bRemovedDuringTheCall = oRemove.FinishedWithin(std::chrono::milliseconds(200));

        pState->oTarget.oRelease.set_value();
        const bool bRemovedAfterIt = oRemove.FinishedWithin(std::chrono::seconds(5));
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bEntered, L"the notification never started");
        Assert::IsFalse(bRemovedDuringTheCall, L"RemoveNotifyTargetAndWait returned while another thread was inside the target");
        Assert::IsTrue(bRemovedAfterIt, L"RemoveNotifyTargetAndWait never returned after the call ended");
        Assert::IsTrue(bNotifyFinished, L"the notification never finished");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }

    TEST_METHOD(TestRemoveNotifyTargetDoesNotWaitForAnotherThreadsCall)
    {
        // RemoveNotifyTarget is also how a target is muted while holding a lock
        // its own handler takes (TriggerViewModel::DoFrame), so it must never wait.
        struct State
        {
            ViewModelHarness vmViewModel;
            BlockingTarget oTarget;
        };
        auto* pState = new State();
        pState->vmViewModel.AddNotifyTarget(pState->oTarget);
        auto fEntered = pState->oTarget.oEntered.get_future();

        ra::tests::DetachedCall oNotify([pState]() { pState->vmViewModel.SetString(L"from a worker"); });
        const bool bEntered = (fEntered.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

        ra::tests::DetachedCall oRemove(
            [pState]() noexcept { pState->vmViewModel.RemoveNotifyTarget(pState->oTarget); });
        const bool bRemovedDuringTheCall = oRemove.FinishedWithin(std::chrono::seconds(5));

        pState->oTarget.oRelease.set_value();
        const bool bNotifyFinished = oNotify.FinishedWithin(std::chrono::seconds(5));

        Assert::IsTrue(bEntered, L"the notification never started");
        Assert::IsTrue(bRemovedDuringTheCall, L"RemoveNotifyTarget waited for another thread's call");
        Assert::IsTrue(bNotifyFinished, L"the notification never finished");

        delete pState; // reached only when every call finished: on a failure above it is leaked on purpose
    }
};

} // namespace tests
} // namespace ui
} // namespace ra
