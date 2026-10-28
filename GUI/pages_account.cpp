#include "ui.hpp"
#include <QCheckBox>
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
    auto* presetRow = new QHBoxLayout;
    presetRow->setSpacing(12);
    auto* preset = new QComboBox;
    preset->setObjectName("loginPreset");
    preset->setPlaceholderText("登录预设");
    preset->setMinimumWidth(260);
    auto* deletePreset = new QPushButton("删除预设");
    deletePreset->setObjectName("deletePreset");
    deletePreset->setMinimumWidth(100);
    presetRow->addWidget(preset, 1);
    presetRow->addWidget(deletePreset);
    auto* remember = new QCheckBox("记住此登录（仅服务器与用户名）");
    remember->setObjectName("rememberLogin");
    loginLayout->addLayout(presetRow);
    loginLayout->addWidget(remember);

    const auto refreshPresets = [=]()
    {
        preset->clear();
        for (const auto& item : loadLoginPresets())
        {
            preset->addItem(QString("%1 @ %2:%3")
                                .arg(item.username)
                                .arg(item.host)
                                .arg(item.port),
                            QStringList{item.host, QString::number(item.port),
                                        item.username, item.tls ? "1" : "0",
                                        item.caFile});
        }
        preset->setCurrentIndex(-1);
        preset->setEnabled(preset->count() != 0);
    };
    refreshPresets();
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
    auto* caption = makeHint("当前账户");
    caption->setObjectName("accountCaption");
    auto* address = makeHint({});
    address->setObjectName("accountServer");
    infoLayout->addWidget(caption);
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
    QObject::connect(preset, &QComboBox::currentIndexChanged, page.widget,
                     [=](int index)
                     {
                         const QStringList data =
                             preset->itemData(index).toStringList();
                         if (data.size() < 3)
                         {
                             return;
                         }
                         server.host->setText(data.at(0));
                         server.port->setValue(data.at(1).toInt());
                         server.username->setText(data.at(2));
                         server.tls->setChecked(data.value(3) == "1");
                         server.caFile->setText(data.value(4));
                         server.password->clear();
                     });
    QObject::connect(
        deletePreset, &QPushButton::clicked, page.widget,
        [=]()
        {
            const QStringList data = preset->currentData().toStringList();
            if (data.size() < 3)
            {
                return;
            }
            removeLoginPreset({data.at(0),
                               static_cast<uint16_t>(data.at(1).toInt()),
                               data.at(2)});
            refreshPresets();
        });
    QObject::connect(mode, &QComboBox::currentIndexChanged, page.widget,
                     updateMode);
    const auto updateAccount = [=]()
    {
        const bool authenticated = session->signedIn();
        // The login form stays visible so the settings page can edit the draft
        // at any time; it only takes effect after a successful login.
        loginView->setVisible(true);
        accountView->setVisible(authenticated);
        task.frame->setVisible(!authenticated);
        controls.start->setVisible(!authenticated);
        username->setText(session->username());
        const ServerValues signedIn = session->credentials();
        address->setText(QString::fromStdString(signedIn.host) + ":" +
                         QString::number(signedIn.port));
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
                    session->accept(values, revision);
                    if (remember->isChecked())
                    {
                        saveLoginPreset(
                            {QString::fromStdString(values.host), values.port,
                             QString::fromStdString(values.username),
                             values.tls.enabled,
                             QString::fromStdString(values.tls.caFile)});
                        refreshPresets();
                    }
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
            const auto values = session->credentials();
            if (QMessageBox::question(
                    page.widget, "注销账户",
                    QString("永久删除账户“%1”？有云端备份时将拒绝注销。")
                        .arg(QString::fromStdString(values.username)),
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
