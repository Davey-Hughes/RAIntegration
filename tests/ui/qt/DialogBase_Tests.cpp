#ifndef _WIN32

#include "ui/qt/DialogBase.hh"

#include "tests/mocks/MockWindowConfiguration.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QPointer>
#include <QSize>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

namespace {

class TestViewModel : public WindowViewModelBase
{
};

class TestWindow : public DialogBase
{
public:
    explicit TestWindow(WindowViewModelBase& vmWindow) : DialogBase(vmWindow)
    {
        setMinimumSize(240, 90);
        m_bindWindow.SetSizeKey("Test Window");
        m_bindWindow.SetWidget(*this);
    }
};

} // namespace

TEST_CLASS(DialogBase_Tests)
{
public:
    TEST_METHOD(TestShowingItSetsIsVisible)
    {
        TestViewModel vmWindow;
        QtTestHost oQt;
        QPointer<TestWindow> pWindow;
        oQt.RunOnQt([&vmWindow, &pWindow]() {
            pWindow = new TestWindow(vmWindow);
            pWindow->show();
        });
        const bool bVisible = vmWindow.IsVisible();
        oQt.RunOnQt([&pWindow]() { delete pWindow.data(); });

        Assert::IsTrue(bVisible);
    }

    TEST_METHOD(TestClosingItClearsIsVisibleAndDeletesIt)
    {
        TestViewModel vmWindow;
        QtTestHost oQt;
        QPointer<TestWindow> pWindow;
        oQt.RunOnQt([&vmWindow, &pWindow]() {
            pWindow = new TestWindow(vmWindow);
            pWindow->show();
        });

        oQt.RunOnQt([&pWindow]() { pWindow->close(); });
        const bool bVisible = vmWindow.IsVisible();
        const bool bDeleted = oQt.WaitOnQt([&pWindow]() { return pWindow.isNull(); });

        Assert::IsFalse(bVisible);
        Assert::IsTrue(bDeleted, L"the closed window was not deleted");
    }

    TEST_METHOD(TestASavedSizeIsRestored)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockWindowConfiguration.SetWindowSize("Test Window", ra::ui::Size{300, 200});
        TestViewModel vmWindow;
        QtTestHost oQt;
        QPointer<TestWindow> pWindow;
        QSize oSize;
        oQt.RunOnQt([&vmWindow, &pWindow, &oSize]() {
            pWindow = new TestWindow(vmWindow);
            pWindow->show();
            oSize = pWindow->size();
        });
        oQt.RunOnQt([&pWindow]() { delete pWindow.data(); });

        Assert::AreEqual(300, oSize.width());
        Assert::AreEqual(200, oSize.height());
    }

    TEST_METHOD(TestASavedSizeBelowTheMinimumIsClamped)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        mockWindowConfiguration.SetWindowSize("Test Window", ra::ui::Size{100, 50});
        TestViewModel vmWindow;
        QtTestHost oQt;
        QPointer<TestWindow> pWindow;
        QSize oSize;
        oQt.RunOnQt([&vmWindow, &pWindow, &oSize]() {
            pWindow = new TestWindow(vmWindow);
            pWindow->show();
            oSize = pWindow->size();
        });
        oQt.RunOnQt([&pWindow]() { delete pWindow.data(); });

        Assert::AreEqual(240, oSize.width());
        Assert::AreEqual(90, oSize.height());
    }

    TEST_METHOD(TestResizingItSavesTheSize)
    {
        ra::services::mocks::MockWindowConfiguration mockWindowConfiguration;
        TestViewModel vmWindow;
        QtTestHost oQt;
        QPointer<TestWindow> pWindow;
        oQt.RunOnQt([&vmWindow, &pWindow]() {
            pWindow = new TestWindow(vmWindow);
            pWindow->show();
            pWindow->resize(400, 250);
        });

        const bool bSaved = oQt.WaitOnQt([&mockWindowConfiguration]() {
            const auto oSize = mockWindowConfiguration.GetWindowSize("Test Window");
            return oSize.Width == 400 && oSize.Height == 250;
        });
        oQt.RunOnQt([&pWindow]() { delete pWindow.data(); });

        Assert::IsTrue(bSaved, L"the new size was not saved");
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
