#include "ui.hpp"
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <QCheckBox>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
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
    QPushButton *browseSource, *browseOutput, *selectEntries;
    QLabel* selectionSummary;
    QComboBox *pack, *compression, *encryption;
    form->addRow("源目录", pathRow(source, browseSource, "sourcePath"));
    form->addRow("备份内容", selectionRow(selectEntries, selectionSummary));
    auto* outputRow = pathRow(output, browseOutput, "outputPath");
    form->addRow("输出归档", outputRow);
    auto* outputLabel = form->labelForField(outputRow);
    auto* trigger = new QComboBox;
    trigger->setObjectName("automaticMode");
    trigger->addItems({"手动", "定时", "实时"});
    form->addRow("执行", trigger);
    auto* interval = new QSpinBox;
    interval->setObjectName("automaticInterval");
    interval->setRange(1, 604800);
    interval->setValue(60);
    interval->setSuffix(" 秒");
    auto* keep = new QSpinBox;
    keep->setObjectName("automaticKeep");
    keep->setRange(1, 10000);
    keep->setValue(5);
    keep->setSuffix(" 份");
    auto* automaticRow = new QHBoxLayout;
    automaticRow->addWidget(interval);
    automaticRow->addWidget(keep);
    form->addRow("间隔 / 保留", automaticRow);
    const auto updateMode = [=]()
    {
        const bool enabled = trigger->currentIndex() != 0;
        interval->setVisible(enabled);
        keep->setVisible(enabled);
        form->labelForField(automaticRow)->setVisible(enabled);
        qobject_cast<QLabel*>(outputLabel)
            ->setText(enabled ? "保存目录" : "输出归档");
        interval->setToolTip(trigger->currentIndex() == 2
                                 ? "文件变化稳定后等待的合并时间"
                                 : "两次快照之间的等待时间");
    };
    QObject::connect(trigger, &QComboBox::currentIndexChanged, page.widget,
                     [=]()
                     {
                         output->clear();
                         interval->setValue(trigger->currentIndex() == 2 ? 1
                                                                         : 60);
                         updateMode();
                     });
    updateMode();
    addAlgorithmRows(form, pack, compression, encryption, key);
    configuration.body->addLayout(form);
    page.layout->addWidget(configuration.frame, 1);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "备份");
    page.footer->addWidget(task.frame);

    bindPathPicker(page.widget, browseSource, source, "选择源目录",
                   selectDirectory);
    QObject::connect(browseOutput, &QPushButton::clicked, page.widget,
                     [=]()
                     {
                         const auto picked =
                             trigger->currentIndex() == 0
                                 ? selectArchiveOutput(page.widget,
                                                       "选择输出归档",
                                                       output->text())
                                 : selectDirectory(page.widget, "选择保存目录",
                                                   output->text());
                         if (!picked.isEmpty())
                         {
                             output->setText(picked);
                         }
                     });

    // The selection belongs to one source directory: changing the path drops
    // it so a stale entry list can never be archived silently.
    auto selection = std::make_shared<QStringList>();
    QObject::connect(source, &QLineEdit::textChanged, page.widget,
                     [=]()
                     {
                         selection->clear();
                         selectionSummary->setText("全部内容");
                     });
    QObject::connect(
        selectEntries, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString directory = source->text().trimmed();
            if (directory.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请先选择源目录。");
                return;
            }
            QStringList chosen = *selection;
            if (!selectSourceEntries(page.widget, directory, &chosen))
            {
                return;
            }
            *selection = chosen;
            selectionSummary->setText(
                chosen.isEmpty() ? QString("全部内容")
                                 : QString("已选 %1 项").arg(chosen.size()));
        });
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString src = source->text().trimmed();
            const QString out = output->text().trimmed();
            if (src.isEmpty() || out.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请选择源目录和输出位置。");
                return;
            }
            const bool automatic = trigger->currentIndex() != 0;
            if (!automatic && QFile::exists(out))
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

            const auto includes = toUtf8Strings(*selection);
            AutomaticOptions automaticOptions;
            automaticOptions.realtime = trigger->currentIndex() == 2;
            automaticOptions.interval = std::chrono::seconds(interval->value());
            automaticOptions.keep = static_cast<size_t>(keep->value());
            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback& progress)
                {
                    auto options =
                        algorithmOptions(algorithms, cancel, progress);
                    options.includes = includes;
                    auto result =
                        automatic
                            ? BackupEngine::automatic(src.toStdString(),
                                                      out.toStdString(),
                                                      options, automaticOptions)
                            : BackupEngine::create(src.toStdString(),
                                                   out.toStdString(), options);
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
