#include "backup/core.hpp"
#include "backup/network.hpp"

#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace backup;

namespace {

struct JobControls {
    QPushButton* start = nullptr;
    QPushButton* cancel = nullptr;
    QProgressBar* progress = nullptr;
    QTextEdit* log = nullptr;
};

struct Card {
    QFrame* frame = nullptr;
    QVBoxLayout* body = nullptr;
};

struct Page {
    QWidget* widget = nullptr;
    QVBoxLayout* layout = nullptr;
};

QString stageName(const QString& stage) {
    if (stage == "scan") return "扫描目录";
    if (stage == "pack") return "写入归档";
    if (stage == "compress-rle") return "RLE 压缩";
    if (stage == "decompress-rle") return "RLE 解压";
    if (stage == "huffman-count") return "Huffman 统计";
    if (stage == "compress-huffman") return "Huffman 压缩";
    if (stage == "decompress-huffman") return "Huffman 解压";
    if (stage == "encrypt") return "加密载荷";
    if (stage == "decrypt") return "解密载荷";
    if (stage == "extract") return "提取文件";
    if (stage == "restore-entry") return "恢复元数据";
    if (stage == "upload") return "上传归档";
    if (stage == "download") return "下载归档";
    if (stage == "conflict-preview") return "冲突预检";
    return stage;
}

QString formatBytes(uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value >= 100.0 ? 0 : 1);
    return QString("%1 %2").arg(QString::number(value, 'f', precision), units[unit]);
}

void appendLog(QTextEdit* log, const QString& text) {
    log->append(QString("[%1]  %2")
                    .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

Page makePage() {
    Page page;
    page.widget = new QWidget;
    page.layout = new QVBoxLayout(page.widget);
    page.layout->setContentsMargins(0, 0, 0, 0);
    page.layout->setSpacing(14);
    return page;
}

Card makeCard(const QString& title, const QString& description = {}) {
    Card card;
    card.frame = new QFrame;
    card.frame->setObjectName("card");
    card.body = new QVBoxLayout(card.frame);
    card.body->setContentsMargins(20, 18, 20, 18);
    card.body->setSpacing(12);

    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("cardTitle");
    card.body->addWidget(titleLabel);

    if (!description.isEmpty()) {
        auto* descriptionLabel = new QLabel(description);
        descriptionLabel->setObjectName("cardDescription");
        descriptionLabel->setWordWrap(true);
        card.body->addWidget(descriptionLabel);
    }
    return card;
}

QFormLayout* makeForm() {
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 0);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(11);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return form;
}

QLabel* makeHint(const QString& text) {
    auto* hint = new QLabel(text);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    return hint;
}

QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse,
                     const QString& placeholder) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    edit = new QLineEdit;
    edit->setClearButtonEnabled(true);
    edit->setPlaceholderText(placeholder);
    browse = new QPushButton("浏览");
    browse->setObjectName("secondaryButton");
    browse->setFixedWidth(82);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    return row;
}

JobControls addJobControls(QVBoxLayout* layout, const QString& startText) {
    JobControls controls;

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    controls.start = new QPushButton(startText);
    controls.start->setObjectName("primaryButton");
    controls.start->setMinimumWidth(170);
    controls.cancel = new QPushButton("取消任务");
    controls.cancel->setObjectName("dangerButton");
    controls.cancel->setEnabled(false);
    auto* copyLog = new QPushButton("复制日志");
    copyLog->setObjectName("quietButton");
    auto* clearLog = new QPushButton("清空");
    clearLog->setObjectName("quietButton");

    buttons->addWidget(controls.start);
    buttons->addWidget(controls.cancel);
    buttons->addStretch();
    buttons->addWidget(copyLog);
    buttons->addWidget(clearLog);
    layout->addLayout(buttons);

    controls.progress = new QProgressBar;
    controls.progress->setRange(0, 100);
    controls.progress->setValue(0);
    controls.progress->setFormat("等待开始");
    layout->addWidget(controls.progress);

    controls.log = new QTextEdit;
    controls.log->setObjectName("taskLog");
    controls.log->setReadOnly(true);
    controls.log->setAcceptRichText(false);
    controls.log->setMinimumHeight(128);
    controls.log->setPlaceholderText("任务日志将在这里显示");
    controls.log->document()->setMaximumBlockCount(800);
    layout->addWidget(controls.log, 1);

    QObject::connect(copyLog, &QPushButton::clicked, controls.log, [log = controls.log]() {
        QApplication::clipboard()->setText(log->toPlainText());
    });
    QObject::connect(clearLog, &QPushButton::clicked, controls.log, &QTextEdit::clear);
    return controls;
}

