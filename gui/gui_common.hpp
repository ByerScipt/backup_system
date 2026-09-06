#pragma once
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
#include <QScopedPointer>

using namespace backup;

struct JobControls {
    QPushButton* start = nullptr;
    QPushButton* cancel = nullptr;
    QProgressBar* progress = nullptr;
    QTextEdit* log = nullptr;
};
struct Card {
    QFrame* frame = nullptr;
    QVBoxLayout* body = nullptr;
};
struct Page {
    QWidget* widget = nullptr;
    QVBoxLayout* layout = nullptr;
};
struct AlgorithmValues {
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
    std::string password;
};
struct ServerFields {
    QLineEdit* host = nullptr;
    QSpinBox* port = nullptr;
    QLineEdit* username = nullptr;
    QLineEdit* password = nullptr;
};
struct ServerValues {
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
};

using Job = std::function<QString(std::atomic_bool*, const ProgressCallback&)>;

class MessageBoxButtonIconFilter final : public QObject {
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

QString preferredChineseFontFamily();
QFont applicationFont();
QString stageName(const QString& stage);
QString formatBytes(uint64_t bytes);
void appendLog(QTextEdit* log, const QString& text);
Page makePage();
Card makeCard(const QString& title, const QString& description = {});
QFormLayout* makeForm();
QLabel* makeHint(const QString& text);
QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse, const QString& placeholder);
JobControls addJobControls(QVBoxLayout* layout, const QString& startText);
void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess = {});
void addAlgorithmRows(QFormLayout* form, QComboBox*& pack, QComboBox*& compression,
                      QComboBox*& encryption, QLineEdit*& key);
AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key);
BackupOptions algorithmOptions(const AlgorithmValues& values, std::atomic_bool* cancel,
                               ProgressCallback progress);
ServerFields addServerRows(QFormLayout* form);
ServerValues snapshotServer(const ServerFields& fields);
network::BackupClient makeClient(const ServerValues& values);
QString temporaryArchivePath();
void configureFileDialog(QFileDialog& dialog, QWidget* owner, bool directoryMode);
QString selectDirectory(QWidget* owner, const QString& title, const QString& initialPath = {});
QString selectArchive(QWidget* owner, const QString& title, const QString& initialPath = {});
QString selectArchiveOutput(QWidget* owner, const QString& title, const QString& initialPath = {});
QWidget* localBackupPage();
QWidget* localRestorePage();
QWidget* remoteBackupPage();
QWidget* remoteRestorePage();
QWidget* remoteListPage();
void addSecurityItem(QVBoxLayout* layout, const QString& number, const QString& text);
QWidget* userPage();
