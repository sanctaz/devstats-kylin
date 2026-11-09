#include "diagnostics_panel.h"
#include "charts.h"
#include "process_actions.h"
#include "ui_components.h"
#include "ui_theme.h"

#include <QAbstractTextDocumentLayout>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QTextBrowser>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using DevStatsUI::card;
using DevStatsUI::label;

namespace {
constexpr int KeyRole = Qt::UserRole, ProcessRole = Qt::UserRole + 1;
const QString None = QString::fromUtf8("—");

double n(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 ? v.toDouble() : -1; }
QString number(const QJsonValue &v, int decimals = 1) { return n(v) < 0 ? None : QString::number(n(v), 'f', decimals); }
QString cpuText(const QJsonValue &v) { return n(v) < 0 ? None : QString::number(n(v), 'f', 1) + '%'; }
QString milliseconds(const QJsonValue &v) {
    if (n(v) < 0) return None;
    return n(v) >= 1000 ? QString::number(n(v) / 1000, 'f', 1) + QStringLiteral(" 秒") : QString::number(n(v), 'f', 0) + QStringLiteral(" 毫秒");
}
QString eventTime(const QJsonValue &value, bool compact = false) {
    if (n(value) < 0) return None;
    const auto date = QDateTime::fromMSecsSinceEpoch(qint64(value.toDouble() * 1000)).toLocalTime();
    if (!date.isValid()) return None;
    return date.toString(compact ? "MM-dd HH:mm:ss" : "yyyy-MM-dd HH:mm:ss");
}
QString eventDuration(const QJsonValue &value) {
    if (n(value) < 0) return None;
    const qint64 seconds = qint64(value.toDouble());
    if (seconds < 60) return QStringLiteral("%1 秒").arg(seconds);
    if (seconds < 3600) return QStringLiteral("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
    return QStringLiteral("%1 小时 %2 分").arg(seconds / 3600).arg(seconds / 60 % 60);
}
QString kindCaptions(const QJsonArray &kinds) {
    const QMap<QString, QString> captions{{"cpu", QStringLiteral("CPU")}, {"memory", QStringLiteral("内存")}, {"io", QStringLiteral("磁盘")},
        {"oom", QStringLiteral("内存耗尽")}, {"responsiveness", QStringLiteral("调度")}, {"display", QStringLiteral("显示")}};
    QStringList text;
    for (const auto &kind : kinds) if (captions.contains(kind.toString())) text << captions.value(kind.toString());
    return text.isEmpty() ? QStringLiteral("未知") : text.join(QStringLiteral(" · "));
}
QPair<QString, QString> eventStatus(const QString &status) {
    if (status == "active") return {QStringLiteral("进行中"), QStringLiteral("warn")};
    if (status == "recovered") return {QStringLiteral("已恢复"), QStringLiteral("ok")};
    if (status == "interrupted") return {QStringLiteral("已中断"), QStringLiteral("neutral")};
    return {QStringLiteral("未知"), QStringLiteral("neutral")};
}
QString strongestText(const QJsonObject &event) {
    const auto app = event.value("strongestApplication").toObject();
    if (app.isEmpty()) return None;
    const QString basis = event.value("strongestApplicationBasis").toString();
    QString detail;
    if (basis == "cpu") detail = cpuText(app.value("cpuPercent"));
    else if (basis == "memory") detail = formatBytes(n(app.value("memoryBytes")));
    else if (basis == "io") detail = formatBytes(n(app.value("ioBytesPerSecond")), true);
    return app.value("name").toString(QStringLiteral("未知")) + (detail.isEmpty() || detail == None ? QString() : QStringLiteral(" · ") + detail);
}
QString esc(const QString &text) { return text.toHtmlEscaped(); }
QString sampleRow(const QString &title, const QJsonObject &sample) {
    const auto cpu = sample.value("cpu").toObject(), memory = sample.value("memory").toObject();
    const auto probe = sample.value("responsiveness").toObject(), disk = sample.value("disk").toObject(), vm = sample.value("vm").toObject();
    const double swap = n(vm.value("swapInBytesPerSecond")) >= 0 || n(vm.value("swapOutBytesPerSecond")) >= 0
        ? std::max(0., n(vm.value("swapInBytesPerSecond"))) + std::max(0., n(vm.value("swapOutBytesPerSecond"))) : -1;
    return QStringLiteral("<tr><td>%1</td><td align=right>%2</td><td align=right>%3</td><td align=right>%4</td><td align=right>%5</td><td align=right>%6</td></tr>")
        .arg(esc(title), formatPercent(n(cpu.value("usage"))),
             formatBytes(n(memory.value("availableBytes"))), formatBytes(swap, true),
             disk.isEmpty() ? formatPercent(n(cpu.value("iowait"))) : formatPercent(n(disk.value("busiestUtilization"))),
             milliseconds(probe.value("schedulerMaxMs")));
}
// Rich text rather than a text dump: an event should read like a short report.
QString eventHtml(const QJsonObject &event, const DevStatsUI::Colors &c) {
    QString html = QStringLiteral("<style>h4{margin:10px 0 3px 0;font-size:10.5pt;} td{padding:1px 8px 1px 0;} th{padding:1px 8px 1px 0;font-weight:500;color:%1;} p{margin:2px 0;} .m{color:%1;}</style>").arg(c.muted.name());
    html += QStringLiteral("<p><b style='font-size:12pt'>%1</b></p>").arg(esc(event.value("summary").toString(QStringLiteral("资源压力事件"))));
    if (!event.value("headlineEvidence").toString().isEmpty()) html += QStringLiteral("<p>%1</p>").arg(esc(event.value("headlineEvidence").toString()));
    html += QStringLiteral("<p class=m>%1 · 持续 %2 · %3%4</p>").arg(eventTime(event.value("startTimestamp")), eventDuration(event.value("durationSeconds")),
        eventStatus(event.value("status").toString()).first, event.value("episodeCount").toInt() > 1 ? QStringLiteral(" · 合并 %1 段").arg(event.value("episodeCount").toInt()) : QString());
    if (!event.value("interruptionReason").toString().isEmpty()) html += QStringLiteral("<p class=m>%1</p>").arg(esc(event.value("interruptionReason").toString()));

    const QMap<QString, QString> labels{{"onset", QStringLiteral("开始时")}, {"peakCPU", QStringLiteral("CPU 峰值")}, {"peakMemoryPressure", QStringLiteral("内存峰值")},
        {"peakIO", QStringLiteral("磁盘峰值")}, {"peakLatency", QStringLiteral("延迟峰值")}, {"last", QStringLiteral("最后")}, {"oom", QStringLiteral("OOM")}};
    const auto witnesses = event.value("witnesses").toArray();
    if (!witnesses.isEmpty()) {
        html += QStringLiteral("<h4>关键时刻</h4><table cellspacing=0><tr><th align=left>时刻</th><th align=right>CPU</th><th align=right>可用内存</th><th align=right>换页</th><th align=right>磁盘</th><th align=right>调度延迟</th></tr>");
        for (const auto &value : witnesses) {
            const auto sample = value.toObject();
            QStringList marks;
            for (const auto &mark : sample.value("labels").toArray()) if (labels.contains(mark.toString())) marks << labels.value(mark.toString());
            const auto at = QDateTime::fromMSecsSinceEpoch(qint64(sample.value("timestamp").toDouble() * 1000)).toLocalTime().toString("HH:mm:ss");
            html += sampleRow(at + (marks.isEmpty() ? QString() : QStringLiteral(" · ") + marks.join('/')), sample);
        }
        html += QStringLiteral("</table>");
    }
    // Findings seen across the event, most useful first and without repeats.
    QStringList seen;
    QString findings;
    for (const auto *section : {"witnesses", "prelude"}) for (const auto &value : event.value(section).toArray()) {
        for (const auto &entry : value.toObject().value("findings").toArray()) {
            const auto finding = entry.toObject();
            const QString title = finding.value("title").toString();
            if (title.isEmpty() || seen.contains(title) || seen.size() >= 6) continue;
            seen << title;
            findings += QStringLiteral("<p><b>%1</b><br><span class=m>%2</span></p>").arg(esc(title), esc(finding.value("evidence").toString()));
        }
    }
    if (!findings.isEmpty()) html += QStringLiteral("<h4>发现</h4>") + findings;
    // Who was busy: the peak witness per resource, merged by process name.
    QJsonObject peak;
    for (const auto &value : witnesses) if (!value.toObject().value("topProcesses").toArray().isEmpty()) peak = value.toObject();
    for (const auto &value : witnesses) {
        const auto sample = value.toObject();
        for (const auto &mark : sample.value("labels").toArray()) if (mark.toString().startsWith("peak") && !sample.value("topProcesses").toArray().isEmpty()) peak = sample;
    }
    const auto rows = peak.value("topProcesses").toArray();
    if (!rows.isEmpty()) {
        html += QStringLiteral("<h4>当时最忙的进程</h4><table cellspacing=0><tr><th align=left>进程</th><th align=left>用户</th><th align=right>PID</th><th align=right>CPU</th><th align=right>内存</th></tr>");
        for (const auto &value : rows) {
            const auto p = value.toObject();
            html += QStringLiteral("<tr><td>%1</td><td>%2</td><td align=right>%3</td><td align=right>%4</td><td align=right>%5</td></tr>")
                .arg(esc(p.value("name").toString(QStringLiteral("未知"))), esc(p.value("owner").toString(QStringLiteral("我"))), number(p.value("pid"), 0), cpuText(p.value("cpuPercent")), formatBytes(n(p.value("memoryBytes"))));
        }
        html += QStringLiteral("</table><p class=m>PID 是当时的记录，现在可能已被复用，不能据此结束进程。</p>");
    }
    return html;
}
void setRowAlignment(QTreeWidgetItem *item, int columns) {
    for (int column = 1; column < columns; ++column) item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
}
}

struct DiagnosticsPanel::Impl {
    DiagnosticsPanel *q;
    QJsonObject snapshot, histories, incident;
    QJsonArray incidents, renderedFindings;
    QFrame *banner = nullptr;
    QLabel *bannerPill = nullptr, *headline = nullptr, *summary = nullptr, *feedback = nullptr;
    QTimer *feedbackTimer = nullptr;
    QPushButton *runButton = nullptr, *cancelRun = nullptr, *reliefButton = nullptr;
    QProgressBar *runProgress = nullptr;
    SectionTabs *sections = nullptr;
    QVector<QWidget *> sectionPages;
    QGridLayout *meterGrid = nullptr;
    QVector<MeterTile *> meters;
    int meterColumns = 0;
    QVBoxLayout *findingRows = nullptr;
    QLabel *findingsTitle = nullptr, *suspectHint = nullptr, *coverage = nullptr, *actionHint = nullptr;
    QToolButton *closeRun = nullptr;
    QWidget *runChecksHost = nullptr;
    QGridLayout *runChecks = nullptr;
    QVector<QLabel *> checkPills;
    int checkColumns = 6;
    QTreeWidget *apps = nullptr;
    QLineEdit *search = nullptr, *incidentSearch = nullptr;
    QComboBox *sort = nullptr, *incidentRange = nullptr;
    QPushButton *terminate = nullptr, *forceKill = nullptr, *addRule = nullptr;
    ToggleSwitch *autoRecord = nullptr;
    QTreeWidget *incidentList = nullptr;
    QGridLayout *bannerGrid = nullptr;
    QWidget *bannerActions = nullptr;
    bool bannerNarrow = false;
    QTextBrowser *incidentDetail = nullptr;
    QLabel *incidentState = nullptr, *incidentError = nullptr;
    QPushButton *exportIncident = nullptr;
    QString selectedIncidentId;
    bool incidentsReceived = false, incidentsDirty = true, detailDirty = false, detailReceived = false;
    QHash<QString, HistoryChart *> charts;
    qint64 historyStart = 0, historyEnd = 0;
    bool dirty = true, runDismissed = false, findingsShown = false;
    QJsonObject renderedRun;
    QString lastRunState = QStringLiteral("idle");

