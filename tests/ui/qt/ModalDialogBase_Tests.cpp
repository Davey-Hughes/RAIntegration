#ifndef _WIN32

#include "ui/qt/ModalDialogBase.hh"

#include "ui/qt/bindings/TextBoxBinding.hh"

#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QApplication>
#include <QDialogButtonBox>
#include <QKeyEvent>
#include <QLineEdit>
#include <QVBoxLayout>

#include <functional>
#include <thread>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

using ra::ui::qt::bindings::TextBoxBinding;

namespace {

class FormViewModel : public WindowViewModelBase
{
public:
    static const StringModelProperty TextProperty;

    std::wstring CopyText() const { return CopyValue(TextProperty); }
    void SetText(const std::wstring& sValue) { SetValue(TextProperty, sValue); }
};

const StringModelProperty FormViewModel::TextProperty("ModalDialogBaseTests", "Text", L"");

// One text box, LostFocus mode, and OK/Cancel. Built on the Qt thread.
class TestDialog : public ModalDialogBase
{
public:
    explicit TestDialog(FormViewModel& vmForm) : ModalDialogBase(vmForm), m_bindText(vmForm)
    {
        pLineEdit = new QLineEdit(this);
        auto* pLayout = new QVBoxLayout(this);
        pLayout->addWidget(pLineEdit);
        pLayout->addWidget(CreateButtons());

        m_bindText.BindText(FormViewModel::TextProperty);
        m_bindText.SetControl(*pLineEdit);
        RegisterTextBox(m_bindText);

        m_bindWindow.SetWidget(*this);

        connect(this, &QDialog::finished, this, [this]() {
            ++nFinished;
            if (fOnFinished)
                fOnFinished();
        });
    }

    QLineEdit* pLineEdit = nullptr;
    std::function<bool()> fCanAccept;
    std::function<void()> fOnFinished;
    int nFinished = 0;

protected:
    bool CanAccept() override { return fCanAccept ? fCanAccept() : true; }

private:
    TextBoxBinding m_bindText;
};

TestDialog* Open(QtTestHost& oQt, FormViewModel& vmForm)
{
    TestDialog* pDialog = nullptr;
    oQt.RunOnQt([&vmForm, &pDialog]() {
        pDialog = new TestDialog(vmForm);
        pDialog->show();
    });
    return pDialog;
}

void Delete(QtTestHost& oQt, QDialog* pDialog)
{
    oQt.RunOnQt([pDialog]() { delete pDialog; });
}

} // namespace

