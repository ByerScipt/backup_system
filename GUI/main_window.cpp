#include "ui.hpp"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QEvent>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QVBoxLayout>
#include <algorithm>

namespace backup::gui
{
QMainWindow* createMainWindow()
{
    auto* window = new QMainWindow;
    window->setWindowTitle("Backup Studio");
    window->setMinimumSize(1024, 700);
    window->resize(1180, 780);
    window->setStyleSheet(applicationStyle());
    auto* root = new QWidget;
    root->setObjectName("appRoot");
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    auto* rail = new QFrame;
    rail->setObjectName("navigationRail");
    rail->setFixedWidth(190);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(16, 24, 16, 20);
    railLayout->setSpacing(6);
    auto* brand = new QLabel("Backup Studio");
    brand->setObjectName("brand");
    railLayout->addWidget(brand);
    railLayout->addWidget(makeHint("文件备份与还原"));
    railLayout->addSpacing(22);

    auto* pages = new QStackedWidget;
    pages->setObjectName("pageStack");
    for (auto* page :
         {localBackupPage(), localRestorePage(), remoteBackupPage(),
          remoteRestorePage(), remoteListPage(), userPage()})
    {
        pages->addWidget(page);
    }
    const QStringList titles = {"本地备份", "本地还原", "远程备份",
                                "远程还原", "备份历史", "账号注册"};
    auto* navigation = new QButtonGroup(window);
    navigation->setExclusive(true);
    for (int index = 0; index < titles.size(); ++index)
    {
        if (index == 0 || index == 2 || index == 5)
        {
            auto* section = new QLabel(index == 0   ? "本地工作区"
                                       : index == 2 ? "远程工作区"
                                                    : "账号");
            section->setObjectName("navSection");
            railLayout->addSpacing(10);
            railLayout->addWidget(section);
        }
        auto* button = new QPushButton(titles.at(index));
        button->setObjectName("navButton");
        button->setCheckable(true);
        navigation->addButton(button, index);
        railLayout->addWidget(button);
    }
    railLayout->addStretch();
    railLayout->addWidget(makeHint("v" BACKUP_SYSTEM_VERSION));

    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(24, 22, 24, 20);
    contentLayout->setSpacing(16);
    auto* title = new QLabel;
    title->setObjectName("pageTitle");
    contentLayout->addWidget(title);
    contentLayout->addWidget(pages, 1);
    QObject::connect(navigation, &QButtonGroup::idClicked, window,
                     [=](int index)
                     {
                         pages->setCurrentIndex(index);
                         title->setText(titles.at(index));
                     });
    auto* table = pages->widget(4)->findChild<QTableWidget*>();
    QObject::connect(
        pages->widget(4)->findChild<QPushButton*>("restoreSelected"),
        &QPushButton::clicked, window,
        [=]()
        {
            if (auto* item = table->item(table->currentRow(), 0))
            {
                pages->widget(3)
                    ->findChild<QLineEdit*>("backupId")
                    ->setText(item->data(Qt::UserRole).toString());
                navigation->button(3)->click();
            }
        });
    const auto invalidateHistory = [=]()
    {
        table->setRowCount(0);
        pages->widget(4)
            ->findChild<QLabel*>("historyStatus")
            ->setText("连接信息已变更，请刷新列表");
    };
    // Keep connection fields in sync in memory only; never persist passwords.
    for (const char* name : {"serverHost", "serverUsername", "serverPassword"})
    {
        const auto fields = pages->findChildren<QLineEdit*>(name);
        for (auto* field : fields)
        {
            QObject::connect(field, &QLineEdit::textChanged, window,
                             [=](const QString& text)
                             {
                                 for (auto* peer : fields)
                                 {
                                     if (peer->text() != text)
                                     {
                                         peer->setText(text);
                                     }
                                 }
                                 invalidateHistory();
                             });
        }
    }
    const auto ports = pages->findChildren<QSpinBox*>("serverPort");
    for (auto* port : ports)
    {
        QObject::connect(port, &QSpinBox::valueChanged, window,
                         [=](int value)
                         {
                             for (auto* peer : ports)
                             {
                                 if (peer->value() != value)
                                 {
                                     peer->setValue(value);
                                 }
                             }
                             invalidateHistory();
                         });
    }
    rootLayout->addWidget(rail);
    rootLayout->addWidget(content, 1);
    window->setCentralWidget(root);
    bool validPage = false;
    const int initial =
        qEnvironmentVariableIntValue("BACKUP_GUI_PAGE", &validPage);
    navigation
        ->button(validPage && initial >= 0 && initial < pages->count() ? initial
                                                                       : 0)
        ->click();
    return window;
}
namespace
{
QString preferredChineseFontFamily()
{
    const QStringList installed =
        QFontDatabase::families(QFontDatabase::SimplifiedChinese);
    const QStringList preferred = {"Noto Sans CJK SC",   "Source Han Sans SC",
                                   "Microsoft YaHei UI", "Microsoft YaHei",
                                   "PingFang SC",        "Droid Sans Fallback"};
    for (const QString& candidate : preferred)
    {
        for (const QString& family : installed)
        {
            if (family.compare(candidate, Qt::CaseInsensitive) == 0)
            {
                return family;
            }
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::GeneralFont).family();
}

} // namespace

QFont applicationFont()
{
    QFont font = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
    font.setFamilies({preferredChineseFontFamily()});
    const qreal systemPointSize =
        font.pointSizeF() > 0.0 ? font.pointSizeF() : 10.0;
    font.setPointSizeF(std::max<qreal>(10.0, systemPointSize));
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

bool MessageBoxButtonIconFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Show)
    {
        if (auto* messageBox = qobject_cast<QMessageBox*>(watched))
        {
            for (auto* button : messageBox->buttons())
            {
                button->setIcon(QIcon{});
            }
            const std::pair<QMessageBox::StandardButton, const char*> labels[] =
                {{QMessageBox::Ok, "确定"},
                 {QMessageBox::Cancel, "取消"},
                 {QMessageBox::Yes, "继续"},
                 {QMessageBox::No, "取消"},
                 {QMessageBox::Close, "关闭"}};
            for (const auto& label : labels)
            {
                if (auto* button = messageBox->button(label.first))
                {
                    button->setText(QString::fromUtf8(label.second));
                }
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

QString applicationStyle()
{
    QFile file(":/style.qss");
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

} // namespace backup::gui
