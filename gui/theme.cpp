#include "gui_common.hpp"

QString preferredChineseFontFamily() {
    const QStringList installed =
        QFontDatabase::families(QFontDatabase::SimplifiedChinese);
    const QStringList preferred = {
        "Noto Sans CJK SC", "Source Han Sans SC", "Microsoft YaHei UI",
        "Microsoft YaHei", "PingFang SC", "Droid Sans Fallback"};
    for (const QString& candidate : preferred) {
        for (const QString& family : installed) {
            if (family.compare(candidate, Qt::CaseInsensitive) == 0) {
                return family;
            }
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

QFont applicationFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setFamilies({preferredChineseFontFamily()});
    const qreal systemPointSize =
        font.pointSizeF() > 0.0 ? font.pointSizeF() : 10.0;
    font.setPointSizeF(std::max<qreal>(11.0, systemPointSize));
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}


bool MessageBoxButtonIconFilter::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::Show) {
        if (auto* messageBox = qobject_cast<QMessageBox*>(watched)) {
            for (auto* button : messageBox->buttons()) {
                button->setIcon(QIcon{});
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

QString stageName(const QString& stage) {
    if (stage == "scan") return "扫描目录";
    if (stage == "pack") return "写入归档";
    if (stage == "compress-rle") return "RLE 压缩";
    if (stage == "decompress-rle") return "RLE 解压";
    if (stage == "huffman-count") return "Huffman 统计";
    if (stage == "compress-huffman") return "Huffman 压缩";
    if (stage == "decompress-huffman") return "Huffman 解压";
    if (stage == "encrypt") return "加密载荷";
    if (stage == "decrypt") return "解密载荷";
    if (stage == "extract") return "提取文件";
    if (stage == "restore-entry") return "恢复元数据";
    if (stage == "upload") return "上传归档";
    if (stage == "download") return "下载归档";
    if (stage == "conflict-preview") return "冲突预检";
    return stage;
}

QString formatBytes(uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value >= 100.0 ? 0 : 1);
    return QString("%1 %2").arg(QString::number(value, 'f', precision), units[unit]);
}

void appendLog(QTextEdit* log, const QString& text) {
    log->append(QString("[%1]  %2")
                    .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

