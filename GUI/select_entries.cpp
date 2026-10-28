#include "ui.hpp"

#include <QCheckBox>
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

// Category keys with their display labels; the table order is also the
// order of the category filter choices.
const std::pair<const char*, const char*> kCategoryLabels[] = {
    {"image", "图片"},   {"document", "文档"}, {"code", "代码"},
    {"video", "视频"},   {"audio", "音频"},    {"archive", "压缩包"},
    {"program", "程序"}, {"other", "其他"},
};

QString categoryLabel(const QString& category)
{
    for (const auto& [key, label] : kCategoryLabels)
    {
        if (category == key)
        {
            return QString::fromUtf8(label);
        }
    }
    // Directories and special entries have no content category.
    return {};
}

// Derives the content category of a regular file from its extension. Only
// regular files get one, so the category filter never touches directories,
// symlinks or special entries (a directory stays visible via its matches).
QString entryCategory(const QString& name, const QString& type)
{
    if (type != "file")
    {
        return {};
    }
    const int dot = name.lastIndexOf('.');
    if (dot <= 0)
    {
        return QStringLiteral("other");
    }
    const QString extension = name.mid(dot + 1).toLower();
    struct CategoryRow
    {
        const char* key;
        const char* extensions; // space separated, lowercase, no dots
    };
    static const CategoryRow kRows[] = {
        {"image", "jpg jpeg png gif bmp webp svg ico tif tiff"},
        {"document",
         "txt md pdf doc docx xls xlsx ppt pptx odt ods odp csv rtf"},
        {"code",
         "c h cc cpp cxx hpp py js jsx ts tsx java kt rs go rb php cs sh "
         "json xml yml yaml toml ini html css scss sql cmake mk"},
        {"video", "mp4 mkv avi mov wmv flv webm m4v mpg mpeg"},
        {"audio", "mp3 flac wav ogg aac m4a wma opus"},
        {"archive", "zip 7z rar tar gz tgz bz2 xz zst bak cab"},
        {"program", "exe msi com bat deb rpm appimage run dll so dylib"},
    };
    for (const auto& row : kRows)
    {
        const QStringList extensions =
            QString::fromLatin1(row.extensions).split(' ');
        if (extensions.contains(extension))
        {
            return QString::fromLatin1(row.key);
        }
    }
    return QStringLiteral("other");
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
            return category < right.category;
        }
        if (column == 3)
        {
            return size < right.size;
        }
        if (column == 4)
        {
            return modified < right.modified;
        }
        return name.localeAwareCompare(right.name) < 0;
    }

    QString path;
    QString name;
    QString type;
    QString category; // empty unless a regular file
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
        category = entryCategory(name, type);
        setText(0, name);
        setText(1, typeLabel(type));
        setText(2, categoryLabel(category));
        setText(3, size == 0 ? QString() : formatBytes(size));
        setText(4, modified == 0
                       ? QString()
                       : QDateTime::fromSecsSinceEpoch(modified).toString(
                             "yyyy-MM-dd HH:mm"));
        setTextAlignment(3, Qt::AlignRight | Qt::AlignVCenter);
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
    case 5:
        return modified >= now - 90 * day;
    case 6:
        return modified >= now - 365 * day;
    default:
        return true;
    }
}

bool matchesCategory(const QString& category, const QString& filter)
{
    return filter.isEmpty() || category == filter;
}

// Size bands apply to regular files only: every other type reports size 0
// and would otherwise pollute the empty-file band.
bool matchesSize(uint64_t size, const QString& type, int choice)
{
    if (choice == 0)
    {
        return true;
    }
    if (type != "file")
    {
        return false;
    }
    constexpr uint64_t kMiB = 1024 * 1024;
    switch (choice)
    {
    case 1:
        return size == 0;
    case 2:
        return size < kMiB;
    case 3:
        return size >= kMiB && size < 100 * kMiB;
    case 4:
        return size >= 100 * kMiB;
    default:
        return true;
    }
}

