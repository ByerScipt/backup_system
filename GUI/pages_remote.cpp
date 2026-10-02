#include "ui.hpp"
#include <QFormLayout>
#include <QFrame>
#include <QVBoxLayout>

#include <QCheckBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QTemporaryDir>
#include <QWidget>
#include <atomic>
#include <exception>
#include <stdexcept>

namespace backup::gui
{

QWidget* remoteBackupPage()
{
    Page page = makePage();

    Card connection = makeCard();
    const ServerFields server = addServerRows(connection.body);

    Card archiveCard = makeCard();
    auto* archiveForm = makeForm();
    QLineEdit* source;
    QPushButton* browse;
    archiveForm->addRow("源目录", pathRow(source, browse, "sourcePath"));
    auto* name = new QLineEdit;
    name->setClearButtonEnabled(true);
    name->setObjectName("backupName");
    name->setPlaceholderText("可选");
    archiveForm->addRow("备份名称", name);
    QComboBox *pack, *compression, *encryption;
    QLineEdit* key;
    addAlgorithmRows(archiveForm, pack, compression, encryption, key);
    archiveCard.body->addLayout(archiveForm);

    page.layout->addWidget(connection.frame, 1);
    page.layout->addWidget(archiveCard.frame, 3);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "上传");
    page.footer->addWidget(task.frame);

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

    Card connection = makeCard();
    const ServerFields server = addServerRows(connection.body);

    Card restoreCard = makeCard();
    auto* restoreForm = makeForm();
    auto* id = new QLineEdit;
    id->setObjectName("backupId");
    id->setClearButtonEnabled(true);
    id->setPlaceholderText("从历史选择");
    restoreForm->addRow("备份 ID", id);
    QLineEdit* destination;
    QPushButton* browse;
    restoreForm->addRow("目标目录",
                        pathRow(destination, browse, "destinationPath"));
    auto* key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setObjectName("archivePassword");
    key->setPlaceholderText("加密时填写");
    restoreForm->addRow("归档密码", key);
    auto* overwrite = new QCheckBox("允许覆盖同名文件");
    restoreForm->addRow("覆盖策略", overwrite);
    restoreCard.body->addLayout(restoreForm);

    page.layout->addWidget(connection.frame, 1);
    page.layout->addWidget(restoreCard.frame, 2);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "还原");
    page.footer->addWidget(task.frame);

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