using Job = std::function<QString(std::atomic_bool*, const ProgressCallback&)>;

void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess = {}) {
    if (owner->property("jobRunning").toBool()) {
        QMessageBox::information(owner, "任务进行中",
                                 "请等待当前任务完成，或先取消当前任务。");
        return;
    }

    owner->setProperty("jobRunning", true);
    controls.start->setEnabled(false);
    controls.cancel->setEnabled(true);
    controls.progress->setValue(0);
    controls.progress->setFormat("正在准备");
    controls.log->clear();
    appendLog(controls.log, "任务开始");

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    controls.cancel->disconnect();
    QObject::connect(
        controls.cancel, &QPushButton::clicked, owner,
        [cancelled, cancelButton = controls.cancel, log = controls.log]() {
            cancelled->store(true);
            cancelButton->setEnabled(false);
            appendLog(log, "已请求取消，正在安全结束当前阶段");
        });

    QPointer<QWidget> safeOwner(owner);
    QPointer<QProgressBar> safeProgress(controls.progress);
    QPointer<QTextEdit> safeLog(controls.log);
    QPointer<QPushButton> safeStart(controls.start);
    QPointer<QPushButton> safeCancel(controls.cancel);

    auto progress = [safeOwner, safeProgress, safeLog](const ProgressEvent& event) {
        if (!safeOwner) return;
        const QString stage = stageName(QString::fromStdString(event.stage));
        const QString detail = QString::fromStdString(event.detail);
        const uint64_t completed = event.completed;
        const uint64_t total = event.total;
        QMetaObject::invokeMethod(
            safeOwner,
            [safeProgress, safeLog, stage, detail, completed, total]() {
                if (!safeProgress || !safeLog) return;
                if (total != 0) {
                    const long double ratio =
                        static_cast<long double>(completed) * 100.0L /
                        static_cast<long double>(total);
                    safeProgress->setValue(
                        static_cast<int>(std::clamp(ratio, 0.0L, 100.0L)));
                    safeProgress->setFormat(stage + "  %p%");
                } else {
                    safeProgress->setFormat(stage);
                }
                if (!detail.isEmpty() && (completed == 0 || completed == total)) {
                    appendLog(safeLog, stage + " · " + detail);
                }
            },
            Qt::QueuedConnection);
    };

    QThread* thread = QThread::create([=]() {
        QString message;
        bool ok = false;
        try {
            message = job(cancelled.get(), progress);
            ok = true;
        } catch (const std::exception& error) {
            message = QString::fromUtf8(error.what());
        } catch (...) {
            message = "未知错误";
        }

        if (!safeOwner) return;
        QMetaObject::invokeMethod(
            safeOwner,
            [=]() {
                if (!safeOwner || !safeProgress || !safeLog || !safeStart ||
                    !safeCancel) {
                    return;
                }
                safeStart->setEnabled(true);
                safeCancel->setEnabled(false);
                safeOwner->setProperty("jobRunning", false);
                safeProgress->setValue(ok ? 100 : 0);
                safeProgress->setFormat(ok ? "任务完成" : "任务失败");
                appendLog(safeLog, (ok ? "✓  " : "✕  ") + message);
                if (ok && afterSuccess) afterSuccess();
                if (!ok) {
                    QMessageBox::critical(safeOwner, "任务失败", message);
                }
            },
            Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void addAlgorithmRows(QFormLayout* form, QComboBox*& pack,
                      QComboBox*& compression, QComboBox*& encryption,
                      QLineEdit*& key) {
    pack = new QComboBox;
    pack->addItem("顺序归档  ·  Stream", "stream");
    pack->addItem("中央索引  ·  Index", "index");
    pack->setToolTip("顺序归档适合流式处理；中央索引适合快速列出条目。");
    form->addRow("打包算法", pack);

    compression = new QComboBox;
    compression->addItem("不压缩  ·  None", "none");
    compression->addItem("游程编码  ·  RLE", "rle");
    compression->addItem("规范编码  ·  Huffman", "huffman");
    compression->setToolTip("RLE 适合重复数据，Huffman 适合一般数据。");
    form->addRow("压缩算法", compression);

    encryption = new QComboBox;
    encryption->addItem("不加密  ·  None", "none");
    encryption->addItem("密钥流异或  ·  XOR", "xor");
    encryption->addItem("字节移位  ·  Vigenère", "vigenere");
    encryption->setToolTip("XOR 与 Vigenère 是课程教学算法，不用于生产敏感数据。");
    form->addRow("加密算法", encryption);

    key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("启用加密后输入归档密钥");
    key->setEnabled(false);
    form->addRow("归档密钥", key);

    QObject::connect(
        encryption, &QComboBox::currentIndexChanged, key,
        [encryption, key]() {
            key->setEnabled(encryption->currentData().toString() != "none");
        });
}

struct AlgorithmValues {
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
    std::string password;
};

AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key) {
    AlgorithmValues values{
        parsePackAlgorithm(pack->currentData().toString().toStdString()),
        parseCompressionAlgorithm(
            compression->currentData().toString().toStdString()),
        parseEncryptionAlgorithm(
            encryption->currentData().toString().toStdString()),
        key->text().toStdString()};
    if (values.encryption != EncryptionAlgorithm::None &&
        values.password.empty()) {
        throw std::runtime_error("启用加密后必须填写归档密钥");
    }
    return values;
}

BackupOptions algorithmOptions(const AlgorithmValues& values,
                               std::atomic_bool* cancel,
                               ProgressCallback progress) {
    BackupOptions options;
    options.pack = values.pack;
    options.compression = values.compression;
    options.encryption = values.encryption;
    options.password = values.password;
    options.cancel = cancel;
    options.progress = std::move(progress);
    return options;
}

struct ServerFields {
    QLineEdit* host = nullptr;
    QSpinBox* port = nullptr;
    QLineEdit* username = nullptr;
    QLineEdit* password = nullptr;
};

ServerFields addServerRows(QFormLayout* form) {
    ServerFields fields;
    fields.host = new QLineEdit("127.0.0.1");
    fields.host->setClearButtonEnabled(true);
    fields.host->setPlaceholderText("服务器地址");
    form->addRow("服务器", fields.host);

    fields.port = new QSpinBox;
    fields.port->setRange(1, 65535);
    fields.port->setValue(8848);
    form->addRow("端口", fields.port);

    fields.username = new QLineEdit;
    fields.username->setClearButtonEnabled(true);
    fields.username->setPlaceholderText("账号名称");
    form->addRow("用户名", fields.username);

    fields.password = new QLineEdit;
    fields.password->setEchoMode(QLineEdit::Password);
    fields.password->setClearButtonEnabled(true);
    fields.password->setPlaceholderText("账号密码");
    form->addRow("账号密码", fields.password);
    return fields;
}

struct ServerValues {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
};

ServerValues snapshotServer(const ServerFields& fields) {
    if (fields.host->text().trimmed().isEmpty() ||
        fields.username->text().trimmed().isEmpty() ||
        fields.password->text().isEmpty()) {
        throw std::runtime_error("服务器、用户名和账号密码不能为空");
    }
    return {fields.host->text().trimmed().toStdString(),
            static_cast<uint16_t>(fields.port->value()),
            fields.username->text().trimmed().toStdString(),
            fields.password->text().toStdString()};
}

network::BackupClient makeClient(const ServerValues& values) {
    return {values.host, values.port, values.username, values.password};
}

QString temporaryArchivePath() {
    return QDir::tempPath() + "/backup-gui-" +
           QUuid::createUuid().toString(QUuid::WithoutBraces) + ".bak";
}

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
        const QString value =
            QFileDialog::getExistingDirectory(page.widget, "选择源目录");
        if (!value.isEmpty()) source->setText(value);
    });
    QObject::connect(browseOutput, &QPushButton::clicked, page.widget, [=]() {
        const QString value = QFileDialog::getSaveFileName(
            page.widget, "选择输出归档", {}, "Backup Archive (*.bak)");
        if (!value.isEmpty()) output->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString src = source->text().trimmed();
        const QString out = output->text().trimmed();
        if (src.isEmpty() || out.isEmpty()) {
            QMessageBox::warning(page.widget, "输入错误",
                                 "请选择源目录和输出归档。");
            return;
        }
        if (QFile::exists(out)) {
            QMessageBox::warning(page.widget, "文件冲突",
                                 "输出归档已经存在，请选择新文件名。");
            return;
        }

        AlgorithmValues algorithms;
        try {
            algorithms =
                snapshotAlgorithms(pack, compression, encryption, key);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "输入错误",
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
        const QString value = QFileDialog::getOpenFileName(
            page.widget, "选择备份归档", {}, "Backup Archive (*.bak)");
        if (!value.isEmpty()) archive->setText(value);
    });
    QObject::connect(
        browseDestination, &QPushButton::clicked, page.widget, [=]() {
            const QString value = QFileDialog::getExistingDirectory(
                page.widget, "选择目标目录");
            if (!value.isEmpty()) destination->setText(value);
        });

    auto previewResult = std::make_shared<RestorePreview>();
    QObject::connect(previewButton, &QPushButton::clicked, page.widget, [=]() {
        const QString input = archive->text().trimmed();
        const QString dest = destination->text().trimmed();
        const QString password = key->text();
        if (input.isEmpty() || dest.isEmpty()) {
            QMessageBox::warning(page.widget, "输入错误",
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
            QMessageBox::warning(page.widget, "输入错误",
                                 "请选择备份归档和目标目录。");
            return;
        }
        if (allowOverwrite &&
            QMessageBox::question(
                page.widget, "确认覆盖",
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

QWidget* remoteBackupPage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card connection =
        makeCard("服务器连接", "填写服务器地址和账号信息。");
    auto* serverForm = makeForm();
    const ServerFields server = addServerRows(serverForm);
    connection.body->addLayout(serverForm);

    Card archiveCard =
        makeCard("归档配置", "选择目录、备份名称和归档算法。");
    auto* archiveForm = makeForm();
    QLineEdit* source;
    QPushButton* browse;
    archiveForm->addRow("源目录",
                        pathRow(source, browse, "选择需要上传的目录"));
    auto* name = new QLineEdit;
    name->setClearButtonEnabled(true);
    name->setPlaceholderText("可选的易读名称");
    archiveForm->addRow("备份名称", name);
    QComboBox *pack, *compression, *encryption;
    QLineEdit* key;
    addAlgorithmRows(archiveForm, pack, compression, encryption, key);
    archiveCard.body->addLayout(archiveForm);

    top->addWidget(connection.frame, 1);
    top->addWidget(archiveCard.frame, 1);
    page.layout->addLayout(top);

    Card task = makeCard("任务状态");
    JobControls controls = addJobControls(task.body, "构建并上传");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(browse, &QPushButton::clicked, page.widget, [=]() {
        const QString value =
            QFileDialog::getExistingDirectory(page.widget, "选择源目录");
        if (!value.isEmpty()) source->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString src = source->text().trimmed();
        const QString backupName = name->text().trimmed();
        if (src.isEmpty()) {
            QMessageBox::warning(page.widget, "输入错误", "请选择源目录。");
            return;
        }

        AlgorithmValues algorithms;
        ServerValues serverValues;
        try {
            algorithms =
                snapshotAlgorithms(pack, compression, encryption, key);
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "输入错误",
                                 QString::fromUtf8(error.what()));
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool* cancel, const ProgressCallback& progress) {
                const QString temporary = temporaryArchivePath();
                struct Cleanup {
                    QString path;
                    ~Cleanup() { QFile::remove(path); }
                } cleanup{temporary};

                auto options =
                    algorithmOptions(algorithms, cancel, progress);
                auto created = BackupEngine::create(
                    src.toStdString(), temporary.toStdString(), options);
                if (!created.success) {
                    throw std::runtime_error(created.message);
                }

                auto client = makeClient(serverValues);
                std::string id;
                std::string error;
                if (!client.upload(temporary.toStdString(),
                                   backupName.toStdString(), id, error,
                                   progress, cancel)) {
                    throw std::runtime_error(error);
                }
                return QString("远程备份完成 · ID %1")
                    .arg(QString::fromStdString(id));
            });
    });
    return page.widget;
}

QWidget* remoteRestorePage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card connection =
        makeCard("服务器连接", "填写与注册时相同的账号信息。");
    auto* serverForm = makeForm();
    const ServerFields server = addServerRows(serverForm);
    connection.body->addLayout(serverForm);

    Card restoreCard =
        makeCard("还原配置", "填写备份 ID、目标目录和归档密钥。");
    auto* restoreForm = makeForm();
    auto* id = new QLineEdit;
    id->setClearButtonEnabled(true);
    id->setPlaceholderText("远程列表中的备份 ID");
    restoreForm->addRow("备份 ID", id);
    QLineEdit* destination;
    QPushButton* browse;
    restoreForm->addRow(
        "目标目录",
        pathRow(destination, browse, "选择还原目标目录"));
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("未加密归档可留空");
    restoreForm->addRow("归档密钥", key);
    auto* overwrite = new QCheckBox("允许覆盖同名文件");
    restoreForm->addRow("覆盖策略", overwrite);
    restoreCard.body->addLayout(restoreForm);

    top->addWidget(connection.frame, 1);
    top->addWidget(restoreCard.frame, 1);
    page.layout->addLayout(top);

    Card task = makeCard("任务状态");
    JobControls controls = addJobControls(task.body, "下载并还原");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(browse, &QPushButton::clicked, page.widget, [=]() {
        const QString value =
            QFileDialog::getExistingDirectory(page.widget, "选择目标目录");
        if (!value.isEmpty()) destination->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString backupId = id->text().trimmed();
        const QString dest = destination->text().trimmed();
        const QString password = key->text();
        const bool allowOverwrite = overwrite->isChecked();
        if (backupId.isEmpty() || dest.isEmpty()) {
            QMessageBox::warning(page.widget, "输入错误",
                                 "请填写备份 ID 和目标目录。");
            return;
        }

        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "输入错误",
                                 QString::fromUtf8(error.what()));
            return;
        }
        if (allowOverwrite &&
            QMessageBox::question(
                page.widget, "确认覆盖",
                "目标中已有的同名文件会被替换，是否继续？") !=
                QMessageBox::Yes) {
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool* cancel, const ProgressCallback& progress) {
                const QString temporary = temporaryArchivePath();
                struct Cleanup {
                    QString path;
                    ~Cleanup() { QFile::remove(path); }
                } cleanup{temporary};

                auto client = makeClient(serverValues);
                std::string error;
                if (!client.download(backupId.toStdString(),
                                     temporary.toStdString(), error, progress,
                                     cancel)) {
                    throw std::runtime_error(error);
                }

                RestoreOptions options;
                options.password = password.toStdString();
                options.overwrite = allowOverwrite;
                options.cancel = cancel;
                options.progress = progress;
                auto restored = BackupEngine::restore(
                    temporary.toStdString(), dest.toStdString(), options);
                if (!restored.success) {
                    throw std::runtime_error(restored.message);
                }
                return QString("远程还原完成 · %1 个条目")
                    .arg(restored.entryCount);
            });
    });
    return page.widget;
}

