#include "ui.hpp"
#include <QClipboard>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QWidget>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

namespace backup::gui
{

QWidget* remoteBackupPage()
{
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card connection = makeCard("服务器连接", "填写服务器地址和账号信息。");
    auto* serverForm = makeForm();
    const ServerFields server = addServerRows(serverForm);
    connection.body->addLayout(serverForm);

    Card archiveCard = makeCard("归档配置", "选择目录、备份名称和归档算法。");
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

    bindPathPicker(page.widget, browse, source, "选择源目录", selectDirectory);
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString src = source->text().trimmed();
            const QString backupName = name->text().trimmed();
            if (src.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误", "请选择源目录。");
                return;
            }

            AlgorithmValues algorithms;
            ServerValues serverValues;
            try
            {
                algorithms =
                    snapshotAlgorithms(pack, compression, encryption, key);
                serverValues = snapshotServer(server);
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
                    QTemporaryDir staging;
                    if (!staging.isValid())
                    {
                        throw std::runtime_error("无法创建临时工作目录");
                    }
                    const QString temporary = staging.filePath("archive.bak");

                    auto options =
                        algorithmOptions(algorithms, cancel, progress);
                    auto created = BackupEngine::create(
                        src.toStdString(), temporary.toStdString(), options);
                    if (!created.success)
                    {
                        throw std::runtime_error(created.message);
                    }

                    auto client = makeClient(serverValues);
                    std::string id;
                    std::string error;
                    if (!client.upload(temporary.toStdString(),
                                       backupName.toStdString(), id, error,
                                       progress, cancel))
                    {
                        throw std::runtime_error(error);
                    }
                    return QString("远程备份完成 · ID %1")
                        .arg(QString::fromStdString(id));
                });
        });
    return page.widget;
}

QWidget* remoteRestorePage()
{
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card connection = makeCard("服务器连接", "填写与注册时相同的账号信息。");
    auto* serverForm = makeForm();
    const ServerFields server = addServerRows(serverForm);
    connection.body->addLayout(serverForm);

    Card restoreCard =
        makeCard("还原配置", "填写备份 ID、目标目录和归档密码。");
    auto* restoreForm = makeForm();
    auto* id = new QLineEdit;
    id->setObjectName("backupId");
    id->setClearButtonEnabled(true);
    id->setPlaceholderText("远程列表中的备份 ID");
    restoreForm->addRow("备份 ID", id);
    QLineEdit* destination;
    QPushButton* browse;
    restoreForm->addRow("目标目录",
                        pathRow(destination, browse, "选择还原目标目录"));
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("未加密归档可留空");
    restoreForm->addRow("归档密码", key);
    auto* overwrite = new QCheckBox("允许覆盖同名文件");
    restoreForm->addRow("覆盖策略", overwrite);
    restoreCard.body->addLayout(restoreForm);

    top->addWidget(connection.frame, 1);
    top->addWidget(restoreCard.frame, 1);
    page.layout->addLayout(top);

    Card task = makeCard("任务状态");
    JobControls controls = addJobControls(task.body, "下载并还原");
    page.layout->addWidget(task.frame, 1);

    bindPathPicker(page.widget, browse, destination, "选择还原目标目录",
                   selectDirectory);
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString backupId = id->text().trimmed();
            const QString dest = destination->text().trimmed();
            const QString password = key->text();
            const bool allowOverwrite = overwrite->isChecked();
            if (backupId.isEmpty() || dest.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请填写备份 ID 和目标目录。");
                return;
            }

            ServerValues serverValues;
            try
            {
                serverValues = snapshotServer(server);
            }
            catch (const std::exception& error)
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     QString::fromUtf8(error.what()));
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
                    QTemporaryDir staging;
                    if (!staging.isValid())
                    {
                        throw std::runtime_error("无法创建临时工作目录");
                    }
                    const QString temporary = staging.filePath("archive.bak");

                    auto client = makeClient(serverValues);
                    std::string error;
                    if (!client.download(backupId.toStdString(),
                                         temporary.toStdString(), error,
                                         progress, cancel))
                    {
                        throw std::runtime_error(error);
                    }

                    RestoreOptions options;
                    options.password = password.toStdString();
                    options.overwrite = allowOverwrite;
                    options.cancel = cancel;
                    options.progress = progress;
                    auto restored = BackupEngine::restore(
                        temporary.toStdString(), dest.toStdString(), options);
                    if (!restored.success)
                    {
                        throw std::runtime_error(restored.message);
                    }
                    return QString("远程还原完成 · %1 个条目")
                        .arg(restored.entryCount);
                });
        });
    return page.widget;
}

} // namespace backup::gui
