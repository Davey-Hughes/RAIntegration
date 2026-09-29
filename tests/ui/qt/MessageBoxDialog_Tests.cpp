#ifndef _WIN32

#include "ui/qt/MessageBoxDialog.hh"

#include "ui/qt/QtDesktop.hh"

#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/ModalCaller.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QAbstractButton>
#include <QApplication>
#include <QCheckBox>
#include <QKeyEvent>
#include <QLabel>

#include <atomic>
#include <chrono>
#include <functional>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

namespace {

using ra::ui::viewmodels::MessageBoxViewModel;

MessageBoxDialog* FindBox() { return dynamic_cast<MessageBoxDialog*>(QApplication::activeModalWidget()); }

void RejectAllBoxes()
{
    for (auto* pWidget : QApplication::topLevelWidgets())
    {
        auto* pBox = dynamic_cast<MessageBoxDialog*>(pWidget);
        if (pBox != nullptr && pBox->isVisible())
            pBox->reject();
    }
}

void Click(MessageBoxDialog& oBox, QMessageBox::StandardButton nButton)
{
    auto* pButton = oBox.button(nButton);
    if (pButton != nullptr)
        pButton->click();
    else
        oBox.reject(); // the answer will be wrong, and the test says so
}

void PressEscape(MessageBoxDialog& oBox)
{
    QKeyEvent oEvent(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&oBox, &oEvent);
}

// Shows a box with nButtons from a worker - as ShowModal is called in
// practice - lets fAnswer close it on the Qt thread, and returns the answer
// the worker got.
DialogResult AnswerFromWorker(QtTestHost& oQt, const QtDesktop& oDesktop, MessageBoxViewModel::Buttons nButtons,
                              const std::function<void(MessageBoxDialog&)>& fAnswer)
{
    MessageBoxViewModel vmMessageBox(L"message");
    vmMessageBox.SetButtons(nButtons);
    ModalCaller oCaller(oDesktop, vmMessageBox);

    const bool bShown = oQt.WaitOnQt([]() { return FindBox() != nullptr; });
    if (bShown)
        oQt.RunOnQt([&fAnswer]() { fAnswer(*FindBox()); });
    const bool bReturned = oCaller.Returned(std::chrono::seconds(2));
    if (!bReturned)
        oQt.RunOnQt(RejectAllBoxes);

    Assert::IsTrue(bShown, L"no message box appeared");
    Assert::IsTrue(bReturned, L"closing the box did not release the caller");
    return oCaller.Result();
}

// As AnswerFromWorker, for a Yes/No box with a checkbox that is ticked on the
// Qt thread before fAnswer closes the box. Also returns the view model's tick.
DialogResult AnswerTickedFromWorker(QtTestHost& oQt, const QtDesktop& oDesktop,
                                    const std::function<void(MessageBoxDialog&)>& fAnswer, bool& bChecked)
{
    MessageBoxViewModel vmMessageBox(L"message");
    vmMessageBox.SetButtons(MessageBoxViewModel::Buttons::YesNo);
    vmMessageBox.SetCheckBoxText(L"Don't remind me");
    ModalCaller oCaller(oDesktop, vmMessageBox);

    const bool bShown = oQt.WaitOnQt([]() { return FindBox() != nullptr; });
    bool bTicked = false;
    if (bShown)
    {
        oQt.RunOnQt([&fAnswer, &bTicked]() {
            auto* pBox = FindBox();
            if (pBox->checkBox() != nullptr)
            {
                pBox->checkBox()->click();
                bTicked = pBox->checkBox()->isChecked();
            }
            fAnswer(*pBox);
        });
    }
    const bool bReturned = oCaller.Returned(std::chrono::seconds(2));
    if (!bReturned)
        oQt.RunOnQt(RejectAllBoxes);

    Assert::IsTrue(bShown, L"no message box appeared");
    Assert::IsTrue(bTicked, L"the box had no checkbox to tick");
    Assert::IsTrue(bReturned, L"closing the box did not release the caller");
    bChecked = vmMessageBox.IsCheckBoxChecked();
    return oCaller.Result();
}

} // namespace

