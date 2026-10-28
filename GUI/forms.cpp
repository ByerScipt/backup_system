#include "ui.hpp"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QSize>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QTreeView>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <stdexcept>
#include <utility>

namespace backup::gui
{

void addAlgorithmRows(QFormLayout* form, QComboBox*& pack,
                      QComboBox*& compression, QComboBox*& encryption,
                      QLineEdit*& key)
{
    pack = new QComboBox;
    pack->setObjectName("packAlgorithm");
    pack->addItem("顺序归档", "stream");
    pack->addItem("索引归档", "index");
    pack->setToolTip("顺序归档适合流式处理；中央索引适合快速列出条目。");
    form->addRow("打包", pack);

    compression = new QComboBox;
    compression->setObjectName("compressionAlgorithm");
    compression->addItem("无", "none");
    compression->addItem("RLE", "rle");
    compression->addItem("Huffman", "huffman");
    compression->addItem("LZ77", "lz77");
    compression->setToolTip(
        "RLE 适合重复数据，Huffman 适合一般数据，LZ77 压缩率更高但更慢。");
    form->addRow("压缩", compression);

    encryption = new QComboBox;
    encryption->setObjectName("encryptionAlgorithm");
    encryption->addItem("无", "none");
    encryption->addItem("ChaCha20", "chacha20");
    encryption->addItem("AES-256 CTR", "aes256");
    encryption->setToolTip("ChaCha20（RFC 8439）与 AES-256 CTR "
                           "均为现代密码，密钥由口令经盐值迭代派生。");
    form->addRow("加密", encryption);

    key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setObjectName("archivePassword");
    key->setPlaceholderText("加密时填写");
    key->setEnabled(false);
    form->addRow("归档密码", key);

    QObject::connect(
        encryption, &QComboBox::currentIndexChanged, key, [encryption, key]()
        { key->setEnabled(encryption->currentData().toString() != "none"); });
}

AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key)
{
    AlgorithmValues values{
        parsePackAlgorithm(pack->currentData().toString().toStdString()),
        parseCompressionAlgorithm(
            compression->currentData().toString().toStdString()),
        parseEncryptionAlgorithm(
            encryption->currentData().toString().toStdString()),
        key->text().toStdString()};
    if (values.encryption != EncryptionAlgorithm::None &&
        values.password.empty())
    {
        throw std::runtime_error("启用加密后必须填写归档密码");
    }
    return values;
}

BackupOptions algorithmOptions(const AlgorithmValues& values,
                               std::atomic_bool* cancel,
                               ProgressCallback progress)
{
    BackupOptions options;
    options.pack = values.pack;
    options.compression = values.compression;
    options.encryption = values.encryption;
    options.password = values.password;
    options.cancel = cancel;
    options.progress = std::move(progress);
    return options;
}

ServerFields addServerRows(QVBoxLayout* layout, QLineEdit* confirmation)
{
    ServerFields fields;
    fields.host = new QLineEdit("127.0.0.1");
    fields.host->setObjectName("serverHost");
    fields.host->setClearButtonEnabled(true);

    fields.port = new QSpinBox;
    fields.port->setObjectName("serverPort");
    fields.port->setRange(1, 65535);
    fields.port->setValue(8848);
    fields.port->setButtonSymbols(QAbstractSpinBox::NoButtons);

    fields.username = new QLineEdit;
    fields.username->setObjectName("serverUsername");
    fields.username->setClearButtonEnabled(true);

    fields.password = new QLineEdit;
    fields.password->setObjectName("serverPassword");
    fields.password->setEchoMode(QLineEdit::Password);
    fields.password->setClearButtonEnabled(true);
    fields.password->setToolTip("用于登录服务器，与归档加密密码相互独立。");
    auto* left = makeForm();
    left->setFormAlignment(Qt::AlignTop);
    left->addRow("服务器", fields.host);
    left->addRow("用户名", fields.username);
    auto* right = makeForm();
    right->setFormAlignment(Qt::AlignTop);
    right->addRow("端口", fields.port);
    right->addRow("账号密码", fields.password);
    if (confirmation)
    {
        auto* label = new QLabel("确认密码");
        label->setBuddy(confirmation);
        fields.confirmationLabel = label;
        // Qt 6.4 removes a hidden form row even with retainSizeWhenHidden.
        // Visible hosts keep the row; the children retain its size in login.
        const auto host = [](QWidget* child)
        {
            auto* widget = new QWidget;
            auto* content = new QVBoxLayout(widget);
            content->setContentsMargins(0, 0, 0, 0);
            content->addWidget(child);
            return widget;
        };
        right->addRow(host(label), host(confirmation));
    }
    auto* columns = new QHBoxLayout;
    columns->setSpacing(32);
    columns->addLayout(left, 1);
    columns->addLayout(right, 1);
    layout->addLayout(columns);
    fields.tls = new QCheckBox("TLS");
    fields.tls->setObjectName("tlsEnabled");
    auto* security = new QHBoxLayout;
    security->addWidget(fields.tls);
    QPushButton* certificate;
    security->addLayout(pathRow(fields.caFile, certificate, "tlsCaFile"), 1);
    fields.caFile->setPlaceholderText("CA 证书（留空使用系统信任）");
    fields.caFile->setEnabled(false);
    certificate->setEnabled(false);
    QObject::connect(fields.tls, &QCheckBox::toggled, fields.caFile,
                     &QWidget::setEnabled);
    QObject::connect(fields.tls, &QCheckBox::toggled, certificate,
                     &QWidget::setEnabled);
    bindPathPicker(layout->parentWidget(), certificate, fields.caFile,
                   "选择 CA 证书", selectCertificate);
    layout->addLayout(security);
    return fields;
}

