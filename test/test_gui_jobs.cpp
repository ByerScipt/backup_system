#include "ui.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
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
#include <QTreeWidget>
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
                ("login/register switch moved the title, selector or fields; "
                 "title dy=" +
                 QString::number(bounds(title).y() - titleBefore.y()) +
                 ", selector dy=" +
                 QString::number(bounds(mode).y() - modeBefore.y()) +
                 ", password dy=" +
                 QString::number(registration.y() - passwordBefore.y()) +
                 ", password dx=" +
                 QString::number(registration.x() - passwordBefore.x()))
                    .toStdString()
                    .c_str());
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
    auto* session = window->findChild<backup::gui::AccountSession*>();
    session->accept(QStringLiteral("alignment-test"), session->revision());
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
                        navigation.first()->height(),
                    "navigation rows have unequal heights");
            const int page =
                navigation.at(index)->property("pageIndex").toInt();
            // Group headers separate pages 1/2; login is anchored at the
            // bottom.
            if (page == 1 || page == 3 || page == 4)
            {
                require(navigation.at(index)->y() -
                                navigation.at(index - 1)->geometry().bottom() -
                                1 ==
                            gap,
                        "navigation rows in a group have unequal gaps");
            }
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
        require(navigation.at(0)->isChecked(),
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

// The selector must archive only the checked entries, keep the structure and
// drop the selection when the source directory changes.
bool driveEntrySelector(QWidget* owner, const QString& source,
                        QStringList& selection,
                        const std::function<void(QDialog*)>& action,
                        QPushButton* opener = nullptr)
{
    QString failure;
    bool driven = false;
    QTimer driver, deadline;
    QObject::connect(&driver, &QTimer::timeout, owner,
                     [&]()
                     {
                         auto* dialog = qobject_cast<QDialog*>(
                             QApplication::activeModalWidget());
                         if (driven || !dialog)
                         {
                             return;
                         }
                         driven = true;
                         try
                         {
                             action(dialog);
                         }
                         catch (const std::exception& error)
                         {
                             failure = QString::fromUtf8(error.what());
                             dialog->reject();
                         }
                     });
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, owner,
                     [&]()
                     {
                         failure = "entry selector did not close";
                         if (auto* dialog = qobject_cast<QDialog*>(
                                 QApplication::activeModalWidget()))
                         {
                             dialog->reject();
                         }
                     });
    driver.start(10);
    deadline.start(1500);
    bool accepted = true;
    if (opener)
    {
        opener->click();
    }
    else
    {
        accepted = backup::gui::selectSourceEntries(owner, source, &selection);
    }
    require(driven && failure.isEmpty(), failure.isEmpty()
                                             ? "entry selector never appeared"
                                             : failure.toStdString().c_str());
    return accepted;
}

