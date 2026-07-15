#include "backup/core.hpp"
#include "backup/network.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QStringList>
#include <QSpinBox>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace backup;

namespace {

struct JobControls {
    QPushButton* start = nullptr;
    QPushButton* cancel = nullptr;
    QProgressBar* progress = nullptr;
    QTextEdit* log = nullptr;
};

void appendLog(QTextEdit* log, const QString& text) {
    log->append(QString("[%1] %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

using Job = std::function<QString(std::atomic_bool*, const ProgressCallback&)>;

void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess = {}) {
    if (owner->property("jobRunning").toBool()) {
        QMessageBox::information(owner, "任务进行中", "请等待当前任务完成或先取消当前任务。");
        return;
    }
    owner->setProperty("jobRunning", true);
    controls.start->setEnabled(false);
    controls.cancel->setEnabled(true);
    controls.progress->setValue(0);
    controls.log->clear();
    appendLog(controls.log, "任务开始");
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    controls.cancel->disconnect();
    QObject::connect(controls.cancel, &QPushButton::clicked, owner, [cancelled]() { cancelled->store(true); });

    QPointer<QWidget> safeOwner(owner);
    QPointer<QProgressBar> safeProgress(controls.progress);
    QPointer<QTextEdit> safeLog(controls.log);
    QPointer<QPushButton> safeStart(controls.start);
    QPointer<QPushButton> safeCancel(controls.cancel);
    auto progress = [safeOwner, safeProgress, safeLog](const ProgressEvent& event) {
        if (!safeOwner) return;
        QString stage = QString::fromStdString(event.stage);
        QString detail = QString::fromStdString(event.detail);
        uint64_t completed = event.completed, total = event.total;
        QMetaObject::invokeMethod(safeOwner, [safeProgress, safeLog, stage, detail, completed, total]() {
            if (!safeProgress || !safeLog) return;
            if (total) safeProgress->setValue(static_cast<int>(std::min<uint64_t>(100, completed * 100 / total)));
            safeProgress->setFormat(total ? stage + " %p%" : stage);
            if (!detail.isEmpty() && (completed == 0 || completed == total)) appendLog(safeLog, stage + ": " + detail);
        }, Qt::QueuedConnection);
    };

    QThread* thread = QThread::create([=]() {
        QString message;
        bool ok = false;
        try { message = job(cancelled.get(), progress); ok = true; }
        catch (const std::exception& error) { message = QString::fromUtf8(error.what()); }
        catch (...) { message = "未知错误"; }
        if (!safeOwner) return;
        QMetaObject::invokeMethod(safeOwner, [=]() {
            if (!safeOwner || !safeProgress || !safeLog || !safeStart || !safeCancel) return;
            safeStart->setEnabled(true); safeCancel->setEnabled(false);
            safeOwner->setProperty("jobRunning", false);
            safeProgress->setValue(ok ? 100 : 0);
            appendLog(safeLog, (ok ? "✓ " : "✗ ") + message);
            if (ok && afterSuccess) afterSuccess();
        }, Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse, const QString& placeholder) {
    auto* row = new QHBoxLayout;
    edit = new QLineEdit; edit->setPlaceholderText(placeholder);
    browse = new QPushButton("浏览...");
    row->addWidget(edit, 1); row->addWidget(browse);
    return row;
}

JobControls addJobControls(QVBoxLayout* layout, const QString& startText) {
    JobControls controls;
    auto* row = new QHBoxLayout;
    controls.start = new QPushButton(startText); controls.start->setObjectName("primaryButton");
    controls.cancel = new QPushButton("取消"); controls.cancel->setEnabled(false);
    row->addWidget(controls.start, 1); row->addWidget(controls.cancel);
    layout->addLayout(row);
    controls.progress = new QProgressBar; controls.progress->setRange(0,100); controls.progress->setValue(0);
    layout->addWidget(controls.progress);
    controls.log = new QTextEdit; controls.log->setReadOnly(true); controls.log->setMinimumHeight(140);
    controls.log->document()->setMaximumBlockCount(800); layout->addWidget(controls.log,1);
    return controls;
}

void addAlgorithmRows(QFormLayout* form, QComboBox*& pack, QComboBox*& compression,
                      QComboBox*& encryption, QLineEdit*& key) {
    pack = new QComboBox; pack->addItems({"stream", "index"}); form->addRow("打包算法",pack);
    compression = new QComboBox; compression->addItems({"none","rle","huffman"}); form->addRow("压缩算法",compression);
    encryption = new QComboBox; encryption->addItems({"none","xor","vigenere"}); form->addRow("加密算法",encryption);
    key = new QLineEdit; key->setEchoMode(QLineEdit::Password); key->setPlaceholderText("选择加密算法时必填"); form->addRow("归档密钥",key);
}

struct AlgorithmValues {
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
    std::string password;
};

AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key) {
    AlgorithmValues values{parsePackAlgorithm(pack->currentText().toStdString()),
                           parseCompressionAlgorithm(compression->currentText().toStdString()),
                           parseEncryptionAlgorithm(encryption->currentText().toStdString()),
                           key->text().toStdString()};
    if (values.encryption != EncryptionAlgorithm::None && values.password.empty())
        throw std::runtime_error("请选择加密算法后必须填写归档密钥");
    return values;
}

BackupOptions algorithmOptions(const AlgorithmValues& values, std::atomic_bool* cancel,
                               ProgressCallback progress) {
    BackupOptions options;
    options.pack=values.pack; options.compression=values.compression; options.encryption=values.encryption;
    options.password=values.password; options.cancel=cancel; options.progress=std::move(progress);
    return options;
}

struct ServerFields {
    QLineEdit* host=nullptr; QSpinBox* port=nullptr; QLineEdit* username=nullptr; QLineEdit* password=nullptr;
};

ServerFields addServerRows(QFormLayout* form) {
    ServerFields fields;
    fields.host=new QLineEdit("127.0.0.1");form->addRow("服务器",fields.host);
    fields.port=new QSpinBox;fields.port->setRange(1,65535);fields.port->setValue(8848);form->addRow("端口",fields.port);
    fields.username=new QLineEdit;form->addRow("用户名",fields.username);
    fields.password=new QLineEdit;fields.password->setEchoMode(QLineEdit::Password);form->addRow("账号密码",fields.password);
    return fields;
}

struct ServerValues {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
};

ServerValues snapshotServer(const ServerFields& fields) {
    if(fields.host->text().trimmed().isEmpty()||fields.username->text().trimmed().isEmpty()||fields.password->text().isEmpty())
        throw std::runtime_error("服务器、用户名和账号密码不能为空");
    return {fields.host->text().trimmed().toStdString(),static_cast<uint16_t>(fields.port->value()),
            fields.username->text().trimmed().toStdString(),fields.password->text().toStdString()};
}

network::BackupClient makeClient(const ServerValues& values) {
    return {values.host, values.port, values.username, values.password};
}

QString temporaryArchivePath() {
    return QDir::tempPath()+"/backup-gui-"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".bak";
}

QWidget* localBackupPage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* form=new QFormLayout;
    QLineEdit *source,*output,*key;QPushButton *browseSource,*browseOutput;QComboBox *pack,*compression,*encryption;
    form->addRow("源目录",pathRow(source,browseSource,"选择需要备份的目录"));
    form->addRow("输出归档",pathRow(output,browseOutput,"例如 /home/user/data.bak"));
    addAlgorithmRows(form,pack,compression,encryption,key);layout->addLayout(form);
    auto controls=addJobControls(layout,"开始本地备份");
    QObject::connect(browseSource,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getExistingDirectory(page,"选择源目录");if(!value.isEmpty())source->setText(value);});
    QObject::connect(browseOutput,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getSaveFileName(page,"选择输出归档",{},"Backup (*.bak)");if(!value.isEmpty())output->setText(value);});
    QObject::connect(controls.start,&QPushButton::clicked,page,[=](){
        QString src=source->text().trimmed(),out=output->text().trimmed();if(src.isEmpty()||out.isEmpty()){QMessageBox::warning(page,"输入错误","请选择源目录和输出归档。");return;}
        if(QFile::exists(out)){QMessageBox::warning(page,"文件冲突","输出归档已经存在，请选择新文件名。");return;}
        AlgorithmValues algorithms;
        try { algorithms=snapshotAlgorithms(pack,compression,encryption,key); }
        catch(const std::exception& error){QMessageBox::warning(page,"输入错误",QString::fromUtf8(error.what()));return;}
        startJob(page,controls,[=](std::atomic_bool* cancel,const ProgressCallback& progress){auto options=algorithmOptions(algorithms,cancel,progress);auto result=BackupEngine::create(src.toStdString(),out.toStdString(),options);if(!result.success)throw std::runtime_error(result.message);return QString("备份完成：%1 个条目，%2 字节").arg(result.entryCount).arg(result.outputBytes);});
    }); return page;
}

QWidget* localRestorePage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* form=new QFormLayout;
    QLineEdit *archive,*destination;QPushButton *browseArchive,*browseDestination;
    form->addRow("备份归档",pathRow(archive,browseArchive,"选择 .bak 文件"));form->addRow("目标目录",pathRow(destination,browseDestination,"还原到该目录下"));
    auto* key=new QLineEdit;key->setEchoMode(QLineEdit::Password);form->addRow("归档密钥",key);auto* overwrite=new QCheckBox("允许覆盖同名文件（目录类型冲突仍会拒绝）");form->addRow("覆盖策略",overwrite);auto* previewButton=new QPushButton("预览归档条目与目标冲突");form->addRow("还原预检",previewButton);layout->addLayout(form);
    auto* conflictView=new QTextEdit;conflictView->setReadOnly(true);conflictView->setMaximumHeight(110);conflictView->setPlaceholderText("预检结果会在这里列出；归档解码和校验在后台线程执行。");layout->addWidget(conflictView);
    auto controls=addJobControls(layout,"开始本地还原");
    QObject::connect(browseArchive,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getOpenFileName(page,"选择备份归档",{},"Backup (*.bak)");if(!value.isEmpty())archive->setText(value);});
    QObject::connect(browseDestination,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getExistingDirectory(page,"选择目标目录");if(!value.isEmpty())destination->setText(value);});
    auto previewResult=std::make_shared<RestorePreview>();
    QObject::connect(previewButton,&QPushButton::clicked,page,[=](){QString input=archive->text().trimmed(),dest=destination->text().trimmed(),password=key->text();if(input.isEmpty()||dest.isEmpty()){QMessageBox::warning(page,"输入错误","请选择备份归档和目标目录。");return;}JobControls previewControls=controls;previewControls.start=previewButton;startJob(page,previewControls,[=](std::atomic_bool*,const ProgressCallback&){*previewResult=BackupEngine::preview(input.toStdString(),dest.toStdString(),password.toStdString());return QString("预检完成：%1 个条目，%2 个冲突").arg(previewResult->entries.size()).arg(previewResult->conflicts.size());},[=](){QStringList lines;if(previewResult->conflicts.empty())lines<<"未发现已有路径冲突。";else{lines<<QString("发现 %1 个冲突：").arg(previewResult->conflicts.size());for(const auto& path:previewResult->conflicts)lines<<QString::fromStdString(path);}conflictView->setPlainText(lines.join('\n'));});});
    QObject::connect(controls.start,&QPushButton::clicked,page,[=](){QString input=archive->text().trimmed(),dest=destination->text().trimmed(),password=key->text();bool allowOverwrite=overwrite->isChecked();if(input.isEmpty()||dest.isEmpty()){QMessageBox::warning(page,"输入错误","请选择备份归档和目标目录。");return;}if(allowOverwrite&&QMessageBox::question(page,"确认覆盖","目标中已有的同名文件会被替换，是否继续？")!=QMessageBox::Yes)return;
        startJob(page,controls,[=](std::atomic_bool* cancel,const ProgressCallback& progress){auto preview=BackupEngine::preview(input.toStdString(),dest.toStdString(),password.toStdString());if(!preview.conflicts.empty()){QStringList lines;for(size_t i=0;i<std::min<size_t>(preview.conflicts.size(),20);++i)lines<<QString::fromStdString(preview.conflicts[i]);progress({"conflict-preview",0,0,lines.join("；").toStdString()});if(!allowOverwrite)throw std::runtime_error("目标存在 "+std::to_string(preview.conflicts.size())+" 个冲突；请查看预检结果，确认后启用覆盖策略");}RestoreOptions options;options.password=password.toStdString();options.overwrite=allowOverwrite;options.cancel=cancel;options.progress=progress;auto result=BackupEngine::restore(input.toStdString(),dest.toStdString(),options);if(!result.success)throw std::runtime_error(result.message);return QString("还原完成：%1 个条目，%2 文件字节").arg(result.entryCount).arg(result.outputBytes);});
    }); return page;
}

