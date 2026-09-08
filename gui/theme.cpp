#include "ui.hpp"
#include <QAbstractButton>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QIcon>
#include <QMessageBox>
#include <QStringList>
#include <algorithm>

namespace backup::gui {

namespace {
QString preferredChineseFontFamily() {
    const QStringList installed = QFontDatabase::families(QFontDatabase::SimplifiedChinese);
    const QStringList preferred = {"Noto Sans CJK SC", "Source Han Sans SC", "Microsoft YaHei UI",
                                   "Microsoft YaHei",  "PingFang SC",        "Droid Sans Fallback"};
    for (const QString& candidate : preferred) {
        for (const QString& family : installed) {
            if (family.compare(candidate, Qt::CaseInsensitive) == 0) {
                return family;
            }
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

} // namespace

QFont applicationFont() {
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setFamilies({preferredChineseFontFamily()});
    const qreal systemPointSize = font.pointSizeF() > 0.0 ? font.pointSizeF() : 10.0;
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

QString applicationStyle(qreal scale) {
    QFile file(":/style.qss");
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString style = QString::fromUtf8(file.readAll());

    const auto points = [scale](qreal base) { return QString::number(base * scale, 'f', 1); };
    const auto pixels = [scale](int base) {
        return QString::number(qRound(static_cast<qreal>(base) * scale));
    };
    style.replace("@BASE_PT@", points(11.0));
    style.replace("@SMALL_PT@", points(9.5));
    style.replace("@TINY_PT@", points(8.5));
    style.replace("@BRAND_PT@", points(14.0));
    style.replace("@TITLE_PT@", points(20.0));
    style.replace("@CARD_PT@", points(12.0));
    style.replace("@STEP_PT@", points(14.0));
    style.replace("@NAV_H@", pixels(44));
    style.replace("@CONTROL_H@", pixels(40));
    style.replace("@QUIET_H@", pixels(32));
    style.replace("@CHECK_SIZE@", pixels(18));
    style.replace("@PROGRESS_H@", pixels(22));
    style.replace("@HEADER_H@", pixels(36));
    style.replace("@SCROLL_W@", pixels(10));
    style.replace("@STEP_W@", pixels(40));
    return style;
}

} // namespace backup::gui