void testEntrySelection()
{
    QTemporaryDir directory;
    require(directory.isValid(), "cannot create selection workspace");
    const std::filesystem::path root(directory.path().toStdString());
    std::filesystem::create_directories(root / "source" / "docs");
    std::filesystem::create_directories(root / "source" / "images");
    std::ofstream(root / "source" / "docs" / "report.txt") << "selected";
    std::ofstream(root / "source" / "docs" / "draft.txt") << "skipped";
    std::ofstream(root / "source" / "docs" / "other.txt") << "other";
    std::ofstream(root / "source" / "images" / "photo.bin") << "image";
    std::ofstream(root / "source" / "images" / "photo.jpg") << "jpeg";
    std::ofstream big(root / "source" / "images" / "big.raw");
    big << std::string(2 * 1024 * 1024, 'x');
    big.close();
    std::ofstream(root / "source" / "docs" / "empty.cfg");
    std::ofstream(root / "source" / ".config") << "hidden";
    std::filesystem::create_directories(root / "source" / ".git");
    std::ofstream(root / "source" / ".git" / "HEAD") << "ref";
    // 100 days old: outside the 90-day band, inside the one-year band.
    const auto oldTime = std::filesystem::file_time_type::clock::now() -
                         std::chrono::hours(24 * 100);
    std::filesystem::last_write_time(root / "source" / "docs" / "other.txt",
                                     oldTime);

    std::unique_ptr<QWidget> page(backup::gui::localBackupPage());
    const QString source = QString::fromStdString((root / "source").string());
    field(page.get(), "sourcePath")->setText(source);
    field(page.get(), "outputPath")
        ->setText(QString::fromStdString((root / "selected.bak").string()));
    page->show();

    QStringList selection{"docs/report.txt"};
    require(
        !driveEntrySelector(
            page.get(), source, selection,
            [](QDialog* dialog)
            {
                dialog->findChild<QPushButton*>("selectAllEntries")->click();
                dialog->findChild<QDialogButtonBox*>()
                    ->button(QDialogButtonBox::Cancel)
                    ->click();
            }) &&
            selection == QStringList{"docs/report.txt"},
        "cancel changed the previous selection");
    require(driveEntrySelector(
                page.get(), source, selection,
                [](QDialog* dialog)
                {
                    auto* tree = dialog->findChild<QTreeWidget*>("entryTree");
                    tree->findItems("draft.txt",
                                    Qt::MatchExactly | Qt::MatchRecursive, 0)
                        .front()
                        ->setCheckState(0, Qt::Checked);
                    dialog->findChild<QDialogButtonBox*>()
                        ->button(QDialogButtonBox::Ok)
                        ->click();
                }) &&
                selection.size() == 2 &&
                selection.contains("docs/report.txt") &&
                selection.contains("docs/draft.txt"),
            "two nested selections became a whole-tree selection");
    require(
        driveEntrySelector(
            page.get(), source, selection,
            [](QDialog* dialog)
            {
                auto* box = dialog->findChild<QDialogButtonBox*>();
                dialog->findChild<QPushButton*>("clearEntries")->click();
                require(!box->button(QDialogButtonBox::Ok)->isEnabled(),
                        "clearing selection can silently back up all");
                dialog->findChild<QPushButton*>("selectAllEntries")->click();
                box->button(QDialogButtonBox::Ok)->click();
            }) &&
            selection.isEmpty(),
        "select all did not restore whole-tree selection");
    require(driveEntrySelector(
                page.get(), source, selection,
                [](QDialog* dialog)
                {
                    auto* tree = dialog->findChild<QTreeWidget*>("entryTree");
                    for (int index = 0; index < tree->topLevelItemCount();
                         ++index)
                    {
                        require(tree->topLevelItem(index)->checkState(0) ==
                                    Qt::Checked,
                                "whole-tree selection is not shown checked");
                    }
                    dialog->findChild<QDialogButtonBox*>()
                        ->button(QDialogButtonBox::Ok)
                        ->click();
                }) &&
                selection.isEmpty(),
            "reopening whole-tree selection changed its meaning");
    std::filesystem::create_directory(root / "empty-source");
    require(driveEntrySelector(
                page.get(),
                QString::fromStdString((root / "empty-source").string()),
                selection,
                [](QDialog* dialog)
                {
                    require(dialog->findChild<QTreeWidget*>("entryTree")
                                    ->topLevelItemCount() == 0,
                            "empty source contains unexpected entries");
                    dialog->findChild<QDialogButtonBox*>()
                        ->button(QDialogButtonBox::Ok)
                        ->click();
                }),
            "an empty directory cannot be selected for backup");

    require(
        driveEntrySelector(
            page.get(), source, selection,
            [](QDialog* dialog)
            {
                auto* tree = dialog->findChild<QTreeWidget*>("entryTree");
                auto* category =
                    dialog->findChild<QComboBox*>("entryCategoryFilter");
                auto* size = dialog->findChild<QComboBox*>("entrySizeFilter");
                auto* age = dialog->findChild<QComboBox*>("entryAgeFilter");
                auto* showHidden =
                    dialog->findChild<QCheckBox*>("entryShowHidden");
                auto* owner = dialog->findChild<QComboBox*>("entryOwnerFilter");
                auto* check =
                    dialog->findChild<QPushButton*>("checkFilteredEntries");
                auto* uncheck =
                    dialog->findChild<QPushButton*>("uncheckFilteredEntries");
                require(tree != nullptr && category != nullptr &&
                            size != nullptr && age != nullptr &&
                            showHidden != nullptr && owner != nullptr &&
                            check != nullptr && uncheck != nullptr,
                        "selection dialog lacks the new filter widgets");
                const auto find = [tree](const QString& name)
                {
                    const auto found = tree->findItems(
                        name, Qt::MatchExactly | Qt::MatchRecursive, 0);
                    return found.isEmpty() ? nullptr : found.first();
                };
                auto* photo = find("photo.jpg");
                auto* report = find("report.txt");
                auto* big = find("big.raw");
                auto* images = find("images");
                auto* empty = find("empty.cfg");
                auto* other = find("other.txt");
                auto* dotfile = find(".config");
                auto* dotdir = find(".git");
                require(photo != nullptr && report != nullptr &&
                            big != nullptr && images != nullptr &&
                            empty != nullptr && other != nullptr &&
                            dotfile != nullptr && dotdir != nullptr,
                        "selection tree lost fixture entries");
                require(tree->columnCount() == 5,
                        "entry tree has no category column");
                require(owner->count() == 2,
                        "owner filter lost the source UID");
                owner->setCurrentIndex(1);
                require(!report->isHidden(), "owner filter hid its own file");
                owner->addItem("missing UID", QVariant::fromValue(UINT32_MAX));
                owner->setCurrentIndex(2);
                require(report->isHidden(),
                        "owner filter kept another owner's file");
                owner->setCurrentIndex(0);
                require(photo->text(2) == "图片" && report->text(2) == "文档" &&
                            big->text(2) == "其他" && images->text(2).isEmpty(),
                        "category labels are wrong");
                dialog->findChild<QPushButton*>("clearEntries")->click();
                category->setCurrentIndex(category->findData("image"));
                require(!photo->isHidden() && report->isHidden() &&
                            big->isHidden() && !images->isHidden(),
                        "category filter hides the wrong entries");
                const QString evidence =
                    qEnvironmentVariable("BACKUP_GUI_EVIDENCE_DIR");
                if (!evidence.isEmpty())
                {
                    QDir().mkpath(evidence);
                    require(
                        dialog->grab().save(evidence + "/entry-selection.png"),
                        "entry selection screenshot failed");
                }
                category->setCurrentIndex(0);
                size->setCurrentIndex(3); // 1 MB – 100 MB
                require(!big->isHidden() && photo->isHidden() &&
                            report->isHidden(),
                        "size band 1-100MB keeps the wrong entries");
                size->setCurrentIndex(1); // 空文件
                require(!empty->isHidden() && report->isHidden(),
                        "empty-file band keeps the wrong entries");
                size->setCurrentIndex(0);
                age->setCurrentIndex(5); // 最近 90 天
                require(other->isHidden(),
                        "90-day band kept a 100-day-old file");
                age->setCurrentIndex(6); // 最近一年
                require(!other->isHidden(),
                        "one-year band hid a 100-day-old file");
                age->setCurrentIndex(0);
                showHidden->setChecked(false);
                require(dotfile->isHidden() && dotdir->isHidden() &&
                            !report->isHidden(),
                        "hidden toggle did not hide dot entries");
                showHidden->setChecked(true);
                require(!dotfile->isHidden() && !dotdir->isHidden(),
                        "hidden toggle did not restore dot entries");
                category->setCurrentIndex(category->findData("image"));
                check->click();
                require(photo->checkState(0) == Qt::Checked &&
                            images->checkState(0) == Qt::PartiallyChecked,
                        "checking filtered results forced a directory");
                uncheck->click();
                require(photo->checkState(0) == Qt::Unchecked &&
                            images->checkState(0) == Qt::Unchecked,
                        "unchecking filtered results kept the states");
                check->click();
                dialog->findChild<QDialogButtonBox*>()
                    ->button(QDialogButtonBox::Ok)
                    ->click();
            }) &&
            selection == QStringList{"images/photo.jpg"},
        "filtered selection took in entries hidden by the filter");

    const auto hiddenSource = root / "hidden-source";
    std::filesystem::create_directories(hiddenSource / ".hidden" / "nested");
    std::ofstream(hiddenSource / ".hidden/nested/secret.txt") << "hidden";
    std::ofstream(hiddenSource / "visible.txt") << "visible";
    QStringList hiddenSelection;
    require(
        driveEntrySelector(
            page.get(), QString::fromStdString(hiddenSource.string()),
            hiddenSelection,
            [](QDialog* dialog)
            {
                auto* tree = dialog->findChild<QTreeWidget*>("entryTree");
                auto* secret =
                    tree->findItems("secret.txt",
                                    Qt::MatchExactly | Qt::MatchRecursive, 0)
                        .front();
                dialog->findChild<QPushButton*>("clearEntries")->click();
                dialog->findChild<QCheckBox*>("entryShowHidden")
                    ->setChecked(false);
                dialog->findChild<QPushButton*>("checkFilteredEntries")
                    ->click();
                require(secret->checkState(0) == Qt::Unchecked,
                        "filtered check selected a hidden descendant");
                secret->setCheckState(0, Qt::Checked);
                dialog->findChild<QPushButton*>("uncheckFilteredEntries")
                    ->click();
                require(secret->checkState(0) == Qt::Checked,
                        "filtered uncheck cleared a hidden descendant");
                dialog->findChild<QPushButton*>("clearEntries")->click();
                dialog->findChild<QPushButton*>("checkFilteredEntries")
                    ->click();
                dialog->findChild<QDialogButtonBox*>()
                    ->button(QDialogButtonBox::Ok)
                    ->click();
            }) &&
            hiddenSelection == QStringList{"visible.txt"},
        "hidden subtree changed a filtered backup into a whole-tree backup");

    QString failure;
    bool driven = false;
    QTimer driver;
    QObject::connect(
        &driver, &QTimer::timeout, page.get(),
        [&]()
        {
            if (driven)
            {
                return;
            }
            auto* dialog =
                qobject_cast<QDialog*>(QApplication::activeModalWidget());
            if (dialog == nullptr || dialog->objectName() != "entrySelection")
            {
                return;
            }
            auto* tree = dialog->findChild<QTreeWidget*>("entryTree");
            auto* filter = dialog->findChild<QLineEdit*>("entryNameFilter");
            auto* box = dialog->findChild<QDialogButtonBox*>();
            if (tree == nullptr || filter == nullptr || box == nullptr)
            {
                failure = "selection dialog is incomplete";
                dialog->reject();
                return;
            }
            const auto find = [tree](const QString& name)
            {
                const auto found = tree->findItems(
                    name, Qt::MatchExactly | Qt::MatchRecursive, 0);
                return found.isEmpty() ? nullptr : found.first();
            };
            auto* report = find("report.txt");
            auto* draft = find("draft.txt");
            auto* images = find("images");
            if (report == nullptr || draft == nullptr || images == nullptr)
            {
                failure = "selection tree lost source entries";
                dialog->reject();
                return;
            }
            dialog->findChild<QPushButton*>("clearEntries")->click();
            filter->setText("report");
            if (!draft->isHidden())
            {
                failure = "name filter did not hide a non-matching file";
            }
            filter->clear();
            if (draft->isHidden())
            {
                failure = "cleared filter kept hiding source entries";
            }
            // Checking a file must leave unrelated branches alone.
            report->setCheckState(0, Qt::Checked);
            if (images->checkState(0) != Qt::Unchecked ||
                draft->checkState(0) != Qt::Unchecked)
            {
                failure = "checking one file changed another branch";
            }
            driven = true;
            box->button(QDialogButtonBox::Ok)->click();
        });
    driver.start(10);
    page->findChild<QPushButton*>("selectEntries")->click();
    driver.stop();
    require(driven && failure.isEmpty(), failure.isEmpty()
                                             ? "selection dialog never appeared"
                                             : failure.toStdString().c_str());
    require(page->findChild<QLabel*>("selectionSummary")
                ->text()
                .contains("已选 1 项"),
            "selection summary did not report the checked entry");

    QString jobError;
    QTimer guard;
    QObject::connect(&guard, &QTimer::timeout, page.get(),
                     [&]()
                     {
                         if (auto* box = qobject_cast<QMessageBox*>(
                                 QApplication::activeModalWidget()))
                         {
                             jobError = box->text();
                             box->reject();
                         }
                     });
    guard.start(10);
    page->findChild<QPushButton*>("primaryButton")->click();
    // Editing the next job's source must not mutate the running job's inputs.
    field(page.get(), "sourcePath")->setText(source + "/docs");
    require(waitFor([&]() { return !page->property("jobRunning").toBool(); }),
            "selected backup did not finish");
    guard.stop();
    require(jobError.isEmpty(),
            ("selected backup reported: " + jobError).toStdString().c_str());

    const auto archive = (root / "selected.bak").string();
    const auto destination = (root / "restored").string();
    require(std::filesystem::exists(archive),
            "selected backup produced no archive");
    require(backup::BackupEngine::restore(archive, destination, {}).success,
            "selected archive did not restore");
    require(std::filesystem::exists(root / "restored" / "source" / "docs" /
                                    "report.txt"),
            "the selected file is missing from the archive");
    require(!std::filesystem::exists(root / "restored" / "source" / "docs" /
                                     "draft.txt"),
            "an unselected file was archived");
    require(!std::filesystem::exists(root / "restored" / "source" / "images"),
            "an unselected directory was archived");

    // A new source directory must not inherit the previous selection.
    require(page->findChild<QLabel*>("selectionSummary")->text() == "全部内容",
            "changing the source kept a stale selection");
}

