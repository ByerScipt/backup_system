#include "ui.hpp"
#include <QClipboard>

#include <QApplication>
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

namespace backup::gui {

Page makePage() {
    Page page;
    page.widget = new QWidget;
    auto* outerLayout = new QVBoxLayout(page.widget);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(0);

    auto* scroll = new QScrollArea;
    scroll->setObjectName("pageScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    auto* content = new QWidget;
    content->setObjectName("pageContent");
    page.layout = new QVBoxLayout(content);
    page.layout->setContentsMargins(0, 0, 0, 0);
    page.layout->setSpacing(14);
    page.layout->setSizeConstraint(QLayout::SetMinimumSize);
    scroll->setWidget(content);
    outerLayout->addWidget(scroll);
    return page;
}

Card makeCard(const QString& title, const QString& description) {
    Card card;
    card.frame = new QFrame;
    card.frame->setObjectName("card");
    card.body = new QVBoxLayout(card.frame);
    card.body->setContentsMargins(20, 18, 20, 18);
    card.body->setSpacing(12);

    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName("cardTitle");
    card.body->addWidget(titleLabel);

    if (!description.isEmpty()) {
        auto* descriptionLabel = new QLabel(description);
        descriptionLabel->setObjectName("cardDescription");
        descriptionLabel->setWordWrap(true);
        card.body->addWidget(descriptionLabel);
    }
    return card;
}

QFormLayout* makeForm() {
    auto* form = new QFormLayout;
    form->setContentsMargins(0, 4, 0, 0);
    form->setHorizontalSpacing(18);
    form->setVerticalSpacing(11);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return form;
}

QLabel* makeHint(const QString& text) {
    auto* hint = new QLabel(text);
    hint->setObjectName("hint");
    hint->setWordWrap(true);
    return hint;
}

QHBoxLayout* pathRow(QLineEdit*& edit, QPushButton*& browse, const QString& placeholder) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    edit = new QLineEdit;
    edit->setClearButtonEnabled(true);
    edit->setPlaceholderText(placeholder);
    browse = new QPushButton("浏览");
    browse->setObjectName("secondaryButton");
    browse->setMinimumWidth(82);
    row->addWidget(edit, 1);
    row->addWidget(browse);
    return row;
}

JobControls addJobControls(QVBoxLayout* layout, const QString& startText) {
    JobControls controls;

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    controls.start = new QPushButton(startText);
    controls.start->setObjectName("primaryButton");
    controls.start->setMinimumWidth(170);
    controls.cancel = new QPushButton("取消任务");
    controls.cancel->setObjectName("dangerButton");
    controls.cancel->setEnabled(false);
    auto* copyLog = new QPushButton("复制日志");
    copyLog->setObjectName("quietButton");
    auto* clearLog = new QPushButton("清空");
    clearLog->setObjectName("quietButton");

    buttons->addWidget(controls.start);
    buttons->addWidget(controls.cancel);
    buttons->addStretch();
    buttons->addWidget(copyLog);
    buttons->addWidget(clearLog);
    layout->addLayout(buttons);

    controls.progress = new QProgressBar;
    controls.progress->setRange(0, 100);
    controls.progress->setValue(0);
    controls.progress->setFormat("等待开始");
    layout->addWidget(controls.progress);

    controls.log = new QTextEdit;
    controls.log->setObjectName("taskLog");
    controls.log->setReadOnly(true);
    controls.log->setAcceptRichText(false);
    controls.log->setMinimumHeight(72);
    controls.log->setPlaceholderText("任务日志将在这里显示");
    controls.log->document()->setMaximumBlockCount(800);
    layout->addWidget(controls.log, 1);

    QObject::connect(copyLog, &QPushButton::clicked, controls.log, [log = controls.log]() {
        QApplication::clipboard()->setText(log->toPlainText());
    });
    QObject::connect(clearLog, &QPushButton::clicked, controls.log, &QTextEdit::clear);
    return controls;
}

void startJob(QWidget* owner, const JobControls& controls, Job job,
              std::function<void()> afterSuccess) {
    if (owner->property("jobRunning").toBool()) {
        QMessageBox::information(owner, "Task Running", "请等待当前任务完成，或先取消当前任务。");
        return;
    }

    owner->setProperty("jobRunning", true);
    controls.start->setEnabled(false);
    controls.cancel->setEnabled(true);
    controls.progress->setValue(0);
    controls.progress->setFormat("正在准备");
    controls.log->clear();
    appendLog(controls.log, "任务开始");

    auto cancelled = std::make_shared<std::atomic_bool>(false);
    controls.cancel->disconnect();
    QObject::connect(controls.cancel, &QPushButton::clicked, owner,
                     [cancelled, cancelButton = controls.cancel, log = controls.log]() {
                         cancelled->store(true);
                         cancelButton->setEnabled(false);
                         appendLog(log, "已请求取消，正在安全结束当前阶段");
                     });

    QPointer<QWidget> safeOwner(owner);
    QPointer<QProgressBar> safeProgress(controls.progress);
    QPointer<QTextEdit> safeLog(controls.log);
    QPointer<QPushButton> safeStart(controls.start);
    QPointer<QPushButton> safeCancel(controls.cancel);

    auto progress = [safeOwner, safeProgress, safeLog](const ProgressEvent& event) {
        if (!safeOwner)
            return;
        const QString stage = stageName(QString::fromStdString(event.stage));
        const QString detail = QString::fromStdString(event.detail);
        const uint64_t completed = event.completed;
        const uint64_t total = event.total;
        QMetaObject::invokeMethod(
            safeOwner,
            [safeProgress, safeLog, stage, detail, completed, total]() {
                if (!safeProgress || !safeLog)
                    return;
                if (total != 0) {
                    const long double ratio = static_cast<long double>(completed) * 100.0L /
                                              static_cast<long double>(total);
                    safeProgress->setValue(static_cast<int>(std::clamp(ratio, 0.0L, 100.0L)));
                    safeProgress->setFormat(stage + "  %p%");
                } else {
                    safeProgress->setFormat(stage);
                }
                if (!detail.isEmpty() && (completed == 0 || completed == total)) {
                    appendLog(safeLog, stage + " · " + detail);
                }
            },
            Qt::QueuedConnection);
    };

    QThread* thread = QThread::create([=]() {
        QString message;
        bool ok = false;
        try {
            message = job(cancelled.get(), progress);
            ok = true;
        } catch (const std::exception& error) {
            message = QString::fromUtf8(error.what());
        } catch (...) {
            message = "未知错误";
        }

        if (!safeOwner)
            return;
        QMetaObject::invokeMethod(
            safeOwner,
            [=]() {
                if (!safeOwner || !safeProgress || !safeLog || !safeStart || !safeCancel) {
                    return;
                }
                safeStart->setEnabled(true);
                safeCancel->setEnabled(false);
                safeOwner->setProperty("jobRunning", false);
                safeProgress->setValue(ok ? 100 : 0);
                safeProgress->setFormat(ok ? "任务完成" : "任务失败");
                appendLog(safeLog, (ok ? "✓  " : "✕  ") + message);
                if (ok && afterSuccess)
                    afterSuccess();
                if (!ok) {
                    QMessageBox::critical(safeOwner, "Task Failed", message);
                }
            },
            Qt::QueuedConnection);
    });
    QObject::connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

QString stageName(const QString& stage) {
    if (stage == "scan")
        return "扫描目录";
    if (stage == "pack")
        return "写入归档";
    if (stage == "compress-rle")
        return "RLE 压缩";
    if (stage == "decompress-rle")
        return "RLE 解压";
    if (stage == "huffman-count")
        return "Huffman 统计";
    if (stage == "compress-huffman")
        return "Huffman 压缩";
    if (stage == "decompress-huffman")
        return "Huffman 解压";
    if (stage == "encrypt")
        return "加密载荷";
    if (stage == "decrypt")
        return "解密载荷";
    if (stage == "extract")
        return "提取文件";
    if (stage == "restore-entry")
        return "恢复元数据";
    if (stage == "upload")
        return "上传归档";
    if (stage == "download")
        return "下载归档";
    if (stage == "conflict-preview")
        return "冲突预检";
    return stage;
}

QString formatBytes(uint64_t bytes) {
    static constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double value = static_cast<double>(bytes);
    size_t unit = 0;
    while (value >= 1024.0 && unit + 1 < std::size(units)) {
        value /= 1024.0;
        ++unit;
    }
    const int precision = unit == 0 ? 0 : (value >= 100.0 ? 0 : 1);
    return QString("%1 %2").arg(QString::number(value, 'f', precision), units[unit]);
}

void appendLog(QTextEdit* log, const QString& text) {
    log->append(QString("[%1]  %2").arg(QDateTime::currentDateTime().toString("HH:mm:ss"), text));
}

} // namespace backup::gui
