#include "ui.hpp"

#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHash>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QVBoxLayout>

#include <algorithm>
#include <functional>
#include <vector>
namespace backup::gui
{
namespace
{
QString typeLabel(const QString& type)
{
    if (type == "file")
    {
        return "普通文件";
    }
    if (type == "directory")
    {
        return "文件夹";
    }
    if (type == "symlink")
    {
        return "符号链接";
    }
    if (type == "fifo")
    {
        return "命名管道";
    }
    if (type == "character-device")
    {
        return "字符设备";
    }
    if (type == "block-device")
    {
        return "块设备";
    }
    if (type == "unix-socket")
    {
        return "套接字";
    }
    return type;
}

// Keeps the listing metadata on the item so filters and column sorting never
// touch the filesystem again.
class EntryItem final : public QTreeWidgetItem
{
public:
    EntryItem(QTreeWidgetItem* parent, const SourceEntryInfo& info)
        : QTreeWidgetItem(parent)
    {
        assign(info);
    }
    EntryItem(QTreeWidget* tree, const SourceEntryInfo& info)
        : QTreeWidgetItem(tree)
    {
        assign(info);
    }

    // Size and time columns sort by value, not by their formatted text.
    bool operator<(const QTreeWidgetItem& other) const override
    {
        const auto& right = static_cast<const EntryItem&>(other);
        const int column = treeWidget() ? treeWidget()->sortColumn() : 0;
        if (column == 1)
        {
            return type < right.type;
        }
        if (column == 2)
        {
            return size < right.size;
        }
        if (column == 3)
        {
            return modified < right.modified;
        }
        return name.localeAwareCompare(right.name) < 0;
    }