void testAutomaticGui()
{
    QTemporaryDir directory;
    const std::filesystem::path root(directory.path().toStdString());
    std::filesystem::create_directory(root / "source");
    std::ofstream(root / "source/file") << "automatic GUI";
    for (int mode : {1, 2})
    {
        std::unique_ptr<QWidget> page(backup::gui::localBackupPage());
        page->findChild<QComboBox*>("automaticMode")->setCurrentIndex(mode);
        field(page.get(), "sourcePath")
            ->setText(QString::fromStdString((root / "source").string()));
        const auto output = root / std::to_string(mode);
        field(page.get(), "outputPath")
            ->setText(QString::fromStdString(output.string()));
        page->findChild<QPushButton*>("primaryButton")->click();
        std::filesystem::path archive;
        require(waitFor(
                    [&]()
                    {
                        if (std::filesystem::exists(output))
                        {
                            for (const auto& entry :
                                 std::filesystem::directory_iterator(output))
                            {
                                if (entry.path().extension() == ".bak")
                                {
                                    archive = entry.path();
                                    return true;
                                }
                            }
                        }
                        return false;
                    }),
                "automatic GUI did not publish its first snapshot");
        page->findChild<QPushButton*>("dangerButton")->click();
        require(
            waitFor([&]() { return !page->property("jobRunning").toBool(); }),
            "automatic GUI could not stop");
        require(backup::BackupEngine::restore(
                    archive.string(),
                    (root / ("restored-" + std::to_string(mode))).string(), {})
                    .success,
                "automatic GUI produced an unreadable snapshot");
    }
}