QWidget* remoteBackupPage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* form=new QFormLayout;auto server=addServerRows(form);
    QLineEdit *source;QPushButton *browse;form->addRow("源目录",pathRow(source,browse,"选择需要上传的目录"));
    auto* name=new QLineEdit;name->setPlaceholderText("可选的易读名称");form->addRow("备份名称",name);QComboBox *pack,*compression,*encryption;QLineEdit* key;addAlgorithmRows(form,pack,compression,encryption,key);layout->addLayout(form);
    auto controls=addJobControls(layout,"构建并上传");QObject::connect(browse,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getExistingDirectory(page,"选择源目录");if(!value.isEmpty())source->setText(value);});
    QObject::connect(controls.start,&QPushButton::clicked,page,[=](){QString src=source->text().trimmed(),backupName=name->text().trimmed();if(src.isEmpty()){QMessageBox::warning(page,"输入错误","请选择源目录。");return;}AlgorithmValues algorithms;ServerValues serverValues;try{algorithms=snapshotAlgorithms(pack,compression,encryption,key);serverValues=snapshotServer(server);}catch(const std::exception& error){QMessageBox::warning(page,"输入错误",QString::fromUtf8(error.what()));return;}startJob(page,controls,[=](std::atomic_bool* cancel,const ProgressCallback& progress){QString temp=temporaryArchivePath();struct Cleanup{QString p;~Cleanup(){QFile::remove(p);}}cleanup{temp};auto options=algorithmOptions(algorithms,cancel,progress);auto created=BackupEngine::create(src.toStdString(),temp.toStdString(),options);if(!created.success)throw std::runtime_error(created.message);auto client=makeClient(serverValues);std::string id,error;if(!client.upload(temp.toStdString(),backupName.toStdString(),id,error))throw std::runtime_error(error);return QString("远程备份完成，ID：%1").arg(QString::fromStdString(id));});});return page;
}

