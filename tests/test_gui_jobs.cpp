#include "ui.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QTemporaryDir>
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
} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
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
    for (int i = 0; i < 2; ++i)
    {
        auto* page = new QWidget(&window);
        layout->addWidget(page);
        auto controls =
            backup::gui::addJobControls(new QVBoxLayout(page), "Test");
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
    window.show();
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
    QTimer::singleShot(3500, &app, &QApplication::quit);
    app.exec();
    timer.stop();
    if (!success)
    {
        std::cerr << "window did not cancel and join all background jobs "
                     "before closing\n";
        return 1;
    }
    if (!testOverwriteRestore(app))
    {
        std::cerr
            << "GUI confirmed overwrite did not restore archive content\n";
        return 1;
    }
    std::cout << "GUI progress, overwrite and deferred-close tests passed.\n";
    return 0;
}