void testStaleRemoteResults(const backup::gui::ServerValues& credentials)
{
    // Switch sessions after the response is queued but before the UI applies
    // it. This also covers completion racing with logout and another login.
    class SessionSwitch final : public QObject
    {
    public:
        std::function<void()> change;
        bool switched = false;
        bool eventFilter(QObject* watched, QEvent* event) override
        {
            if (!switched && event->type() == QEvent::MetaCall &&
                watched->property("jobRunning").toBool())
            {
                switched = true;
                change();
            }
            return false;
        }
    };
    for (bool restore : {false, true})
    {
        for (bool relogin : {false, true})
        {
            backup::gui::AccountSession session(nullptr);
            session.accept(credentials, session.revision());
            std::unique_ptr<QWidget> page(
                restore ? backup::gui::remoteRestorePage(&session)
                        : backup::gui::remoteListPage(&session));
            SessionSwitch switcher;
            switcher.change = [&]()
            {
                auto next = credentials;
                if (relogin)
                {
                    session.reset();
                }
                else
                {
                    next.port = next.port == 65535 ? 1 : next.port + 1;
                }
                session.accept(next, session.revision());
            };
            page->installEventFilter(&switcher);
            page->findChild<QPushButton*>(restore ? "refreshRemoteRestore"
                                                  : "primaryButton")
                ->click();
            require(waitFor([&]()
                            { return !page->property("jobRunning").toBool(); }),
                    "stale-result request did not finish");
            require(
                switcher.switched &&
                    page->findChild<QProgressBar*>()->value() == 100,
                "stale-result test did not intercept a successful response");
            const int count =
                restore ? page->findChild<QComboBox*>("backupId")->count()
                        : page->findChild<QTableWidget*>()->rowCount();
            require(count == 0, "old session repopulated a remote listing");
        }
    }
}