QWidget* remoteListPage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card connection =
        makeCard("服务器连接", "填写账号信息后刷新列表。");
    auto* serverForm = makeForm();
    const ServerFields server = addServerRows(serverForm);
    connection.body->addLayout(serverForm);
    connection.body->addStretch();
    connection.body->addWidget(
        makeHint("提示：双击表格中的任意行可复制备份 ID。"));

    Card listCard = makeCard("备份历史");
    auto* table = new QTableWidget(0, 4);
    table->setHorizontalHeaderLabels({"备份 ID", "名称", "大小", "创建时间"});
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table->horizontalHeader()->setSectionResizeMode(2,
                                                    QHeaderView::ResizeToContents);
    table->horizontalHeader()->setSectionResizeMode(3,
                                                    QHeaderView::ResizeToContents);
    table->verticalHeader()->setVisible(false);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSortingEnabled(true);
    table->setMinimumHeight(250);
    listCard.body->addWidget(table, 1);

    top->addWidget(connection.frame, 1);
    top->addWidget(listCard.frame, 2);
    page.layout->addLayout(top, 2);

    Card task = makeCard("同步状态");
    JobControls controls = addJobControls(task.body, "刷新远程列表");
    controls.log->setMaximumHeight(82);
    page.layout->addWidget(task.frame, 1);

    auto entries =
        std::make_shared<std::vector<network::RemoteBackupEntry>>();
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "输入错误",
                                 QString::fromUtf8(error.what()));
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool*, const ProgressCallback&) {
                auto client = makeClient(serverValues);
                std::string error;
                *entries = client.list(error);
                if (!error.empty()) throw std::runtime_error(error);
                return QString("已同步 %1 个备份").arg(entries->size());
            },
            [=]() {
                table->setSortingEnabled(false);
                table->setRowCount(static_cast<int>(entries->size()));
                for (int row = 0; row < table->rowCount(); ++row) {
                    const auto& entry =
                        (*entries)[static_cast<size_t>(row)];
                    table->setItem(
                        row, 0,
                        new QTableWidgetItem(
                            QString::fromStdString(entry.id)));
                    table->setItem(
                        row, 1,
                        new QTableWidgetItem(
                            entry.name.empty()
                                ? "未命名备份"
                                : QString::fromStdString(entry.name)));
                    table->setItem(
                        row, 2,
                        new QTableWidgetItem(formatBytes(entry.size)));
                    table->setItem(
                        row, 3,
                        new QTableWidgetItem(
                            QDateTime::fromSecsSinceEpoch(
                                static_cast<qint64>(entry.timestamp))
                                .toString("yyyy-MM-dd  HH:mm")));
                }
                table->setSortingEnabled(true);
            });
    });

    QObject::connect(
        table, &QTableWidget::cellDoubleClicked, page.widget,
        [table, controls](int row, int) {
            if (auto* item = table->item(row, 0)) {
                QApplication::clipboard()->setText(item->text());
                appendLog(controls.log, "已复制备份 ID");
            }
        });
    return page.widget;
}

