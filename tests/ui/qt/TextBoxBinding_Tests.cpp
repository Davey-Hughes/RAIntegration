#ifndef _WIN32

#include "ui/qt/bindings/TextBoxBinding.hh"

#include "tests/ui/qt/QtTestHost.hh"

#include <QLineEdit>
#include <QString>

#include <atomic>
#include <chrono>
#include <future>
#include <string>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace bindings {
namespace tests {

using ra::ui::qt::tests::QtTestHost;

namespace {

class TextViewModel : public ViewModelBase
{
public:
    static const StringModelProperty TextProperty;
    static const BoolModelProperty ReadOnlyProperty;

    std::wstring CopyText() const { return CopyValue(TextProperty); }
    void SetText(const std::wstring& sValue) { SetValue(TextProperty, sValue); }
    void SetReadOnly(bool bValue) { SetValue(ReadOnlyProperty, bValue); }

    // When set, a change of TextProperty makes the view model read-only: a property it derives, on the same thread.
    std::atomic<bool> bDeriveReadOnly{false};

protected:
    using ViewModelBase::OnValueChanged;

    void OnValueChanged(const StringModelProperty::ChangeArgs& args) override
    {
        ViewModelBase::OnValueChanged(args);
        if (bDeriveReadOnly && args.Property == TextProperty)
            SetValue(ReadOnlyProperty, true);
    }
};

const StringModelProperty TextViewModel::TextProperty("TextBoxBindingTests", "Text", L"");
const BoolModelProperty TextViewModel::ReadOnlyProperty("TextBoxBindingTests", "ReadOnly", false);

// Counts the text changes the view model announces - a write-back from the control included.
class TextChangeCounter : public ViewModelBase::NotifyTarget
{
public:
    void OnViewModelStringValueChanged(const StringModelProperty::ChangeArgs& args) override
    {
        if (args.Property == TextViewModel::TextProperty)
            ++nChanges;
    }

    std::atomic<int> nChanges{0};
};

// A line edit and its binding, made and deleted on the Qt thread. The binding is declared after the control, so it
// goes first, as a dialog's member bindings go before its child widgets.
struct BoundLineEdit
{
    explicit BoundLineEdit(TextViewModel& vmText) : oBinding(vmText) {}

    QLineEdit oLineEdit;
    TextBoxBinding oBinding;
};

BoundLineEdit* Create(QtTestHost& oQt, TextViewModel& vmText, TextBoxBinding::UpdateMode nMode)
{
    BoundLineEdit* pBound = nullptr;
    oQt.RunOnQt([&vmText, &pBound, nMode]() {
        pBound = new BoundLineEdit(vmText);
        pBound->oBinding.BindText(TextViewModel::TextProperty, nMode);
        pBound->oBinding.BindReadOnly(TextViewModel::ReadOnlyProperty);
        pBound->oBinding.SetControl(pBound->oLineEdit);
    });
    return pBound;
}

void Delete(QtTestHost& oQt, BoundLineEdit* pBound)
{
    oQt.RunOnQt([pBound]() { delete pBound; });
}

// What a user does: Qt emits textEdited for the user's edits only, never for setText.
void Type(QLineEdit& oLineEdit, const QString& sText)
{
    oLineEdit.setText(sText);
    Q_EMIT oLineEdit.textEdited(sText);
}

} // namespace

TEST_CLASS(TextBoxBinding_Tests)
{
public:
    TEST_METHOD(TestSetControlShowsTheCurrentValues)
    {
        TextViewModel vmText;
        vmText.SetText(L"initial");
        vmText.SetReadOnly(true);
        QtTestHost oQt;

        std::wstring sShown;
        bool bReadOnly = false;
        oQt.RunOnQt([&vmText, &sShown, &bReadOnly]() {
            BoundLineEdit oBound(vmText);
            oBound.oBinding.BindText(TextViewModel::TextProperty);
            oBound.oBinding.BindReadOnly(TextViewModel::ReadOnlyProperty);
            oBound.oBinding.SetControl(oBound.oLineEdit);
            sShown = oBound.oLineEdit.text().toStdWString();
            bReadOnly = oBound.oLineEdit.isReadOnly();
        });

        Assert::AreEqual(std::wstring(L"initial"), sShown);
        Assert::IsTrue(bReadOnly);
    }

    TEST_METHOD(TestAChangeFromAWorkerReachesTheControl)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::LostFocus);

        std::thread([&vmText]() { vmText.SetText(L"from a worker"); }).join();
        const bool bShown =
            oQt.WaitOnQt([pBound]() { return pBound->oLineEdit.text() == QStringLiteral("from a worker"); });
        Delete(oQt, pBound);