ServerValues snapshotServer(const ServerFields& fields)
{
    if (fields.host->text().trimmed().isEmpty() ||
        fields.username->text().trimmed().isEmpty() ||
        fields.password->text().isEmpty())
    {
        throw std::runtime_error("服务器、用户名和账号密码不能为空");
    }
    return {fields.host->text().trimmed().toStdString(),
            static_cast<uint16_t>(fields.port->value()),
            fields.username->text().trimmed().toStdString(),
            fields.password->text().toStdString(),
            {fields.tls->isChecked(),
             fields.caFile->text().trimmed().toStdString()}};
}

network::BackupClient makeClient(const ServerValues& values)
{
    return {values.host, values.port, values.username, values.password,
            values.tls};
}

namespace
{
// Presets live in the user's own settings store. Only host, port and account
// name are written; the password is never persisted.
QSettings presetSettings()
{
    return QSettings(QStringLiteral("BackupStudio"),
                     QStringLiteral("Backup Studio"));
}

void writeLoginPresets(const QVector<LoginPreset>& presets)
{
    QSettings settings = presetSettings();
    settings.remove(QStringLiteral("loginPresets"));
    settings.beginWriteArray(QStringLiteral("loginPresets"));
    for (int index = 0; index < presets.size(); ++index)
    {
        settings.setArrayIndex(index);
        settings.setValue(QStringLiteral("host"), presets.at(index).host);
        settings.setValue(QStringLiteral("port"), presets.at(index).port);
        settings.setValue(QStringLiteral("username"),
                          presets.at(index).username);
        settings.setValue(QStringLiteral("tls"), presets.at(index).tls);
        settings.setValue(QStringLiteral("caFile"), presets.at(index).caFile);
    }
    settings.endArray();
    settings.sync();
}
} // namespace

QVector<LoginPreset> loadLoginPresets()
{
    QSettings settings = presetSettings();
    QVector<LoginPreset> presets;
    const int size = settings.beginReadArray(QStringLiteral("loginPresets"));
    for (int index = 0; index < size; ++index)
    {
        settings.setArrayIndex(index);
        LoginPreset preset;
        preset.host = settings.value(QStringLiteral("host")).toString();
        preset.port = static_cast<uint16_t>(
            settings.value(QStringLiteral("port"), 8848).toUInt());
        preset.username = settings.value(QStringLiteral("username")).toString();
        preset.tls = settings.value(QStringLiteral("tls"), false).toBool();
        preset.caFile = settings.value(QStringLiteral("caFile")).toString();
        if (!preset.host.isEmpty() && !preset.username.isEmpty())
        {
            presets.push_back(preset);
        }
    }
    settings.endArray();
    return presets;
}

void saveLoginPreset(const LoginPreset& preset)
{
    if (preset.host.trimmed().isEmpty() || preset.username.trimmed().isEmpty())
    {
        return;
    }
    QVector<LoginPreset> presets = loadLoginPresets();
    LoginPreset entry{preset.host.trimmed(), preset.port,
                      preset.username.trimmed(), preset.tls, preset.caFile};
    bool replaced = false;
    for (auto& existing : presets)
    {
        if (existing.host == entry.host && existing.port == entry.port &&
            existing.username == entry.username)
        {
            existing = entry;
            replaced = true;
        }
    }
    if (!replaced)
    {
        presets.push_back(entry);
    }
    writeLoginPresets(presets);
}

void removeLoginPreset(const LoginPreset& preset)
{
    QVector<LoginPreset> presets = loadLoginPresets();
    for (int index = presets.size() - 1; index >= 0; --index)
    {
        if (presets.at(index).host == preset.host &&
            presets.at(index).port == preset.port &&
            presets.at(index).username == preset.username)
        {
            presets.remove(index);
        }
    }
    writeLoginPresets(presets);
}