    explicit Impl(DiagnosticsPanel *widget) : q(widget) {}

    void layoutMeters(int width) {
        const int columns = width >= 1040 ? 6 : width >= 520 ? 3 : 2;
        if (columns == meterColumns) return;
        meterColumns = columns;
        for (int i = 0; i < meters.size(); ++i) {
            meterGrid->removeWidget(meters[i]);
            meterGrid->addWidget(meters[i], i / columns, i % columns);
        }
        for (int i = 0; i < 6; ++i) meterGrid->setColumnStretch(i, i < columns ? 1 : 0);
    }
    // Below this width the buttons would squeeze the headline into a narrow column.
    void layoutBanner(int width) {
        const bool narrow = width < 680;
        if (narrow == bannerNarrow) return;
        bannerNarrow = narrow;
        bannerGrid->removeWidget(bannerActions);
        if (narrow) bannerGrid->addWidget(bannerActions, 1, 1, Qt::AlignLeft);
        else bannerGrid->addWidget(bannerActions, 0, 2, Qt::AlignTop);
    }
    void layoutChecks(int width) {
        const int columns = width >= 620 ? 6 : 3;
        if (columns == checkColumns && !checkPills.isEmpty() && runChecks->itemAtPosition(0, 0)) return;
        checkColumns = columns;
        for (int i = 0; i < checkPills.size(); ++i) {
            runChecks->removeWidget(checkPills[i]);
            runChecks->addWidget(checkPills[i], i / columns, i % columns, Qt::AlignLeft);
        }
        runChecks->setColumnStretch(6, 1);
    }
    QWidget *findingRow(const QJsonObject &finding, bool first) {
        auto *row = new QFrame;
        row->setProperty("rowItem", true);
        row->setProperty("first", first);
        auto *layout = new QHBoxLayout(row);
        layout->setContentsMargins(0, first ? 0 : 8, 0, 0);
        layout->setSpacing(10);
        const QString severity = finding.value("severity").toString();
        layout->addWidget(DevStatsUI::pill(DevStatsUI::severityCaption(severity), DevStatsUI::severityTone(severity)), 0, Qt::AlignTop);
        auto *column = new QVBoxLayout; column->setSpacing(2);
        QString title = finding.value("title").toString();
        if (finding.contains("persistence")) title += finding.value("persistence").toString() == "sustained"
            ? QStringLiteral("（持续，%1/%2 次）").arg(finding.value("observedSamples").toInt()).arg(finding.value("totalSamples").toInt())
            : QStringLiteral("（短暂，%1/%2 次）").arg(finding.value("observedSamples").toInt()).arg(finding.value("totalSamples").toInt());
        column->addWidget(label(title, "strong", "findingTitle"));
        if (!finding.value("evidence").toString().isEmpty()) column->addWidget(label(finding.value("evidence").toString(), "caption"));
        if (!finding.value("suggestion").toString().isEmpty()) column->addWidget(label(finding.value("suggestion").toString(), "muted"));
        layout->addLayout(column, 1);
        if (finding.value("action").toString() == "relief") {
            auto *relief = new QPushButton(QStringLiteral("内存减压…"));
            QObject::connect(relief, &QPushButton::clicked, q, &DiagnosticsPanel::memoryReliefRequested);
            layout->addWidget(relief, 0, Qt::AlignTop);
        }
        return row;
    }
    void fillFindings(QVBoxLayout *column, const QJsonArray &findings, const QString &empty) {
        while (auto *item = column->takeAt(0)) { delete item->widget(); delete item; }
        int count = 0;
        for (const auto &value : findings) {
            column->addWidget(findingRow(value.toObject(), count == 0));
            if (++count == 8) break;
        }
        if (!count) column->addWidget(label(empty, "caption", "findingsEmpty"));
    }
    void suspectRow(QTreeWidget *tree, const QJsonObject &row, const QHash<QString, bool> &expanded, const QString &selected, bool peaks) {
        const QString key = row.value("key").toString();
        const QString owner = row.value("own").toBool() ? QStringLiteral("我") : row.value("kernelThread").toBool() ? QStringLiteral("内核") : row.value("owner").toString();
        const auto rowMembers = row.value("members").toArray();
        const QString count = row.value("processCount").toInt() > 1 ? QStringLiteral("%1 个进程").arg(row.value("processCount").toInt())
            : rowMembers.size() == 1 ? QStringLiteral("PID ") + number(rowMembers.first().toObject().value("pid"), 0) : QString();
        const QStringList cells = peaks
            ? QStringList{row.value("name").toString(), owner, count, cpuText(row.value("averageCpuPercent")), formatBytes(n(row.value("peakMemoryBytes"))), formatBytes(n(row.value("peakIOBytesPerSecond")), true), row.value("reason").toString()}
            : QStringList{row.value("name").toString(), owner, count, cpuText(row.value("cpuPercent")), formatBytes(n(row.value("memoryBytes"))), formatBytes(n(row.value("ioBytesPerSecond")), true), row.value("reason").toString()};
        auto *item = new QTreeWidgetItem(tree, cells);
        setRowAlignment(item, 6);
        item->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        item->setData(0, KeyRole, key);
        const auto members = row.value("members").toArray();
        if (members.size() == 1) item->setData(0, ProcessRole, members.first().toObject());
        item->setToolTip(0, row.value("name").toString());
        item->setToolTip(6, row.value("reason").toString());
        if (selected == key) tree->setCurrentItem(item);
        if (members.size() > 1) {
            for (const auto &value : members) {
                const auto p = value.toObject();
                const QString state = p.value("state").toString();
                const QStringList child{p.value("name").toString(), QString(), QStringLiteral("PID %1%2").arg(number(p.value("pid"), 0), state == "D" ? QStringLiteral(" · 等待磁盘") : QString()),
                    cpuText(p.value("cpuPercent")), formatBytes(n(p.value("memoryBytes"))),
                    formatBytes(n(p.value("ioReadBytesPerSecond")) >= 0 || n(p.value("ioWriteBytesPerSecond")) >= 0 ? std::max(0., n(p.value("ioReadBytesPerSecond"))) + std::max(0., n(p.value("ioWriteBytesPerSecond"))) : -1, true), QString()};
                auto *process = new QTreeWidgetItem(item, child);
                setRowAlignment(process, 6);
                const QString childKey = QStringLiteral("p:%1:%2").arg(p.value("pid").toDouble(), 0, 'f', 0).arg(p.value("startTicks").toDouble(), 0, 'f', 0);
                process->setData(0, KeyRole, childKey);
                process->setData(0, ProcessRole, p);
                process->setToolTip(0, p.value("executablePath").toString());
                if (selected == childKey) tree->setCurrentItem(process);
            }
            item->setExpanded(expanded.value(key));
        }
    }
    bool showingRun() const {
        return !runDismissed && snapshot.value("diagnosticRun").toObject().value("state").toString() == "completed";
    }
    void renderSuspects() {
        QHash<QString, bool> expanded;
        for (int i = 0; i < apps->topLevelItemCount(); ++i) { auto *item = apps->topLevelItem(i); if (item->isExpanded()) expanded.insert(item->data(0, KeyRole).toString(), true); }
        const QString selected = apps->currentItem() ? apps->currentItem()->data(0, KeyRole).toString() : QString();
        const int scroll = apps->verticalScrollBar()->value();
        const bool fromRun = showingRun();
        const auto source = fromRun ? snapshot.value("diagnosticRun").toObject().value("suspects").toArray() : snapshot.value("suspects").toArray();
        QVector<QJsonObject> rows;
        for (const auto &value : source) rows.append(value.toObject());
        QString order = sort->currentData().toString();
        if (fromRun) order = order == "cpuPercent" ? QStringLiteral("averageCpuPercent") : order == "memoryBytes" ? QStringLiteral("peakMemoryBytes") : order == "ioBytesPerSecond" ? QStringLiteral("peakIOBytesPerSecond") : order;
        if (!order.isEmpty()) std::stable_sort(rows.begin(), rows.end(), [&order](const QJsonObject &a, const QJsonObject &b) { return n(a.value(order)) > n(b.value(order)); });
        QSignalBlocker blocker(apps);
        apps->setUpdatesEnabled(false);
        apps->clear();
        apps->headerItem()->setText(2, fromRun ? QStringLiteral("进程") : QStringLiteral("进程 / PID"));
        apps->headerItem()->setText(3, fromRun ? QStringLiteral("平均 CPU") : QStringLiteral("CPU"));
        apps->headerItem()->setText(4, fromRun ? QStringLiteral("内存峰值") : QStringLiteral("内存"));
        apps->headerItem()->setText(5, fromRun ? QStringLiteral("读写峰值") : QStringLiteral("读写"));
        for (const auto &row : rows) suspectRow(apps, row, expanded, selected, fromRun);
        apps->verticalScrollBar()->setValue(scroll);
        apps->setUpdatesEnabled(true);
        suspectHint->setText(fromRun ? QStringLiteral("诊断期间最忙的程序。它们和卡顿高度相关，但不等于已证明是原因。")
                                     : QStringLiteral("现在最忙的程序，含系统进程。它们和卡顿高度相关，但不等于已证明是原因。"));
        filterSuspects();
    }
    void filterSuspects() {
        const QString query = search->text().trimmed();
        for (int i = 0; i < apps->topLevelItemCount(); ++i) {
            auto *app = apps->topLevelItem(i);
            const bool matchApp = query.isEmpty() || app->text(0).contains(query, Qt::CaseInsensitive);
            bool matchChild = false;
            for (int j = 0; j < app->childCount(); ++j) {
                auto *child = app->child(j);
                const bool match = matchApp || child->text(0).contains(query, Qt::CaseInsensitive) || child->text(2).contains(query, Qt::CaseInsensitive);
                child->setHidden(!match);
                matchChild |= match;
            }
            app->setHidden(!matchApp && !matchChild);
            if (!query.isEmpty() && matchChild && !matchApp) app->setExpanded(true);
        }
        selection();
    }
    bool fresh() const {
        const double age = n(snapshot.value("processSampleAgeSeconds"));
        const double at = n(snapshot.value("timestamp"));
        return age >= 0 && age <= 5 && at > 0 && std::abs(QDateTime::currentMSecsSinceEpoch() / 1000. - at) <= 6;
    }
    QJsonObject selectedProcess() const {
        return apps->currentItem() ? apps->currentItem()->data(0, ProcessRole).toJsonObject() : QJsonObject();
    }
    void selection() {
        const auto process = selectedProcess();
        QString hint = QStringLiteral("选中一个进程后可以结束它，或加入自动查杀。多进程应用请先展开。");
        bool can = false;
        const bool own = !process.value("executablePath").toString().isEmpty();
        if (!process.isEmpty()) {
            if (!own) hint = QStringLiteral("这是其他用户或系统的进程，普通用户无法结束。");
            else if (!fresh()) hint = QStringLiteral("进程数据已过期，请稍候。");
            else {
                const auto inspected = ProcessActions().inspect(process);
                can = inspected.value("canTerminate").toBool();
                hint = inspected.value("message").toString();
            }
        }
        terminate->setEnabled(can); forceKill->setEnabled(can);
        addRule->setEnabled(own);
        terminate->setToolTip(hint); forceKill->setToolTip(hint);
        actionHint->setText(hint);
    }
    void renderMeters() {
        const auto list = snapshot.value("diagnostics").toObject().value("meters").toArray();
        for (int i = 0; i < meters.size() && i < list.size(); ++i) meters[i]->setMeter(list[i].toObject());
    }
    void renderBanner() {
        const auto diagnosis = snapshot.value("diagnostics").toObject();
        const auto run = snapshot.value("diagnosticRun").toObject();
        const QString state = run.value("state").toString("idle");
        const bool running = state == "running";
        if (state != lastRunState) { lastRunState = state; if (running || state == "completed") runDismissed = false; }
        const QString liveState = diagnosis.value("state").toString();
        const QString tone = liveState == "critical" ? QStringLiteral("critical") : liveState == "attention" ? QStringLiteral("warn") : liveState == "normal" ? QStringLiteral("ok") : QString();
        DevStatsUI::setTone(banner, tone == "ok" ? QString() : tone);
        DevStatsUI::setTone(bannerPill, tone);
        bannerPill->setText(tone == "critical" ? QStringLiteral("严重") : tone == "warn" ? QStringLiteral("注意") : tone == "ok" ? QStringLiteral("流畅") : QStringLiteral("采样中"));
        headline->setText(diagnosis.value("headline").toString(QStringLiteral("正在收集数据")));
        summary->setText(running ? QStringLiteral("正在观察 %1 / 10 秒 · 卡顿正在发生时诊断最准确，不会关闭任何程序").arg(qBound(0, int(run.value("elapsedSeconds").toDouble()), 10))
                                 : diagnosis.value("summary").toString());
        runButton->setEnabled(!running);
        runButton->setText(running ? QStringLiteral("诊断中…") : state == "idle" ? QStringLiteral("一键诊断") : QStringLiteral("重新诊断"));
        cancelRun->setVisible(running);
        runProgress->setVisible(running);
        runProgress->setValue(qBound(0, run.value("progress").toInt(), 100));
        reliefButton->setVisible(liveState == "critical" || liveState == "attention");
    }
    void renderFindings() {
        const auto run = snapshot.value("diagnosticRun").toObject();
        if (showingRun()) {
            findingsTitle->setText(QStringLiteral("诊断结果：%1 · %2").arg(run.value("headline").toString(), eventTime(run.value("finishedAt"), true)));
            closeRun->show();
            runChecksHost->show();
            if (run == renderedRun) return;
            renderedRun = run;
            renderedFindings = {};
            qDeleteAll(checkPills);
            checkPills.clear();
            for (const auto &value : run.value("checks").toArray()) {
                const auto check = value.toObject();
                const QString status = check.value("status").toString();
                const QString tone = status == "pressure" ? QStringLiteral("warn") : status == "ok" ? QStringLiteral("ok") : QStringLiteral("neutral");
                checkPills.append(DevStatsUI::pill(check.value("title").toString() + QStringLiteral(" ") + (status == "pressure" ? QStringLiteral("有压力") : status == "ok" ? QStringLiteral("正常") : QStringLiteral("不可测")), tone));
            }
            layoutChecks(q->width());
            fillFindings(findingRows, run.value("findings").toArray(), run.value("summary").toString());
            return;
        }
        findingsTitle->setText(QStringLiteral("发现的问题"));
        closeRun->hide();
        runChecksHost->hide();
        const auto findings = snapshot.value("diagnostics").toObject().value("findings").toArray();
        if (findingsShown && findings == renderedFindings && renderedRun.isEmpty()) return;
        findingsShown = true;
        renderedRun = {};
        renderedFindings = findings;
        fillFindings(findingRows, findings, QStringLiteral("当前没有发现问题。"));
    }
    void filterIncidents() {
        const QString query = incidentSearch->text().trimmed();
        const qint64 days = incidentRange->currentData().toLongLong();
        const double since = QDateTime::currentMSecsSinceEpoch() / 1000. - days * 86400.;
        int visible = 0;
        for (int i = 0; i < incidentList->topLevelItemCount(); ++i) {
            auto *item = incidentList->topLevelItem(i);
            const auto event = item->data(0, ProcessRole).toJsonObject();
            const QString text = item->text(2) + ' ' + item->text(3) + ' ' + event.value("summary").toString();
            const bool match = (days == 0 || event.value("lastTimestamp").toDouble(event.value("startTimestamp").toDouble()) >= since)
                && (query.isEmpty() || text.contains(query, Qt::CaseInsensitive));
            item->setHidden(!match);
            visible += match;
        }
        incidentState->setText(!incidentsReceived ? QStringLiteral("正在读取…") : incidents.isEmpty()
            ? QStringLiteral("还没有卡顿记录。出现连续压力或短暂冻结时会自动保存，保留 31 天。")
            : QStringLiteral("显示 %1 / 最近 %2 条，保留 31 天").arg(visible).arg(incidents.size()));
        if (incidentList->currentItem() && incidentList->currentItem()->isHidden()) {
            QTreeWidgetItem *next = nullptr;
            for (int i = 0; i < incidentList->topLevelItemCount(); ++i) if (!incidentList->topLevelItem(i)->isHidden()) { next = incidentList->topLevelItem(i); break; }
            incidentList->setCurrentItem(next);
        }
    }
    void selectIncident(bool reload = false) {
        const QString id = incidentList->currentItem() ? incidentList->currentItem()->data(0, KeyRole).toString() : QString();
        if (id == selectedIncidentId) { if (reload && !id.isEmpty()) emit q->incidentRequested(id); return; }
        selectedIncidentId = id; incident = {}; detailReceived = false; detailDirty = true;
        exportIncident->setEnabled(false);
        renderIncidentDetail();
        if (!id.isEmpty()) emit q->incidentRequested(id);
    }
    void renderIncidentDetail() {
        detailDirty = false;
        const auto c = DevStatsUI::colors(q->window()->palette());
        QString html;
        if (selectedIncidentId.isEmpty()) html = QStringLiteral("<p>选择一条记录查看详情。</p>");
        else if (!detailReceived) html = QStringLiteral("<p>正在读取…</p>");
        else if (incident.isEmpty() || !incident.value("error").toString().isEmpty())
            html = QStringLiteral("<p>无法读取这条记录：%1</p>").arg(esc(incident.value("error").toString(QStringLiteral("记录可能已超过保留期限。"))));
        else html = eventHtml(incident, c);
        incidentDetail->setHtml(html);
        fitIncidentDetail();
    }
    // The page scrolls as a whole, so the report is exactly as tall as its text.
    void fitIncidentDetail() {
        const int width = incidentDetail->width();
        if (width <= 0) return;
        incidentDetail->document()->setTextWidth(width);
        incidentDetail->setFixedHeight(int(incidentDetail->document()->size().height()) + 4);
    }
    void renderIncidents() {
        incidentsDirty = false;
        const bool any = !incidents.isEmpty();
        incidentList->setVisible(any); incidentDetail->setVisible(any); exportIncident->setVisible(any);
        const auto oldSummary = incidentList->currentItem() ? incidentList->currentItem()->data(0, ProcessRole).toJsonObject() : QJsonObject();
        const int scroll = incidentList->verticalScrollBar()->value();
        bool sameIds = incidentList->topLevelItemCount() == incidents.size();
        if (sameIds) for (int i = 0; i < incidents.size(); ++i)
            sameIds &= incidentList->topLevelItem(i)->data(0, KeyRole).toString() == incidents[i].toObject().value("id").toString();
        {
            QSignalBlocker block(incidentList);
            if (!sameIds) incidentList->clear();
            QTreeWidgetItem *selected = nullptr;
            for (int i = 0; i < incidents.size(); ++i) {
                const auto event = incidents[i].toObject();
                auto *item = sameIds ? incidentList->topLevelItem(i) : new QTreeWidgetItem(incidentList);
                const auto status = eventStatus(event.value("status").toString());
                const QStringList cells{eventTime(event.value("startTimestamp"), true), eventDuration(event.value("durationSeconds")),
                    event.value("summary").toString(kindCaptions(event.value("kinds").toArray())), strongestText(event), status.first};
                for (int column = 0; column < cells.size(); ++column) { item->setText(column, cells[column]); item->setToolTip(column, cells[column]); }
                item->setData(4, MetricItemToneRole, status.second);
                item->setData(0, KeyRole, event.value("id").toString());
                item->setData(0, ProcessRole, event);
                if (event.value("id").toString() == selectedIncidentId) selected = item;
            }
            if (!selected && incidentList->topLevelItemCount()) selected = incidentList->topLevelItem(0);
            incidentList->setCurrentItem(selected);
        }
        incidentList->doItemsLayout();
        incidentList->verticalScrollBar()->setValue(scroll);
        DevStatsUI::fitRows(incidentList, 1, 8);
        const auto currentSummary = incidentList->currentItem() ? incidentList->currentItem()->data(0, ProcessRole).toJsonObject() : QJsonObject();
        selectIncident(!currentSummary.isEmpty() && currentSummary != oldSummary);
        if (detailDirty) renderIncidentDetail();
        filterIncidents();
    }
    void render() {
        dirty = false;
        const auto diagnosis = snapshot.value("diagnostics").toObject();
        const QString recorderError = snapshot.value("incidentError").toString();
        incidentError->setText(recorderError.isEmpty() ? QString() : QStringLiteral("记录提示：") + recorderError);
        incidentError->setVisible(!recorderError.isEmpty());
        renderBanner();
        renderMeters();
        renderFindings();
        QStringList notes;
        for (const auto &value : showingRun() ? snapshot.value("diagnosticRun").toObject().value("coverage").toArray() : diagnosis.value("coverage").toArray()) notes << value.toString();
        coverage->setText(notes.join('\n'));
        if (sections->currentIndex() == 0) renderSuspects();
        if (sections->currentIndex() == 2) renderHistory();
    }
    void renderHistory() {
        for (auto it = charts.cbegin(); it != charts.cend(); ++it) it.value()->setPoints(histories.value(it.key()).toArray(), historyStart, historyEnd);
    }
    static constexpr int MetricItemToneRole = Qt::UserRole + 40;
};

DiagnosticsPanel::DiagnosticsPanel(QWidget *parent) : QWidget(parent), d(new Impl(this)) {
    setObjectName("diagnosticsPanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10);

    d->banner = new QFrame;
    d->banner->setObjectName("diagnosticsHero");
    d->banner->setProperty("card", true);
    auto *hero = new QVBoxLayout(d->banner);
    hero->setContentsMargins(16, 12, 14, 12); hero->setSpacing(8);
    d->bannerGrid = new QGridLayout; d->bannerGrid->setHorizontalSpacing(12); d->bannerGrid->setVerticalSpacing(8);
    d->bannerGrid->setColumnStretch(1, 1);
    d->bannerPill = DevStatsUI::pill(QStringLiteral("采样中"), {}, "diagnosisState");
    d->bannerGrid->addWidget(d->bannerPill, 0, 0, Qt::AlignTop);
    auto *words = new QVBoxLayout; words->setSpacing(2);
    d->headline = label(QStringLiteral("正在收集数据"), "headline", "diagnosisSummary");
    d->summary = label({}, "caption", "diagnosticRunStatus");
    words->addWidget(d->headline); words->addWidget(d->summary);
    d->bannerGrid->addLayout(words, 0, 1);
    d->reliefButton = new QPushButton(QStringLiteral("内存减压…"));
    d->reliefButton->setObjectName("memoryReliefFromDiagnostics");
    d->reliefButton->hide();
    d->cancelRun = new QPushButton(QStringLiteral("取消"));
    d->cancelRun->setObjectName("cancelDiagnostics"); d->cancelRun->hide();
    d->runButton = new QPushButton(QStringLiteral("一键诊断"));
    d->runButton->setObjectName("runDiagnostics");
    d->runButton->setProperty("primary", true);
    d->runButton->setToolTip(QStringLiteral("观察约 10 秒，找出卡顿原因和相关进程。不会关闭任何程序。"));
    auto *exportReport = new QPushButton(QStringLiteral("导出报告…"));
    exportReport->setObjectName("exportDiagnostics");
    d->bannerActions = new QWidget;
    auto *actionRow = new QHBoxLayout(d->bannerActions); actionRow->setContentsMargins(0, 0, 0, 0); actionRow->setSpacing(8);
    for (auto *button : {d->reliefButton, exportReport, d->cancelRun, d->runButton}) actionRow->addWidget(button);
    d->bannerGrid->addWidget(d->bannerActions, 0, 2, Qt::AlignTop);
    hero->addLayout(d->bannerGrid);
    d->runProgress = new QProgressBar;
    d->runProgress->setObjectName("diagnosticRunProgress");
    d->runProgress->setProperty("thin", true);
    d->runProgress->setAccessibleName(QStringLiteral("诊断进度"));
    d->runProgress->setRange(0, 100); d->runProgress->setTextVisible(false); d->runProgress->hide();
    hero->addWidget(d->runProgress);
    d->feedback = label({}, "caption", "diagnosticsFeedback");
    d->feedback->hide();
    hero->addWidget(d->feedback);
    d->feedbackTimer = new QTimer(this);
    d->feedbackTimer->setSingleShot(true);
    d->feedbackTimer->setInterval(15000);
    connect(d->feedbackTimer, &QTimer::timeout, this, [this] { setFeedback(QString()); });
    layout->addWidget(d->banner);
    connect(d->runButton, &QPushButton::clicked, this, [this] { d->sections->setCurrentIndex(0); d->runDismissed = false; emit diagnosticRunRequested(true); });
    connect(d->cancelRun, &QPushButton::clicked, this, [this] { emit diagnosticRunRequested(false); });
    connect(exportReport, &QPushButton::clicked, this, &DiagnosticsPanel::exportReportRequested);
    connect(d->reliefButton, &QPushButton::clicked, this, &DiagnosticsPanel::memoryReliefRequested);

    d->sections = new SectionTabs;
    d->sections->setObjectName("diagnosticsSections");
    layout->addWidget(d->sections);
    QVector<QVBoxLayout *> pages;
    for (const auto &caption : {QStringLiteral("实时"), QStringLiteral("卡顿记录"), QStringLiteral("压力历史")}) {
        auto *page = new QWidget;
        page->setProperty("pageBody", true);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 0, 0); column->setSpacing(10); column->setAlignment(Qt::AlignTop);
        pages.append(column);
        d->sections->addTab(caption);
        d->sectionPages.append(page);
        page->setVisible(d->sectionPages.size() == 1);
        layout->addWidget(page);
    }

    // Live page: meters, then what is wrong, then who is involved.
    auto *meterHost = new QWidget;
    d->meterGrid = new QGridLayout(meterHost);
    d->meterGrid->setContentsMargins(0, 0, 0, 0); d->meterGrid->setSpacing(8);
    for (const auto &name : {"cpu", "memory", "disk", "response", "thermal", "blocked"})
        d->meters.append(new MeterTile(QStringLiteral("diagnostics_") + QString::fromLatin1(name)));
    d->layoutMeters(800);
    pages[0]->addWidget(meterHost);

    d->closeRun = new QToolButton;
    d->closeRun->setObjectName("diagnosticRunClose");
    d->closeRun->setText(QStringLiteral("收起结果"));
    d->closeRun->hide();
    auto *findingsColumn = card(pages[0], QString(), {}, nullptr);
    findingsColumn->parentWidget()->setObjectName("diagnosisFindings");
    auto *findingsHeader = new QHBoxLayout; findingsHeader->setSpacing(8);
    d->findingsTitle = label(QStringLiteral("发现的问题"), "heading", "diagnosisFindingsTitle");
    findingsHeader->addWidget(d->findingsTitle, 1);
    findingsHeader->addWidget(d->closeRun, 0, Qt::AlignTop);
    findingsColumn->addLayout(findingsHeader);
    d->runChecksHost = new QWidget;
    d->runChecksHost->setObjectName("diagnosticRunChecks");
    d->runChecks = new QGridLayout(d->runChecksHost);
    d->runChecks->setContentsMargins(0, 0, 0, 0); d->runChecks->setSpacing(6);
    d->runChecksHost->hide();
    findingsColumn->addWidget(d->runChecksHost);
    d->findingRows = new QVBoxLayout; d->findingRows->setSpacing(8);
    findingsColumn->addLayout(d->findingRows);
    d->fillFindings(d->findingRows, {}, QStringLiteral("等待采样…"));
    connect(d->closeRun, &QToolButton::clicked, this, [this] { d->runDismissed = true; d->render(); });

    auto *suspects = card(pages[0], QStringLiteral("相关进程"));
    d->suspectHint = label({}, "caption", "diagnosticsSuspectHint");
    suspects->addWidget(d->suspectHint);
    auto *tools = new QHBoxLayout;
    d->search = new QLineEdit;
    d->search->setObjectName("diagnosticsSearch");
    d->search->setPlaceholderText(QStringLiteral("搜索程序或 PID"));
    d->search->setClearButtonEnabled(true);
    tools->addWidget(d->search, 1);
    d->sort = new QComboBox;
    d->sort->setObjectName("diagnosticsSort");
    d->sort->addItem(QStringLiteral("最相关"), QString());
    d->sort->addItem(QStringLiteral("按 CPU"), "cpuPercent");
    d->sort->addItem(QStringLiteral("按内存"), "memoryBytes");
    d->sort->addItem(QStringLiteral("按读写"), "ioBytesPerSecond");
    tools->addWidget(d->sort);
    suspects->addLayout(tools);
    d->apps = DevStatsUI::table("diagnosticsApplications", {QStringLiteral("程序"), QStringLiteral("用户"), QStringLiteral("进程 / PID"), "CPU", QStringLiteral("内存"), QStringLiteral("读写"), QStringLiteral("说明")});
    d->apps->setRootIsDecorated(true);
    d->apps->setMinimumHeight(290);
    d->apps->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->apps->header()->setSectionResizeMode(6, QHeaderView::Stretch);
    d->apps->headerItem()->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
    d->apps->headerItem()->setTextAlignment(6, Qt::AlignLeft | Qt::AlignVCenter);
    d->apps->setContextMenuPolicy(Qt::CustomContextMenu);
    suspects->addWidget(d->apps);
    auto *actions = new QHBoxLayout; actions->setSpacing(8);
    d->terminate = new QPushButton(QStringLiteral("结束进程…")); d->terminate->setObjectName("terminateDiagnosticProcess");
    d->forceKill = new QPushButton(QStringLiteral("强制结束…")); d->forceKill->setObjectName("killDiagnosticProcess"); d->forceKill->setProperty("danger", true);
    d->addRule = new QPushButton(QStringLiteral("加入自动查杀…")); d->addRule->setObjectName("addRuleFromDiagnostics");
    d->terminate->setEnabled(false); d->forceKill->setEnabled(false); d->addRule->setEnabled(false);
    actions->addWidget(d->terminate); actions->addWidget(d->forceKill); actions->addWidget(d->addRule);
    d->actionHint = label(QStringLiteral("选中一个进程后可以结束它，或加入自动查杀。"), "muted", "diagnosticsActionHint");
    actions->addWidget(d->actionHint, 1);
    suspects->addLayout(actions);
    auto *coverageBody = new QWidget;
    auto *coverageLayout = new QVBoxLayout(coverageBody); coverageLayout->setContentsMargins(0, 0, 0, 0);
    d->coverage = label({}, "muted", "diagnosisCoverage");
    coverageLayout->addWidget(d->coverage);
    DevStatsUI::disclosure(suspects, QStringLiteral("数据来源与限制"), "diagnosisDetailsToggle", coverageBody);

    auto runAction = [this](const QString &action) {
        const auto process = d->selectedProcess();
        if (!process.isEmpty() && d->fresh()) emit processActionRequested(process, action);
    };
    connect(d->terminate, &QPushButton::clicked, this, [runAction] { runAction("terminate"); });
    connect(d->forceKill, &QPushButton::clicked, this, [runAction] { runAction("kill"); });
    connect(d->addRule, &QPushButton::clicked, this, [this] { const auto process = d->selectedProcess(); if (!process.isEmpty()) emit addRuleRequested(process); });
    connect(d->apps, &QTreeWidget::itemSelectionChanged, this, [this] { d->selection(); });
    connect(d->search, &QLineEdit::textChanged, this, [this] { d->filterSuspects(); });
    connect(d->sort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { d->renderSuspects(); });
    connect(d->apps, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        auto *item = d->apps->itemAt(position);
        if (!item) return;
        d->apps->setCurrentItem(item);
        d->selection();
        QMenu menu(d->apps);
        auto *terminate = menu.addAction(QStringLiteral("结束进程…")); terminate->setEnabled(d->terminate->isEnabled());
        auto *kill = menu.addAction(QStringLiteral("强制结束…")); kill->setEnabled(d->forceKill->isEnabled());
        menu.addSeparator();
        auto *rule = menu.addAction(QStringLiteral("加入自动查杀…")); rule->setEnabled(d->addRule->isEnabled());
        const auto *chosen = menu.exec(d->apps->viewport()->mapToGlobal(position));
        if (chosen == terminate) d->terminate->click();
        else if (chosen == kill) d->forceKill->click();
        else if (chosen == rule) d->addRule->click();
    });