        Assert::IsTrue(bShown, L"the control never showed the worker's change");
    }

    TEST_METHOD(TestReadOnlyFollowsTheViewModel)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::LostFocus);

        std::thread([&vmText]() { vmText.SetReadOnly(true); }).join();
        const bool bReadOnly = oQt.WaitOnQt([pBound]() { return pBound->oLineEdit.isReadOnly(); });
        Delete(oQt, pBound);

        Assert::IsTrue(bReadOnly, L"the control never became read-only");
    }

    TEST_METHOD(TestLostFocusWritesWhenEditingFinishes)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::LostFocus);

        std::wstring sAfterEdit, sAfterFinish;
        oQt.RunOnQt([pBound, &vmText, &sAfterEdit, &sAfterFinish]() {
            Type(pBound->oLineEdit, QStringLiteral("typed"));
            sAfterEdit = vmText.CopyText();
            Q_EMIT pBound->oLineEdit.editingFinished();
            sAfterFinish = vmText.CopyText();
        });
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(), sAfterEdit);
        Assert::AreEqual(std::wstring(L"typed"), sAfterFinish);
    }

    TEST_METHOD(TestKeyPressWritesOnEachEdit)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::KeyPress);

        std::wstring sFirst, sSecond;
        oQt.RunOnQt([pBound, &vmText, &sFirst, &sSecond]() {
            Type(pBound->oLineEdit, QStringLiteral("a"));
            sFirst = vmText.CopyText();
            Type(pBound->oLineEdit, QStringLiteral("ab"));
            sSecond = vmText.CopyText();
        });
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(L"a"), sFirst);
        Assert::AreEqual(std::wstring(L"ab"), sSecond);
    }

    TEST_METHOD(TestTypingWritesAfterAPause)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::Typing);

        std::wstring sAtOnce;
        oQt.RunOnQt([pBound, &vmText, &sAtOnce]() {
            Type(pBound->oLineEdit, QStringLiteral("typed"));
            sAtOnce = vmText.CopyText();
        });
        const bool bWritten =
            QtTestHost::WaitFor([&vmText]() { return vmText.CopyText() == L"typed"; }, std::chrono::seconds(2));
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(), sAtOnce, L"written at once, not after the pause");
        Assert::IsTrue(bWritten, L"never written after the pause");
    }

    TEST_METHOD(TestNoneNeverWrites)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::None);

        std::wstring sAfter;
        oQt.RunOnQt([pBound, &vmText, &sAfter]() {
            Type(pBound->oLineEdit, QStringLiteral("typed"));
            Q_EMIT pBound->oLineEdit.editingFinished();
            sAfter = vmText.CopyText();
        });
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(), sAfter);
    }

    TEST_METHOD(TestAnUpdateFromTheViewModelIsNotWrittenBack)
    {
        // Two changes are queued while the Qt thread is held. A control that wrote back on a programmatic setText
        // would write the stale first one over the second, and the view model would announce more than two changes.
        TextViewModel vmText;
        TextChangeCounter oCounter;
        vmText.AddNotifyTarget(oCounter);
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::KeyPress);

        std::promise<void> oRelease;
        std::shared_future<void> fRelease = oRelease.get_future().share();
        std::atomic<bool> bHeld{false};
        oQt.Host().Invoke([&bHeld, fRelease]() {
            bHeld = true;
            fRelease.wait_for(std::chrono::seconds(5));
        });
        const bool bWasHeld = QtTestHost::WaitFor([&bHeld]() { return bHeld.load(); });

        std::thread([&vmText]() {
            vmText.SetText(L"one");
            vmText.SetText(L"two");
        }).join();
        oRelease.set_value();

        const bool bShown = oQt.WaitOnQt([pBound]() { return pBound->oLineEdit.text() == QStringLiteral("two"); });
        Delete(oQt, pBound);
        vmText.RemoveNotifyTarget(oCounter);

        Assert::IsTrue(bWasHeld, L"the Qt thread was never held");
        Assert::IsTrue(bShown, L"the control never showed the last change");
        Assert::AreEqual(2, oCounter.nChanges.load(), L"the control wrote a view-model change back");
        Assert::AreEqual(std::wstring(L"two"), vmText.CopyText());
    }

    TEST_METHOD(TestTheBindingStaysRegisteredWhileItWrites)
    {
        // The view model derives ReadOnly from the text on the writing thread. SetValue's remove/re-add would leave
        // the binding out while that happens; SetValueFromControl keeps it in.
        TextViewModel vmText;
        vmText.bDeriveReadOnly = true;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::KeyPress);

        bool bReadOnly = false;
        oQt.RunOnQt([pBound, &bReadOnly]() {
            Type(pBound->oLineEdit, QStringLiteral("typed"));
            bReadOnly = pBound->oLineEdit.isReadOnly();
        });
        Delete(oQt, pBound);

        Assert::IsTrue(bReadOnly, L"the derived change never reached the control");
    }

    TEST_METHOD(TestUpdateSourceWritesNow)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::LostFocus);

        std::wstring sAfter;
        oQt.RunOnQt([pBound, &vmText, &sAfter]() {
            pBound->oLineEdit.setText(QStringLiteral("typed")); // no signal: the user has not left the field
            pBound->oBinding.UpdateSource();
            sAfter = vmText.CopyText();
        });
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(L"typed"), sAfter);
    }

    TEST_METHOD(TestDetachStopsBothDirections)
    {
        TextViewModel vmText;
        QtTestHost oQt;
        auto* pBound = Create(oQt, vmText, TextBoxBinding::UpdateMode::KeyPress);
        oQt.RunOnQt([pBound]() { pBound->oBinding.Detach(); });

        std::thread([&vmText]() { vmText.SetText(L"from a worker"); }).join();

        std::wstring sShown, sAfterEdit;
        oQt.RunOnQt([pBound, &vmText, &sShown, &sAfterEdit]() {
            sShown = pBound->oLineEdit.text().toStdWString();
            Type(pBound->oLineEdit, QStringLiteral("typed"));
            pBound->oBinding.UpdateSource();
            sAfterEdit = vmText.CopyText();
        });
        Delete(oQt, pBound);

        Assert::AreEqual(std::wstring(), sShown, L"a change reached the control after Detach");
        Assert::AreEqual(std::wstring(L"from a worker"), sAfterEdit, L"the control wrote after Detach");
    }
};

} // namespace tests
} // namespace bindings
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
