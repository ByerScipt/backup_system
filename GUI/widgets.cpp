#include "ui.hpp"
#include <QClipboard>

#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QString>
#include <QTextEdit>
#include <QThread>
#include <QVBoxLayout>
#include <QWidget>
#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <iterator>
#include <memory>

namespace backup::gui
{

namespace
{
// One guard per top-level window: closing cancels every page's job and leaves
// the event loop alive until workers have cleaned up their temporary files.
class JobWindowGuard final : public QObject
{
public:
    explicit JobWindowGuard(QWidget* window) : QObject(window), window_(window)
    {
        window->installEventFilter(this);
        connect(qApp, &QCoreApplication::aboutToQuit, this,
                [this]() { joinAll(); });
    }
    ~JobWindowGuard() override
    {
        joinAll();
    }

    void add(QThread* thread, std::shared_ptr<std::atomic_bool> cancelled)
    {
        jobs_.push_back({thread, std::move(cancelled)});
        connect(thread, &QThread::finished, this,
                [this, thread]()
                {
                    thread->wait();
                    jobs_.erase(std::remove_if(jobs_.begin(), jobs_.end(),
                                               [thread](const ActiveJob& job) {
                                                   return job.thread == thread;
                                               }),
                                jobs_.end());
                    if (closePending_ && jobs_.empty())
                    {
                        window_->close();
                    }
                });
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (watched == window_ && event->type() == QEvent::Close &&
            !jobs_.empty())
        {
            closePending_ = true;
            for (const auto& job : jobs_)
            {
                job.cancelled->store(true);
            }
            static_cast<QCloseEvent*>(event)->ignore();
            return true;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    struct ActiveJob
    {
        QThread* thread;
        std::shared_ptr<std::atomic_bool> cancelled;
    };
    void joinAll()
    {
        for (const auto& job : jobs_)
        {
            job.cancelled->store(true);
        }
        for (const auto& job : jobs_)
        {
            job.thread->wait();
        }
    }
    QWidget* window_;
    bool closePending_ = false;
    std::vector<ActiveJob> jobs_;
};

JobWindowGuard* jobGuard(QWidget* window)
{
    for (QObject* child : window->children())
    {
        if (auto* guard = dynamic_cast<JobWindowGuard*>(child))
        {
            return guard;
        }
    }
    return new JobWindowGuard(window);
}
} // namespace

Page makePage()
{
    Page page;
    page.widget = new QWidget;
    auto* outerLayout = new QVBoxLayout(page.widget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(20);

    auto* scroll = new QScrollArea;
    scroll->setObjectName("pageScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    content->setObjectName("pageContent");
    page.layout = new QVBoxLayout(content);
    page.layout->setContentsMargins(0, 0, 0, 0);
    page.layout->setSpacing(20);
    page.layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    outerLayout->addWidget(scroll, 1);
    page.footer = new QVBoxLayout;
    outerLayout->addLayout(page.footer);
    return page;
}

Card makeCard()
{
    Card card;
    card.frame = new QFrame;
    card.frame->setObjectName("card");
    card.body = new QVBoxLayout(card.frame);
    card.body->setContentsMargins(28, 24, 28, 24);
    card.body->setSpacing(16);
    card.body->setAlignment(Qt::AlignVCenter);
    return card;
}

QFormLayout* makeForm()
{
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 0, 0, 0);
    form->setHorizontalSpacing(28);
    form->setVerticalSpacing(22);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFormAlignment(Qt::AlignVCenter);
    return form;
}

QLabel* makeHint(const QString& text)
{
    auto* hint = new QLabel(text);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    return hint;
}

QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse,
                     const QString& objectName)
{
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(12);
    edit = new QLineEdit;
    edit->setClearButtonEnabled(true);
    edit->setObjectName(objectName);
    browse = new QPushButton("浏览");
    browse->setObjectName("secondaryButton");
    browse->setMinimumWidth(100);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    return row;
}

JobControls addJobControls(QVBoxLayout* layout, const QString& startText)
{
    JobControls controls;

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    controls.start = new QPushButton(startText);
    controls.start->setObjectName("primaryButton");
    controls.start->setMinimumWidth(160);
    controls.cancel = new QPushButton("取消");
    controls.cancel->setObjectName("dangerButton");
    controls.cancel->setEnabled(false);
    controls.cancel->hide();
    auto* toggleLog = new QPushButton("日志");
    toggleLog->setObjectName("toggleLog");
    toggleLog->setCheckable(true);
    auto* copyLog = new QPushButton("复制");
    copyLog->setObjectName("quietButton");
    auto* clearLog = new QPushButton("清空");
    clearLog->setObjectName("quietButton");
    copyLog->hide();
    clearLog->hide();

    buttons->addWidget(controls.start);
    buttons->addWidget(controls.cancel);
    buttons->addStretch();
    buttons->addWidget(copyLog);
    buttons->addWidget(clearLog);
    buttons->addWidget(toggleLog);
    layout->addLayout(buttons);

    controls.progress = new QProgressBar;
    controls.progress->setRange(0, 100);
    controls.progress->setValue(0);
    controls.progress->hide();
    layout->addWidget(controls.progress);

    controls.log = new QTextEdit;
    controls.log->setObjectName("taskLog");
    controls.log->setReadOnly(true);
    controls.log->setAcceptRichText(false);
    controls.log->setMinimumHeight(110);
    controls.log->setMaximumHeight(200);
    controls.log->hide();
    controls.log->document()->setMaximumBlockCount(800);
    layout->addWidget(controls.log);

    QObject::connect(toggleLog, &QPushButton::toggled, controls.log,
                     [log = controls.log, copyLog, clearLog](bool visible)
                     {
                         log->setVisible(visible);
                         copyLog->setVisible(visible);
                         clearLog->setVisible(visible);
                     });
    QObject::connect(copyLog, &QPushButton::clicked, controls.log,
                     [log = controls.log]() {
                         QApplication::clipboard()->setText(log->toPlainText());
                     });
    QObject::connect(clearLog, &QPushButton::clicked, controls.log,
                     &QTextEdit::clear);
    return controls;
}

void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess)
{
    if (owner->property("jobRunning").toBool())
    {
        QMessageBox::information(owner, "任务进行中",
                                 "请等待当前任务完成，或先取消当前任务。");
        return;
    }

    owner->setProperty("jobRunning", true);
    controls.start->setEnabled(false);
    controls.cancel->setEnabled(true);
    controls.cancel->show();
    controls.progress->show();
    controls.progress->setValue(0);
    controls.progress->setFormat("正在准备");
    controls.log->clear();
    appendLog(controls.log, "任务开始");

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    controls.cancel->disconnect();
    QObject::connect(
        controls.cancel, &QPushButton::clicked, owner,
        [cancelled, cancelButton = controls.cancel, log = controls.log]()
        {
            cancelled->store(true);
            cancelButton->setEnabled(false);
            appendLog(log, "已请求取消，正在安全结束当前阶段");
        });

    QPointer<QWidget> safeOwner(owner);
    QPointer<QProgressBar> safeProgress(controls.progress);
    QPointer<QTextEdit> safeLog(controls.log);
    QPointer<QPushButton> safeStart(controls.start);
    QPointer<QPushButton> safeCancel(controls.cancel);

    auto progress =
        [safeOwner, safeProgress, safeLog](const ProgressEvent& event)
    {
        if (!safeOwner)
        {
            return;
        }
        const QString stage = stageName(QString::fromStdString(event.stage));
        const QString detail = QString::fromStdString(event.detail);
        const uint64_t completed = event.completed;
        const uint64_t total = event.total;
        QMetaObject::invokeMethod(
            safeOwner,
            [safeProgress, safeLog, stage, detail, completed, total]()
            {
                if (!safeProgress || !safeLog)
                {
                    return;
                }
                if (total != 0)
                {
                    const long double ratio =
                        static_cast<long double>(completed) * 100.0L /
                        static_cast<long double>(total);
                    safeProgress->setValue(
                        static_cast<int>(std::clamp(ratio, 0.0L, 100.0L)));
                    safeProgress->setFormat(stage + "  %p%");
                }
                else
                {
                    safeProgress->setFormat(stage);
                }
                if (!detail.isEmpty() && (completed == 0 || completed == total))
                {
                    appendLog(safeLog, stage + " · " + detail);
                }
            },
            Qt::QueuedConnection);
    };

    QThread* thread = QThread::create(
        [=]()
        {
            QString message;
            bool ok = false;
            try
            {
                message = job(cancelled.get(), progress);
                ok = true;
            }
            catch (const std::exception& error)
            {
                message = QString::fromUtf8(error.what());
            }
            catch (...)
            {
                message = "未知错误";
            }

            if (!safeOwner)
            {
                return;
            }
            QMetaObject::invokeMethod(
                safeOwner,
                [=]()
                {
                    if (!safeOwner || !safeProgress || !safeLog || !safeStart ||
                        !safeCancel)
                    {
                        return;
                    }
                    safeStart->setEnabled(true);
                    safeCancel->setEnabled(false);
                    safeCancel->hide();
                    safeOwner->setProperty("jobRunning", false);
                    safeProgress->setValue(ok ? 100 : 0);
                    const bool wasCancelled = !ok && cancelled->load();
                    safeProgress->setFormat(
                        ok ? "任务完成"
                           : (wasCancelled ? "任务已取消" : "任务失败"));
                    appendLog(safeLog, (ok ? "✓  " : "✕  ") + message);
                    if (ok && afterSuccess)
                    {
                        afterSuccess();
                    }
                    if (!ok && !wasCancelled)
                    {
                        QMessageBox::critical(safeOwner, "任务失败", message);
                    }
                },
                Qt::QueuedConnection);
        });
    jobGuard(owner->window())->add(thread, cancelled);
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

QString stageName(const QString& stage)
{
    static const std::pair<const char*, const char*> names[] = {
        {"scan", "扫描目录"},
        {"pack", "写入归档"},
        {"compress-rle", "RLE 压缩"},
        {"decompress-rle", "RLE 解压"},
        {"huffman-count", "Huffman 统计"},
        {"compress-huffman", "Huffman 压缩"},
        {"decompress-huffman", "Huffman 解压"},
        {"encrypt", "加密载荷"},
        {"decrypt", "解密载荷"},
        {"extract", "提取文件"},
        {"restore-entry", "恢复元数据"},
        {"network-upload", "上传归档"},
        {"network-download", "下载归档"},
        {"conflict-preview", "冲突预检"},
        {"compress-copy", "复制未压缩数据"},
        {"decompress-copy", "复制未压缩数据"},
        {"read-archive", "读取归档"},
        {"finalize", "生成最终归档"}};
    for (const auto& name : names)
    {
        if (stage == name.first)
        {
            return QString::fromUtf8(name.second);
        }
    }
    return stage;
}

QString formatBytes(uint64_t bytes)
{
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units))
    {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value >= 100.0 ? 0 : 1);
    return QString("%1 %2").arg(QString::number(value, 'f', precision),
                                units[unit]);
}

void appendLog(QTextEdit* log, const QString& text)
{
    log->append(
        QString("[%1]  %2")
            .arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

} // namespace backup::gui
