#include "gui_common.hpp"

void addAlgorithmRows(QFormLayout* form, QComboBox*& pack,
                      QComboBox*& compression, QComboBox*& encryption,
                      QLineEdit*& key) {
    pack = new QComboBox;
    pack->addItem("顺序归档  ·  Stream", "stream");
    pack->addItem("中央索引  ·  Index", "index");
    pack->setToolTip("顺序归档适合流式处理；中央索引适合快速列出条目。");
    form->addRow("打包算法", pack);

    compression = new QComboBox;
    compression->addItem("不压缩  ·  None", "none");
    compression->addItem("游程编码  ·  RLE", "rle");
    compression->addItem("规范编码  ·  Huffman", "huffman");
    compression->setToolTip("RLE 适合重复数据，Huffman 适合一般数据。");
    form->addRow("压缩算法", compression);

    encryption = new QComboBox;
    encryption->addItem("不加密  ·  None", "none");
    encryption->addItem("流密码  ·  ChaCha20", "chacha20");
    encryption->addItem("分组密码  ·  AES-256 CTR", "aes256");
    encryption->setToolTip("ChaCha20（RFC 8439）与 AES-256 CTR 均为现代密码，密钥由口令经盐值迭代派生。");
    form->addRow("加密算法", encryption);

    key = new QLineEdit;
    key->setEchoMode(QLineEdit::Password);
    key->setClearButtonEnabled(true);
    key->setPlaceholderText("启用加密后输入归档密钥");
    key->setEnabled(false);
    form->addRow("归档密钥", key);

    QObject::connect(
        encryption, &QComboBox::currentIndexChanged, key,
        [encryption, key]() {
            key->setEnabled(encryption->currentData().toString() != "none");
        });
}


AlgorithmValues snapshotAlgorithms(QComboBox* pack, QComboBox* compression,
                                   QComboBox* encryption, QLineEdit* key) {
    AlgorithmValues values{
        parsePackAlgorithm(pack->currentData().toString().toStdString()),
        parseCompressionAlgorithm(
            compression->currentData().toString().toStdString()),
        parseEncryptionAlgorithm(
            encryption->currentData().toString().toStdString()),
        key->text().toStdString()};
    if (values.encryption != EncryptionAlgorithm::None &&
        values.password.empty()) {
        throw std::runtime_error("启用加密后必须填写归档密钥");
    }
    return values;
}

BackupOptions algorithmOptions(const AlgorithmValues& values,
                               std::atomic_bool* cancel,
                               ProgressCallback progress) {
    BackupOptions options;
    options.pack = values.pack;
    options.compression = values.compression;
    options.encryption = values.encryption;
    options.password = values.password;
    options.cancel = cancel;
    options.progress = std::move(progress);
    return options;
}



ServerFields addServerRows(QFormLayout* form) {
    ServerFields fields;
    fields.host = new QLineEdit("127.0.0.1");
    fields.host->setClearButtonEnabled(true);
    fields.host->setPlaceholderText("服务器地址");
    form->addRow("服务器", fields.host);

    fields.port = new QSpinBox;
    fields.port->setRange(1, 65535);
    fields.port->setValue(8848);
    fields.port->setButtonSymbols(QAbstractSpinBox::NoButtons);
    auto* portRow = new QHBoxLayout;
    portRow->setContentsMargins(0, 0, 0, 0);
    portRow->setSpacing(7);
    auto* decreasePort = new QPushButton("−");
    auto* increasePort = new QPushButton("+");
    for (auto* button : {decreasePort, increasePort}) {
        button->setObjectName("stepButton");
        button->setAutoRepeat(true);
        button->setAutoRepeatDelay(350);
        button->setAutoRepeatInterval(80);
    }
    decreasePort->setToolTip("端口减 1");
    increasePort->setToolTip("端口加 1");
    decreasePort->setAccessibleName("减少端口");
    increasePort->setAccessibleName("增加端口");
    QObject::connect(decreasePort, &QPushButton::clicked, fields.port,
                     &QSpinBox::stepDown);
    QObject::connect(increasePort, &QPushButton::clicked, fields.port,
                     &QSpinBox::stepUp);
    portRow->addWidget(fields.port, 1);
    portRow->addWidget(decreasePort);
    portRow->addWidget(increasePort);
    form->addRow("端口", portRow);

    fields.username = new QLineEdit;
    fields.username->setClearButtonEnabled(true);
    fields.username->setPlaceholderText("账号名称");
    form->addRow("用户名", fields.username);

    fields.password = new QLineEdit;
    fields.password->setEchoMode(QLineEdit::Password);
    fields.password->setClearButtonEnabled(true);
    fields.password->setPlaceholderText("账号密码");
    form->addRow("账号密码", fields.password);
    return fields;
}



ServerValues snapshotServer(const ServerFields& fields) {
    if (fields.host->text().trimmed().isEmpty() ||
        fields.username->text().trimmed().isEmpty() ||
        fields.password->text().isEmpty()) {
        throw std::runtime_error("服务器、用户名和账号密码不能为空");
    }
    return {fields.host->text().trimmed().toStdString(),
            static_cast<uint16_t>(fields.port->value()),
            fields.username->text().trimmed().toStdString(),
            fields.password->text().toStdString()};
}

network::BackupClient makeClient(const ServerValues& values) {
    return {values.host, values.port, values.username, values.password};
}

QString temporaryArchivePath() {
    return QDir::tempPath() + "/backup-gui-" +
           QUuid::createUuid().toString(QUuid::WithoutBraces) + ".bak";
}

