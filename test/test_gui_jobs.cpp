#include "ui.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFontInfo>
#include <QFrame>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionComboBox>
#include <QTableWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>
#include <QTextEdit>
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
    page->findChild<QLineEdit*>("archivePath")
        ->setText(QString::fromStdString(archive));
    page->findChild<QLineEdit*>("destinationPath")
        ->setText(QString::fromStdString(target));
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

QLineEdit* field(QWidget* page, const QString& objectName)
{
    if (auto* edit = page->findChild<QLineEdit*>(objectName))
    {
        return edit;
    }
    throw std::runtime_error("GUI field not found: " +
                             objectName.toStdString());
}

void require(bool condition, const char* message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

void resizeWindow(QMainWindow* window, const QSize& size)
{
    QElapsedTimer deadline;
    deadline.start();
    // Native configure events may overwrite an earlier resize request.
    do
    {
        window->resize(size);
        QTest::qWait(100);
    } while (window->size() != size && deadline.elapsed() < 6000);
    require(window->size() == size,
            "window manager did not apply the requested size");
}

void testAccountAlignment()
{
    std::unique_ptr<QMainWindow> window(backup::gui::createMainWindow());
    auto* pages = window->findChild<QStackedWidget*>("pageStack");
    auto* account = pages->widget(5);
    auto* mode = account->findChild<QComboBox*>("accountMode");
    window->show();
    for (const QSize size : {QSize(1320, 900), QSize(2100, 1400)})
    {
        resizeWindow(window.get(), size);
        mode->setCurrentIndex(0);
        QTest::qWait(50);
        const auto bounds = [&window](QWidget* widget) {
            return QRect(widget->mapTo(window.get(), QPoint()), widget->size());
        };
        auto* title = account->findChild<QLabel*>("startTitle");
        auto* password = field(account, "serverPassword");
        const QRect titleBefore = bounds(title);
        const QRect modeBefore = bounds(mode);
        const QRect passwordBefore = bounds(password);
        const auto capture = [&](const QString& name)
        {
            const QString evidence =
                qEnvironmentVariable("BACKUP_GUI_EVIDENCE_DIR");
            if (!evidence.isEmpty())
            {
                QDir().mkpath(evidence);
                require(
                    window->grab().save(evidence + "/account-" + name + "-" +
                                        QString::number(size.width()) + ".png"),
                    "account alignment screenshot failed");
            }
        };
        capture("login");
        mode->setCurrentIndex(1);
        QTest::qWait(50);
        const QRect confirmation = bounds(field(account, "confirmPassword"));
        const QRect registration = bounds(password);
        require(confirmation.x() == registration.x() &&
                    confirmation.width() == registration.width(),
                ("registration columns differ; password x=" +
                 QString::number(registration.x()) + ", confirmation x=" +
                 QString::number(confirmation.x()) + "; title moved by " +
                 QString::number(bounds(title).y() - titleBefore.y()))
                    .toStdString()
                    .c_str());
        require(bounds(title) == titleBefore && bounds(mode) == modeBefore &&
                    registration == passwordBefore,
                "login/register switch moved the title, selector or fields");
        require(
            bounds(field(account, "serverHost")).y() ==
                    bounds(account->findChild<QSpinBox*>("serverPort")).y() &&
                bounds(field(account, "serverUsername")).y() ==
                    registration.y(),
            "connection rows have unequal vertical alignment");
        capture("register");
        mode->setCurrentIndex(0);
        QTest::qWait(50);
        require(bounds(title) == titleBefore &&
                    bounds(password) == passwordBefore,
                "returning to login moved the text");
    }
}

void testDialogPlacement()
{
    std::unique_ptr<QMainWindow> window(backup::gui::createMainWindow());
    auto* pages = window->findChild<QStackedWidget*>("pageStack");
    window->show();
    window->move(300, 180);
    QTest::qWait(300);
    for (int index = 0; index < pages->count(); ++index)
    {
        pages->setCurrentIndex(index);
        auto* page = pages->widget(index);
        bool observed = false;
        bool topLevelParent = false;
        QPoint delta;
        QTimer poll;
        QObject::connect(
            &poll, &QTimer::timeout, window.get(),
            [&]()
            {
                if (auto* dialog = qobject_cast<QMessageBox*>(
                        QApplication::activeModalWidget()))
                {
                    observed = true;
                    topLevelParent = dialog->parentWidget() == window.get();
                    const auto frame = dialog->frameGeometry();
                    const auto area = window->screen()->availableGeometry();
                    // Offscreen's 800px screen cannot contain the
                    // main window; expect the nearest visible center.
                    auto expected = window->frameGeometry().center();
                    expected.setX(std::clamp(
                        expected.x(), area.left() + (frame.width() - 1) / 2,
                        area.right() - frame.width() / 2));
                    expected.setY(std::clamp(
                        expected.y(), area.top() + (frame.height() - 1) / 2,
                        area.bottom() - frame.height() / 2));
                    delta = frame.center() - expected;
                    dialog->reject();
                }
            });
        poll.start(100);
        QMessageBox::warning(page, "输入有误", "位置回归测试");
        require(observed && topLevelParent && delta.manhattanLength() <= 24,
                ("page warning is not centered: " + QString::number(index) +
                 ", offset " + QString::number(delta.x()) + "," +
                 QString::number(delta.y()))
                    .toStdString()
                    .c_str());
        observed = false;
        QMessageBox::question(page, "确认", "位置回归测试",
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No);
        require(observed && topLevelParent && delta.manhattanLength() <= 24,
                "confirmation is not centered");
        const backup::gui::JobControls controls{
            page->findChild<QPushButton*>("primaryButton"),
            page->findChild<QPushButton*>("dangerButton"),
            page->findChild<QProgressBar*>(),
            page->findChild<QTextEdit*>("taskLog")};
        for (bool background : {false, true})
        {
            pages->setCurrentIndex(index);
            observed = false;
            backup::gui::startJob(
                page, controls,
                [](std::atomic_bool*,
                   const backup::ProgressCallback&) -> QString
                { throw std::runtime_error("位置回归测试"); });
            if (background)
            {
                pages->setCurrentIndex((index + 1) % pages->count());
            }
            require(waitFor([&]() { return observed; }) && topLevelParent &&
                        delta.manhattanLength() <= 24,
                    ("job error is not centered: " + QString::number(index) +
                     ", background=" + QString::number(background) +
                     ", offset " + QString::number(delta.x()) + "," +
                     QString::number(delta.y()))
                        .toStdString()
                        .c_str());
        }
    }
}

QRect textBounds(const QImage& image)
{
    const int margin = qRound(10 * image.devicePixelRatio());
    QRect bounds;
    for (int y = margin; y < image.height() - margin; ++y)
    {
        for (int x = margin; x < image.width() - margin; ++x)
        {
            const QColor color = image.pixelColor(x, y);
            if (color.alpha() > 200 && color.red() < 120 &&
                color.green() < 150 && color.blue() < 200)
            {
                bounds = bounds.united(QRect(x, y, 1, 1));
            }
        }
    }
    return bounds;
}

void testSelectionAlignment()
{
    std::unique_ptr<QMainWindow> window(backup::gui::createMainWindow());
    auto* pages = window->findChild<QStackedWidget*>("pageStack");
    const auto navigation = window->findChildren<QPushButton*>("navButton");
    window->show();
    QApplication::processEvents();
    const QString evidence = qEnvironmentVariable("BACKUP_GUI_EVIDENCE_DIR");
    if (!evidence.isEmpty())
    {
        QDir().mkpath(evidence);
    }
    for (const QSize size : {QSize(1320, 900), QSize(2100, 1400)})
    {
        resizeWindow(window.get(), size);
        const int gap =
            navigation.at(1)->y() - navigation.at(0)->geometry().bottom() - 1;
        for (int index = 1; index < navigation.size(); ++index)
        {
            require(navigation.at(index)->height() ==
                            navigation.first()->height() &&
                        navigation.at(index)->y() -
                                navigation.at(index - 1)->geometry().bottom() -
                                1 ==
                            gap,
                    "navigation rows have unequal heights or gaps");
        }
        for (auto* button : navigation)
        {
            button->setFocus();
            QApplication::processEvents();
            const QRect geometry = button->geometry();
            const QFont font = button->font();
            const QRect idle = textBounds(button->grab().toImage());
            require(!idle.isEmpty(), "navigation text was not rendered");
            QTest::mousePress(button, Qt::LeftButton);
            require(textBounds(button->grab().toImage()) == idle,
                    "pressed navigation text moved");
            QTest::mouseRelease(button, Qt::LeftButton);
            QApplication::processEvents();
            require(button->font() == font && button->geometry() == geometry,
                    "selected navigation changed its font or frame");
            require(button->isChecked() &&
                        pages->currentIndex() ==
                            button->property("pageIndex").toInt(),
                    "navigation highlight does not match the current page");
            const QImage selected = button->grab().toImage();
            const QColor border = selected.pixelColor(
                qRound(selected.devicePixelRatio()), selected.height() / 2);
            require(border.alpha() == 255 && border.red() < 170 &&
                        border.blue() > 190,
                    "selected navigation has no visible blue outline");
        }
        pages->setCurrentIndex(0);
        QApplication::processEvents();
        require(navigation.at(1)->isChecked(),
                "page change left the previous navigation highlighted");
        for (auto* combo : pages->widget(0)->findChildren<QComboBox*>())
        {
            const QRect geometry = combo->geometry();
            const auto labelImage = [=]()
            {
                const QImage image = combo->grab().toImage();
                return image.copy(0, 0,
                                  image.width() - 44 * image.devicePixelRatio(),
                                  image.height());
            };
            const QRect closed = textBounds(labelImage());
            const QImage image = combo->grab().toImage();
            const int arrowWidth = qRound(44 * image.devicePixelRatio());
            require(!textBounds(image.copy(image.width() - arrowWidth, 0,
                                           arrowWidth, image.height()))
                         .isEmpty(),
                    "combo arrow is missing");
            combo->showPopup();
            QTest::qWait(20);
            QStyleOptionComboBox option;
            option.initFrom(combo);
            require(combo->style()->styleHint(QStyle::SH_ComboBox_Popup,
                                              &option, combo) == 0,
                    "combo uses an overlapping menu popup");
            require(textBounds(labelImage()) == closed,
                    "opening the combo moved its text");
            const QRect fieldRect(combo->mapToGlobal(QPoint()), combo->size());
            auto* popup = combo->view()->window();
            const QRect screen = combo->screen()->availableGeometry();
            if (screen.contains(fieldRect) &&
                screen.bottom() >= fieldRect.bottom() + popup->height())
            {
                const QPoint position = popup->mapToGlobal(QPoint());
                require(position.x() == fieldRect.x() &&
                            position.y() >= fieldRect.bottom(),
                        "combo popup is not aligned below its field");
            }
            if (!evidence.isEmpty())
            {
                const QString suffix = QString::number(size.width());
                window->grab().save(evidence + "/window-" + suffix + ".png");
                popup->grab().save(evidence + "/popup-" + suffix + ".png");
            }
            QTest::mouseClick(combo->view()->viewport(), Qt::LeftButton,
                              Qt::NoModifier,
                              combo->view()
                                  ->visualRect(combo->view()->currentIndex())
                                  .center());
            QApplication::processEvents();
            require(textBounds(labelImage()) == closed &&
                        combo->geometry() == geometry && !popup->isVisible(),
                    "choosing an option moved the combo or left it open");
        }
    }
    require(window->findChild<QFrame*>("navigationRail")->width() >= 320,
            "large-window sidebar is too narrow");
    window->showMaximized();
    QTest::qWait(50);
    auto* rail = window->findChild<QFrame*>("navigationRail");
    require(pages->mapTo(window.get(), QPoint()).x() >= rail->width(),
            "maximized content overlaps the navigation rail");
    if (!evidence.isEmpty())
    {
        window->grab().save(evidence + "/maximized.png");
    }
}

void testPresentation()
{
    std::unique_ptr<QMainWindow> window(backup::gui::createMainWindow());
    auto* pages = window->findChild<QStackedWidget*>("pageStack");
    window->show();
    QApplication::processEvents();
    const QFontInfo font(field(pages->widget(0), "sourcePath")->font());
    require(font.pointSizeF() >= 15.5, "rendered input font is below 16pt");
    const qreal baseSize = window->font().pointSizeF();
    const std::pair<QSize, int> steps[] = {
        {QSize(1499, 999), 0},  {QSize(1500, 1000), 2}, {QSize(1560, 1040), 2},
        {QSize(1800, 1200), 4}, {QSize(2100, 1400), 6}, {QSize(2099, 1400), 4},
        {QSize(2100, 1399), 4}, {QSize(1900, 800), 0},  {QSize(1320, 900), 0}};
    for (const auto& [size, increase] : steps)
    {
        resizeWindow(window.get(), size);
        require(window->font().pointSizeF() == baseSize + increase,
                "font did not follow the window-size tier");
        for (int index = 0; index < 6; ++index)
        {
            const QFontInfo actionFont(
                pages->widget(index)
                    ->findChild<QPushButton*>("primaryButton")
                    ->font());
            require(qAbs(actionFont.pointSizeF() - baseSize - increase) <= 0.5,
                    "page action did not inherit the font tier");
        }
        auto* log = pages->widget(0)->findChild<QTextEdit*>("taskLog");
        require(qAbs(QFontInfo(log->font()).pointSizeF() - baseSize -
                     increase) <= 0.5,
                "log retained a fixed small font");
    }
    for (int index = 0; index < 6; ++index)
    {
        pages->setCurrentIndex(index);
        resizeWindow(window.get(), QSize(1320, 900));
        auto* page = pages->widget(index);
        auto* start = page->findChild<QPushButton*>("primaryButton");
        require(start->mapTo(window.get(), start->rect().center()).y() >
                    window->height() * 0.8,
                "primary action should use the bottom of the page");
        for (const char* name :
             {"sourcePath", "outputPath", "archivePath", "destinationPath"})
        {
            if (auto* edit = page->findChild<QLineEdit*>(name))
            {
                require(waitFor([&]() { return edit->width() >= 600; }),
                        "path field is too narrow");
            }
        }
        resizeWindow(window.get(), window->minimumSize());
        auto* toggle = page->findChild<QPushButton*>("toggleLog");
        toggle->setChecked(true);
        QTest::qWait(50);
        require(!start->visibleRegion().isEmpty() &&
                    !page->findChild<QTextEdit*>("taskLog")
                         ->visibleRegion()
                         .isEmpty(),
                "small-window logging obscured the action area");
        toggle->setChecked(false);
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
    require(pages->currentIndex() == 5, "start page is not the default");
    require(pages->count() == 6, "unexpected settings page remains");
    for (int index = 0; index < 6; ++index)
    {
        require(
            pages->widget(index)->findChild<QTextEdit*>("taskLog")->isHidden(),
            "task log should be folded by default");
        require(pages->widget(index)->findChild<QProgressBar*>()->isHidden(),
                "idle progress should not occupy the page");
    }
    const auto navigate = [&](int index)
    {
        const QStringList names = {"本地备份", "本地还原", "远程备份",
                                   "远程还原", "备份历史", "开始"};
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
    field(local, "sourcePath")
        ->setText(QString::fromStdString(source.string()));
    field(local, "outputPath")->setText(root + "/local.bak");
    local->findChildren<QComboBox*>().at(1)->setCurrentIndex(2);
    local->findChildren<QComboBox*>().at(2)->setCurrentIndex(1);
    field(local, "archivePassword")->setText("archive-pass");
    run(local);
    auto* log = local->findChild<QTextEdit*>("taskLog");
    auto* toggleLog = local->findChild<QPushButton*>("toggleLog");
    const QString output = log->toPlainText();
    require(output.contains("备份完成") && log->isHidden(),
            "folded log lost job output");
    QTest::mouseClick(toggleLog, Qt::LeftButton);
    require(log->isVisible(), "log toggle did not expand output");
    QTest::mouseClick(toggleLog, Qt::LeftButton);
    require(log->isHidden() && log->toPlainText() == output,
            "log folding changed output");
    require(local->findChild<QPushButton*>("dangerButton")->isHidden(),
            "finished job retained cancel action");
    auto* restoreLocal = navigate(1);
    field(restoreLocal, "archivePath")->setText(root + "/local.bak");
    field(restoreLocal, "destinationPath")->setText(root + "/local-restored");
    field(restoreLocal, "archivePassword")->setText("archive-pass");
    run(restoreLocal);

    auto* account = navigate(5);
    account->findChild<QComboBox*>("accountMode")->setCurrentIndex(1);
    account->findChild<QSpinBox*>("serverPort")->setValue(port);
    account->findChild<QLineEdit*>("serverUsername")->setText("gui_user");
    account->findChild<QLineEdit*>("serverPassword")->setText("gui-pass");
    field(account, "confirmPassword")->setText("gui-pass");
    run(account);
    require(account->findChild<QLabel*>("accountName")->text() == "gui_user" &&
                account->findChild<QPushButton*>("logoutAccount")->isVisible(),
            "registration did not show account information");
    QTest::mouseClick(account->findChild<QPushButton*>("logoutAccount"),
                      Qt::LeftButton);
    require(field(account, "serverPassword")->text().isEmpty() &&
                field(account, "confirmPassword")->text().isEmpty() &&
                account->findChild<QLabel*>("accountName")->text().isEmpty(),
            "logout retained credentials or account information");
    field(account, "serverPassword")->setText("wrong-password");
    modalError.clear();
    QTest::mouseClick(account->findChild<QPushButton*>("primaryButton"),
                      Qt::LeftButton);
    require(
        waitFor([&]() { return !account->property("jobRunning").toBool(); }) &&
            modalError.startsWith("任务失败") &&
            account->findChild<QLabel*>("accountName")->text().isEmpty(),
        "incorrect password did not show an error while logged out");
    field(account, "serverPassword")->setText("gui-pass");
    run(account);
    require(account->findChild<QLabel*>("accountName")->text() == "gui_user",
            "login did not restore account information");
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
    field(upload, "sourcePath")
        ->setText(QString::fromStdString(source.string()));
    field(upload, "backupName")->setText("课堂备份");
    run(upload);
    std::ofstream(source / "较大文件") << std::string(12000, 'x');
    field(upload, "backupName")->setText("较大备份");
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
    field(restoreRemote, "destinationPath")->setText(root + "/remote-restored");
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
    account = navigate(5);
    require(account->findChild<QLabel*>("accountName")->text().isEmpty(),
            "credential change retained authenticated UI");
    field(account, "serverUsername")->setText("gui_user");
    run(account);
    // Confirmation defaults to No; declining must leave the account intact.
    modalError.clear();
    QTest::mouseClick(account->findChild<QPushButton*>("deleteAccount"),
                      Qt::LeftButton);
    require(modalError.startsWith("注销账户") &&
                !account->property("jobRunning").toBool(),
            "declining deletion started a job");
    QTest::mouseClick(account->findChild<QPushButton*>("switchAccount"),
                      Qt::LeftButton);
    require(field(account, "serverUsername")->text().isEmpty() &&
                field(account, "serverPassword")->text().isEmpty(),
            "switching accounts retained credentials");
    account->findChild<QComboBox*>("accountMode")->setCurrentIndex(1);
    field(account, "serverUsername")->setText("temporary_gui_user");
    field(account, "serverPassword")->setText("temporary-pass");
    field(account, "confirmPassword")->setText("temporary-pass");
    run(account);
    dismiss.stop();
    QTimer::singleShot(10, window.get(),
                       []()
                       {
                           if (auto* dialog = qobject_cast<QMessageBox*>(
                                   QApplication::activeModalWidget()))
                           {
                               dialog->button(QMessageBox::Yes)->click();
                           }
                       });
    QTest::mouseClick(account->findChild<QPushButton*>("deleteAccount"),
                      Qt::LeftButton);
    require(account->property("jobRunning").toBool(),
            "confirming deletion did not start a job");
    dismiss.start(10);
    require(
        waitFor([&]() { return !account->property("jobRunning").toBool(); }) &&
            account->findChild<QProgressBar*>()->value() == 100 &&
            field(account, "serverUsername")->text().isEmpty(),
        ("confirmed empty-account deletion failed: " +
         account->findChild<QTextEdit*>("taskLog")->toPlainText() +
         "; account=" + field(account, "serverUsername")->text() +
         "; progress=" +
         QString::number(account->findChild<QProgressBar*>()->value()))
            .toStdString()
            .c_str());
    auto client = backup::network::BackupClient(
        "127.0.0.1", port, "temporary_gui_user", "temporary-pass");
    std::string error;
    require(!client.login(error),
            "GUI deletion did not remove the server account");
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
        if (app.arguments().contains("--dialog-placement"))
        {
            testDialogPlacement();
            return 0;
        }
        testAccountAlignment();
        testDialogPlacement();
        backup::gui::AccountSession session(&app);
        const auto revision = session.revision();
        session.reset();
        session.accept("stale-login", revision);
        require(session.username().isEmpty(),
                "stale login restored the account");
        testSelectionAlignment();
        testPresentation();
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
