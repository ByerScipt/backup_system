#include "main_window.hpp"
#include "ui.hpp"
#include <QButtonGroup>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMetaObject>
#include <QObject>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QShowEvent>
#include <QStackedWidget>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>
#include <QWindow>
#include <algorithm>
#include <cmath>

namespace backup::gui {

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        scaleTimer_.setSingleShot(true);
        scaleTimer_.setInterval(70);
        QObject::connect(&scaleTimer_, &QTimer::timeout, this,
                         [this]() { applyResponsiveTheme(); });

        setWindowTitle("Backup Studio");
        setMinimumSize(1024, 700);
        resize(1180, 780);

        auto* root = new QWidget;
        root->setObjectName("appRoot");
        auto* rootLayout = new QHBoxLayout(root);
        rootLayout->setContentsMargins(0, 0, 0, 0);
        rootLayout->setSpacing(0);

        sidebar_ = new QFrame;
        sidebar_->setObjectName("sidebar");
        sidebarLayout_ = new QVBoxLayout(sidebar_);
        sidebarLayout_->setContentsMargins(18, 24, 18, 20);
        sidebarLayout_->setSpacing(7);

        auto* brand = new QLabel("Backup Studio");
        brand->setObjectName("brand");
        sidebarLayout_->addWidget(brand);
        sidebarLayout_->addSpacing(20);

        auto* pages = new QStackedWidget;
        pages->setObjectName("pageStack");
        pages->addWidget(localBackupPage());
        pages->addWidget(localRestorePage());
        pages->addWidget(remoteBackupPage());
        pages->addWidget(remoteRestorePage());
        pages->addWidget(remoteListPage());
        pages->addWidget(userPage());

        auto* navigation = new QButtonGroup(this);
        navigation->setExclusive(true);
        auto addSection = [this](const QString& text) {
            auto* label = new QLabel(text);
            label->setObjectName("navSection");
            sidebarLayout_->addSpacing(7);
            sidebarLayout_->addWidget(label);
        };
        auto addNavigation = [&](int index, const QString& text) {
            auto* button = new QPushButton(text);
            button->setObjectName("navButton");
            button->setCheckable(true);
            button->setCursor(Qt::PointingHandCursor);
            navigation->addButton(button, index);
            sidebarLayout_->addWidget(button);
        };

        addSection("本地工作区");
        addNavigation(0, "本地备份");
        addNavigation(1, "本地还原");
        addSection("远程中心");
        addNavigation(2, "远程备份");
        addNavigation(3, "远程还原");
        addNavigation(4, "备份历史");
        addSection("访问控制");
        addNavigation(5, "账号管理");
        navigation->button(0)->setChecked(true);

        sidebarLayout_->addStretch();

        auto* content = new QFrame;
        content->setObjectName("contentRoot");
        contentLayout_ = new QVBoxLayout(content);
        contentLayout_->setContentsMargins(30, 24, 30, 24);
        contentLayout_->setSpacing(18);

        auto* header = new QHBoxLayout;
        auto* pageTitle = new QLabel("本地备份");
        pageTitle->setObjectName("pageTitle");
        header->addWidget(pageTitle);
        header->addStretch();
        contentLayout_->addLayout(header);
        contentLayout_->addWidget(pages, 1);

        const QStringList titles = {"本地备份", "本地还原", "远程备份",
                                    "远程还原", "备份历史", "账号管理"};

        QObject::connect(navigation, &QButtonGroup::idClicked, this, [=](int index) {
            pages->setCurrentIndex(index);
            pageTitle->setText(titles.at(index));
        });
        bool pageFromEnvironment = false;
        const int initialPage =
            qEnvironmentVariableIntValue("BACKUP_GUI_PAGE", &pageFromEnvironment);
        if (pageFromEnvironment && initialPage >= 0 && initialPage < pages->count()) {
            navigation->button(initialPage)->click();
        }

        rootLayout->addWidget(sidebar_);
        rootLayout->addWidget(content, 1);
        setCentralWidget(root);
        applyResponsiveTheme();
    }

protected:
    void resizeEvent(QResizeEvent* event) override {
        QMainWindow::resizeEvent(event);
        scaleTimer_.start();
    }

    void showEvent(QShowEvent* event) override {
        QMainWindow::showEvent(event);
        if (!screenSignalsConnected_ && windowHandle()) {
            screenSignalsConnected_ = true;
            QObject::connect(windowHandle(), &QWindow::screenChanged, this,
                             [this](QScreen* screen) {
                                 watchScreen(screen);
                                 appliedScale_ = 0.0;
                                 applyResponsiveTheme();
                             });
        }
        watchScreen(screen());
        appliedScale_ = 0.0;
        applyResponsiveTheme();
    }

private:
    qreal responsiveScale() const {
        bool hasOverride = false;
        const qreal overrideScale = qEnvironmentVariable("BACKUP_GUI_SCALE").toDouble(&hasOverride);
        if (hasOverride) {
            return std::clamp(overrideScale, 0.9, 2.0);
        }
        const qreal widthRatio = width() / 1180.0;
        const qreal heightRatio = height() / 780.0;
        return std::clamp(std::min(widthRatio, heightRatio), 1.0, 1.5);
    }

    void watchScreen(QScreen* currentScreen) {
        QObject::disconnect(dpiConnection_);
        if (!currentScreen)
            return;
        dpiConnection_ = QObject::connect(currentScreen, &QScreen::logicalDotsPerInchChanged, this,
                                          [this](qreal) {
                                              appliedScale_ = 0.0;
                                              applyResponsiveTheme();
                                          });
    }

    void applyResponsiveTheme() {
        const qreal scale = responsiveScale();
        if (std::abs(scale - appliedScale_) < 0.01)
            return;
        appliedScale_ = scale;
        setStyleSheet(applicationStyle(scale));

        const auto pixels = [scale](int value) {
            return qRound(static_cast<qreal>(value) * scale);
        };
        sidebar_->setFixedWidth(pixels(224));
        sidebarLayout_->setContentsMargins(pixels(18), pixels(24), pixels(18), pixels(20));
        sidebarLayout_->setSpacing(pixels(7));
        contentLayout_->setContentsMargins(pixels(30), pixels(24), pixels(30), pixels(24));
        contentLayout_->setSpacing(pixels(18));

        for (auto* card : findChildren<QFrame*>("card")) {
            if (auto* layout = qobject_cast<QVBoxLayout*>(card->layout())) {
                layout->setContentsMargins(pixels(20), pixels(18), pixels(20), pixels(18));
                layout->setSpacing(pixels(12));
            }
        }
        for (auto* form : findChildren<QFormLayout*>()) {
            form->setHorizontalSpacing(pixels(18));
            form->setVerticalSpacing(pixels(11));
        }
        for (auto* badge : findChildren<QLabel*>("numberBadge")) {
            badge->setFixedSize(pixels(32), pixels(32));
        }
    }

    QFrame* sidebar_ = nullptr;
    QVBoxLayout* sidebarLayout_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QTimer scaleTimer_;
    QMetaObject::Connection dpiConnection_;
    qreal appliedScale_ = 0.0;
    bool screenSignalsConnected_ = false;
};

QMainWindow* createMainWindow() {
    return new MainWindow;
}

} // namespace backup::gui