QWidget* remoteRestorePage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* form=new QFormLayout;auto server=addServerRows(form);auto* id=new QLineEdit;form->addRow("备份 ID",id);QLineEdit *destination;QPushButton *browse;form->addRow("目标目录",pathRow(destination,browse,"选择还原目标"));auto* key=new QLineEdit;key->setEchoMode(QLineEdit::Password);form->addRow("归档密钥",key);auto* overwrite=new QCheckBox("允许覆盖同名文件");form->addRow("覆盖策略",overwrite);layout->addLayout(form);auto controls=addJobControls(layout,"下载并还原");QObject::connect(browse,&QPushButton::clicked,page,[=](){QString value=QFileDialog::getExistingDirectory(page,"选择目标目录");if(!value.isEmpty())destination->setText(value);});QObject::connect(controls.start,&QPushButton::clicked,page,[=](){QString backupId=id->text().trimmed(),dest=destination->text().trimmed(),password=key->text();bool allowOverwrite=overwrite->isChecked();if(backupId.isEmpty()||dest.isEmpty()){QMessageBox::warning(page,"输入错误","请填写备份 ID 和目标目录。");return;}ServerValues serverValues;try{serverValues=snapshotServer(server);}catch(const std::exception& error){QMessageBox::warning(page,"输入错误",QString::fromUtf8(error.what()));return;}if(allowOverwrite&&QMessageBox::question(page,"确认覆盖","目标中已有的同名文件会被替换，是否继续？")!=QMessageBox::Yes)return;startJob(page,controls,[=](std::atomic_bool* cancel,const ProgressCallback& progress){QString temp=temporaryArchivePath();struct Cleanup{QString p;~Cleanup(){QFile::remove(p);}}cleanup{temp};auto client=makeClient(serverValues);std::string error;if(!client.download(backupId.toStdString(),temp.toStdString(),error))throw std::runtime_error(error);RestoreOptions options;options.password=password.toStdString();options.overwrite=allowOverwrite;options.cancel=cancel;options.progress=progress;auto restored=BackupEngine::restore(temp.toStdString(),dest.toStdString(),options);if(!restored.success)throw std::runtime_error(restored.message);return QString("远程还原完成：%1 个条目").arg(restored.entryCount);});});return page;
}