void addSecurityItem(QVBoxLayout* layout, const QString& number,
                     const QString& title, const QString& text) {
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    auto* badge = new QLabel(number);
    badge->setObjectName("numberBadge");
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(32, 32);

    auto* copy = new QVBoxLayout;
    copy->setSpacing(2);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("securityTitle");
    auto* textLabel = makeHint(text);
    copy->addWidget(titleLabel);
    copy->addWidget(textLabel);

    row->addWidget(badge, 0, Qt::AlignTop);
    row->addLayout(copy, 1);
    layout->addLayout(row);
}

QWidget* userPage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card account =
        makeCard("创建账号", "填写服务器信息、用户名和两次相同的密码。");
    auto* form = makeForm();
    const ServerFields server = addServerRows(form);
    auto* confirm = new QLineEdit;
    confirm->setEchoMode(QLineEdit::Password);
    confirm->setClearButtonEnabled(true);
    confirm->setPlaceholderText("再次输入账号密码");
    form->addRow("确认密码", confirm);
    account.body->addLayout(form);

    Card security = makeCard("操作步骤");
    addSecurityItem(security.body, "1", "连接服务器",
                    "确认服务已启动，并填写服务器地址和端口。");
    addSecurityItem(security.body, "2", "设置账号",
                    "填写用户名、账号密码和确认密码。");
    addSecurityItem(security.body, "3", "提交注册",
                    "点击注册账号，成功后即可使用其他远程页面。");
    security.body->addStretch();

    top->addWidget(account.frame, 1);
    top->addWidget(security.frame, 1);
    page.layout->addLayout(top);

    Card task = makeCard("注册状态");
    JobControls controls = addJobControls(task.body, "注册账号");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        if (server.password->text() != confirm->text()) {
            QMessageBox::warning(page.widget, "输入错误",
                                 "两次输入的密码不一致。");
            return;
        }
        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "输入错误",
                                 QString::fromUtf8(error.what()));
            return;
        }

        startJob(
            page.widget, controls,
            [=](std::atomic_bool*, const ProgressCallback&) {
                auto client = makeClient(serverValues);
                std::string error;
                if (!client.registerUser(error)) {
                    throw std::runtime_error(error);
                }
                return QString("账号 %1 注册成功")
                    .arg(QString::fromStdString(serverValues.username));
            });
    });
    return page.widget;
}

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("Backup Studio · 数据备份与还原系统");
        setMinimumSize(1024, 700);
        resize(1180, 780);

        auto* root = new QWidget;
        root->setObjectName("appRoot");
        auto* rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        auto* sidebar = new QFrame;
        sidebar->setObjectName("sidebar");
        sidebar->setFixedWidth(224);
        auto* sidebarLayout = new QVBoxLayout(sidebar);
        sidebarLayout->setContentsMargins(18, 24, 18, 20);
        sidebarLayout->setSpacing(7);

        auto* brandRow = new QHBoxLayout;
        brandRow->setSpacing(11);
        auto* logo = new QLabel("BS");
        logo->setObjectName("logoMark");
        logo->setAlignment(Qt::AlignCenter);
        logo->setFixedSize(44, 44);
        auto* brand = new QLabel("Backup Studio");
        brand->setObjectName("brand");
        brandRow->addWidget(logo);
        brandRow->addWidget(brand, 1);
        sidebarLayout->addLayout(brandRow);
        sidebarLayout->addSpacing(20);

        auto* pages = new QStackedWidget;
        pages->setObjectName("pageStack");
        pages->addWidget(localBackupPage());
        pages->addWidget(localRestorePage());
        pages->addWidget(remoteBackupPage());
        pages->addWidget(remoteRestorePage());
        pages->addWidget(remoteListPage());
        pages->addWidget(userPage());

        auto* navigation = new QButtonGroup(this);
        navigation->setExclusive(true);
        auto addSection = [sidebarLayout](const QString& text) {
            auto* label = new QLabel(text);
            label->setObjectName("navSection");
            sidebarLayout->addSpacing(7);
            sidebarLayout->addWidget(label);
        };
        auto addNavigation = [&](int index, const QString& text) {
            auto* button = new QPushButton(text);
            button->setObjectName("navButton");
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            navigation->addButton(button, index);
            sidebarLayout->addWidget(button);
        };

        addSection("本地工作区");
        addNavigation(0, "本地备份");
        addNavigation(1, "本地还原");
        addSection("远程中心");
        addNavigation(2, "远程备份");
        addNavigation(3, "远程还原");
        addNavigation(4, "备份历史");
        addSection("访问控制");
        addNavigation(5, "账号管理");
        navigation->button(0)->setChecked(true);

        sidebarLayout->addStretch();

        auto* content = new QFrame;
        content->setObjectName("contentRoot");
        auto* contentLayout = new QVBoxLayout(content);
        contentLayout->setContentsMargins(30, 24, 30, 24);
        contentLayout->setSpacing(18);

        auto* header = new QHBoxLayout;
        auto* pageTitle = new QLabel("本地备份");
        pageTitle->setObjectName("pageTitle");
        header->addWidget(pageTitle);
        header->addStretch();
        contentLayout->addLayout(header);
        contentLayout->addWidget(pages, 1);

        const QStringList titles = {
            "本地备份", "本地还原", "远程备份",
            "远程还原", "备份历史", "账号管理"};

        QObject::connect(
            navigation, &QButtonGroup::idClicked, this,
            [=](int index) {
                pages->setCurrentIndex(index);
                pageTitle->setText(titles.at(index));
            });
        bool pageFromEnvironment = false;
        const int initialPage =
            qEnvironmentVariableIntValue("BACKUP_GUI_PAGE", &pageFromEnvironment);
        if (pageFromEnvironment && initialPage >= 0 &&
            initialPage < pages->count()) {
            navigation->button(initialPage)->click();
        }

        rootLayout->addWidget(sidebar);
        rootLayout->addWidget(content, 1);
        setCentralWidget(root);
        setStyleSheet(theme());
    }

