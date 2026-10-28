#pragma once
#include "core.hpp"
#include "network.hpp"
#include <QObject>
#include <QString>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
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
    uint16_t port = 8848;
    std::string username;
    std::string password;
};

// Saved login preset. Deliberately holds no password: only the non-secret
// connection parameters a user may want to reuse are persisted.
struct LoginPreset
{
    QString host;
    uint16_t port = 8848;
    QString username;
};

using Job = std::function<QString(std::atomic_bool*, const ProgressCallback&)>;

// Credentials remain in memory in the session, never on disk; the optional
// login presets store only host, port and account name.
// A revision prevents stale asynchronous logins from restoring logged-out UI.
class AccountSession final : public QObject
{
    Q_OBJECT
public:
    explicit AccountSession(QObject* parent) : QObject(parent) {}
    QString username() const
    {
        return QString::fromStdString(credentials_.username);
    }
    ServerValues credentials() const
    {
        return credentials_;
    }
    bool signedIn() const
    {
        return !credentials_.username.empty();
    }
    uint64_t revision() const
    {
        return revision_;
    }
    void reset()
    {
        ++revision_;
        credentials_ = {};
        emit changed();
    }
    void accept(const ServerValues& values, uint64_t revision)
    {
        if (revision == revision_)
        {
            credentials_ = values;
            emit changed();
        }
    }
    void accept(const QString& username, uint64_t revision)
    {
        if (revision == revision_)
        {
            credentials_.username = username.toStdString();
            emit changed();
        }
    }
signals:
    void changed();
    void signedOut();

private:
    ServerValues credentials_;
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
QHBoxLayout* selectionRow(QPushButton*& button, QLabel*& summary);
std::vector<std::string> toUtf8Strings(const QStringList& values);
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
// Login presets live in the user's settings directory and never hold a
// password; saving twice with the same host/port/account replaces the entry.
QVector<LoginPreset> loadLoginPresets();
void saveLoginPreset(const LoginPreset& preset);
void removeLoginPreset(const LoginPreset& preset);
network::BackupClient makeClient(const ServerValues& values);
QString selectDirectory(QWidget* owner, const QString& title,
                        const QString& initialPath = {});
QString selectArchive(QWidget* owner, const QString& title,
                      const QString& initialPath = {});
QString selectArchiveOutput(QWidget* owner, const QString& title,
                            const QString& initialPath = {});
// Lets the user pick the entries below a source directory. Returns false when
// the dialog was cancelled; an empty selection means "the whole directory".
bool selectSourceEntries(QWidget* owner, const QString& sourceDirectory,
                         QStringList* selection);
void bindPathPicker(QWidget* owner, QPushButton* button, QLineEdit* edit,
                    const QString& title,
                    QString (*picker)(QWidget*, const QString&,
                                      const QString&));
QWidget* localBackupPage();
QWidget* localRestorePage();
// Remote pages take the signed-in credentials from the session; they never
// ask for a server or account themselves.
QWidget* remoteBackupPage(AccountSession* session);
QWidget* remoteRestorePage(AccountSession* session);
QWidget* remoteListPage(AccountSession* session);
QWidget* startPage(AccountSession* session);

} // namespace backup::gui
