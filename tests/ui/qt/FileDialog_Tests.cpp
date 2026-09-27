#ifndef _WIN32

#include "ui/qt/FileDialog.hh"

#include "tests/ui/UIAsserts.hh"
#include "tests/ui/qt/QtTestHost.hh"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace ra {
namespace ui {
namespace qt {
namespace tests {

using ra::ui::viewmodels::FileDialogViewModel;

namespace {

// Creates the dialog through the presenter on the Qt thread, selects sPath, accepts, and deletes it. Returns whether
// accept() closed it.
bool ChooseAndAccept(QtTestHost& oQt, FileDialogViewModel& vmFileDialog, const QString& sPath)
{
    bool bClosed = false;
    oQt.RunOnQt([&vmFileDialog, &sPath, &bClosed]() {
        FileDialog::Presenter oPresenter;
        auto pDialog = oPresenter.CreateModal(vmFileDialog);
        auto* pFileDialog = dynamic_cast<QFileDialog*>(pDialog.get());
        if (pFileDialog == nullptr)
            return;
        pFileDialog->show();
        pFileDialog->selectFile(sPath);
        // QFileDialog re-declares accept() as protected (Qt 6.11); call it through the QDialog* it
        // still is - public there, and virtual dispatch still reaches QFileDialog's override.
        pDialog->accept();
        bClosed = !pFileDialog->isVisible();
    });
    return bClosed;
}

} // namespace

TEST_CLASS(FileDialog_Tests)
{
public:
    TEST_METHOD(TestFolderModeChoosesADirectory)
    {
        QTemporaryDir oTemp;
        Assert::IsTrue(QDir(oTemp.path()).mkdir(QStringLiteral("shots")));
        const QString sFolder = oTemp.filePath(QStringLiteral("shots"));

        FileDialogViewModel vmFileDialog;
        vmFileDialog.SetMode(FileDialogViewModel::Mode::Folder);
        vmFileDialog.SetInitialDirectory(oTemp.path().toStdWString());
        QtTestHost oQt;

        const bool bClosed = ChooseAndAccept(oQt, vmFileDialog, sFolder);

        Assert::IsTrue(bClosed, L"accept() did not close the dialog");
        Assert::AreEqual(DialogResult::OK, vmFileDialog.GetDialogResult());
        Assert::AreEqual(sFolder.toStdWString(), vmFileDialog.GetFileName());
    }

    TEST_METHOD(TestOpenModeChoosesAnExistingFile)
    {
        QTemporaryDir oTemp;
        const QString sFile = oTemp.filePath(QStringLiteral("notes.json"));
        QFile oFile(sFile);
        Assert::IsTrue(oFile.open(QIODevice::WriteOnly));
        oFile.close();

        FileDialogViewModel vmFileDialog;
        vmFileDialog.SetMode(FileDialogViewModel::Mode::Open);
        vmFileDialog.AddFileType(L"JSON File", L"*.json");
        vmFileDialog.SetDefaultExtension(L"json");
        vmFileDialog.SetInitialDirectory(oTemp.path().toStdWString());
        QtTestHost oQt;

        const bool bClosed = ChooseAndAccept(oQt, vmFileDialog, sFile);

        Assert::IsTrue(bClosed, L"accept() did not close the dialog");
        Assert::AreEqual(DialogResult::OK, vmFileDialog.GetDialogResult());
        Assert::AreEqual(sFile.toStdWString(), vmFileDialog.GetFileName());
    }

    TEST_METHOD(TestSaveModeAddsTheDefaultExtension)
    {
        QTemporaryDir oTemp;

        FileDialogViewModel vmFileDialog;
        vmFileDialog.SetMode(FileDialogViewModel::Mode::Save);
        vmFileDialog.AddFileType(L"JSON File", L"*.json");
        vmFileDialog.SetDefaultExtension(L"json");
        vmFileDialog.SetInitialDirectory(oTemp.path().toStdWString());
        QtTestHost oQt;

        const bool bClosed = ChooseAndAccept(oQt, vmFileDialog, oTemp.filePath(QStringLiteral("new")));

        Assert::IsTrue(bClosed, L"accept() did not close the dialog");
        Assert::AreEqual(DialogResult::OK, vmFileDialog.GetDialogResult());
        Assert::AreEqual(oTemp.filePath(QStringLiteral("new.json")).toStdWString(), vmFileDialog.GetFileName());
    }

    TEST_METHOD(TestRejectAnswersCancel)
    {
        FileDialogViewModel vmFileDialog;
        vmFileDialog.SetMode(FileDialogViewModel::Mode::Open);
        vmFileDialog.SetFileName(L"unchanged");
        QtTestHost oQt;

        oQt.RunOnQt([&vmFileDialog]() {
            FileDialog::Presenter oPresenter;
            auto pDialog = oPresenter.CreateModal(vmFileDialog);
            pDialog->show();
            pDialog->reject();
        });

        Assert::AreEqual(DialogResult::Cancel, vmFileDialog.GetDialogResult());
        Assert::AreEqual(std::wstring(L"unchanged"), vmFileDialog.GetFileName());
    }

    TEST_METHOD(TestNameFiltersSelectTheDefaultExtension)
    {
        FileDialogViewModel vmFileDialog;
        vmFileDialog.AddFileType(L"Text File", L"*.txt");
        vmFileDialog.AddFileType(L"JSON File", L"*.json;*.jsn");
        vmFileDialog.SetDefaultExtension(L"json");

        QString sSelected;
        const QStringList vFilters = FileDialog::BuildNameFilters(vmFileDialog, sSelected);

        Assert::IsTrue(vFilters.contains(QStringLiteral("JSON File (*.json *.jsn)")), L"no JSON filter");
        Assert::IsTrue(vFilters.contains(QStringLiteral("Text File (*.txt)")), L"no text filter");
        Assert::AreEqual(std::wstring(L"All Files (*)"), vFilters.last().toStdWString());
        Assert::AreEqual(std::wstring(L"JSON File (*.json *.jsn)"), sSelected.toStdWString());
    }

    TEST_METHOD(TestShowWindowOpensNothing)
    {
        FileDialogViewModel vmFileDialog;
        vmFileDialog.SetMode(FileDialogViewModel::Mode::Open);
        QtTestHost oQt;

        bool bAnyFileDialog = true;
        oQt.RunOnQt([&vmFileDialog, &bAnyFileDialog]() {
            FileDialog::Presenter oPresenter;
            oPresenter.ShowWindow(vmFileDialog);
            bAnyFileDialog = false;
            for (auto* pWidget : QApplication::topLevelWidgets())
                bAnyFileDialog |= (dynamic_cast<FileDialog*>(pWidget) != nullptr && pWidget->isVisible());
        });

        Assert::IsFalse(bAnyFileDialog, L"ShowWindow opened a dialog nobody waits for");
    }
};

} // namespace tests
} // namespace qt
} // namespace ui
} // namespace ra

#endif // !_WIN32