namespace
{
void configureFileDialog(QFileDialog& dialog, QWidget* owner,
                         bool directoryMode)
{
    QScreen* targetScreen = owner ? owner->screen() : nullptr;
    if (!targetScreen)
    {
        targetScreen = QApplication::primaryScreen();
    }

    const QSize available = targetScreen
                                ? targetScreen->availableGeometry().size()
                                : QSize(1280, 800);
    const int maximumWidth = std::max(640, qRound(available.width() * 0.92));
    const int maximumHeight = std::max(480, qRound(available.height() * 0.86));
    const int targetWidth = std::min(maximumWidth, 860);
    const int targetHeight = std::min(maximumHeight, 560);

    dialog.setSizeGripEnabled(true);
    dialog.setMinimumSize(std::min(760, targetWidth),
                          std::min(500, targetHeight));
    dialog.resize(targetWidth, targetHeight);
    dialog.setLabelText(QFileDialog::LookIn, "位置");
    dialog.setLabelText(QFileDialog::Reject, "取消");
    dialog.setLabelText(QFileDialog::FileType, "类型");

    if (directoryMode)
    {
        dialog.setNameFilter("文件夹");
        dialog.setViewMode(QFileDialog::List);
        dialog.setLabelText(QFileDialog::FileName, "目录");
        dialog.setLabelText(QFileDialog::Accept, "选择");
    }
    else
    {
        dialog.setViewMode(QFileDialog::Detail);
        dialog.setLabelText(QFileDialog::FileName, "文件名");
        for (auto* view : dialog.findChildren<QTreeView*>())
        {
            QHeaderView* header = view->header();
            header->setStretchLastSection(false);
            header->setSectionResizeMode(0, QHeaderView::Stretch);
            for (int column = 1; column < header->count(); ++column)
            {
                header->setSectionResizeMode(column,
                                             QHeaderView::ResizeToContents);
            }
        }
    }

    const QString capturePath =
        qEnvironmentVariable("BACKUP_GUI_DIALOG_CAPTURE");
    if (!capturePath.isEmpty())
    {
        QTimer::singleShot(300, &dialog,
                           [&dialog, capturePath]()
                           {
                               dialog.grab().save(capturePath);
                               dialog.reject();
                           });
    }
}

} // namespace

namespace
{
QString choosePath(QWidget* owner, const QString& title, const QString& initial,
                   QFileDialog::FileMode mode, bool save = false,
                   const QString& filter = "备份归档 (*.bak)")
{
    QFileDialog dialog(owner);
    dialog.setOption(QFileDialog::DontUseNativeDialog, true);
    dialog.setWindowTitle(title);
    dialog.setFileMode(mode);
    const bool directory = mode == QFileDialog::Directory;
    dialog.setOption(QFileDialog::ShowDirsOnly, directory);
    if (save)
    {
        dialog.setAcceptMode(QFileDialog::AcceptSave);
        dialog.setDefaultSuffix("bak");
    }
    if (directory)
    {
        dialog.setDirectory(initial);
    }
    else
    {
        dialog.setNameFilter(filter);
        if (!initial.isEmpty())
        {
            dialog.selectFile(initial);
        }
        dialog.setLabelText(QFileDialog::Accept, save ? "保存" : "打开");
    }
    configureFileDialog(dialog, owner, directory);
    return dialog.exec() == QDialog::Accepted ? dialog.selectedFiles().value(0)
                                              : QString{};
}
} // namespace

QString selectDirectory(QWidget* owner, const QString& title,
                        const QString& initial)
{
    return choosePath(owner, title, initial, QFileDialog::Directory);
}
QString selectArchive(QWidget* owner, const QString& title,
                      const QString& initial)
{
    return choosePath(owner, title, initial, QFileDialog::ExistingFile);
}
QString selectArchiveOutput(QWidget* owner, const QString& title,
                            const QString& initial)
{
    return choosePath(owner, title, initial, QFileDialog::AnyFile, true);
}

QString selectCertificate(QWidget* owner, const QString& title,
                          const QString& initial)
{
    return choosePath(owner, title, initial, QFileDialog::ExistingFile, false,
                      "证书 (*.pem *.crt);;所有文件 (*)");
}

void bindPathPicker(QWidget* owner, QPushButton* button, QLineEdit* edit,
                    const QString& title,
                    QString (*picker)(QWidget*, const QString&, const QString&))
{
    QObject::connect(button, &QPushButton::clicked, owner,
                     [=]()
                     {
                         const QString selected =
                             picker(owner, title, edit->text());
                         if (!selected.isEmpty())
                         {
                             edit->setText(selected);
                         }
                     });
}
} // namespace backup::gui