    QString path;
    QString name;
    QString type;
    uint64_t size = 0;
    qint64 modified = 0;

private:
    void assign(const SourceEntryInfo& info)
    {
        path = QString::fromStdString(info.path);
        type = QString::fromStdString(info.type);
        size = info.size;
        modified = info.mtimeSeconds;
        const int slash = path.lastIndexOf('/');
        name = slash < 0 ? path : path.mid(slash + 1);
        setText(0, name);
        setText(1, typeLabel(type));
        setText(2, size == 0 ? QString() : formatBytes(size));
        setText(3, modified == 0
                       ? QString()
                       : QDateTime::fromSecsSinceEpoch(modified).toString(
                             "yyyy-MM-dd HH:mm"));
        setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        setFlags(flags() | Qt::ItemIsUserCheckable);
        setCheckState(0, Qt::Unchecked);
    }
};

void setSubtreeState(QTreeWidgetItem* item, Qt::CheckState state)
{
    item->setCheckState(0, state);
    for (int index = 0; index < item->childCount(); ++index)
    {
        setSubtreeState(item->child(index), state);
    }
}

// A partially selected parent shows PartiallyChecked; fully selected and fully
// cleared parents follow their children.
void refreshAncestors(QTreeWidgetItem* item)
{
    for (auto* parent = item->parent(); parent != nullptr;
         parent = parent->parent())
    {
        const Qt::CheckState first = parent->child(0)->checkState(0);
        bool uniform = true;
        for (int index = 1; index < parent->childCount(); ++index)
        {
            if (parent->child(index)->checkState(0) != first)
            {
                uniform = false;
                break;
            }
        }
        parent->setCheckState(0, uniform ? first : Qt::PartiallyChecked);
    }
}

// Collects the smallest set of selected entries: a checked directory stands
// for its whole subtree, so its children are not repeated.
void collectChecked(QTreeWidgetItem* item, QStringList& chosen)
{
    const Qt::CheckState state = item->checkState(0);
    if (state == Qt::Unchecked)
    {
        return;
    }
    if (state == Qt::Checked)
    {
        chosen << static_cast<EntryItem*>(item)->path;
        return;
    }
    for (int index = 0; index < item->childCount(); ++index)
    {
        collectChecked(item->child(index), chosen);
    }
}

bool matchesType(const QString& type, const QString& filter)
{
    if (filter.isEmpty())
    {
        return true;
    }
    if (filter == "special")
    {
        return type != "file" && type != "directory" && type != "symlink";
    }
    return type == filter;
}

bool matchesAge(qint64 modified, int choice, qint64 now)
{
    const qint64 day = 24 * 60 * 60;
    switch (choice)
    {
    case 1:
        return modified >= now - day;
    case 2:
        return modified >= now - 7 * day;
    case 3:
        return modified >= now - 30 * day;
    case 4:
        return modified < now - 30 * day;
    default:
        return true;
    }
}
} // namespace

bool selectSourceEntries(QWidget* owner, const QString& sourceDirectory,
                         QStringList* selection)
{
    std::vector<SourceEntryInfo> listing;
    try
    {
        listing = BackupEngine::scanSource(sourceDirectory.toStdString());
    }
    catch (const std::exception& error)
    {
        QMessageBox::warning(owner, "无法读取目录",
                             QString::fromUtf8(error.what()));
        return false;
    }

    QDialog dialog(owner);
    dialog.setObjectName("entrySelection");
    dialog.setWindowTitle("选择备份内容");
    dialog.setMinimumSize(820, 560);
    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(10);

    auto* filters = new QHBoxLayout;
    filters->setSpacing(8);
    auto* nameFilter = new QLineEdit;
    nameFilter->setObjectName("entryNameFilter");
    nameFilter->setPlaceholderText("按名称或扩展名筛选");
    nameFilter->setClearButtonEnabled(true);
    auto* typeFilter = new QComboBox;
    typeFilter->setObjectName("entryTypeFilter");
    typeFilter->addItem("全部类型", QString());
    typeFilter->addItem("文件夹", QString("directory"));
    typeFilter->addItem("普通文件", QString("file"));
    typeFilter->addItem("符号链接", QString("symlink"));
    typeFilter->addItem("其他特殊文件", QString("special"));
    auto* ageFilter = new QComboBox;
    ageFilter->setObjectName("entryAgeFilter");
    ageFilter->addItem("全部时间");
    ageFilter->addItem("最近 24 小时");
    ageFilter->addItem("最近 7 天");
    ageFilter->addItem("最近 30 天");
    ageFilter->addItem("30 天以前");
    filters->addWidget(nameFilter, 1);
    filters->addWidget(typeFilter);
    filters->addWidget(ageFilter);
    layout->addLayout(filters);

    auto* toolbar = new QHBoxLayout;
    toolbar->setSpacing(8);
    auto* selectAll = new QPushButton("全选");
    selectAll->setObjectName("selectAllEntries");
    auto* selectNone = new QPushButton("全不选");
    selectNone->setObjectName("clearEntries");
    auto* expandAll = new QPushButton("展开");
    expandAll->setObjectName("expandEntries");
    auto* collapseAll = new QPushButton("折叠");
    collapseAll->setObjectName("collapseEntries");
    for (auto* button : {selectAll, selectNone, expandAll, collapseAll})
    {
        toolbar->addWidget(button);
    }
    toolbar->addStretch(1);
    layout->addLayout(toolbar);

    auto* tree = new QTreeWidget;
    tree->setObjectName("entryTree");
    tree->setColumnCount(4);
    tree->setHeaderLabels({"名称", "类型", "大小", "修改时间"});
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 4; ++column)
    {
        tree->header()->setSectionResizeMode(column,
                                             QHeaderView::ResizeToContents);
    }
    layout->addWidget(tree, 1);

    auto* summary = makeHint(QString());
    layout->addWidget(summary);

    auto* box =
        new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    box->button(QDialogButtonBox::Ok)->setText("确定");
    box->button(QDialogButtonBox::Ok)->setObjectName("primaryButton");
    box->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(box);

    QHash<QString, QTreeWidgetItem*> nodes;
    for (const auto& info : listing)
    {
        const QString path = QString::fromStdString(info.path);
        const int slash = path.lastIndexOf('/');
        QTreeWidgetItem* parent =
            slash <= 0 ? nullptr : nodes.value(path.left(slash));
        auto* item = parent != nullptr ? new EntryItem(parent, info)
                                       : new EntryItem(tree, info);
        nodes.insert(path, item);
    }

