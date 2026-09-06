#include "gui_common.hpp"

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

        const QStringList titles = {
            "本地备份", "本地还原", "远程备份",
            "远程还原", "备份历史", "账号管理"};

        QObject::connect(
            navigation, &QButtonGroup::idClicked, this,
            [=](int index) {
                pages->setCurrentIndex(index);
                pageTitle->setText(titles.at(index));
            });
        bool pageFromEnvironment = false;
        const int initialPage =
            qEnvironmentVariableIntValue("BACKUP_GUI_PAGE", &pageFromEnvironment);
        if (pageFromEnvironment && initialPage >= 0 &&
            initialPage < pages->count()) {
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
            QObject::connect(
                windowHandle(), &QWindow::screenChanged, this,
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
        const qreal overrideScale =
            qEnvironmentVariable("BACKUP_GUI_SCALE").toDouble(&hasOverride);
        if (hasOverride) {
            return std::clamp(overrideScale, 0.9, 2.0);
        }
        const qreal widthRatio = width() / 1180.0;
        const qreal heightRatio = height() / 780.0;
        return std::clamp(std::min(widthRatio, heightRatio), 1.0, 1.5);
    }

    void watchScreen(QScreen* currentScreen) {
        QObject::disconnect(dpiConnection_);
        if (!currentScreen) return;
        dpiConnection_ = QObject::connect(
            currentScreen, &QScreen::logicalDotsPerInchChanged, this,
            [this](qreal) {
                appliedScale_ = 0.0;
                applyResponsiveTheme();
            });
    }

    void applyResponsiveTheme() {
        const qreal scale = responsiveScale();
        if (std::abs(scale - appliedScale_) < 0.01) return;
        appliedScale_ = scale;
        setStyleSheet(theme(scale));

        const auto pixels = [scale](int value) {
            return qRound(static_cast<qreal>(value) * scale);
        };
        sidebar_->setFixedWidth(pixels(224));
        sidebarLayout_->setContentsMargins(
            pixels(18), pixels(24), pixels(18), pixels(20));
        sidebarLayout_->setSpacing(pixels(7));
        contentLayout_->setContentsMargins(
            pixels(30), pixels(24), pixels(30), pixels(24));
        contentLayout_->setSpacing(pixels(18));

        for (auto* card : findChildren<QFrame*>("card")) {
            if (auto* layout = qobject_cast<QVBoxLayout*>(card->layout())) {
                layout->setContentsMargins(
                    pixels(20), pixels(18), pixels(20), pixels(18));
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

    static QString theme(qreal scale) {
        QString style = R"(
            * {
                font-size: @BASE_PT@pt;
                color: #1f2937;
            }
            QMainWindow, QWidget#appRoot {
                background: #f3f6fb;
            }
            QFrame#sidebar {
                background: #111827;
                border: none;
            }
            QLabel#brand {
                color: #f8fafc;
                font-size: @BRAND_PT@pt;
                font-weight: 700;
            }
            QLabel#navSection {
                color: #64748b;
                font-size: @SMALL_PT@pt;
                font-weight: 700;
                padding: 5px 10px 2px 10px;
            }
            QPushButton#navButton {
                min-height: @NAV_H@px;
                padding: 0 13px;
                border: none;
                border-radius: 10px;
                background: transparent;
                color: #aab5c7;
                text-align: left;
                font-weight: 500;
            }
            QPushButton#navButton:hover {
                color: #f8fafc;
                background: #1f2937;
            }
            QPushButton#navButton:checked {
                color: white;
                background: #2563eb;
                font-weight: 650;
            }
            QFrame#contentRoot {
                background: #f3f6fb;
                border: none;
            }
            QLabel#pageTitle {
                color: #111827;
                font-size: @TITLE_PT@pt;
                font-weight: 750;
            }
            QStackedWidget#pageStack {
                background: transparent;
                border: none;
            }
            QScrollArea#pageScroll, QWidget#pageContent {
                background: transparent;
                border: none;
            }
            QFrame#card {
                background: white;
                border: 1px solid #e1e7f0;
                border-radius: 14px;
            }
            QLabel#cardTitle {
                color: #172033;
                font-size: @CARD_PT@pt;
                font-weight: 700;
            }
            QLabel#cardDescription, QLabel#hint {
                color: #718096;
                font-size: @SMALL_PT@pt;
            }
            QLabel#securityTitle {
                color: #253047;
                font-weight: 650;
            }
            QLabel#numberBadge {
                color: #1d4ed8;
                background: #eaf1ff;
                border: 1px solid #cfddff;
                border-radius: 9px;
                font-size: @BASE_PT@pt;
                font-weight: 700;
            }
            QLineEdit, QComboBox, QSpinBox {
                min-height: @CONTROL_H@px;
                padding: 0 11px;
                color: #1f2937;
                background: #fbfcfe;
                border: 1px solid #ccd6e4;
                border-radius: 8px;
                selection-background-color: #2563eb;
            }
            QLineEdit:hover, QComboBox:hover, QSpinBox:hover {
                border-color: #9eacc0;
                background: white;
            }
            QLineEdit:focus, QComboBox:focus, QSpinBox:focus {
                border: 2px solid #3b82f6;
                background: white;
            }
            QLineEdit:disabled {
                color: #9aa7b8;
                background: #f1f4f8;
                border-color: #e1e7ef;
            }
            QComboBox {
                padding-right: 34px;
            }
            QComboBox::drop-down {
                width: 30px;
                border: none;
            }
            QComboBox QAbstractItemView {
                padding: 6px;
                background: white;
                border: 1px solid #ccd6e4;
                selection-color: white;
                selection-background-color: #2563eb;
            }
            QPushButton {
                min-height: @CONTROL_H@px;
                padding: 0 15px;
                border: 1px solid #d1d9e6;
                border-radius: 8px;
                background: #f8fafc;
                color: #334155;
                font-weight: 600;
            }
            QPushButton:hover {
                background: #eef3f9;
                border-color: #aebacc;
            }
            QPushButton:pressed {
                background: #e2e8f0;
            }
            QPushButton#primaryButton {
                color: white;
                background: #2563eb;
                border-color: #2563eb;
            }
            QPushButton#primaryButton:hover {
                background: #1d4ed8;
                border-color: #1d4ed8;
            }
            QPushButton#secondaryButton {
                color: #1d4ed8;
                background: #eff5ff;
                border-color: #c7d7fe;
            }
            QPushButton#secondaryButton:hover {
                background: #e1ecff;
                border-color: #9fbbfd;
            }
            QPushButton#stepButton {
                min-width: @STEP_W@px;
                max-width: @STEP_W@px;
                padding: 0;
                color: #1d4ed8;
                background: #eff5ff;
                border-color: #c7d7fe;
                font-size: @STEP_PT@pt;
                font-weight: 700;
            }
            QPushButton#stepButton:hover {
                background: #dce9ff;
                border-color: #8eaffd;
            }
            QPushButton#dangerButton {
                color: #b42318;
                background: white;
                border-color: #f0c5c1;
            }
            QPushButton#dangerButton:hover {
                background: #fff1f0;
                border-color: #e99b94;
            }
            QPushButton#quietButton {
                min-height: @QUIET_H@px;
                padding: 0 10px;
                color: #64748b;
                background: transparent;
                border-color: transparent;
                font-size: @SMALL_PT@pt;
            }
            QPushButton#quietButton:hover {
                color: #1d4ed8;
                background: #eef4ff;
            }
            QPushButton:disabled {
                color: #9da8b8;
                background: #edf1f6;
                border-color: #e0e6ee;
            }
            QCheckBox {
                spacing: 8px;
                color: #39465a;
            }
            QCheckBox::indicator {
                width: @CHECK_SIZE@px;
                height: @CHECK_SIZE@px;
                border: 1px solid #aeb9ca;
                border-radius: 5px;
                background: white;
            }
            QCheckBox::indicator:checked {
                background: #2563eb;
                border-color: #2563eb;
            }
            QProgressBar {
                min-height: @PROGRESS_H@px;
                max-height: @PROGRESS_H@px;
                color: #334155;
                background: #e8edf4;
                border: none;
                border-radius: 7px;
                text-align: center;
                font-size: @TINY_PT@pt;
                font-weight: 650;
            }
            QProgressBar::chunk {
                background: #22a06b;
                border-radius: 7px;
            }
            QTextEdit {
                padding: 9px;
                color: #334155;
                background: #fbfcfe;
                border: 1px solid #d8e0eb;
                border-radius: 9px;
                selection-background-color: #2563eb;
            }
            QTextEdit#taskLog {
                color: #cbd5e1;
                background: #111827;
                border-color: #273449;
                font-size: @SMALL_PT@pt;
            }
            QTableWidget {
                color: #273449;
                background: white;
                alternate-background-color: #f8fafc;
                border: 1px solid #dbe3ee;
                border-radius: 9px;
                gridline-color: #edf1f5;
                selection-color: #1d4ed8;
                selection-background-color: #e7efff;
            }
            QHeaderView::section {
                min-height: @HEADER_H@px;
                padding: 0 8px;
                color: #526176;
                background: #f1f5f9;
                border: none;
                border-bottom: 1px solid #dbe3ee;
                font-size: @SMALL_PT@pt;
                font-weight: 700;
            }
            QScrollBar:vertical {
                width: @SCROLL_W@px;
                margin: 2px;
                background: transparent;
            }
            QScrollBar::handle:vertical {
                min-height: 24px;
                background: #c4cedb;
                border-radius: 4px;
            }
            QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {
                height: 0;
            }
            QToolTip {
                color: #f8fafc;
                background: #1f2937;
                border: 1px solid #334155;
                padding: 6px;
            }
        )";

        const auto points = [scale](qreal base) {
            return QString::number(base * scale, 'f', 1);
        };
        const auto pixels = [scale](int base) {
            return QString::number(
                qRound(static_cast<qreal>(base) * scale));
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

    QFrame* sidebar_ = nullptr;
    QVBoxLayout* sidebarLayout_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QTimer scaleTimer_;
    QMetaObject::Connection dpiConnection_;
    qreal appliedScale_ = 0.0;
    bool screenSignalsConnected_ = false;
};

QMainWindow* createMainWindow() { return new MainWindow; }
