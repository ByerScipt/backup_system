#include "ui.hpp"
#include <QAbstractItemView>
#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QObject>
#include <QPushButton>
#include <QSpinBox>
#include <QString>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>
#include <atomic>
#include <exception>
#include <memory>
#include <stdexcept>
#include <vector>

namespace backup::gui
{
namespace
{
class HistoryDelegate final : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QString displayText(const QVariant& value,
                        const QLocale& locale) const override
    {
        if (value.metaType().id() == QMetaType::ULongLong)
        {
            return formatBytes(value.toULongLong());
        }
        if (value.metaType().id() == QMetaType::QDateTime)
        {
            return value.toDateTime().toString("yyyy-MM-dd HH:mm");
        }
        return QStyledItemDelegate::displayText(value, locale);
    }
};
} // namespace

QWidget* remoteListPage()
{
    Page page = makePage();
    Card connection = makeCard();
    const ServerFields server = addServerRows(connection.body);

    Card listCard = makeCard();
    auto* search = new QLineEdit;
    search->setObjectName("historySearch");
    search->setPlaceholderText("名称 / ID");
    search->setToolTip("搜索备份名称或完整 ID");
    search->setClearButtonEnabled(true);
    listCard.body->addWidget(search);
    auto* table = new QTableWidget(0, 4);
    table->setObjectName("historyTable");
    table->setHorizontalHeaderLabels({"名称", "大小", "时间", "ID"});
    table->setItemDelegate(new HistoryDelegate(table));
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
    {
        table->horizontalHeader()->setSectionResizeMode(
            column, QHeaderView::ResizeToContents);
    }
    table->verticalHeader()->setVisible(false);
    table->verticalHeader()->setSectionResizeMode(
        QHeaderView::ResizeToContents);
    table->setAlternatingRowColors(true);
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setSortingEnabled(true);
    table->setMinimumHeight(140);
    table->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Ignored);
    listCard.body->addWidget(table, 1);
    auto* status = makeHint("未加载");
    status->setObjectName("historyStatus");
    listCard.body->addWidget(status);
    auto* actions = new QHBoxLayout;
    auto* copy = new QPushButton("复制 ID");
    copy->setObjectName("copyBackupId");
    auto* restore = new QPushButton("还原");
    restore->setObjectName("restoreSelected");
    actions->addWidget(copy);
    actions->addWidget(restore);
    actions->addStretch();
    listCard.body->addLayout(actions);
    page.layout->addWidget(connection.frame);
    page.layout->addWidget(listCard.frame, 1);
    Card task = makeCard();
    JobControls controls = addJobControls(task.body, "刷新");
    page.footer->addWidget(task.frame);

    auto updateSelection = [=]()
    {
        const int row = table->currentRow();
        const bool selected = row >= 0 && !table->isRowHidden(row);
        copy->setEnabled(selected);
        restore->setEnabled(selected);
    };
    updateSelection();
    QObject::connect(table, &QTableWidget::itemSelectionChanged, page.widget,
                     updateSelection);
    auto filterRows = [=]()
    {
        const QString query = search->text().trimmed();
        int visible = 0;
        table->clearSelection();
        table->setCurrentCell(-1, -1);
        for (int row = 0; row < table->rowCount(); ++row)
        {
            auto* item = table->item(row, 0);
            const bool matches =
                item->text().contains(query, Qt::CaseInsensitive) ||
                item->data(Qt::UserRole)
                    .toString()
                    .contains(query, Qt::CaseInsensitive);
            table->setRowHidden(row, !matches);
            visible += matches;
        }
        status->setText(
            table->rowCount() == 0
                ? "无备份"
                : QString("%1 / %2").arg(visible).arg(table->rowCount()));
        updateSelection();
    };
    QObject::connect(search, &QLineEdit::textChanged, page.widget, filterRows);
    QObject::connect(copy, &QPushButton::clicked, page.widget,
                     [=]()
                     {
                         if (auto* item = table->item(table->currentRow(), 0))
                         {
                             QApplication::clipboard()->setText(
                                 item->data(Qt::UserRole).toString());
                             appendLog(controls.log, "已复制完整备份 ID");
                         }
                     });
    QObject::connect(table, &QTableWidget::cellDoubleClicked, restore,
                     [=](int, int) { restore->click(); });

    auto entries = std::make_shared<std::vector<network::RemoteBackupEntry>>();
    QObject::connect(
        controls.start, &QPushButton::clicked, page.widget,
        [=]()
        {
            ServerValues values;
            try
            {
                values = snapshotServer(server);
            }
            catch (const std::exception& error)
            {
                QMessageBox::warning(page.widget, "输入有误",
                                     QString::fromUtf8(error.what()));
                return;
            }
            startJob(
                page.widget, controls,
                [=](std::atomic_bool* cancel, const ProgressCallback&)
                {
                    auto client = makeClient(values);
                    std::string error;
                    *entries = client.list(error, cancel);
                    if (!error.empty())
                    {
                        throw std::runtime_error(error);
                    }
                    return QString("已同步 %1 个备份").arg(entries->size());
                },
                [=]()
                {
                    // Ignore a response if the user changed connection/account
                    // mid-request.
                    if (server.host->text().trimmed().toStdString() !=
                            values.host ||
                        server.port->value() != values.port ||
                        server.username->text().trimmed().toStdString() !=
                            values.username ||
                        server.password->text().toStdString() !=
                            values.password)
                    {
                        return;
                    }
                    table->setSortingEnabled(false);
                    table->setRowCount(static_cast<int>(entries->size()));
                    for (int row = 0; row < table->rowCount(); ++row)
                    {
                        const auto& entry =
                            (*entries)[static_cast<size_t>(row)];
                        const QString id = QString::fromStdString(entry.id);
                        auto* name = new QTableWidgetItem(
                            entry.name.empty()
                                ? "未命名备份"
                                : QString::fromStdString(entry.name));
                        name->setData(Qt::UserRole, id);
                        name->setToolTip(id);
                        table->setItem(row, 0, name);
                        auto* size = new QTableWidgetItem;
                        size->setData(Qt::DisplayRole,
                                      QVariant::fromValue(
                                          static_cast<qulonglong>(entry.size)));
                        table->setItem(row, 1, size);
                        auto* time = new QTableWidgetItem;
                        time->setData(
                            Qt::DisplayRole,
                            QDateTime::fromSecsSinceEpoch(
                                static_cast<qint64>(entry.timestamp)));
                        table->setItem(row, 2, time);
                        auto* shortId = new QTableWidgetItem(id.left(8) + "…");
                        shortId->setToolTip(id);
                        table->setItem(row, 3, shortId);
                    }
                    table->setSortingEnabled(true);
                    table->sortItems(2, Qt::DescendingOrder);
                    filterRows();
                });
        });
    return page.widget;
}

} // namespace backup::gui
