#include "ui.hpp"
#include <QFormLayout>
#include <QFrame>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <exception>
#include <stdexcept>

namespace backup::gui {

namespace {
void addSecurityItem(QVBoxLayout* layout, const QString& number, const QString& title,
                     const QString& text) {
    auto* row = new QHBoxLayout;
    row->setSpacing(12);
    auto* badge = new QLabel(number);
    badge->setObjectName("numberBadge");
    badge->setAlignment(Qt::AlignCenter);
    badge->setFixedSize(32, 32);

    auto* copy = new QVBoxLayout;
    copy->setSpacing(2);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("securityTitle");
    auto* textLabel = makeHint(text);
    copy->addWidget(titleLabel);
    copy->addWidget(textLabel);

    row->addWidget(badge, 0, Qt::AlignTop);
    row->addLayout(copy, 1);
    layout->addLayout(row);
}

} // namespace

QWidget* userPage() {
    Page page = makePage();
    auto* top = new QHBoxLayout;
    top->setSpacing(14);

    Card account = makeCard("创建账号", "填写服务器信息、用户名和两次相同的密码。");
    auto* form = makeForm();
    const ServerFields server = addServerRows(form);
    auto* confirm = new QLineEdit;
    confirm->setEchoMode(QLineEdit::Password);
    confirm->setClearButtonEnabled(true);
    confirm->setPlaceholderText("再次输入账号密码");
    form->addRow("确认密码", confirm);
    account.body->addLayout(form);

    Card security = makeCard("操作步骤");
    addSecurityItem(security.body, "1", "连接服务器", "确认服务已启动，并填写服务器地址和端口。");
    addSecurityItem(security.body, "2", "设置账号", "填写用户名、账号密码和确认密码。");
    addSecurityItem(security.body, "3", "提交注册", "点击注册账号，成功后即可使用其他远程页面。");
    security.body->addStretch();

    top->addWidget(account.frame, 1);
    top->addWidget(security.frame, 1);
    page.layout->addLayout(top);

    Card task = makeCard("注册状态");
    JobControls controls = addJobControls(task.body, "注册账号");
    page.layout->addWidget(task.frame, 1);

    QObject::connect(controls.start, &QPushButton::clicked, page.widget, [=]() {
        if (server.password->text() != confirm->text()) {
            QMessageBox::warning(page.widget, "Input Error", "两次输入的密码不一致。");
            return;
        }
        ServerValues serverValues;
        try {
            serverValues = snapshotServer(server);
        } catch (const std::exception& error) {
            QMessageBox::warning(page.widget, "Input Error", QString::fromUtf8(error.what()));
            return;
        }

        startJob(page.widget, controls, [=](std::atomic_bool*, const ProgressCallback&) {
            auto client = makeClient(serverValues);
            std::string error;
            if (!client.registerUser(error)) {
                throw std::runtime_error(error);
            }
            return QString("账号 %1 注册成功").arg(QString::fromStdString(serverValues.username));
        });
    });
    return page.widget;
}

} // namespace backup::gui