    // Recorded events.
    d->autoRecord = new ToggleSwitch;
    d->autoRecord->setObjectName("autoRecordIncidents");
    d->autoRecord->setChecked(true);
    d->autoRecord->setToolTip(QStringLiteral("关闭后不再新增记录，已有记录保留。"));
    auto *recordHeader = new QWidget;
    auto *recordRow = new QHBoxLayout(recordHeader); recordRow->setContentsMargins(0, 0, 0, 0); recordRow->setSpacing(8);
    recordRow->addWidget(label(QStringLiteral("自动记录"), "caption"));
    recordRow->addWidget(d->autoRecord);
    auto *incidents = card(pages[1], QStringLiteral("卡顿记录"), {}, recordHeader);
    incidents->parentWidget()->setObjectName("incidentCard");
    connect(d->autoRecord, &ToggleSwitch::toggled, this, &DiagnosticsPanel::incidentRecordingRequested);
    d->incidentState = label(QStringLiteral("正在读取…"), "caption", "incidentState");
    incidents->addWidget(d->incidentState);
    d->incidentError = label({}, "caption", "incidentError");
    d->incidentError->hide();
    incidents->addWidget(d->incidentError);
    auto *eventTools = new QHBoxLayout;
    d->incidentSearch = new QLineEdit;
    d->incidentSearch->setObjectName("incidentSearch");
    d->incidentSearch->setPlaceholderText(QStringLiteral("搜索程序或原因"));
    d->incidentSearch->setClearButtonEnabled(true);
    d->incidentRange = new QComboBox;
    d->incidentRange->setObjectName("incidentRange");
    d->incidentRange->addItem(QStringLiteral("全部"), 0); d->incidentRange->addItem(QStringLiteral("近 24 小时"), 1); d->incidentRange->addItem(QStringLiteral("近 7 天"), 7);
    d->exportIncident = new QPushButton(QStringLiteral("导出所选…"));
    d->exportIncident->setObjectName("exportIncident");
    d->exportIncident->setEnabled(false); d->exportIncident->hide();
    eventTools->addWidget(d->incidentSearch, 1); eventTools->addWidget(d->incidentRange); eventTools->addWidget(d->exportIncident);
    incidents->addLayout(eventTools);
    connect(d->incidentSearch, &QLineEdit::textChanged, this, [this] { d->filterIncidents(); });
    connect(d->incidentRange, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { d->filterIncidents(); });
    d->incidentList = DevStatsUI::table("incidentList", {QStringLiteral("时间"), QStringLiteral("持续"), QStringLiteral("原因"), QStringLiteral("最忙的程序"), QStringLiteral("状态")});
    d->incidentList->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->incidentList->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    d->incidentList->hide();
    incidents->addWidget(d->incidentList);
    d->incidentDetail = new QTextBrowser;
    d->incidentDetail->setObjectName("incidentDetail");
    d->incidentDetail->setOpenLinks(false);
    d->incidentDetail->setFrameShape(QFrame::NoFrame);
    d->incidentDetail->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->incidentDetail->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->incidentDetail->document()->setDocumentMargin(2);
    d->incidentDetail->installEventFilter(this);
    d->incidentDetail->hide();
    incidents->addWidget(DevStatsUI::separator());
    incidents->addWidget(d->incidentDetail);
    connect(d->incidentList, &QTreeWidget::itemSelectionChanged, this, [this] { d->selectIncident(); });
    connect(d->exportIncident, &QPushButton::clicked, this, [this] { if (!d->selectedIncidentId.isEmpty()) emit exportIncidentRequested(d->selectedIncidentId); });

