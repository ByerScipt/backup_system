#include "ui.hpp"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QScreen>
#include <QSize>
#include <QSpinBox>
#include <QString>
#include <QTimer>
#include <QTreeView>
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
    pack->addItem("顺序归档  ·  Stream", "stream");
    pack->addItem("中央索引  ·  Index", "index");
    pack->setToolTip("顺序归档适合流式处理；中央索引适合快速列出条目。");
    form->addRow("打包算法", pack);

    compression = new QComboBox;
    compression->addItem("不压缩  ·  None", "none");
    compression->addItem("游程编码  ·  RLE", "rle");
    compression->addItem("霍夫曼编码  ·  Huffman", "huffman");
    compression->setToolTip("RLE 适合重复数据，Huffman 适合一般数据。");
    form->addRow("压缩算法", compression);

    encryption = new QComboBox;
    encryption->addItem("不加密  ·  None", "none");
    encryption->addItem("流密码  ·  ChaCha20", "chacha20");
    encryption->addItem("分组密码  ·  AES-256 CTR", "aes256");
    encryption->setToolTip("ChaCha20（RFC 8439）与 AES-256 CTR "
                           "均为现代密码，密钥由口令经盐值迭代派生。");
    form->addRow("加密算法", encryption);

    key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("启用加密后输入归档密码");
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

ServerFields addServerRows(QFormLayout* form)
{
    ServerFields fields;
    fields.host = new QLineEdit("127.0.0.1");
    fields.host->setObjectName("serverHost");
    fields.host->setClearButtonEnabled(true);
    fields.host->setPlaceholderText("服务器地址");
    form->addRow("服务器", fields.host);

    fields.port = new QSpinBox;
    fields.port->setObjectName("serverPort");
    fields.port->setRange(1, 65535);
    fields.port->setValue(8848);
    form->addRow("端口", fields.port);

    fields.username = new QLineEdit;
    fields.username->setObjectName("serverUsername");
    fields.username->setClearButtonEnabled(true);
    fields.username->setPlaceholderText("账号名称");
    form->addRow("用户名", fields.username);

    fields.password = new QLineEdit;
    fields.password->setObjectName("serverPassword");
    fields.password->setEchoMode(QLineEdit::Password);
    fields.password->setClearButtonEnabled(true);
    fields.password->setPlaceholderText("账号密码");
    form->addRow("账号密码", fields.password);
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
            fields.password->text().toStdString()};
}

network::BackupClient makeClient(const ServerValues& values)
{
    return {values.host, values.port, values.username, values.password};
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
                   QFileDialog::FileMode mode, bool save = false)
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
        dialog.setNameFilter("备份归档 (*.bak)");
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