    bool updating = false;
    auto updateSummary = [&]()
    {
        int selected = 0;
        int total = 0;
        int files = 0;
        uint64_t bytes = 0;
        for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it)
        {
            ++total;
            if ((*it)->checkState(0) != Qt::Checked)
            {
                continue;
            }
            ++selected;
            const auto* entry = static_cast<const EntryItem*>(*it);
            if (entry->type == "file")
            {
                ++files;
                bytes += entry->size;
            }
        }
        if (selected == 0)
        {
            summary->setText(total == 0 ? "目录为空：将只备份该目录本身。"
                                        : "未选择任何条目：将备份整个目录。");
            return;
        }
        summary->setText(QString("已选择 %1/%2 项：%3 个文件，合计 %4")
                             .arg(selected)
                             .arg(total)
                             .arg(files)
                             .arg(formatBytes(bytes)));
    };

    auto applyFilter = [&]()
    {
        const QString needle = nameFilter->text().trimmed();
        const QString type = typeFilter->currentData().toString();
        const int age = ageFilter->currentIndex();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        std::function<bool(QTreeWidgetItem*)> visit =
            [&](QTreeWidgetItem* item) -> bool
        {
            const auto* entry = static_cast<const EntryItem*>(item);
            bool matches = matchesType(entry->type, type) &&
                           matchesAge(entry->modified, age, now);
            if (matches && !needle.isEmpty())
            {
                matches = entry->name.contains(needle, Qt::CaseInsensitive) ||
                          entry->path.contains(needle, Qt::CaseInsensitive);
            }
            bool childVisible = false;
            for (int index = 0; index < item->childCount(); ++index)
            {
                childVisible |= visit(item->child(index));
            }
            const bool visible = matches || childVisible;
            item->setHidden(!visible);
            return visible;
        };
        for (int index = 0; index < tree->topLevelItemCount(); ++index)
        {
            visit(tree->topLevelItem(index));
        }
    };

    QObject::connect(tree, &QTreeWidget::itemChanged, &dialog,
                     [&](QTreeWidgetItem* item, int column)
                     {
                         if (column != 0 || updating)
                         {
                             return;
                         }
                         updating = true;
                         if (item->checkState(0) != Qt::PartiallyChecked)
                         {
                             setSubtreeState(item, item->checkState(0));
                         }
                         refreshAncestors(item);
                         updating = false;
                         updateSummary();
                     });
    QObject::connect(nameFilter, &QLineEdit::textChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(typeFilter, &QComboBox::currentIndexChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(ageFilter, &QComboBox::currentIndexChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(
        selectAll, &QPushButton::clicked, &dialog,
        [&]()
        {
            updating = true;
            for (int index = 0; index < tree->topLevelItemCount(); ++index)
            {
                setSubtreeState(tree->topLevelItem(index), Qt::Checked);
            }
            updating = false;
            updateSummary();
        });
    QObject::connect(
        selectNone, &QPushButton::clicked, &dialog,
        [&]()
        {
            updating = true;
            for (int index = 0; index < tree->topLevelItemCount(); ++index)
            {
                setSubtreeState(tree->topLevelItem(index), Qt::Unchecked);
            }
            updating = false;
            updateSummary();
        });
    QObject::connect(expandAll, &QPushButton::clicked, &dialog,
                     [&]() { tree->expandAll(); });
    QObject::connect(collapseAll, &QPushButton::clicked, &dialog,
                     [&]() { tree->collapseAll(); });

    tree->setSortingEnabled(true);
    tree->sortItems(0, Qt::AscendingOrder);

    // Re-opening the dialog keeps the previous selection.
    updating = true;
    for (const QString& path : *selection)
    {
        if (auto* item = nodes.value(path))
        {
            setSubtreeState(item, Qt::Checked);
            refreshAncestors(item);
        }
    }
    updating = false;
    updateSummary();

    if (dialog.exec() != QDialog::Accepted)
    {
        return false;
    }
    QStringList chosen;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
    {
        collectChecked(tree->topLevelItem(index), chosen);
    }
    // Every top-level entry selected is the same archive as "no selection".
    if (chosen.size() == tree->topLevelItemCount())
    {
        chosen.clear();
    }
    *selection = chosen;
    return true;
}
} // namespace backup::gui
