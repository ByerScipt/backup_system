#include "gui_common.hpp"

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
        const QString value = selectDirectory(
            page.widget, "Select Source Directory", source->text());
        if (!value.isEmpty()) source->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString src = source->text().trimmed();
        const QString backupName = name->text().trimmed();
        if (src.isEmpty()) {
            QMessageBox::warning(page.widget, "Input Error", "请选择源目录。");
            return;
        }

        AlgorithmValues algorithms;
        ServerValues serverValues;
        try {
            algorithms =
                snapshotAlgorithms(pack, compression, encryption, key);
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "Input Error",
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
        const QString value = selectDirectory(
            page.widget, "Select Destination Directory",
            destination->text());
        if (!value.isEmpty()) destination->setText(value);
    });
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        const QString backupId = id->text().trimmed();
        const QString dest = destination->text().trimmed();
        const QString password = key->text();
        const bool allowOverwrite = overwrite->isChecked();
        if (backupId.isEmpty() || dest.isEmpty()) {
            QMessageBox::warning(page.widget, "Input Error",
                                 "请填写备份 ID 和目标目录。");
            return;
        }

        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "Input Error",
                                 QString::fromUtf8(error.what()));
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
    page.layout->addLayout(top, 3);

    Card task = makeCard("同步状态");
    JobControls controls = addJobControls(task.body, "刷新远程列表");
    page.layout->addWidget(task.frame, 2);

    auto entries =
        std::make_shared<std::vector<network::RemoteBackupEntry>>();
    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "Input Error",
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