TEST_CLASS(ModalDialogBase_Tests)
{
public:
    TEST_METHOD(TestOkFlushesTheTextAndAnswersOK)
    {
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        int nFinished = 0;
        bool bVisible = true;
        oQt.RunOnQt([pDialog, &nFinished, &bVisible]() {
            pDialog->pLineEdit->setText(QStringLiteral("typed")); // not left the field: not written yet
            pDialog->pLineEdit->setModified(true);                // as a user's edit marks it
            pDialog->accept();
            nFinished = pDialog->nFinished;
            bVisible = pDialog->isVisible();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(L"typed"), vmForm.CopyText());
        Assert::AreEqual(DialogResult::OK, vmForm.GetDialogResult());
        Assert::AreEqual(1, nFinished, L"finished() emitted a wrong number of times");
        Assert::IsFalse(bVisible);
    }

    TEST_METHOD(TestCancelAnswersCancelWithoutWriting)
    {
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        oQt.RunOnQt([pDialog]() {
            pDialog->pLineEdit->setText(QStringLiteral("typed"));
            pDialog->reject();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(), vmForm.CopyText());
        Assert::AreEqual(DialogResult::Cancel, vmForm.GetDialogResult());
    }

    TEST_METHOD(TestEscapeAnswersCancel)
    {
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        oQt.RunOnQt([pDialog]() {
            QKeyEvent oEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
            QApplication::sendEvent(pDialog, &oEvent);
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(DialogResult::Cancel, vmForm.GetDialogResult());
    }

    TEST_METHOD(TestCanAcceptFalseKeepsItOpen)
    {
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        bool bVisibleAfterRefusal = false;
        int nFinishedAfterRefusal = -1;
        oQt.RunOnQt([pDialog, &bVisibleAfterRefusal, &nFinishedAfterRefusal]() {
            pDialog->fCanAccept = []() { return false; };
            pDialog->accept();
            bVisibleAfterRefusal = pDialog->isVisible();
            nFinishedAfterRefusal = pDialog->nFinished;

            pDialog->fCanAccept = []() { return true; };
            pDialog->accept();
        });
        Delete(oQt, pDialog);

        Assert::IsTrue(bVisibleAfterRefusal, L"closed although CanAccept said no");
        Assert::AreEqual(0, nFinishedAfterRefusal);
        Assert::AreEqual(DialogResult::OK, vmForm.GetDialogResult());
    }

    TEST_METHOD(TestAResultTheViewModelSetStands)
    {
        // The view model closes its own dialog: WindowBinding closes the window, which QDialog turns into reject().
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        std::thread([&vmForm]() { vmForm.SetDialogResult(DialogResult::OK); }).join();
        const bool bClosed = oQt.WaitOnQt([pDialog]() { return pDialog->nFinished == 1; });
        Delete(oQt, pDialog);

        Assert::IsTrue(bClosed, L"the view model's result never closed the dialog");
        Assert::AreEqual(DialogResult::OK, vmForm.GetDialogResult());
    }

    TEST_METHOD(TestAnEarlierResultIsClearedWhenCreated)
    {
        FormViewModel vmForm;
        vmForm.SetDialogResult(DialogResult::OK); // from an earlier showing
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        DialogResult nWhileOpen = DialogResult::OK;
        oQt.RunOnQt([&vmForm, &nWhileOpen]() { nWhileOpen = vmForm.GetDialogResult(); });
        oQt.RunOnQt([pDialog]() { pDialog->reject(); });
        Delete(oQt, pDialog);

        Assert::AreEqual(DialogResult::None, nWhileOpen);
        Assert::AreEqual(DialogResult::Cancel, vmForm.GetDialogResult());
    }

    TEST_METHOD(TestNothingReachesTheDialogAfterFinished)
    {
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        std::wstring sShown;
        oQt.RunOnQt([pDialog, &vmForm, &sShown]() {
            pDialog->fOnFinished = [&vmForm]() { vmForm.SetText(L"after finished"); };
            pDialog->reject();
            sShown = pDialog->pLineEdit->text().toStdWString();
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(std::wstring(), sShown, L"a binding was still attached when finished() was emitted");
    }

    TEST_METHOD(TestTheViewModelCanBeDestroyedWhenFinishedIsEmitted)
    {
        // What QtDesktop's caller does: it wakes on finished() and destroys its stack view model. Destroying the
        // dialog afterwards must leave the view model alone. Under AddressSanitizer a touch is a report.
        QtTestHost oQt;

        bool bDone = false;
        oQt.RunOnQt([&bDone]() {
            auto* pForm = new FormViewModel();
            auto* pDialog = new TestDialog(*pForm);
            pDialog->show();
            pDialog->fOnFinished = [pForm]() { delete pForm; };
            pDialog->accept();
            delete pDialog;
            bDone = true;
        });

        Assert::IsTrue(bDone);
    }

    TEST_METHOD(TestAResultSetOnTheQtThreadFinishesAfterTheSetter)
    {
        // The view model answers itself on the Qt thread, and its caller destroys it on finished(). Finishing inside
        // SetDialogResult would destroy the view model under its own call. Under AddressSanitizer that is a report.
        QtTestHost oQt;

        TestDialog* pDialog = nullptr;
        int nFinishedInside = -1;
        oQt.RunOnQt([&pDialog, &nFinishedInside]() {
            auto* pForm = new FormViewModel();
            pDialog = new TestDialog(*pForm);
            pDialog->show();
            pDialog->fOnFinished = [pForm]() { delete pForm; };
            pForm->SetDialogResult(DialogResult::OK);
            nFinishedInside = pDialog->nFinished;
        });
        const bool bFinished = oQt.WaitOnQt([pDialog]() { return pDialog->nFinished == 1; });
        Delete(oQt, pDialog);

        Assert::AreEqual(0, nFinishedInside, L"finished inside SetDialogResult");
        Assert::IsTrue(bFinished, L"the view model's result never finished the dialog");
    }

    TEST_METHOD(TestARejectDuringCanAcceptFinishesOnlyAfterIt)
    {
        // QtDesktop::CloseAll rejects every modal at shutdown - this one included, while CanAccept is in a nested
        // modal. Finishing then would release the caller while CanAccept, a view model method, is on the stack.
        FormViewModel vmForm;
        QtTestHost oQt;
        auto* pDialog = Open(oQt, vmForm);

        int nFinishedInside = -1, nFinishedAfter = -1;
        oQt.RunOnQt([pDialog, &nFinishedInside, &nFinishedAfter]() {
            pDialog->fCanAccept = [pDialog, &nFinishedInside]() {
                pDialog->reject();
                nFinishedInside = pDialog->nFinished;
                return true;
            };
            pDialog->accept();
            nFinishedAfter = pDialog->nFinished;
        });
        Delete(oQt, pDialog);

        Assert::AreEqual(0, nFinishedInside, L"finished while CanAccept was still running");
        Assert::AreEqual(1, nFinishedAfter);
        Assert::AreEqual(DialogResult::Cancel, vmForm.GetDialogResult());
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
