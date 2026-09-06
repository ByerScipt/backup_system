#include "gui_common.hpp"

QWidget* localBackupPage() {
    Page page = makePage();
    Card configuration =
        makeCard("备份配置", "选择来源、输出位置以及归档处理策略。");
    auto* form = makeForm();

    QLineEdit *source, *output, *key;
    QPushButton *browseSource, *browseOutput;
    QComboBox *pack, *compression, *encryption;
    form->addRow("源目录",
                 pathRow(source, browseSource, "选择需要备份的目录"));
    form->addRow("输出归档",
                 pathRow(output, browseOutput, "例如 /home/user/data.bak"));
    addAlgorithmRows(form, pack, compression, encryption, key);
    configuration.body->addLayout(form);
    page.layout->addWidget(configuration.frame);

    Card task = makeCard("任务状态");
    JobControls controls = addJobControls(task.body, "开始本地备份");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(browseSource, &QPushButton::clicked, page.widget, [=]() {
        const QString value = selectDirectory(
            page.widget, "Select Source Directory", source->text());
        if (!value.isEmpty()) source->setText(value);
    });
    QObject::connect(browseOutput, &QPushButton::clicked, page.widget, [=]() {
        const QString value = selectArchiveOutput(
            page.widget, "Select Output Archive", output->text());
        if (!value.isEmpty()) output->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString src = source->text().trimmed();
        const QString out = output->text().trimmed();
        if (src.isEmpty() || out.isEmpty()) {
            QMessageBox::warning(page.widget, "Input Error",
                                 "请选择源目录和输出归档。");
            return;
        }
        if (QFile::exists(out)) {
            QMessageBox::warning(page.widget, "File Conflict",
                                 "输出归档已经存在，请选择新文件名。");
            return;
        }

        AlgorithmValues algorithms;
        try {
            algorithms =
                snapshotAlgorithms(pack, compression, encryption, key);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "Input Error",
                                 QString::fromUtf8(error.what()));
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool* cancel, const ProgressCallback& progress) {
                auto options =
                    algorithmOptions(algorithms, cancel, progress);
                auto result = BackupEngine::create(src.toStdString(),
                                                   out.toStdString(), options);
                if (!result.success) {
                    throw std::runtime_error(result.message);
                }
                return QString("备份完成 · %1 个条目 · %2")
                    .arg(result.entryCount)
                    .arg(formatBytes(result.outputBytes));
            });
    });
    return page.widget;
}

QWidget* localRestorePage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card configuration =
        makeCard("还原配置", "默认拒绝覆盖，建议先执行预检。");
    auto* form = makeForm();
    QLineEdit *archive, *destination;
    QPushButton *browseArchive, *browseDestination;
    form->addRow("备份归档",
                 pathRow(archive, browseArchive, "选择 .bak 归档"));
    form->addRow("目标目录",
                 pathRow(destination, browseDestination, "还原到该目录下"));
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("未加密归档可留空");
    form->addRow("归档密钥", key);
    auto* overwrite = new QCheckBox("允许覆盖同名文件");
    form->addRow("覆盖策略", overwrite);
    configuration.body->addLayout(form);

    Card preview =
        makeCard("冲突预检", "填写归档和目标目录后执行预检。");
    auto* previewButton = new QPushButton("预览归档与目标冲突");
    previewButton->setObjectName("secondaryButton");
    auto* conflictView = new QTextEdit;
    conflictView->setReadOnly(true);
    conflictView->setAcceptRichText(false);
    conflictView->setPlaceholderText("尚未执行预检");
    conflictView->setMinimumHeight(118);
    preview.body->addWidget(previewButton);
    preview.body->addWidget(conflictView, 1);

    top->addWidget(configuration.frame, 3);
    top->addWidget(preview.frame, 2);
    page.layout->addLayout(top);

    Card task = makeCard("任务状态");
    JobControls controls = addJobControls(task.body, "开始本地还原");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(browseArchive, &QPushButton::clicked, page.widget, [=]() {
        const QString value = selectArchive(
            page.widget, "Select Backup Archive", archive->text());
        if (!value.isEmpty()) archive->setText(value);
    });
    QObject::connect(
        browseDestination, &QPushButton::clicked, page.widget, [=]() {
            const QString value = selectDirectory(
                page.widget, "Select Destination Directory",
                destination->text());
            if (!value.isEmpty()) destination->setText(value);
        });

    auto previewResult = std::make_shared<RestorePreview>();
    QObject::connect(previewButton, &QPushButton::clicked, page.widget, [=]() {
        const QString input = archive->text().trimmed();
        const QString dest = destination->text().trimmed();
        const QString password = key->text();
        if (input.isEmpty() || dest.isEmpty()) {
            QMessageBox::warning(page.widget, "Input Error",
                                 "请选择备份归档和目标目录。");
            return;
        }

        JobControls previewControls = controls;
        previewControls.start = previewButton;
        startJob(
            page.widget, previewControls,
            [=](std::atomic_bool*, const ProgressCallback&) {
                *previewResult = BackupEngine::preview(
                    input.toStdString(), dest.toStdString(),
                    password.toStdString());
                return QString("预检完成 · %1 个条目 · %2 个冲突")
                    .arg(previewResult->entries.size())
                    .arg(previewResult->conflicts.size());
            },
            [=]() {
                QStringList lines;
                if (previewResult->conflicts.empty()) {
                    lines << "✓ 未发现已有路径冲突";
                } else {
                    lines << QString("发现 %1 个冲突：")
                                 .arg(previewResult->conflicts.size());
                    for (const auto& path : previewResult->conflicts) {
                        lines << QString::fromStdString(path);
                    }
                }
                conflictView->setPlainText(lines.join('\n'));
            });
    });

    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString input = archive->text().trimmed();
        const QString dest = destination->text().trimmed();
        const QString password = key->text();
        const bool allowOverwrite = overwrite->isChecked();
        if (input.isEmpty() || dest.isEmpty()) {
            QMessageBox::warning(page.widget, "Input Error",
                                 "请选择备份归档和目标目录。");
            return;
        }
        if (allowOverwrite &&
            QMessageBox::question(
                page.widget, "Confirm Overwrite",
                "目标中已有的同名文件会被替换，是否继续？") !=
                QMessageBox::Yes) {
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool* cancel, const ProgressCallback& progress) {
                auto resultPreview = BackupEngine::preview(
                    input.toStdString(), dest.toStdString(),
                    password.toStdString());
                if (!resultPreview.conflicts.empty()) {
                    QStringList lines;
                    for (size_t i = 0;
                         i < std::min<size_t>(resultPreview.conflicts.size(), 20);
                         ++i) {
                        lines << QString::fromStdString(
                            resultPreview.conflicts[i]);
                    }
                    progress({"conflict-preview", 0, 0,
                              lines.join("；").toStdString()});
                    if (!allowOverwrite) {
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
                if (!result.success) {
                    throw std::runtime_error(result.message);
                }
                return QString("还原完成 · %1 个条目 · %2")
                    .arg(result.entryCount)
                    .arg(formatBytes(result.outputBytes));
            });
    });
    return page.widget;
}

