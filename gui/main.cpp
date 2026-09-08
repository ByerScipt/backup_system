#include "main_window.hpp"
#include "ui.hpp"
#include <QApplication>
#include <QDir>
#include <QFont>
#include <QMainWindow>
#include <QScopedPointer>
#include <QString>
#include <QTimer>
#include <algorithm>

using namespace backup::gui;

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    app.setApplicationName("Backup Studio");
    app.setApplicationDisplayName("Backup Studio");
    app.setApplicationVersion(BACKUP_SYSTEM_VERSION);
    app.setFont(applicationFont());
    MessageBoxButtonIconFilter messageBoxButtonIconFilter;
    app.installEventFilter(&messageBoxButtonIconFilter);

    QScopedPointer<QMainWindow> window(createMainWindow());
    QMainWindow* windowPtr = window.data();
    bool widthProvided = false;
    bool heightProvided = false;
    const int captureWidth = qEnvironmentVariableIntValue("BACKUP_GUI_WIDTH", &widthProvided);
    const int captureHeight = qEnvironmentVariableIntValue("BACKUP_GUI_HEIGHT", &heightProvided);
    if (widthProvided && heightProvided) {
        window->resize(std::max(captureWidth, window->minimumWidth()),
                       std::max(captureHeight, window->minimumHeight()));
    }
    window->show();
    const QString dialogCapturePath = qEnvironmentVariable("BACKUP_GUI_DIALOG_CAPTURE");
    if (!dialogCapturePath.isEmpty()) {
        QTimer::singleShot(100, windowPtr, [&]() {
            selectDirectory(windowPtr, "Select Source Directory", QDir::currentPath());
            app.quit();
        });
        return app.exec();
    }
    const QString capturePath = qEnvironmentVariable("BACKUP_GUI_CAPTURE");
    if (!capturePath.isEmpty()) {
        QTimer::singleShot(300, &app, [&]() {
            window->grab().save(capturePath);
            app.quit();
        });
    }
    return app.exec();
}