TEST_CLASS(MessageBoxDialog_Tests)
{
public:
    TEST_METHOD(TestButtonsMapToAnswers)
    {
        QtTestHost oQt;
        QtDesktop oDesktop;

        struct Case
        {
            MessageBoxViewModel::Buttons nButtons;
            QMessageBox::StandardButton nClick;
            DialogResult nExpected;
        };
        const Case vCases[] = {
            {MessageBoxViewModel::Buttons::OK, QMessageBox::Ok, DialogResult::OK},
            {MessageBoxViewModel::Buttons::OKCancel, QMessageBox::Ok, DialogResult::OK},
            {MessageBoxViewModel::Buttons::OKCancel, QMessageBox::Cancel, DialogResult::Cancel},
            {MessageBoxViewModel::Buttons::YesNo, QMessageBox::Yes, DialogResult::Yes},
            {MessageBoxViewModel::Buttons::YesNo, QMessageBox::No, DialogResult::No},
            {MessageBoxViewModel::Buttons::YesNoCancel, QMessageBox::Yes, DialogResult::Yes},
            {MessageBoxViewModel::Buttons::YesNoCancel, QMessageBox::No, DialogResult::No},
            {MessageBoxViewModel::Buttons::YesNoCancel, QMessageBox::Cancel, DialogResult::Cancel},
            {MessageBoxViewModel::Buttons::RetryCancel, QMessageBox::Retry, DialogResult::Retry},
            {MessageBoxViewModel::Buttons::RetryCancel, QMessageBox::Cancel, DialogResult::Cancel},
        };

        for (const auto& oCase : vCases)
        {
            const auto nAnswer = AnswerFromWorker(oQt, oDesktop, oCase.nButtons,
                                                  [&oCase](MessageBoxDialog& oBox) { Click(oBox, oCase.nClick); });
            Assert::AreEqual(oCase.nExpected, nAnswer);
        }
    }

    TEST_METHOD(TestClosingWithoutAButtonGivesTheEscapeAnswer)
    {
        QtTestHost oQt;
        QtDesktop oDesktop;

        struct Case
        {
            MessageBoxViewModel::Buttons nButtons;
            DialogResult nExpected;
        };
        const Case vCases[] = {
            {MessageBoxViewModel::Buttons::OK, DialogResult::OK},
            {MessageBoxViewModel::Buttons::OKCancel, DialogResult::Cancel},
            {MessageBoxViewModel::Buttons::YesNo, DialogResult::No},
            {MessageBoxViewModel::Buttons::YesNoCancel, DialogResult::Cancel},
            {MessageBoxViewModel::Buttons::RetryCancel, DialogResult::Cancel},
        };

        for (const auto& oCase : vCases)
        {
            // Esc clicks the escape button
            Assert::AreEqual(oCase.nExpected, AnswerFromWorker(oQt, oDesktop, oCase.nButtons, PressEscape));

            // reject() - the close button, or shutdown - clicks nothing
            Assert::AreEqual(oCase.nExpected, AnswerFromWorker(oQt, oDesktop, oCase.nButtons,
                                                               [](MessageBoxDialog& oBox) { oBox.reject(); }));
        }
    }

    TEST_METHOD(TestTheBoxShowsTheViewModelAsPlainText)
    {
        MessageBoxViewModel vmMessageBox(L"The <b>message</b>");
        vmMessageBox.SetHeader(L"The header");
        vmMessageBox.SetWindowTitle(L"RALibRetro");
        vmMessageBox.SetIcon(MessageBoxViewModel::Icon::Warning);
        vmMessageBox.SetButtons(MessageBoxViewModel::Buttons::YesNo);
        QtTestHost oQt;

        QString sTitle, sText, sInformative;
        bool bPlain = false;
        int nRichLabels = -1;
        bool bWarningIcon = false;
        bool bYesNo = false;
        oQt.RunOnQt([&]() {
            MessageBoxDialog oBox(vmMessageBox);
            sTitle = oBox.windowTitle();
            sText = oBox.text();
            sInformative = oBox.informativeText();
            bPlain = (oBox.textFormat() == Qt::PlainText);
            nRichLabels = 0;
            for (const auto* pLabel : oBox.findChildren<QLabel*>())
            {
                if (!pLabel->text().isEmpty() && pLabel->textFormat() != Qt::PlainText)
                    ++nRichLabels;
            }
            bWarningIcon = (oBox.icon() == QMessageBox::Warning);
            bYesNo = (oBox.standardButtons() == (QMessageBox::Yes | QMessageBox::No));
        });

        Assert::AreEqual(std::wstring(L"RALibRetro"), sTitle.toStdWString());
        Assert::AreEqual(std::wstring(L"The header"), sText.toStdWString());
        Assert::AreEqual(std::wstring(L"The <b>message</b>"), sInformative.toStdWString());
        Assert::IsTrue(bPlain, L"the text format is not plain");
        Assert::AreEqual(0, nRichLabels);
        Assert::IsTrue(bWarningIcon);
        Assert::IsTrue(bYesNo);
    }

    TEST_METHOD(TestWithoutAHeaderTheMessageIsTheText)
    {
        MessageBoxViewModel vmMessageBox(L"Only a message");
        QtTestHost oQt;

        QString sText, sInformative;
        oQt.RunOnQt([&]() {
            MessageBoxDialog oBox(vmMessageBox);
            sText = oBox.text();
            sInformative = oBox.informativeText();
        });

        Assert::AreEqual(std::wstring(L"Only a message"), sText.toStdWString());
        Assert::IsTrue(sInformative.isEmpty());
    }

    TEST_METHOD(TestIcons)
    {
        QtTestHost oQt;

        struct Case
        {
            MessageBoxViewModel::Icon nIcon;
            QMessageBox::Icon nExpected;
        };
        const Case vCases[] = {
            {MessageBoxViewModel::Icon::None, QMessageBox::NoIcon},
            {MessageBoxViewModel::Icon::Info, QMessageBox::Information},
            {MessageBoxViewModel::Icon::Warning, QMessageBox::Warning},
            {MessageBoxViewModel::Icon::Error, QMessageBox::Critical},
        };

        for (const auto& oCase : vCases)
        {
            MessageBoxViewModel vmMessageBox(L"message");
            vmMessageBox.SetIcon(oCase.nIcon);
            bool bMatches = false;
            oQt.RunOnQt([&]() {
                MessageBoxDialog oBox(vmMessageBox);
                bMatches = (oBox.icon() == oCase.nExpected);
            });

            Assert::IsTrue(bMatches, L"wrong icon");
        }
    }

    TEST_METHOD(TestNoCheckBoxWithoutText)
    {
        MessageBoxViewModel vmMessageBox(L"message");
        QtTestHost oQt;

        bool bHasCheckBox = true;
        oQt.RunOnQt([&]() {
            MessageBoxDialog oBox(vmMessageBox);
            bHasCheckBox = (oBox.checkBox() != nullptr);
        });

        Assert::IsFalse(bHasCheckBox);
    }

    TEST_METHOD(TestTheCheckBoxShowsTheViewModel)
    {
        QtTestHost oQt;

        for (const bool bChecked : {false, true})
        {
            MessageBoxViewModel vmMessageBox(L"message");
            vmMessageBox.SetCheckBoxText(L"Don't <b>remind</b> me");
            vmMessageBox.SetCheckBoxChecked(bChecked);
            std::wstring sText;
            bool bShownChecked = !bChecked;
            oQt.RunOnQt([&]() {
                MessageBoxDialog oBox(vmMessageBox);
                if (oBox.checkBox() != nullptr)
                {
                    sText = oBox.checkBox()->text().toStdWString();
                    bShownChecked = oBox.checkBox()->isChecked();
                }
            });

            Assert::AreEqual(std::wstring(L"Don't <b>remind</b> me"), sText);
            Assert::AreEqual(bChecked, bShownChecked);
        }
    }

    TEST_METHOD(TestTheTickIsKeptWhateverClosesTheBox)
    {
        QtTestHost oQt;
        QtDesktop oDesktop;

        struct Case
        {
            const wchar_t* sHow;
            std::function<void(MessageBoxDialog&)> fClose;
            DialogResult nExpected;
        };
        const Case vCases[] = {
            {L"Yes", [](MessageBoxDialog& oBox) { Click(oBox, QMessageBox::Yes); }, DialogResult::Yes},
            {L"No", [](MessageBoxDialog& oBox) { Click(oBox, QMessageBox::No); }, DialogResult::No},
            {L"Esc", PressEscape, DialogResult::No},
            {L"reject()", [](MessageBoxDialog& oBox) { oBox.reject(); }, DialogResult::No},
        };

        for (const auto& oCase : vCases)
        {
            bool bChecked = false;
            Assert::AreEqual(oCase.nExpected, AnswerTickedFromWorker(oQt, oDesktop, oCase.fClose, bChecked), oCase.sHow);
            Assert::IsTrue(bChecked, oCase.sHow);
        }
    }

    TEST_METHOD(TestTheTickIsWrittenBeforeTheAnswer)
    {
        // What the view model says about the tick at the moment its result
        // changes: the waiting caller wakes then, and may destroy it at once.
        class ResultWatcher : public ra::ui::ViewModelBase::NotifyTarget
        {
        public:
            void OnViewModelIntValueChanged(const IntModelProperty::ChangeArgs& args) override
            {
                if (args.Property == ra::ui::WindowViewModelBase::DialogResultProperty && m_pViewModel != nullptr)
                    m_nCheckedAtResult = m_pViewModel->IsCheckBoxChecked() ? 1 : 0;
            }

            const MessageBoxViewModel* m_pViewModel = nullptr;
            std::atomic<int> m_nCheckedAtResult{-1};
        };

        ResultWatcher oWatcher; // before the view model, which holds a reference to it
        MessageBoxViewModel vmMessageBox(L"message");
        vmMessageBox.SetButtons(MessageBoxViewModel::Buttons::YesNo);
        vmMessageBox.SetCheckBoxText(L"Don't remind me");
        oWatcher.m_pViewModel = &vmMessageBox;
        vmMessageBox.AddNotifyTarget(oWatcher);
        QtTestHost oQt;
        QtDesktop oDesktop;

        {
            ModalCaller oCaller(oDesktop, vmMessageBox);
            const bool bShown = oQt.WaitOnQt([]() { return FindBox() != nullptr; });
            if (bShown)
            {
                oQt.RunOnQt([]() {
                    auto* pBox = FindBox();
                    if (pBox->checkBox() != nullptr)
                        pBox->checkBox()->click();
                    Click(*pBox, QMessageBox::Yes);
                });
            }
            const bool bReturned = oCaller.Returned(std::chrono::seconds(2));
            if (!bReturned)
                oQt.RunOnQt(RejectAllBoxes);

            Assert::IsTrue(bShown, L"no message box appeared");
            Assert::IsTrue(bReturned, L"closing the box did not release the caller");
            Assert::AreEqual(DialogResult::Yes, oCaller.Result());
        }

        vmMessageBox.RemoveNotifyTarget(oWatcher);
        Assert::AreEqual(1, oWatcher.m_nCheckedAtResult.load(), L"the tick was not in the view model when the answer was");
    }

    TEST_METHOD(TestShowWindowOpensNothing)
    {
        MessageBoxViewModel vmMessageBox(L"message");
        QtTestHost oQt;
        QtDesktop oDesktop;

        oDesktop.ShowWindow(vmMessageBox);
        oQt.RunOnQt([]() {}); // the queued ShowWindow has run

        int nBoxes = -1;
        oQt.RunOnQt([&nBoxes]() {
            nBoxes = 0;
            for (auto* pWidget : QApplication::topLevelWidgets())
            {
                if (dynamic_cast<MessageBoxDialog*>(pWidget) != nullptr)
                    ++nBoxes;
            }
        });

        Assert::AreEqual(0, nBoxes);
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
