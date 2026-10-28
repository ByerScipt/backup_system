#include "ui.hpp"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
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
#include <QResizeEvent>
#include <QScreen>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStringList>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <array>

namespace backup::gui
{
namespace
{
class MainWindow final : public QMainWindow
{
public:
    MainWindow() : baseFont_(applicationFont()), baseStyle_(applicationStyle())
    {
        updateAppearance();
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QMainWindow::resizeEvent(event);
        updateAppearance();
    }

private:
    void updateAppearance()
    {
        int tier = 0;
        for (const QSize threshold :
             {QSize(1500, 1000), QSize(1800, 1200), QSize(2100, 1400)})
        {
            if (width() >= threshold.width() && height() >= threshold.height())
            {
                ++tier;
            }
        }
        if (tier != fontTier_)
        {
            fontTier_ = tier;
            QFont font = baseFont_;
            font.setPointSizeF(baseFont_.pointSizeF() + 2 * tier);
            setFont(font);
            // Styled descendants need an explicit size when the tier changes.
            setStyleSheet(baseStyle_ +
                          QString("\nQMainWindow QWidget { font-size: %1pt; }"
                                  "\nQLabel#startTitle { font-size: %2pt; "
                                  "font-weight: 600; }")
                              .arg(font.pointSizeF())
                              .arg(font.pointSizeF() + 8));
        }
        if (auto* rail = findChild<QFrame*>("navigationRail"))
        {
            int railWidth = std::clamp(width() / 6, 260, 420);
            if (auto* brand = rail->findChild<QLabel*>("brand"))
            {
                brand->ensurePolished();
                railWidth = std::max(railWidth, brand->sizeHint().width() + 40);
            }
            rail->setFixedWidth(railWidth);
        }
        if (auto* root = centralWidget())
        {
            root->layout()->activate();
        }
    }