    // Long-term pressure.
    auto *historyColumn = card(pages[2], QStringLiteral("压力历史"), QStringLiteral("沿用顶部的时间范围，保留 31 天。未运行或休眠的时间段没有数据。"));
    auto *grid = new QGridLayout; grid->setSpacing(14);
    const QList<std::tuple<QString, QString, HistoryChart::Unit>> plots{
        {"schedulerLatency", QStringLiteral("调度延迟"), HistoryChart::Unit::Milliseconds},
        {"diskBusy", QStringLiteral("磁盘繁忙"), HistoryChart::Unit::Percent},
        {"cpuIowait", QStringLiteral("CPU I/O 等待"), HistoryChart::Unit::Percent},
        {"swapOut", QStringLiteral("换出速率"), HistoryChart::Unit::BytesPerSecond},
        {"memoryPressure", QStringLiteral("内存停顿（PSI）"), HistoryChart::Unit::Percent},
        {"ioPressure", QStringLiteral("I/O 停顿（PSI）"), HistoryChart::Unit::Percent}};
    int index = 0;
    for (const auto &plot : plots) {
        auto *cell = new QVBoxLayout; cell->setSpacing(2);
        cell->addWidget(label(std::get<1>(plot), "caption"));
        auto *chart = new HistoryChart(std::get<2>(plot));
        chart->setObjectName(std::get<0>(plot) + "History");
        cell->addWidget(chart);
        d->charts.insert(std::get<0>(plot), chart);
        grid->addLayout(cell, index / 2, index % 2);
        ++index;
    }
    historyColumn->addLayout(grid);
    connect(d->sections, &SectionTabs::currentChanged, this, [this](int current) {
        for (int i = 0; i < d->sectionPages.size(); ++i) d->sectionPages[i]->setVisible(i == current);
        d->render();
        if (current == 1) { if (d->incidentsDirty) d->renderIncidents(); if (d->detailDirty) d->renderIncidentDetail(); }
    });
}

