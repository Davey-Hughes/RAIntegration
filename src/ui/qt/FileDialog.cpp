#include "ui/qt/FileDialog.hh"

#include "util/Log.hh"
#include "util/Strings.hh"

#include <QString>

namespace ra {
namespace ui {
namespace qt {

using ra::ui::viewmodels::FileDialogViewModel;

bool FileDialog::Presenter::IsSupported(const ra::ui::WindowViewModelBase& vmWindow)
{
    return dynamic_cast<const FileDialogViewModel*>(&vmWindow) != nullptr;
}

void FileDialog::Presenter::ShowWindow(ra::ui::WindowViewModelBase& vmWindow)
{
    // Win32 shows it modally even from ShowWindow. Here nobody would wait for it, and its answer would go to a view
    // model that may be gone. No caller does this, so it is refused visibly, as a message box is.
    RA_LOG_WARN("File dialog \"%s\" shown without waiting for an answer - not shown; use ShowModal",
                ra::util::String::Narrow(vmWindow.GetWindowTitle()).c_str());
}

std::unique_ptr<QDialog> FileDialog::Presenter::CreateModal(ra::ui::WindowViewModelBase& vmWindow)
{
    return std::make_unique<FileDialog>(dynamic_cast<FileDialogViewModel&>(vmWindow));
}

FileDialog::FileDialog(FileDialogViewModel& vmFileDialog) : QFileDialog(nullptr), m_pViewModel(&vmFileDialog)
{
    setOption(QFileDialog::DontUseNativeDialog);
    setWindowTitle(QString::fromStdWString(vmFileDialog.GetWindowTitle()));

    const auto& sInitialDirectory = vmFileDialog.GetInitialDirectory();
    if (!sInitialDirectory.empty())
        setDirectory(QString::fromStdWString(sInitialDirectory));

    switch (vmFileDialog.GetMode())
    {
        case FileDialogViewModel::Mode::Folder:
            setFileMode(QFileDialog::Directory);
            setOption(QFileDialog::ShowDirsOnly);
            break;

        case FileDialogViewModel::Mode::Save:
            setAcceptMode(QFileDialog::AcceptSave);
            setFileMode(QFileDialog::AnyFile);
            if (!vmFileDialog.GetOverwritePrompt())
                setOption(QFileDialog::DontConfirmOverwrite);
            break;

        default:
            setAcceptMode(QFileDialog::AcceptOpen);
            setFileMode(QFileDialog::ExistingFile);
            break;
    }

    if (vmFileDialog.GetMode() != FileDialogViewModel::Mode::Folder)
    {
        const auto& sDefaultExtension = vmFileDialog.GetDefaultExtension();
        if (!sDefaultExtension.empty())
            setDefaultSuffix(QString::fromStdWString(sDefaultExtension));

        QString sSelectedFilter;
        setNameFilters(BuildNameFilters(vmFileDialog, sSelectedFilter));
        if (!sSelectedFilter.isEmpty())
            selectNameFilter(sSelectedFilter);
    }

    const auto& sFileName = vmFileDialog.GetFileName();
    if (!sFileName.empty())
        selectFile(QString::fromStdWString(sFileName));
}

QStringList FileDialog::BuildNameFilters(const FileDialogViewModel& vmFileDialog, QString& sSelectedFilter)
{
    // Win32's patterns are "*.a;*.b"; Qt's filters are "Description (*.a *.b)".
    const auto& sDefaultExtension = vmFileDialog.GetDefaultExtension();
    const QString sDefaultPattern = QStringLiteral("*.") + QString::fromStdWString(sDefaultExtension);

    QStringList vFilters;
    for (const auto& pPair : vmFileDialog.GetFileTypes())
    {
        const QStringList vPatterns = QString::fromStdWString(pPair.first).split(QLatin1Char(';'), Qt::SkipEmptyParts);
        const QString sFilter = QString::fromStdWString(pPair.second) + QStringLiteral(" (") +
                                vPatterns.join(QLatin1Char(' ')) + QLatin1Char(')');
        vFilters.append(sFilter);

        if (sSelectedFilter.isEmpty() && !sDefaultExtension.empty() &&
            vPatterns.contains(sDefaultPattern, Qt::CaseInsensitive))
        {
            sSelectedFilter = sFilter;
        }
    }

    vFilters.append(QStringLiteral("All Files (*)"));
    return vFilters;
}

void FileDialog::done(int nResult)
{
    if (m_pViewModel != nullptr)
    {
        const QStringList vFiles = selectedFiles();
        if (nResult == QDialog::Accepted && !vFiles.isEmpty())
        {
            m_pViewModel->SetFileName(vFiles.front().toStdWString());
            m_pViewModel->SetDialogResult(ra::ui::DialogResult::OK);
        }
        else
        {
            m_pViewModel->SetDialogResult(ra::ui::DialogResult::Cancel);
        }

        // before finished(): the waiting caller reads it as soon as it wakes, and may destroy the view model then
        m_pViewModel = nullptr;
    }

    QFileDialog::done(nResult);
}

} // namespace qt
} // namespace ui
} // namespace ra
