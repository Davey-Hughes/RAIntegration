#ifndef RA_UI_QT_FILEDIALOG_HH
#define RA_UI_QT_FILEDIALOG_HH
#pragma once

#include "ui/qt/IDialogPresenter.hh"
#include "ui/viewmodels/FileDialogViewModel.hh"

#include <QFileDialog>
#include <QStringList>

namespace ra {
namespace ui {
namespace qt {

/// <summary>
/// A FileDialogViewModel as a QFileDialog: the Qt counterpart of ui/win32/FileDialog. Always Qt's own dialog, never the
/// desktop's native one: QtDesktop shows a modal with show() and waits for finished(), which only a real QDialog
/// honours.
/// </summary>
class FileDialog : public QFileDialog
{
public:
    class Presenter : public IDialogPresenter
    {
    public:
        bool IsSupported(const ra::ui::WindowViewModelBase& vmWindow) override;
        void ShowWindow(ra::ui::WindowViewModelBase& vmWindow) override;
        std::unique_ptr<QDialog> CreateModal(ra::ui::WindowViewModelBase& vmWindow) override;
    };

    explicit FileDialog(ra::ui::viewmodels::FileDialogViewModel& vmFileDialog);

    /// <summary>Writes the chosen path and the answer to the view model - once - then closes as QFileDialog does.</summary>
    void done(int nResult) override;

    /// <summary>
    /// Qt name filters for the view model's file types, "All Files (*)" last. <paramref name="sSelectedFilter" /> gets
    /// the one holding the default extension, as Win32 selects it, or stays empty.
    /// </summary>
    static QStringList BuildNameFilters(const ra::ui::viewmodels::FileDialogViewModel& vmFileDialog,
                                        QString& sSelectedFilter);

private:
    ra::ui::viewmodels::FileDialogViewModel* m_pViewModel; // cleared once answered: the caller may destroy it then
};

} // namespace qt
} // namespace ui
} // namespace ra

#endif // !RA_UI_QT_FILEDIALOG_HH