bool matchesHidden(const QString& name, bool showHidden)
{
    return showHidden || !name.startsWith('.');
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
    dialog.setMinimumSize(1020, 620);
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
    auto* categoryFilter = new QComboBox;
    categoryFilter->setObjectName("entryCategoryFilter");
    categoryFilter->addItem("全部类别", QString());
    for (const auto& [key, label] : kCategoryLabels)
    {
        categoryFilter->addItem(QString::fromUtf8(label),
                                QString::fromLatin1(key));
    }
    auto* sizeFilter = new QComboBox;
    sizeFilter->setObjectName("entrySizeFilter");
    sizeFilter->addItem("全部大小");
    sizeFilter->addItem("空文件");
    sizeFilter->addItem("小于 1 MB");
    sizeFilter->addItem("1 MB – 100 MB");
    sizeFilter->addItem("不小于 100 MB");
    auto* ageFilter = new QComboBox;
    ageFilter->setObjectName("entryAgeFilter");
    ageFilter->addItem("全部时间");
    ageFilter->addItem("最近 24 小时");
    ageFilter->addItem("最近 7 天");
    ageFilter->addItem("最近 30 天");
    ageFilter->addItem("30 天以前");
    ageFilter->addItem("最近 90 天");
    ageFilter->addItem("最近一年");
    filters->addWidget(nameFilter, 1);
    filters->addWidget(typeFilter);
    filters->addWidget(categoryFilter);
    filters->addWidget(sizeFilter);
    filters->addWidget(ageFilter);
    auto* showHidden = new QCheckBox("显示隐藏条目");
    showHidden->setObjectName("entryShowHidden");
    showHidden->setChecked(true);
    filters->addWidget(showHidden);
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
    auto* checkFiltered = new QPushButton("勾选筛选结果");
    checkFiltered->setObjectName("checkFilteredEntries");
    auto* uncheckFiltered = new QPushButton("取消勾选筛选结果");
    uncheckFiltered->setObjectName("uncheckFilteredEntries");
    for (auto* button : {selectAll, selectNone, checkFiltered, uncheckFiltered})
    {
        toolbar->addWidget(button);
    }
    toolbar->addStretch(1);
    toolbar->addWidget(expandAll);
    toolbar->addWidget(collapseAll);
    layout->addLayout(toolbar);

    auto* tree = new QTreeWidget;
    tree->setObjectName("entryTree");
    tree->setColumnCount(5);
    tree->setHeaderLabels({"名称", "类型", "类别", "大小", "修改时间"});
    tree->setAlternatingRowColors(true);
    tree->setUniformRowHeights(true);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int column = 1; column < 5; ++column)
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
    QObject::connect(box, &QDialogButtonBox::accepted, &dialog,
                     &QDialog::accept);
    QObject::connect(box, &QDialogButtonBox::rejected, &dialog,
                     &QDialog::reject);
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
        box->button(QDialogButtonBox::Ok)
            ->setEnabled(selected != 0 || total == 0);
        if (selected == 0)
        {
            summary->setText(total == 0 ? "目录为空：将只备份该目录本身。"
                                        : "请选择需要备份的条目。");
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
        const QString category = categoryFilter->currentData().toString();
        const int sizeBand = sizeFilter->currentIndex();
        const int age = ageFilter->currentIndex();
        const bool hiddenShown = showHidden->isChecked();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        std::function<bool(QTreeWidgetItem*)> visit =
            [&](QTreeWidgetItem* item) -> bool
        {
            const auto* entry = static_cast<const EntryItem*>(item);
            if (!matchesHidden(entry->name, hiddenShown))
            {
                // A hidden name drops the whole subtree, matches or not.
                item->setHidden(true);
                return false;
            }
            bool matches = matchesType(entry->type, type) &&
                           matchesCategory(entry->category, category) &&
                           matchesSize(entry->size, entry->type, sizeBand) &&
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

    // Applies a check state to the visible entries. Directories are never
    // forced checked: a checked directory archives its whole subtree, which
    // would silently pull in entries the filter just hid.
    const auto setFilteredState = [&](Qt::CheckState state)
    {
        updating = true;
        for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it)
        {
            auto* item = *it;
            if (item->isHidden())
            {
                continue;
            }
            const bool directory =
                static_cast<EntryItem*>(item)->type == "directory";
            if (state == Qt::Unchecked || !directory)
            {
                item->setCheckState(0, state);
            }
        }
        for (QTreeWidgetItemIterator it(tree); *it != nullptr; ++it)
        {
            refreshAncestors(*it);
        }
        updating = false;
        updateSummary();
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
    QObject::connect(categoryFilter, &QComboBox::currentIndexChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(sizeFilter, &QComboBox::currentIndexChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(ageFilter, &QComboBox::currentIndexChanged, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(showHidden, &QCheckBox::toggled, &dialog,
                     [&]() { applyFilter(); });
    QObject::connect(checkFiltered, &QPushButton::clicked, &dialog,
                     [&]() { setFilteredState(Qt::Checked); });
    QObject::connect(uncheckFiltered, &QPushButton::clicked, &dialog,
                     [&]() { setFilteredState(Qt::Unchecked); });
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

    // Empty means the whole tree, including when the dialog is first opened.
    updating = true;
    if (selection->isEmpty())
    {
        for (int index = 0; index < tree->topLevelItemCount(); ++index)
        {
            setSubtreeState(tree->topLevelItem(index), Qt::Checked);
        }
    }
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
    bool wholeTree = true;
    for (int index = 0; index < tree->topLevelItemCount(); ++index)
    {
        auto* item = tree->topLevelItem(index);
        wholeTree = wholeTree && item->checkState(0) == Qt::Checked;
        collectChecked(item, chosen);
    }
    // Every top-level entry selected is the same archive as "no selection".
    if (wholeTree)
    {
        chosen.clear();
    }
    *selection = chosen;
    return true;
}
} // namespace backup::gui