void testUserWorkflow(bool tls = false)
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
    QStringList serverArguments{"--port", QString::number(port), "--storage",
                                root + "/server"};
    backup::network::TlsOptions transport;
    if (tls)
    {
        QProcess certificate;
        certificate.start(BACKUP_OPENSSL_TOOL,
                          {"req", "-x509", "-newkey", "rsa:2048", "-nodes",
                           "-days", "1", "-subj", "/CN=localhost", "-addext",
                           "subjectAltName=DNS:localhost,IP:127.0.0.1",
                           "-keyout", root + "/key.pem", "-out",
                           root + "/cert.pem"});
        require(certificate.waitForFinished(5000) &&
                    certificate.exitCode() == 0,
                "cannot generate GUI TLS test certificate");
        serverArguments << "--tls-cert" << root + "/cert.pem" << "--tls-key"
                        << root + "/key.pem";
        transport = {true, (root + "/cert.pem").toStdString()};
    }
    QProcess server;
    server.start(QCoreApplication::applicationDirPath() + "/backup-server",
                 serverArguments);
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
    const auto navButton = [&](int index) -> QPushButton*
    {
        for (auto* button : window->findChildren<QPushButton*>("navButton"))
        {
            if (button->property("pageIndex").toInt() == index)
            {
                return button;
            }
        }
        return nullptr;
    };
    for (int index : {2, 3, 4})
    {
        require(navButton(index) != nullptr && !navButton(index)->isEnabled(),
                "remote navigation unlocked before login");
    }
    // Group headers navigate: 本地 always, 远程 only after a login.
    const auto groupButton = [&](const QString& text)
    {
        for (auto* button : window->findChildren<QPushButton*>("navGroup"))
        {
            if (button->text() == text)
            {
                return button;
            }
        }
        return static_cast<QPushButton*>(nullptr);
    };
    auto* localHeader = groupButton("本地（无需账户）");
    auto* remoteHeader = groupButton("远程（需要登录）");
    require(localHeader != nullptr && remoteHeader != nullptr,
            "navigation group headers are missing");
    QTest::mouseClick(localHeader, Qt::LeftButton);
    require(pages->currentIndex() == 0,
            "local group header did not open local backup");
    QTest::mouseClick(remoteHeader, Qt::LeftButton);
    require(pages->currentIndex() == 5,
            "signed-out remote header did not open the login page");
    const auto navigate = [&](int index)
    {
        const QStringList names = {"本地备份", "本地还原", "远程备份",
                                   "远程还原", "备份历史", "登录"};
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
    local->findChild<QComboBox*>("compressionAlgorithm")->setCurrentIndex(2);
    local->findChild<QComboBox*>("encryptionAlgorithm")->setCurrentIndex(1);
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
    account->findChild<QCheckBox*>("tlsEnabled")->setChecked(tls);
    field(account, "tlsCaFile")
        ->setText(QString::fromStdString(transport.caFile));
    require(account->findChild<QComboBox*>("accountMode")->count() == 2,
            "settings page lost its login modes");
    require(account->findChild<QCheckBox*>("rememberLogin") != nullptr &&
                account->findChild<QComboBox*>("loginPreset") != nullptr &&
                account->findChild<QPushButton*>("deletePreset") != nullptr,
            "settings page has no login presets");
    account->findChild<QComboBox*>("accountMode")->setCurrentIndex(1);
    account->findChild<QSpinBox*>("serverPort")->setValue(port);
    account->findChild<QLineEdit*>("serverUsername")->setText("gui_user");
    account->findChild<QLineEdit*>("serverPassword")->setText("gui-pass");
    field(account, "confirmPassword")->setText("gui-pass");
    account->findChild<QCheckBox*>("rememberLogin")->setChecked(true);
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
        require(pages->widget(index)
                    ->findChild<QLabel*>("remoteAccount")
                    ->text()
                    .contains("gui_user"),
                "remote page did not adopt the signed-in account");
    }
    QTest::mouseClick(remoteHeader, Qt::LeftButton);
    require(pages->currentIndex() == 2,
            "signed-in remote header did not open remote backup");
    const auto savedPresets = account->findChild<QComboBox*>("loginPreset");
    require(savedPresets->count() >= 1, "login preset was not remembered");
    const QString presetText = QString("gui_user @ 127.0.0.1:%1").arg(port);
    require(savedPresets->findText(presetText) >= 0,
            "saved preset is missing its account");
    // Selecting a preset refills the draft; deleting removes it from disk.
    account->findChild<QLineEdit*>("serverUsername")->clear();
    account->findChild<QSpinBox*>("serverPort")->setValue(1);
    savedPresets->setCurrentIndex(savedPresets->findText(presetText));
    require(field(account, "serverUsername")->text() == "gui_user" &&
                account->findChild<QSpinBox*>("serverPort")->value() == port,
            "selecting a preset did not fill the login draft");
    QTest::mouseClick(account->findChild<QPushButton*>("deletePreset"),
                      Qt::LeftButton);
    require(savedPresets->findText(presetText) < 0,
            "deleting a preset kept it in the list");
    auto* upload = navigate(2);
    field(upload, "sourcePath")
        ->setText(QString::fromStdString(source.string()));
    field(upload, "backupName")->setText("课堂备份");
    std::ofstream(source / "不选.txt") << "not selected";
    QStringList selected;
    driveEntrySelector(
        upload, QString::fromStdString(source.string()), selected,
        [](QDialog* dialog)
        {
            dialog->findChild<QPushButton*>("clearEntries")->click();
            dialog->findChild<QTreeWidget*>("entryTree")
                ->findItems("说明 文本.txt", Qt::MatchExactly, 0)
                .front()
                ->setCheckState(0, Qt::Checked);
            dialog->findChild<QDialogButtonBox*>()
                ->button(QDialogButtonBox::Ok)
                ->click();
        },
        upload->findChild<QPushButton*>("selectEntries"));
    run(upload);
    std::ofstream(source / "较大文件") << std::string(12000, 'x');
    // A new source spelling resets the next upload to the whole tree.
    field(upload, "sourcePath")
        ->setText(QString::fromStdString(source.string()) + "/.");
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
    require(restoreRemote->findChild<QComboBox*>("backupId")->currentText() ==
                id,
            "selected ID was not filled");
    auto* refresh =
        restoreRemote->findChild<QPushButton*>("refreshRemoteRestore");
    auto* restoreIds = restoreRemote->findChild<QComboBox*>("backupId");
    require(refresh != nullptr && restoreIds != nullptr,
            "restore page has no history refresh");
    QTest::mouseClick(refresh, Qt::LeftButton);
    require(waitFor([&]() { return restoreIds->count() == 2; }),
            "history refresh did not list the account backups");
    bool listed = false;
    for (int index = 0; index < restoreIds->count(); ++index)
    {
        listed = listed || restoreIds->itemData(index).toString() == id;
    }
    require(listed, "history refresh lost a backup ID");
    require(restoreIds->currentData().toString() == id,
            "history refresh dropped the chosen ID");
    field(restoreRemote, "destinationPath")->setText(root + "/remote-restored");
    run(restoreRemote);
    require(!std::filesystem::exists(std::filesystem::path(
                (root + "/remote-restored/课堂 资料/不选.txt").toStdString())),
            "remote selection archived an excluded file");
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
    // Signing out locks the remote views and clears their cached listing.
    account = navigate(5);
    QTest::mouseClick(account->findChild<QPushButton*>("logoutAccount"),
                      Qt::LeftButton);
    require(table->rowCount() == 0, "sign-out retained old history");
    require(!history->findChild<QPushButton*>("restoreSelected")->isEnabled(),
            "cleared history retained restore action");
    require(account->findChild<QLabel*>("accountName")->text().isEmpty(),
            "sign-out retained authenticated UI");
    for (int index : {2, 3, 4})
    {
        require(!navButton(index)->isEnabled(),
                "remote navigation stayed unlocked after sign-out");
    }
    field(account, "serverUsername")->setText("gui_user");
    field(account, "serverPassword")->setText("gui-pass");
    run(account);
    // Confirmation defaults to No; declining must leave the account intact.
    field(account, "serverHost")->clear();
    field(account, "serverPassword")->clear();
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
    field(account, "serverHost")->setText("127.0.0.1");
    field(account, "serverPassword")->setText("temporary-pass");
    field(account, "confirmPassword")->setText("temporary-pass");
    run(account);
    auto draft = backup::network::BackupClient(
        "127.0.0.1", port, "draft_gui_user", "draft-pass", transport);
    std::string error;
    require(draft.registerUser(error), "draft account fixture failed");
    field(account, "serverUsername")->setText("draft_gui_user");
    field(account, "serverPassword")->setText("draft-pass");
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
        "127.0.0.1", port, "temporary_gui_user", "temporary-pass", transport);
    require(!client.login(error),
            "GUI deletion did not remove the server account");
    require(draft.login(error), "GUI deletion removed the draft account");
    testStaleRemoteResults(
        {"127.0.0.1", port, "gui_user", "gui-pass", transport});
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
          "compress-copy", "decompress-copy", "compress-lz77",
          "decompress-lz77", "read-archive", "finalize"})
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
        // Runs the entry-selection workflow alone: the account-alignment
        // defect (DEF-01) aborts the shared sequence before this test.
        if (app.arguments().contains("--entry-selection"))
        {
            testEntrySelection();
            return 0;
        }
        // Runs the end-to-end workflow alone for the same reason.
        if (app.arguments().contains("--user-workflow"))
        {
            testUserWorkflow();
            return 0;
        }
        if (app.arguments().contains("--tls-workflow"))
        {
            testUserWorkflow(true);
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
        testEntrySelection();
        testAutomaticGui();
        testUserWorkflow();
        testUserWorkflow(true);
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
