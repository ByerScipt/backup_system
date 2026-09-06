#include "backup/core.hpp"
#include "backup/network.hpp"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTextEdit>
#include <QThread>
#include <QTimer>
#include <QTreeView>
#include <QUuid>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace backup;
#include "gui_common.hpp"
#include "main_window.hpp"

#include <QScopedPointer>

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
    const int captureWidth =
        qEnvironmentVariableIntValue("BACKUP_GUI_WIDTH", &widthProvided);
    const int captureHeight =
        qEnvironmentVariableIntValue("BACKUP_GUI_HEIGHT", &heightProvided);
    if (widthProvided && heightProvided) {
        window->resize(std::max(captureWidth, window->minimumWidth()),
                      std::max(captureHeight, window->minimumHeight()));
    }
    window->show();
    const QString dialogCapturePath =
        qEnvironmentVariable("BACKUP_GUI_DIALOG_CAPTURE");
    if (!dialogCapturePath.isEmpty()) {
        QTimer::singleShot(100, windowPtr, [&]() {
            selectDirectory(windowPtr, "Select Source Directory",
                            QDir::currentPath());
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