QWidget* remoteListPage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* form=new QFormLayout;auto server=addServerRows(form);layout->addLayout(form);auto* table=new QTableWidget(0,4);table->setHorizontalHeaderLabels({"备份 ID","名称","大小","时间"});table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);table->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);layout->addWidget(table,1);auto controls=addJobControls(layout,"刷新列表");controls.log->setMaximumHeight(110);auto entries=std::make_shared<std::vector<network::RemoteBackupEntry>>();QObject::connect(controls.start,&QPushButton::clicked,page,[=](){ServerValues serverValues;try{serverValues=snapshotServer(server);}catch(const std::exception& error){QMessageBox::warning(page,"输入错误",QString::fromUtf8(error.what()));return;}startJob(page,controls,[=](std::atomic_bool*,const ProgressCallback&){auto client=makeClient(serverValues);std::string error;*entries=client.list(error);if(!error.empty())throw std::runtime_error(error);return QString("获取到 %1 个备份").arg(entries->size());},[=](){table->setRowCount(static_cast<int>(entries->size()));for(int row=0;row<table->rowCount();++row){const auto& e=(*entries)[static_cast<size_t>(row)];table->setItem(row,0,new QTableWidgetItem(QString::fromStdString(e.id)));table->setItem(row,1,new QTableWidgetItem(QString::fromStdString(e.name)));table->setItem(row,2,new QTableWidgetItem(QString::number(e.size)));table->setItem(row,3,new QTableWidgetItem(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(e.timestamp)).toString("yyyy-MM-dd HH:mm:ss")));}});});return page;
}