    const QFont baseFont_;
    const QString baseStyle_;
    int fontTier_ = -1;
};
} // namespace

QMainWindow* createMainWindow()
{
    auto* window = new MainWindow;
    window->setWindowTitle("Backup Studio");
    window->setMinimumSize(1100, 760);
    window->resize(1320, 900);
    auto* root = new QWidget;
    root->setObjectName("appRoot");
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    auto* rail = new QFrame;
    rail->setObjectName("navigationRail");
    rail->setFixedWidth(260);
    auto* railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(20, 32, 20, 28);
    railLayout->setSpacing(10);
    auto* brand = new QLabel("Backup Studio");
    brand->setObjectName("brand");
    railLayout->addWidget(brand);
    railLayout->addSpacing(22);

    auto* pages = new QStackedWidget;
    pages->setObjectName("pageStack");
    auto* session = new AccountSession(window);
    for (auto* page : {localBackupPage(), localRestorePage(),
                       remoteBackupPage(session), remoteRestorePage(session),
                       remoteListPage(session), startPage(session)})
    {
        pages->addWidget(page);
    }
    const QStringList titles = {"本地备份", "本地还原", "远程备份",
                                "远程还原", "备份历史", "登录"};
    auto* navigation = new QButtonGroup(window);
    navigation->setExclusive(true);
    const auto addNavigation = [&](int index)
    {
        auto* button = new QPushButton(titles.at(index));
        button->setObjectName("navButton");
        button->setProperty("pageIndex", index);
        button->setCheckable(true);
        navigation->addButton(button, index);
        railLayout->addWidget(button);
        return button;
    };
    // Group headers are buttons too: 本地 jumps to the local backup page and
    // 远程 asks for a login first when the session is signed out.
    const auto addGroupButton =
        [&](const QString& text, const std::function<int()>& target)
    {
        auto* button = new QPushButton(text);
        button->setObjectName("navGroup");
        button->setCursor(Qt::PointingHandCursor);
        QObject::connect(button, &QPushButton::clicked, window,
                         [=]() { navigation->button(target())->click(); });
        railLayout->addWidget(button);
    };
    addGroupButton("本地（无需账户）", []() { return 0; });
    addNavigation(0);
    addNavigation(1);
    railLayout->addSpacing(12);
    addGroupButton("远程（需要登录）",
                   [=]() { return session->signedIn() ? 2 : 5; });
    const std::array<QPushButton*, 3> remoteButtons{
        addNavigation(2), addNavigation(3), addNavigation(4)};
    railLayout->addStretch();
    addNavigation(5);
    // Remote features unlock only while the session holds a login.
    const auto applyLoginState = [=]()
    {
        const bool signedIn = session->signedIn();
        for (auto* button : remoteButtons)
        {
            button->setEnabled(signedIn);
        }
        if (!signedIn && pages->currentIndex() >= 2 &&
            pages->currentIndex() <= 4)
        {
            navigation->button(5)->click();
        }
    };
    QObject::connect(session, &AccountSession::changed, window,
                     applyLoginState);
    applyLoginState();

    auto* content = new QWidget;
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(32, 32, 32, 28);
    contentLayout->setSpacing(16);
    contentLayout->addWidget(pages, 1);
    QObject::connect(navigation, &QButtonGroup::idClicked, pages,
                     &QStackedWidget::setCurrentIndex);
    QObject::connect(pages, &QStackedWidget::currentChanged, navigation,
                     [navigation](int index)
                     {
                         if (auto* button = navigation->button(index))
                         {
                             button->setChecked(true);
                         }
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
                    ->findChild<QComboBox*>("backupId")
                    ->setEditText(item->data(Qt::UserRole).toString());
                navigation->button(3)->click();
            }
        });
    const auto invalidateHistory = [=]()
    {
        table->setRowCount(0);
        pages->widget(4)
            ->findChild<QLabel*>("historyStatus")
            ->setText("请刷新");
    };
    QObject::connect(
        session, &AccountSession::signedOut, window,
        [=]()
        {
            invalidateHistory();
            for (int index : {2, 3, 4})
            {
                auto* page = pages->widget(index);
                if (page->property("jobRunning").toBool())
                {
                    page->findChild<QPushButton*>("dangerButton")->click();
                }
            }
        });
    // Credentials live only in the session; the settings page edits a draft
    // that takes effect after a successful login.
    QObject::connect(session, &AccountSession::changed, window,
                     invalidateHistory);
    rootLayout->addWidget(rail);
    rootLayout->addWidget(content, 1);
    window->setCentralWidget(root);
    bool validPage = false;
    const int initial =
        qEnvironmentVariableIntValue("BACKUP_GUI_PAGE", &validPage);
    navigation
        ->button(validPage && initial >= 0 && initial < pages->count() ? initial
                                                                       : 5)
        ->click();
    return window;
}
namespace
{
QString preferredChineseFontFamily()
{
    const QStringList installed =
        QFontDatabase::families(QFontDatabase::SimplifiedChinese);
    const QStringList preferred = {"Microsoft YaHei UI", "Microsoft YaHei",
                                   "PingFang SC",        "Noto Sans CJK SC",
                                   "Source Han Sans SC", "Droid Sans Fallback"};
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
    font.setPointSizeF(std::max<qreal>(16.0, systemPointSize));
    font.setWeight(QFont::Normal);
    font.setStretch(QFont::Unstretched);
    font.setStyleHint(QFont::SansSerif);
    font.setStyleStrategy(QFont::PreferAntialias);
    return font;
}

bool MessageBoxButtonIconFilter::eventFilter(QObject* watched, QEvent* event)
{
    if (event->type() == QEvent::Polish)
    {
        if (auto* messageBox = qobject_cast<QMessageBox*>(watched))
        {
            auto* parent = messageBox->parentWidget();
            if (parent && parent != parent->window())
            {
                messageBox->setParent(parent->window(),
                                      messageBox->windowFlags());
            }
        }
    }
    if (event->type() == QEvent::Show || event->type() == QEvent::Resize)
    {
        if (auto* messageBox = qobject_cast<QMessageBox*>(watched))
        {
            // Queue after native placement and layout (including translated
            // buttons). A hidden page is not a reliable positioning anchor.
            QTimer::singleShot(
                0, messageBox,
                [messageBox]()
                {
                    auto* parent = messageBox->parentWidget();
                    if (!messageBox->isVisible() || !parent)
                    {
                        return;
                    }
                    auto* window = parent->window();
                    const auto area = window->screen()->availableGeometry();
                    const auto frame = messageBox->frameGeometry();
                    auto corner = window->frameGeometry().center() -
                                  QRect(QPoint(), frame.size()).center();
                    corner.setX(
                        std::clamp(corner.x(), area.left(),
                                   std::max(area.left(),
                                            area.right() - frame.width() + 1)));
                    corner.setY(std::clamp(
                        corner.y(), area.top(),
                        std::max(area.top(),
                                 area.bottom() - frame.height() + 1)));
                    messageBox->move(messageBox->pos() + corner -
                                     frame.topLeft());
                });
        }
    }
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
