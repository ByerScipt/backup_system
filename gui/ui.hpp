#pragma once
#include "backup/core.hpp"
#include "backup/network.hpp"
#include <QObject>
#include <QString>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
class QComboBox;
class QFont;
class QFormLayout;
class QFrame;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QVBoxLayout;
class QWidget;
namespace backup::gui {
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

QFont applicationFont();
QString applicationStyle(qreal scale);
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
AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression, QComboBox* encryption,
                                   QLineEdit* key);
BackupOptions algorithmOptions(const AlgorithmValues& values, std::atomic_bool* cancel,
                               ProgressCallback progress);
ServerFields addServerRows(QFormLayout* form);
ServerValues snapshotServer(const ServerFields& fields);
network::BackupClient makeClient(const ServerValues& values);
QString temporaryArchivePath();
QString selectDirectory(QWidget* owner, const QString& title, const QString& initialPath = {});
QString selectArchive(QWidget* owner, const QString& title, const QString& initialPath = {});
QString selectArchiveOutput(QWidget* owner, const QString& title, const QString& initialPath = {});
QWidget* localBackupPage();
QWidget* localRestorePage();
QWidget* remoteBackupPage();
QWidget* remoteRestorePage();
QWidget* remoteListPage();
QWidget* userPage();

} // namespace backup::gui
