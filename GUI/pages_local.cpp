#include "ui.hpp"
#include <QFormLayout>
#include <QFrame>
#include <QVBoxLayout>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTextEdit>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>

namespace backup::gui
{

QWidget* localBackupPage()
{
    Page page = makePage();
    Card configuration = makeCard();
    auto* form = makeForm();

    QLineEdit *source, *output, *key;
    QPushButton *browseSource, *browseOutput;
    QComboBox *pack, *compression, *encryption;
    form->addRow("源目录", pathRow(source, browseSource, "sourcePath"));
    form->addRow("输出归档", pathRow(output, browseOutput, "outputPath"));
    addAlgorithmRows(form, pack, compression, encryption, key);
    configuration.body->addLayout(form);
    page.layout->addWidget(configuration.frame, 1);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "备份");
    page.footer->addWidget(task.frame);

    bindPathPicker(page.widget, browseSource, source, "选择源目录",
                   selectDirectory);
    bindPathPicker(page.widget, browseOutput, output, "选择输出归档",
                   selectArchiveOutput);
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString src = source->text().trimmed();
            const QString out = output->text().trimmed();
            if (src.isEmpty() || out.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请选择源目录和输出归档。");
                return;
            }
            if (QFile::exists(out))
            {
                QMessageBox::warning(page.widget, "文件冲突",
                                     "输出归档已经存在，请选择新文件名。");
                return;
            }

            AlgorithmValues algorithms;
            try
            {
                algorithms =
                    snapshotAlgorithms(pack, compression, encryption, key);
            }
            catch (const std::exception& error)
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     QString::fromUtf8(error.what()));
                return;
            }

            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback& progress)
                {
                    auto options =
                        algorithmOptions(algorithms, cancel, progress);
                    auto result = BackupEngine::create(
                        src.toStdString(), out.toStdString(), options);
                    if (!result.success)
                    {
                        throw std::runtime_error(result.message);
                    }
                    return QString("备份完成 · %1 个条目 · %2")
                        .arg(result.entryCount)
                        .arg(formatBytes(result.outputBytes));
                });
        });
    return page.widget;
}

QWidget* localRestorePage()
{
    Page page = makePage();

    Card configuration = makeCard();
    auto* form = makeForm();
    QLineEdit *archive, *destination;
    QPushButton *browseArchive, *browseDestination;
    form->addRow("备份归档", pathRow(archive, browseArchive, "archivePath"));
    form->addRow("目标目录",
                 pathRow(destination, browseDestination, "destinationPath"));
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setObjectName("archivePassword");
    key->setPlaceholderText("加密时填写");
    form->addRow("归档密码", key);
    auto* overwrite = new QCheckBox("允许覆盖同名文件");
    form->addRow("覆盖策略", overwrite);
    configuration.body->addLayout(form);

    Card preview = makeCard();
    auto* previewButton = new QPushButton("检查冲突");
    previewButton->setObjectName("secondaryButton");
    auto* conflictView = new QTextEdit;
    conflictView->setReadOnly(true);
    conflictView->setAcceptRichText(false);
    conflictView->setPlaceholderText("未检查");
    conflictView->setMinimumHeight(120);
    preview.body->addWidget(previewButton);
    preview.body->addWidget(conflictView, 1);

    page.layout->addWidget(configuration.frame, 3);
    page.layout->addWidget(preview.frame, 2);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "还原");
    page.footer->addWidget(task.frame);

    bindPathPicker(page.widget, browseArchive, archive, "选择备份归档",
                   selectArchive);
    bindPathPicker(page.widget, browseDestination, destination,
                   "选择还原目标目录", selectDirectory);

    auto previewResult = std::make_shared<RestorePreview>();
    QObject::connect(
        previewButton, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString input = archive->text().trimmed();
            const QString dest = destination->text().trimmed();
            const QString password = key->text();
            if (input.isEmpty() || dest.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请选择备份归档和目标目录。");
                return;
            }

            JobControls previewControls = controls;
            previewControls.start = previewButton;
            startJob(
                page.widget, previewControls,
                [=](std::atomic_bool* cancel, const ProgressCallback& progress)
                {
                    *previewResult = BackupEngine::preview(
                        input.toStdString(), dest.toStdString(),
                        password.toStdString(), progress, cancel);
                    return QString("预检完成 · %1 个条目 · %2 个冲突")
                        .arg(previewResult->entries.size())
                        .arg(previewResult->conflicts.size());
                },
                [=]()
                {
                    QStringList lines;
                    if (previewResult->conflicts.empty())
                    {
                        lines << "无冲突";
                    }
                    else
                    {
                        lines << QString("发现 %1 个冲突：")
                                     .arg(previewResult->conflicts.size());
                        for (const auto& path : previewResult->conflicts)
                        {
                            lines << QString::fromStdString(path);
                        }
                    }
                    conflictView->setPlainText(lines.join('\n'));
                });
        });

    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString input = archive->text().trimmed();
            const QString dest = destination->text().trimmed();
            const QString password = key->text();
            const bool allowOverwrite = overwrite->isChecked();
            if (input.isEmpty() || dest.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请选择备份归档和目标目录。");
                return;
            }
            if (allowOverwrite &&
                QMessageBox::question(
                    page.widget, "确认覆盖",
                    "目标中已有的同名文件会被替换，是否继续？") !=
                    QMessageBox::Yes)
            {
                return;
            }

            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback& progress)
                {
                    // Explicit overwrite needs no automatic conflict preview;
                    // restore still validates the archive before extraction.
                    if (!allowOverwrite)
                    {
                        auto resultPreview = BackupEngine::preview(
                            input.toStdString(), dest.toStdString(),
                            password.toStdString(), progress, cancel);
                        if (!resultPreview.conflicts.empty())
                        {
                            QStringList lines;
                            for (size_t i = 0;
                                 i < std::min<size_t>(
                                         resultPreview.conflicts.size(), 20);
                                 ++i)
                            {
                                lines << QString::fromStdString(
                                    resultPreview.conflicts[i]);
                            }
                            progress({"conflict-preview", 0, 0,
                                      lines.join("；").toStdString()});
                            throw std::runtime_error(
                                "目标存在 " +
                                std::to_string(resultPreview.conflicts.size()) +
                                " 个冲突；请查看预检结果后启用覆盖策略");
                        }
                    }

                    RestoreOptions options;
                    options.password = password.toStdString();
                    options.overwrite = allowOverwrite;
                    options.cancel = cancel;
                    options.progress = progress;
                    auto result = BackupEngine::restore(
                        input.toStdString(), dest.toStdString(), options);
                    if (!result.success)
                    {
                        throw std::runtime_error(result.message);
                    }
                    return QString("还原完成 · %1 个条目 · %2")
                        .arg(result.entryCount)
                        .arg(formatBytes(result.outputBytes));
                });
        });
    return page.widget;
}

} // namespace backup::gui
