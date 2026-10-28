#include "ui.hpp"
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QTemporaryDir>
#include <QWidget>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>

namespace backup::gui
{

QWidget* remoteBackupPage(AccountSession* session)
{
    Page page = makePage();

    Card accountCard = makeCard();
    auto* accountHint = makeHint(QString());
    accountHint->setObjectName("remoteAccount");
    accountCard.body->addWidget(accountHint);
    const auto updateAccount = [=]()
    {
        const ServerValues values = session->credentials();
        accountHint->setText(
            session->signedIn()
                ? QString("当前账户：%1 @ %2:%3")
                      .arg(QString::fromStdString(values.username))
                      .arg(QString::fromStdString(values.host))
                      .arg(values.port)
                : QString("未登录：请先在「登录」页登录。"));
    };
    QObject::connect(session, &AccountSession::changed, page.widget,
                     updateAccount);
    updateAccount();

    Card archiveCard = makeCard();
    auto* archiveForm = makeForm();
    QLineEdit* source;
    QPushButton* browse;
    archiveForm->addRow("源目录", pathRow(source, browse, "sourcePath"));
    QPushButton* selectEntries;
    QLabel* selectionSummary;
    archiveForm->addRow("备份内容",
                        selectionRow(selectEntries, selectionSummary));
    auto* name = new QLineEdit;
    name->setClearButtonEnabled(true);
    name->setObjectName("backupName");
    name->setPlaceholderText("可选");
    archiveForm->addRow("备份名称", name);
    QComboBox *pack, *compression, *encryption;
    QLineEdit* key;
    addAlgorithmRows(archiveForm, pack, compression, encryption, key);
    archiveCard.body->addLayout(archiveForm);

    page.layout->addWidget(accountCard.frame, 1);
    page.layout->addWidget(archiveCard.frame, 3);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "上传");
    page.footer->addWidget(task.frame);

    bindPathPicker(page.widget, browse, source, "选择源目录", selectDirectory);
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
            const QString backupName = name->text().trimmed();
            if (src.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误", "请选择源目录。");
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
            if (!session->signedIn())
            {
                QMessageBox::warning(page.widget, "未登录",
                                     "远程备份前请先在「登录」页登录。");
                return;
            }
            const ServerValues serverValues = session->credentials();

            const auto includes = toUtf8Strings(*selection);
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
                    options.includes = includes;
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

QWidget* remoteRestorePage(AccountSession* session)
{
    Page page = makePage();

    Card accountCard = makeCard();
    auto* accountHint = makeHint(QString());
    accountHint->setObjectName("remoteAccount");
    accountCard.body->addWidget(accountHint);
    const auto updateAccount = [=]()
    {
        const ServerValues values = session->credentials();
        accountHint->setText(
            session->signedIn()
                ? QString("当前账户：%1 @ %2:%3")
                      .arg(QString::fromStdString(values.username))
                      .arg(QString::fromStdString(values.host))
                      .arg(values.port)
                : QString("未登录：请先在「登录」页登录。"));
    };
    QObject::connect(session, &AccountSession::changed, page.widget,
                     updateAccount);
    updateAccount();

    Card restoreCard = makeCard();
    auto* restoreForm = makeForm();
    auto* id = new QComboBox;
    id->setObjectName("backupId");
    id->setEditable(true);
    id->setInsertPolicy(QComboBox::NoInsert);
    id->lineEdit()->setPlaceholderText("从历史选择或粘贴 ID");
    auto* refresh = new QPushButton("刷新历史");
    refresh->setObjectName("refreshRemoteRestore");
    refresh->setMinimumWidth(100);
    auto* idRow = new QHBoxLayout;
    idRow->setContentsMargins(0, 0, 0, 0);
    idRow->setSpacing(12);
    idRow->addWidget(id, 1);
    idRow->addWidget(refresh);
    restoreForm->addRow("备份 ID", idRow);
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

    page.layout->addWidget(accountCard.frame, 1);
    page.layout->addWidget(restoreCard.frame, 2);

    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "还原");
    page.footer->addWidget(task.frame);

    bindPathPicker(page.widget, browse, destination, "选择还原目标目录",
                   selectDirectory);

    // The list belongs to the entered account: an item keeps its full ID as
    // data, so edited or pasted text stays usable as a raw ID.
    const auto resolveId = [id]()
    {
        const int index = id->currentIndex();
        const QString text = id->currentText().trimmed();
        if (index >= 0 && id->itemText(index) == id->currentText())
        {
            return id->itemData(index).toString();
        }
        return text;
    };
    const auto dropStaleList = [=]()
    {
        const int index = id->currentIndex();
        const bool fromList =
            index >= 0 && id->itemText(index) == id->currentText();
        const QString keep = fromList ? QString() : id->currentText();
        id->clear();
        if (!keep.isEmpty())
        {
            id->setEditText(keep);
        }
    };
    // Dropping the list on account change keeps another account's IDs from
    // being restored by accident; typed IDs stay usable.
    QObject::connect(session, &AccountSession::changed, page.widget,
                     dropStaleList);

    auto listed = std::make_shared<std::vector<network::RemoteBackupEntry>>();
    QObject::connect(
        refresh, &QPushButton::clicked, page.widget,
        [=]()
        {
            if (!session->signedIn())
            {
                QMessageBox::warning(page.widget, "未登录",
                                     "刷新历史前请先在「登录」页登录。");
                return;
            }
            const ServerValues values = session->credentials();
            const QString previous = resolveId();
            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback&)
                {
                    auto client = makeClient(values);
                    std::string error;
                    *listed = client.list(error, cancel);
                    if (!error.empty())
                    {
                        throw std::runtime_error(error);
                    }
                    return QString("已加载 %1 个备份").arg(listed->size());
                },
                [=]()
                {
                    // Ignore a response if the account changed mid-request.
                    if (session->credentials().username != values.username)
                    {
                        return;
                    }
                    id->clear();
                    for (const auto& entry : *listed)
                    {
                        const QString full = QString::fromStdString(entry.id);
                        const QString shown =
                            entry.name.empty()
                                ? full
                                : QString("%1 · %2")
                                      .arg(QString::fromStdString(entry.name))
                                      .arg(QDateTime::fromSecsSinceEpoch(
                                               static_cast<qint64>(
                                                   entry.timestamp))
                                               .toString("yyyy-MM-dd HH:mm"));
                        id->addItem(shown, full);
                    }
                    const int match = id->findData(previous);
                    if (match >= 0)
                    {
                        id->setCurrentIndex(match);
                    }
                    else if (!previous.isEmpty())
                    {
                        id->setEditText(previous);
                    }
                });
        });
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QString backupId = resolveId();
            const QString dest = destination->text().trimmed();
            const QString password = key->text();
            const bool allowOverwrite = overwrite->isChecked();
            if (backupId.isEmpty() || dest.isEmpty())
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     "请填写备份 ID 和目标目录。");
                return;
            }

            if (!session->signedIn())
            {
                QMessageBox::warning(page.widget, "未登录",
                                     "远程还原前请先在「登录」页登录。");
                return;
            }
            const ServerValues serverValues = session->credentials();
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
