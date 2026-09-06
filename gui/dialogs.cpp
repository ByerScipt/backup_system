#include "gui_common.hpp"

void configureFileDialog(QFileDialog& dialog, QWidget* owner,
                         bool directoryMode) {
    QScreen* targetScreen = owner ? owner->screen() : nullptr;
    if (!targetScreen) targetScreen = QApplication::primaryScreen();

    const QSize available =
        targetScreen ? targetScreen->availableGeometry().size()
                     : QSize(1280, 800);
    const int maximumWidth = std::max(640, qRound(available.width() * 0.92));
    const int maximumHeight = std::max(480, qRound(available.height() * 0.86));
    const int targetWidth =
        std::min(maximumWidth,
                 std::clamp(qRound(available.width() * 0.78), 860, 1280));
    const int targetHeight =
        std::min(maximumHeight,
                 std::clamp(qRound(available.height() * 0.74), 560, 840));

    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(std::min(760, targetWidth),
                          std::min(500, targetHeight));
    dialog.resize(targetWidth, targetHeight);
    dialog.setLabelText(QFileDialog::LookIn, "位置");
    dialog.setLabelText(QFileDialog::Reject, "取消");
    dialog.setLabelText(QFileDialog::FileType, "类型");

    if (directoryMode) {
        dialog.setViewMode(QFileDialog::List);
        dialog.setLabelText(QFileDialog::FileName, "目录");
        dialog.setLabelText(QFileDialog::Accept, "选择");
    } else {
        dialog.setViewMode(QFileDialog::Detail);
        dialog.setLabelText(QFileDialog::FileName, "文件名");
        for (auto* view : dialog.findChildren<QTreeView*>()) {
            QHeaderView* header = view->header();
            header->setStretchLastSection(false);
            header->setSectionResizeMode(0, QHeaderView::Stretch);
            for (int column = 1; column < header->count(); ++column) {
                header->setSectionResizeMode(
                    column, QHeaderView::ResizeToContents);
            }
        }
    }

    const QString capturePath =
        qEnvironmentVariable("BACKUP_GUI_DIALOG_CAPTURE");
    if (!capturePath.isEmpty()) {
        QTimer::singleShot(300, &dialog, [&dialog, capturePath]() {
            dialog.grab().save(capturePath);
            dialog.reject();
        });
    }
}

QString selectDirectory(QWidget* owner, const QString& title,
                        const QString& initialPath) {
    QFileDialog dialog(owner);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setWindowTitle(title);
    dialog.setFileMode(QFileDialog::Directory);
    dialog.setOption(QFileDialog::ShowDirsOnly, true);
    if (!initialPath.isEmpty()) dialog.setDirectory(initialPath);
    configureFileDialog(dialog, owner, true);
    if (dialog.exec() != QDialog::Accepted) return {};
    return dialog.selectedFiles().value(0);
}

QString selectArchive(QWidget* owner, const QString& title,
                      const QString& initialPath) {
    QFileDialog dialog(owner);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setWindowTitle(title);
    dialog.setFileMode(QFileDialog::ExistingFile);
    dialog.setNameFilter("Backup Archive (*.bak)");
    if (!initialPath.isEmpty()) dialog.selectFile(initialPath);
    dialog.setLabelText(QFileDialog::Accept, "打开");
    configureFileDialog(dialog, owner, false);
    if (dialog.exec() != QDialog::Accepted) return {};
    return dialog.selectedFiles().value(0);
}

QString selectArchiveOutput(QWidget* owner, const QString& title,
                            const QString& initialPath) {
    QFileDialog dialog(owner);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setWindowTitle(title);
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setFileMode(QFileDialog::AnyFile);
    dialog.setNameFilter("Backup Archive (*.bak)");
    dialog.setDefaultSuffix("bak");
    if (!initialPath.isEmpty()) dialog.selectFile(initialPath);
    dialog.setLabelText(QFileDialog::Accept, "保存");
    configureFileDialog(dialog, owner, false);
    if (dialog.exec() != QDialog::Accepted) return {};
    return dialog.selectedFiles().value(0);
}