QWidget* userPage() {
    auto* page=new QWidget;auto* layout=new QVBoxLayout(page);auto* intro=new QLabel("在备份服务器上创建账号。密码只用于生成加盐验证值，登录采用随机挑战证明。");intro->setWordWrap(true);layout->addWidget(intro);auto* form=new QFormLayout;auto server=addServerRows(form);auto* confirm=new QLineEdit;confirm->setEchoMode(QLineEdit::Password);form->addRow("确认密码",confirm);layout->addLayout(form);auto controls=addJobControls(layout,"注册账号");QObject::connect(controls.start,&QPushButton::clicked,page,[=](){if(server.password->text()!=confirm->text()){QMessageBox::warning(page,"输入错误","两次输入的密码不一致。");return;}ServerValues serverValues;try{serverValues=snapshotServer(server);}catch(const std::exception& error){QMessageBox::warning(page,"输入错误",QString::fromUtf8(error.what()));return;}startJob(page,controls,[=](std::atomic_bool*,const ProgressCallback&){auto client=makeClient(serverValues);std::string error;if(!client.registerUser(error))throw std::runtime_error(error);return QString("账号注册成功");});});return page;
}

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("Backup Studio - 数据备份与还原系统");resize(900,720);
        auto* central=new QWidget;auto* layout=new QVBoxLayout(central);auto* title=new QLabel("Backup Studio");title->setObjectName("title");auto* subtitle=new QLabel("流式归档 · 自研算法 · 完整性校验 · 账号隔离远程备份");subtitle->setObjectName("subtitle");layout->addWidget(title);layout->addWidget(subtitle);
        auto* tabs=new QTabWidget;tabs->addTab(localBackupPage(),"本地备份");tabs->addTab(localRestorePage(),"本地还原");tabs->addTab(remoteBackupPage(),"远程备份");tabs->addTab(remoteRestorePage(),"远程还原");tabs->addTab(remoteListPage(),"远程列表");tabs->addTab(userPage(),"账号管理");layout->addWidget(tabs,1);setCentralWidget(central);
        setStyleSheet(R"(
            QMainWindow { background:#f4f7fb; } QWidget { font-size:13px; color:#1e293b; }
            QLabel#title { font-size:28px; font-weight:700; color:#12355b; }
            QLabel#subtitle { color:#64748b; margin-bottom:8px; }
            QTabWidget::pane { background:white; border:1px solid #dbe3ef; border-radius:8px; }
            QTabBar::tab { padding:10px 14px; background:#e8eef6; margin-right:2px; }
            QTabBar::tab:selected { background:white; color:#1565c0; font-weight:600; }
            QLineEdit,QComboBox,QSpinBox { padding:7px; border:1px solid #cbd5e1; border-radius:5px; background:white; }
            QPushButton { padding:8px 14px; border:1px solid #b8c4d4; border-radius:5px; background:#edf2f7; }
            QPushButton#primaryButton { background:#1565c0; color:white; border-color:#1565c0; font-weight:600; }
            QPushButton:disabled { color:#94a3b8; background:#e2e8f0; }
            QTextEdit,QTableWidget { border:1px solid #dbe3ef; border-radius:5px; background:#fbfdff; }
            QProgressBar { border:1px solid #cbd5e1; border-radius:5px; text-align:center; }
            QProgressBar::chunk { background:#22a06b; border-radius:4px; }
        )");
    }
};

} // namespace

int main(int argc,char* argv[]) {
    QApplication app(argc,argv);app.setApplicationName("Backup Studio");app.setApplicationVersion("1.0");MainWindow window;window.show();return app.exec();
}