DiagnosticsPanel::~DiagnosticsPanel() = default;
void DiagnosticsPanel::setSnapshot(const QJsonObject &snapshot) { d->snapshot = snapshot; d->dirty = true; if (isVisible()) d->render(); }
void DiagnosticsPanel::setHistories(const QJsonObject &h, qint64 start, qint64 end) {
    d->histories = h; d->historyStart = start; d->historyEnd = end; d->dirty = true;
    if (isVisible() && d->sections->currentIndex() == 2) d->renderHistory();
}
void DiagnosticsPanel::setFeedback(const QString &message) {
    d->feedback->setText(message);
    d->feedback->setVisible(!message.isEmpty());
    if (!message.isEmpty()) d->feedbackTimer->start();
}
void DiagnosticsPanel::setIncidents(const QJsonArray &incidents) {
    QJsonArray bounded;
    for (const auto &incident : incidents) { bounded.append(incident); if (bounded.size() == 100) break; }
    if (d->incidentsReceived && bounded == d->incidents) return;
    d->incidents = bounded; d->incidentsReceived = true; d->incidentsDirty = true;
    if (isVisible() && d->sections->currentIndex() == 1) d->renderIncidents();
}
void DiagnosticsPanel::setIncidentDetail(const QJsonObject &incident) {
    if (d->selectedIncidentId.isEmpty() || (!incident.isEmpty() && incident.value("id").toString() != d->selectedIncidentId)) return;
    if (d->detailReceived && d->incident == incident) return;
    d->incident = incident; d->detailReceived = true; d->detailDirty = true;
    d->exportIncident->setEnabled(!incident.isEmpty() && incident.value("error").toString().isEmpty());
    if (isVisible() && d->sections->currentIndex() == 1) d->renderIncidentDetail();
}
void DiagnosticsPanel::setIncidentRecording(bool enabled) { QSignalBlocker block(d->autoRecord); d->autoRecord->setChecked(enabled); }
void DiagnosticsPanel::showIncidents() { d->sections->setCurrentIndex(1); }
void DiagnosticsPanel::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (d->dirty) d->render();
    if (d->sections->currentIndex() == 1) { if (d->incidentsDirty) d->renderIncidents(); if (d->detailDirty) d->renderIncidentDetail(); }
}
void DiagnosticsPanel::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    d->layoutBanner(event->size().width());
    d->layoutMeters(event->size().width());
    d->layoutChecks(event->size().width());
}
void DiagnosticsPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    // The report's secondary text carries its colour inline.
    if (event->type() == QEvent::PaletteChange) {
        d->detailDirty = true;
        if (isVisible() && d->sections->currentIndex() == 1) d->renderIncidentDetail();
    }
}
bool DiagnosticsPanel::eventFilter(QObject *watched, QEvent *event) {
    // The layout gives the report its width after this panel's own resize, so its
    // height can only be worked out here.
    if (watched == d->incidentDetail && event->type() == QEvent::Resize) {
        const auto *resize = static_cast<QResizeEvent *>(event);
        if (resize->size().width() != resize->oldSize().width()) d->fitIncidentDetail();
    }
    return QWidget::eventFilter(watched, event);
}

