#pragma once
#include "core.hpp"
#include "network.hpp"
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
class QMainWindow;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTextEdit;
class QVBoxLayout;
class QWidget;
namespace backup::gui
{
struct JobControls
{
    QPushButton* start = nullptr;
    QPushButton* cancel = nullptr;
    QProgressBar* progress = nullptr;
    QTextEdit* log = nullptr;
};
struct Card
{
    QFrame* frame = nullptr;
    QVBoxLayout* body = nullptr;
};
struct Page
{
    QWidget* widget = nullptr;
    QVBoxLayout* layout = nullptr;
    QVBoxLayout* footer = nullptr;
};
struct AlgorithmValues
{
    PackAlgorithm pack;
    CompressionAlgorithm compression;
    EncryptionAlgorithm encryption;
    std::string password;
};
struct ServerFields
{
    QLineEdit* host = nullptr;
    QSpinBox* port = nullptr;
    QLineEdit* username = nullptr;
    QLineEdit* password = nullptr;
    QWidget* confirmationLabel = nullptr;
};
struct ServerValues
{
    std::string host;
    uint16_t port;
    std::string username;
    std::string password;
};

using Job = std::function<QString(std::atomic_bool*, const ProgressCallback&)>;

// Credentials remain in memory in the shared form fields, never on disk.
// A revision prevents stale asynchronous logins from restoring logged-out UI.
class AccountSession final : public QObject
{
    Q_OBJECT
public:
    explicit AccountSession(QObject* parent) : QObject(parent) {}
    QString username() const
    {
        return username_;
    }
    uint64_t revision() const
    {
        return revision_;
    }
    void reset()
    {
        ++revision_;
        username_.clear();
        emit changed();
    }
    void accept(const QString& username, uint64_t revision)
    {
        if (revision == revision_)
        {
            username_ = username;
            emit changed();
        }
    }
signals:
    void changed();
    void signedOut();

private:
    QString username_;
    uint64_t revision_ = 0;
};

class MessageBoxButtonIconFilter final : public QObject
{
protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
};

QFont applicationFont();
QString applicationStyle();
QMainWindow* createMainWindow();
QString stageName(const QString& stage);
QString formatBytes(uint64_t bytes);
void appendLog(QTextEdit* log, const QString& text);
Page makePage();
Card makeCard();
QFormLayout* makeForm();
QLabel* makeHint(const QString& text);
QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse,
                     const QString& objectName);
JobControls addJobControls(QVBoxLayout* layout, const QString& startText);
void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess = {});
void addAlgorithmRows(QFormLayout* form, QComboBox*& pack,
                      QComboBox*& compression, QComboBox*& encryption,
                      QLineEdit*& key);
AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key);
BackupOptions algorithmOptions(const AlgorithmValues& values,
                               std::atomic_bool* cancel,
                               ProgressCallback progress);
ServerFields addServerRows(QVBoxLayout* layout,
                           QLineEdit* confirmation = nullptr);
ServerValues snapshotServer(const ServerFields& fields);
network::BackupClient makeClient(const ServerValues& values);
QString selectDirectory(QWidget* owner, const QString& title,
                        const QString& initialPath = {});
QString selectArchive(QWidget* owner, const QString& title,
                      const QString& initialPath = {});
QString selectArchiveOutput(QWidget* owner, const QString& title,
                            const QString& initialPath = {});
void bindPathPicker(QWidget* owner, QPushButton* button, QLineEdit* edit,
                    const QString& title,
                    QString (*picker)(QWidget*, const QString&,
                                      const QString&));
QWidget* localBackupPage();
QWidget* localRestorePage();
QWidget* remoteBackupPage();
QWidget* remoteRestorePage();
QWidget* remoteListPage();
QWidget* startPage(AccountSession* session);

} // namespace backup::gui