private:
    static QString theme() {
        return R"(
            * {
                font-family: "Noto Sans CJK SC", "Microsoft YaHei", "Segoe UI", sans-serif;
                font-size: 13px;
                color: #1f2937;
            }
            QMainWindow, QWidget#appRoot {
                background: #f3f6fb;
            }
            QFrame#sidebar {
                background: #111827;
                border: none;
            }
            QLabel#logoMark {
                color: white;
                background: #2563eb;
                border-radius: 12px;
                font-size: 15px;
                font-weight: 800;
            }
            QLabel#brand {
                color: #f8fafc;
                font-size: 17px;
                font-weight: 700;
            }
            QLabel#navSection {
                color: #64748b;
                font-size: 10px;
                font-weight: 700;
                padding: 5px 10px 2px 10px;
            }
            QPushButton#navButton {
                min-height: 42px;
                padding: 0 13px;
                border: none;
                border-radius: 10px;
                background: transparent;
                color: #aab5c7;
                text-align: left;
                font-weight: 500;
            }
            QPushButton#navButton:hover {
                color: #f8fafc;
                background: #1f2937;
            }
            QPushButton#navButton:checked {
                color: white;
                background: #2563eb;
                font-weight: 650;
            }
            QFrame#contentRoot {
                background: #f3f6fb;
                border: none;
            }
            QLabel#pageTitle {
                color: #111827;
                font-size: 25px;
                font-weight: 750;
            }
            QStackedWidget#pageStack {
                background: transparent;
                border: none;
            }
            QFrame#card {
                background: white;
                border: 1px solid #e1e7f0;
                border-radius: 14px;
            }
            QLabel#cardTitle {
                color: #172033;
                font-size: 15px;
                font-weight: 700;
            }
            QLabel#cardDescription, QLabel#hint {
                color: #718096;
                font-size: 11px;
            }
            QLabel#securityTitle {
                color: #253047;
                font-weight: 650;
            }
            QLabel#numberBadge {
                color: #1d4ed8;
                background: #eaf1ff;
                border: 1px solid #cfddff;
                border-radius: 9px;
                font-size: 12px;
                font-weight: 700;
            }
            QLineEdit, QComboBox, QSpinBox {
                min-height: 38px;
                padding: 0 11px;
                color: #1f2937;
                background: #fbfcfe;
                border: 1px solid #ccd6e4;
                border-radius: 8px;
                selection-background-color: #2563eb;
            }
            QLineEdit:hover, QComboBox:hover, QSpinBox:hover {
                border-color: #9eacc0;
                background: white;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
                border: 2px solid #3b82f6;
                background: white;
            }
            QLineEdit:disabled {
                color: #9aa7b8;
                background: #f1f4f8;
                border-color: #e1e7ef;
            }
            QComboBox {
                padding-right: 34px;
            }
            QComboBox::drop-down {
                width: 30px;
                border: none;
            }
            QComboBox QAbstractItemView {
                padding: 6px;
                background: white;
                border: 1px solid #ccd6e4;
                selection-color: white;
                selection-background-color: #2563eb;
            }
            QPushButton {
                min-height: 38px;
                padding: 0 15px;
                border: 1px solid #d1d9e6;
                border-radius: 8px;
                background: #f8fafc;
                color: #334155;
                font-weight: 600;
            }
            QPushButton:hover {
                background: #eef3f9;
                border-color: #aebacc;
            }
            QPushButton:pressed {
                background: #e2e8f0;
            }
            QPushButton#primaryButton {
                color: white;
                background: #2563eb;
                border-color: #2563eb;
            }
            QPushButton#primaryButton:hover {
                background: #1d4ed8;
                border-color: #1d4ed8;
            }
            QPushButton#secondaryButton {
                color: #1d4ed8;
                background: #eff5ff;
                border-color: #c7d7fe;
            }
            QPushButton#secondaryButton:hover {
                background: #e1ecff;
                border-color: #9fbbfd;
            }
            QPushButton#dangerButton {
                color: #b42318;
                background: white;
                border-color: #f0c5c1;
            }
            QPushButton#dangerButton:hover {
                background: #fff1f0;
                border-color: #e99b94;
            }
            QPushButton#quietButton {
                min-height: 32px;
                padding: 0 10px;
                color: #64748b;
                background: transparent;
                border-color: transparent;
                font-size: 11px;
            }
            QPushButton#quietButton:hover {
                color: #1d4ed8;
                background: #eef4ff;
            }
            QPushButton:disabled {
                color: #9da8b8;
                background: #edf1f6;
                border-color: #e0e6ee;
            }
            QCheckBox {
                spacing: 8px;
                color: #39465a;
            }
            QCheckBox::indicator {
                width: 17px;
                height: 17px;
                border: 1px solid #aeb9ca;
                border-radius: 5px;
                background: white;
            }
            QCheckBox::indicator:checked {
                background: #2563eb;
                border-color: #2563eb;
            }
            QProgressBar {
                min-height: 20px;
                max-height: 20px;
                color: #334155;
                background: #e8edf4;
                border: none;
                border-radius: 7px;
                text-align: center;
                font-size: 10px;
                font-weight: 650;
            }
            QProgressBar::chunk {
                background: #22a06b;
                border-radius: 7px;
            }
            QTextEdit {
                padding: 9px;
                color: #334155;
                background: #fbfcfe;
                border: 1px solid #d8e0eb;
                border-radius: 9px;
                selection-background-color: #2563eb;
            }
            QTextEdit#taskLog {
                color: #cbd5e1;
                background: #111827;
                border-color: #273449;
                font-family: "Noto Sans Mono CJK SC", "DejaVu Sans Mono", monospace;
                font-size: 11px;
            }
            QTableWidget {
                color: #273449;
                background: white;
                alternate-background-color: #f8fafc;
                border: 1px solid #dbe3ee;
                border-radius: 9px;
                gridline-color: #edf1f5;
                selection-color: #1d4ed8;
                selection-background-color: #e7efff;
            }
            QHeaderView::section {
                min-height: 34px;
                padding: 0 8px;
                color: #526176;
                background: #f1f5f9;
                border: none;
                border-bottom: 1px solid #dbe3ee;
                font-size: 11px;
                font-weight: 700;
            }
            QScrollBar:vertical {
                width: 9px;
                margin: 2px;
                background: transparent;
            }
            QScrollBar::handle:vertical {
                min-height: 24px;
                background: #c4cedb;
                border-radius: 4px;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0;
            }
            QToolTip {
                color: #f8fafc;
                background: #1f2937;
                border: 1px solid #334155;
                padding: 6px;
            }
        )";
    }
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Backup Studio");
    app.setApplicationDisplayName("Backup Studio");
    app.setApplicationVersion(BACKUP_SYSTEM_VERSION);

    MainWindow window;
    window.show();
    const QString capturePath = qEnvironmentVariable("BACKUP_GUI_CAPTURE");
    if (!capturePath.isEmpty()) {
        QTimer::singleShot(300, &app, [&]() {
            window.grab().save(capturePath);
            app.quit();
        });
    }
    return app.exec();
}
