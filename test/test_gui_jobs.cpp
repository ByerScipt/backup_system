#include "ui.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace
{
bool testOverwriteRestore(QApplication& app)
{
    QTemporaryDir directory;
    if (!directory.isValid())
    {
        return false;
    }
    const std::filesystem::path root(directory.path().toStdString());
    std::filesystem::create_directory(root / "source");
    std::ofstream(root / "source/file") << "archive content";
    const auto archive = (root / "test.bak").string();
    const auto target = (root / "restored").string();
    if (!backup::BackupEngine::create((root / "source").string(), archive, {})
             .success ||
        !backup::BackupEngine::restore(archive, target, {}).success)
    {
        return false;
    }
    std::ofstream(root / "restored/source/file") << "existing content";
    std::unique_ptr<QWidget> page(backup::gui::localRestorePage());
    for (auto* field : page->findChildren<QLineEdit*>())
    {
        if (field->placeholderText() == "选择 .bak 归档")
        {
            field->setText(QString::fromStdString(archive));
        }
        if (field->placeholderText() == "还原到该目录下")
        {
            field->setText(QString::fromStdString(target));
        }
    }
    page->findChild<QCheckBox*>()->setChecked(true);
    auto* start = page->findChild<QPushButton*>("primaryButton");
    auto* progress = page->findChild<QProgressBar*>();
    bool success = false, confirmed = false;
    QTimer poll;
    QObject::connect(
        &poll, &QTimer::timeout, page.get(),
        [&]()
        {
            if (auto* dialog = qobject_cast<QMessageBox*>(
                    QApplication::activeModalWidget()))
            {
                if (dialog->standardButtons().testFlag(QMessageBox::Yes))
                {
                    confirmed = true;
                    dialog->button(QMessageBox::Yes)->click();
                }
                else
                {
                    dialog->reject();
                }
            }
            if (confirmed && !page->property("jobRunning").toBool() &&
                progress->value() == 100)
            {
                std::ifstream in(root / "restored/source/file");
                std::string text;
                std::getline(in, text);
                success = text == "archive content";
                app.quit();
            }
        });
    poll.start(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, &QApplication::quit);
    deadline.start(3500);
    page->show();
    QTimer::singleShot(0, start, &QPushButton::click);
    app.exec();
    return success;
}

bool waitFor(const std::function<bool()>& ready)
{
    QElapsedTimer deadline;
    deadline.start();
    while (!ready() && deadline.elapsed() < 6000)
    {
        QTest::qWait(10);
    }
    return ready();
}

QLineEdit* field(QWidget* page, const QString& placeholder)
{
    for (auto* edit : page->findChildren<QLineEdit*>())
    {
        if (edit->placeholderText() == placeholder)
        {
            return edit;
        }
    }
    throw std::runtime_error("GUI field not found: " +
                             placeholder.toStdString());
}

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void testUserWorkflow()
{
    QTemporaryDir directory;
    require(directory.isValid(), "temporary GUI fixture failed");
    const QString root = directory.path();
    const std::filesystem::path source((root + "/课堂 资料").toStdString());
    std::filesystem::create_directory(source);
    std::ofstream(source / "说明 文本.txt") << "GUI round trip / 中文内容";
    QTcpServer reservation;
    require(reservation.listen(QHostAddress::LocalHost, 0),
            "port reservation failed");
    const auto port = reservation.serverPort();
    reservation.close();
    QProcess server;
    server.start(
        QCoreApplication::applicationDirPath() + "/backup-server",
        {"--port", QString::number(port), "--storage", root + "/server"});
    require(server.waitForStarted(), "GUI test server failed to start");
    require(waitFor(
                [&]()
                {
                    QTcpSocket socket;
                    socket.connectToHost(QHostAddress::LocalHost, port);
                    return socket.waitForConnected(20);
                }),
            "GUI test server did not become ready");

    std::unique_ptr<QMainWindow> window(backup::gui::createMainWindow());
    auto* pages = window->findChild<QStackedWidget*>("pageStack");
    window->show();
    const auto navigate = [&](int index)
    {
        const QStringList names = {"本地备份", "本地还原", "远程备份",
                                   "远程还原", "备份历史", "账号注册"};
        for (auto* button : window->findChildren<QPushButton*>("navButton"))
        {
            if (button->text() == names.at(index))
            {
                QTest::mouseClick(button, Qt::LeftButton);
            }
        }
        require(pages->currentIndex() == index, "navigation failed");
        return pages->widget(index);
    };
    QString modalError;
    QTimer dismiss;
    QObject::connect(&dismiss, &QTimer::timeout, window.get(),
                     [&]()
                     {
                         if (auto* dialog = qobject_cast<QMessageBox*>(
                                 QApplication::activeModalWidget()))
                         {
                             modalError =
                                 dialog->windowTitle() + ": " + dialog->text();
                             dialog->reject();
                         }
                     });
    dismiss.start(10);
    const auto run = [&](QWidget* page)
    {
        modalError.clear();
        QTest::mouseClick(page->findChild<QPushButton*>("primaryButton"),
                          Qt::LeftButton);
        require(
            waitFor([&]() { return !page->property("jobRunning").toBool(); }),
            "GUI job timed out");
        if (!modalError.isEmpty())
        {
            throw std::runtime_error(modalError.toStdString());
        }
        require(page->findChild<QProgressBar*>()->value() == 100,
                "GUI job failed");
    };
    auto* local = navigate(0);
    QTest::mouseClick(local->findChild<QPushButton*>("primaryButton"),
                      Qt::LeftButton);
    require(modalError.startsWith("输入有误"),
            "empty input did not show Chinese validation");
    require(!local->property("jobRunning").toBool(),
            "invalid input started a job");
    field(local, "选择需要备份的目录")
        ->setText(QString::fromStdString(source.string()));
    field(local, "例如 /home/user/data.bak")->setText(root + "/local.bak");
    local->findChildren<QComboBox*>().at(1)->setCurrentIndex(2);
    local->findChildren<QComboBox*>().at(2)->setCurrentIndex(1);
    field(local, "启用加密后输入归档密码")->setText("archive-pass");
    run(local);
    auto* restoreLocal = navigate(1);
    field(restoreLocal, "选择 .bak 归档")->setText(root + "/local.bak");
    field(restoreLocal, "还原到该目录下")->setText(root + "/local-restored");
    field(restoreLocal, "未加密归档可留空")->setText("archive-pass");
    run(restoreLocal);

    auto* account = navigate(5);
    account->findChild<QSpinBox*>("serverPort")->setValue(port);
    account->findChild<QLineEdit*>("serverUsername")->setText("gui_user");
    account->findChild<QLineEdit*>("serverPassword")->setText("gui-pass");
    field(account, "再次输入账号密码")->setText("gui-pass");
    run(account);
    for (int index : {2, 3, 4})
    {
        require(
            pages->widget(index)->findChild<QSpinBox*>("serverPort")->value() ==
                port,
            "connection port was not shared");
        require(pages->widget(index)
                        ->findChild<QLineEdit*>("serverUsername")
                        ->text() == "gui_user",
                "account was not shared");
    }
    auto* upload = navigate(2);
    field(upload, "选择需要上传的目录")
        ->setText(QString::fromStdString(source.string()));
    field(upload, "可选的易读名称")->setText("课堂备份");
    run(upload);
    std::ofstream(source / "较大文件") << std::string(12000, 'x');
    field(upload, "可选的易读名称")->setText("较大备份");
    run(upload);
    auto* history = navigate(4);
    run(history);
    auto* table = history->findChild<QTableWidget*>();
    require(table->rowCount() == 2, "GUI history did not list uploads");
    table->sortItems(1, Qt::AscendingOrder);
    require(table->item(0, 1)->data(Qt::DisplayRole).toULongLong() <
                table->item(1, 1)->data(Qt::DisplayRole).toULongLong(),
            "size sorting is not numeric");
    auto* search = history->findChild<QLineEdit*>("historySearch");
    search->setText("课堂");
    require(!table->isRowHidden(0) && table->isRowHidden(1),
            "name filtering failed");
    search->clear();
    table->selectRow(0);
    QTest::mouseClick(history->findChild<QPushButton*>("copyBackupId"),
                      Qt::LeftButton);
    const QString id = table->item(0, 0)->data(Qt::UserRole).toString();
    require(QApplication::clipboard()->text() == id, "full ID copy failed");
    QTest::mouseClick(history->findChild<QPushButton*>("restoreSelected"),
                      Qt::LeftButton);
    require(pages->currentIndex() == 3, "selected restore navigation failed");
    auto* restoreRemote = pages->widget(3);
    require(restoreRemote->findChild<QLineEdit*>("backupId")->text() == id,
            "selected ID was not filled");
    field(restoreRemote, "选择还原目标目录")
        ->setText(root + "/remote-restored");
    run(restoreRemote);
    for (const QString destination : {"local-restored", "remote-restored"})
    {
        QFile restored(root + "/" + destination + "/课堂 资料/说明 文本.txt");
        require(restored.open(QIODevice::ReadOnly) &&
                    restored.readAll() ==
                        QByteArray("GUI round trip / 中文内容"),
                "restored GUI fixture content differs");
    }
    const QString evidence = qEnvironmentVariable("BACKUP_GUI_EVIDENCE_DIR");
    if (!evidence.isEmpty())
    {
        QDir().mkpath(evidence);
        for (int index = 0; index < 6; ++index)
        {
            navigate(index);
            QTest::qWait(50);
            require(window->grab().save(evidence + "/workflow-" +
                                        QString::number(index) + ".png"),
                    "GUI evidence capture failed");
        }
    }
    history->findChild<QLineEdit*>("serverUsername")->setText("different_user");
    require(table->rowCount() == 0, "account change retained old history");
    require(!history->findChild<QPushButton*>("restoreSelected")->isEnabled(),
            "cleared history retained restore action");
    server.terminate();
    require(server.waitForFinished(2000) && server.exitCode() == 0,
            "workflow server did not stop cleanly");
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setFont(backup::gui::applicationFont());
    backup::gui::MessageBoxButtonIconFilter buttonFilter;
    app.installEventFilter(&buttonFilter);
    for (const auto* stage :
         {"encrypt", "decrypt", "network-upload", "network-download",
          "compress-copy", "decompress-copy", "read-archive", "finalize"})
    {
        if (backup::gui::stageName(stage) == stage)
        {
            std::cerr << "untranslated progress stage: " << stage << '\n';
            return 1;
        }
    }
    app.setQuitOnLastWindowClosed(false);
    QWidget window;
    auto* layout = new QVBoxLayout(&window);
    std::atomic_int finished{0}, cancelledCount{0};
    std::vector<std::pair<QWidget*, backup::gui::JobControls>> jobs;
    for (int i = 0; i < 2; ++i)
    {
        auto* page = new QWidget(&window);
        layout->addWidget(page);
        auto controls =
            backup::gui::addJobControls(new QVBoxLayout(page), "Test");
        jobs.emplace_back(page, controls);
    }
    window.show();
    for (const auto& [page, controls] : jobs)
    {
        backup::gui::startJob(
            page, controls,
            [&](std::atomic_bool* cancelled,
                const backup::ProgressCallback&) -> QString
            {
                const auto deadline =
                    std::chrono::steady_clock::now() + std::chrono::seconds(2);
                while (!cancelled->load() &&
                       std::chrono::steady_clock::now() < deadline)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
                if (cancelled->load())
                {
                    ++cancelledCount;
                }
                // Cleanup must finish before the last window closes.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                ++finished;
                if (cancelled->load())
                {
                    throw std::runtime_error("operation cancelled");
                }
                return "finished";
            });
    }
    bool deferred = false, success = false;
    QTimer::singleShot(50, &window,
                       [&]()
                       {
                           window.close();
                           deferred = window.isVisible();
                       });
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app,
                     [&]()
                     {
                         if (finished == 2 && !window.isVisible())
                         {
                             success = deferred && cancelledCount == 2;
                             app.quit();
                         }
                     });
    timer.start(10);
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &app, &QApplication::quit);
    deadline.start(3500);
    app.exec();
    timer.stop();
    deadline.stop();
    if (!success)
    {
        std::cerr << "window-close test: finished=" << finished
                  << ", cancelled=" << cancelledCount
                  << ", deferred=" << deferred
                  << ", visible=" << window.isVisible() << '\n';
        return 1;
    }
    if (!testOverwriteRestore(app))
    {
        std::cerr
            << "GUI confirmed overwrite did not restore archive content\n";
        return 1;
    }
    try
    {
        testUserWorkflow();
    }
    catch (const std::exception& error)
    {
        std::cerr << "GUI workflow: " << error.what() << '\n';
        return 1;
    }
    std::cout << "GUI workflows, progress, overwrite and deferred-close tests "
                 "passed.\n";
    return 0;
}
