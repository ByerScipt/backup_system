#include "ui.hpp"
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <stdexcept>

namespace backup::gui
{
QWidget* startPage(AccountSession* session)
{
    Page page = makePage();
    Card account = makeCard();
    auto* title = new QLabel("Backup Studio");
    title->setObjectName("startTitle");
    account.body->addWidget(title);
    account.body->addSpacing(20);

    auto* loginView = new QWidget;
    auto* loginLayout = new QVBoxLayout(loginView);
    loginLayout->setContentsMargins(0, 0, 0, 0);
    loginLayout->setSpacing(22);
    auto* mode = new QComboBox;
    mode->setObjectName("accountMode");
    mode->addItems({"登录", "注册"});
    mode->setMaximumWidth(240);
    loginLayout->addWidget(mode);
    auto* confirm = new QLineEdit;
    confirm->setObjectName("confirmPassword");
    confirm->setEchoMode(QLineEdit::Password);
    confirm->setClearButtonEnabled(true);
    const auto server = addServerRows(loginLayout, confirm);
    auto* confirmLabel = server.confirmationLabel;
    // Keep the registration row's footprint so changing mode never recenters
    // the shared title, selector and connection fields.
    for (auto* widget : {static_cast<QWidget*>(confirm), confirmLabel})
    {
        auto policy = widget->sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        widget->setSizePolicy(policy);
    }
    account.body->addWidget(loginView);

    auto* accountView = new QWidget;
    auto* infoLayout = new QVBoxLayout(accountView);
    infoLayout->setContentsMargins(0, 0, 0, 0);
    infoLayout->setSpacing(22);
    auto* username = new QLabel;
    username->setObjectName("accountName");
    auto* address = makeHint({});
    address->setObjectName("accountServer");
    infoLayout->addWidget(username);
    infoLayout->addWidget(address);
    auto* actions = new QHBoxLayout;
    auto* switchAccount = new QPushButton("切换账户");
    switchAccount->setObjectName("switchAccount");
    auto* logout = new QPushButton("退出账户");
    logout->setObjectName("logoutAccount");
    auto* remove = new QPushButton("注销账户");
    remove->setObjectName("deleteAccount");
    actions->setSpacing(12);
    for (auto* button : {switchAccount, logout, remove})
    {
        actions->addWidget(button);
    }
    infoLayout->addLayout(actions);
    account.body->addWidget(accountView);
    page.layout->addWidget(account.frame, 1);

    Card task = makeCard();
    const auto controls = addJobControls(task.body, "登录");
    page.footer->addWidget(task.frame);
    const auto updateMode = [=]()
    {
        const bool registration = mode->currentIndex() == 1;
        confirm->setVisible(registration);
        confirmLabel->setVisible(registration);
        controls.start->setText(registration ? "注册" : "登录");
    };
    QObject::connect(mode, &QComboBox::currentIndexChanged, page.widget,
                     updateMode);
    const auto updateAccount = [=]()
    {
        const bool authenticated = !session->username().isEmpty();
        loginView->setVisible(!authenticated);
        accountView->setVisible(authenticated);
        task.frame->setVisible(!authenticated);
        controls.start->setVisible(!authenticated);
        username->setText(session->username());
        address->setText(server.host->text() + ":" +
                         QString::number(server.port->value()));
    };
    QObject::connect(session, &AccountSession::changed, page.widget,
                     updateAccount);
    const auto signOut = [=](bool switching)
    {
        if (page.widget->property("jobRunning").toBool())
        {
            controls.cancel->click();
        }
        session->reset();
        emit session->signedOut();
        server.password->clear();
        confirm->clear();
        if (switching)
        {
            server.username->clear();
        }
        mode->setCurrentIndex(0);
        server.username->setFocus();
    };
    QObject::connect(logout, &QPushButton::clicked, page.widget,
                     [=]() { signOut(false); });
    QObject::connect(switchAccount, &QPushButton::clicked, page.widget,
                     [=]() { signOut(true); });
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            const bool registration = mode->currentIndex() == 1;
            ServerValues values;
            try
            {
                values = snapshotServer(server);
                if (registration && server.password->text() != confirm->text())
                {
                    throw std::runtime_error("两次输入的密码不一致");
                }
            }
            catch (const std::exception& error)
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     QString::fromUtf8(error.what()));
                return;
            }
            const auto revision = session->revision();
            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback&)
                {
                    auto client = makeClient(values);
                    std::string error;
                    const bool ok = registration
                                        ? client.registerUser(error, cancel)
                                        : client.login(error, cancel);
                    if (!ok)
                    {
                        throw std::runtime_error(error);
                    }
                    return registration ? QString("注册成功")
                                        : QString("登录成功");
                },
                [=]()
                {
                    confirm->clear();
                    session->accept(QString::fromStdString(values.username),
                                    revision);
                });
        });
    QObject::connect(
        remove, &QPushButton::clicked, page.widget,
        [=]()
        {
            if (page.widget->property("jobRunning").toBool())
            {
                return;
            }
            if (session->username().isEmpty())
            {
                return;
            }
            const auto revision = session->revision();
            const auto values = snapshotServer(server);
            if (QMessageBox::question(
                    page.widget, "注销账户",
                    "永久删除此账户？有云端备份时将拒绝注销。",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No) != QMessageBox::Yes)
            {
                return;
            }
            task.frame->show();
            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback&)
                {
                    auto client = makeClient(values);
                    std::string error;
                    if (!client.deleteAccount(error, cancel))
                    {
                        throw std::runtime_error(error);
                    }
                    return QString("账户已注销");
                },
                [=]()
                {
                    if (revision == session->revision())
                    {
                        signOut(true);
                    }
                });
        });
    updateMode();
    updateAccount();
    return page.widget;
}

} // namespace backup::gui