QJsonObject DiagnosticsPanel::report() const {
    auto diagnosis = d->snapshot.value("diagnostics").toObject();
    QJsonArray findings;
    for (const auto &entry : diagnosis.value("findings").toArray()) {
        auto finding = entry.toObject();
        // A mount point can reveal a user or project name; capacity is all that matters.
        if (finding.value("kind").toString() == "disk") finding["title"] = QStringLiteral("磁盘空间不足");
        findings.append(finding);
    }
    diagnosis["findings"] = findings;
    diagnosis.remove("meters");
    auto run = d->snapshot.value("diagnosticRun").toObject();
    QJsonArray runSuspects;
    for (const auto &value : run.value("suspects").toArray()) {
        auto suspect = value.toObject();
        suspect.remove("members"); suspect.remove("key");
        runSuspects.append(suspect);
    }
    run["suspects"] = runSuspects;
    QJsonArray runFindings;
    for (const auto &entry : run.value("findings").toArray()) {
        auto finding = entry.toObject();
        if (finding.value("kind").toString() == "disk") finding["title"] = QStringLiteral("磁盘空间不足");
        runFindings.append(finding);
    }
    run["findings"] = runFindings;
    QJsonArray suspects;
    for (const auto &value : d->snapshot.value("suspects").toArray()) {
        auto suspect = value.toObject();
        suspect.remove("members"); suspect.remove("key");
        suspects.append(suspect);
    }
    QJsonObject out{{"version", QCoreApplication::applicationVersion()}, {"capturedAt", d->snapshot.value("timestamp")}, {"diagnostics", diagnosis},
        {"suspects", suspects}, {"processSampleAgeSeconds", d->snapshot.value("processSampleAgeSeconds")}, {"oneClickDiagnosis", run},
        {"note", QStringLiteral("当前资源观测，进程排行表示相关而非证明。不含命令行、可执行文件路径和项目路径。")}};
    for (const auto &key : {"cpu", "memory", "pressure", "vm", "responsiveness", "thermal", "processStates"}) out[key] = d->snapshot.value(key);
    auto disk = d->snapshot.value("diskIO").toObject();
    out["diskIO"] = disk;
    QJsonObject history;
    for (const auto &key : {"cpu", "memory", "memoryPressure", "ioPressure", "cpuIowait", "swapIn", "swapOut", "schedulerLatency", "diskBusy"}) history[key] = d->histories.value(key);
    out["history"] = history; out["historyStart"] = double(d->historyStart); out["historyEnd"] = double(d->historyEnd);
    return out;
}
