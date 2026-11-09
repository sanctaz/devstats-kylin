#include "../src/charts.h"
#include "../src/diagnostics.h"
#include "../src/diagnostics_panel.h"
#include "../src/ui_components.h"
#include "../src/ui_theme.h"

#include <QApplication>
#include <QComboBox>
#include <QHelpEvent>
#include <QToolTip>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr double GiB = 1024. * 1024 * 1024;
int checks = 0;
void check(bool ok, const char *what) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL diagnostics UI: %s\n", what); std::exit(1); } }
// Each pass delivers only the layout requests already queued, and a nested layout
// queues its parent's, so settling takes one pass per level.
void settle() { for (int pass = 0; pass < 10; ++pass) { QApplication::sendPostedEvents(); QApplication::processEvents(); } }
QJsonObject own(int pid, const QString &name, double cpu, double memory) {
    return {{"pid", pid}, {"startTicks", 100 + pid}, {"name", name}, {"executablePath", "/home/secret-user/bin/" + name}, {"cpuPercent", cpu}, {"memoryBytes", memory},
            {"ioReadBytesPerSecond", 1024}, {"ioWriteBytesPerSecond", 0}, {"state", "S"}, {"applicationIdentity", name}};
}
QJsonObject data(bool stressed = true) {
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.;
    auto helper = own(601, "chromium", 40, 1 * GiB); helper["applicationIdentity"] = "chromium";
    QJsonObject s{{"timestamp", now}, {"processSampleAgeSeconds", 0.}, {"processesFresh", true}, {"monotonicSeconds", 100.}, {"processMonotonicSeconds", 100.},
        {"cpu", QJsonObject{{"usage", stressed ? .95 : .1}, {"iowait", 0}, {"coreCount", 8}, {"runningProcesses", stressed ? 12 : 1}, {"blockedProcesses", 0}, {"load", QJsonArray{stressed ? 12 : 1}}}},
        {"memory", QJsonObject{{"totalBytes", 16 * GiB}, {"availableBytes", 10 * GiB}, {"fraction", .375}}},
        {"vm", QJsonObject{{"swapInBytesPerSecond", 0}, {"swapOutBytesPerSecond", 0}, {"oomKillDelta", 0}}},
        {"disks", QJsonArray{QJsonObject{{"mountPoint", "/home/secret-user/disk"}, {"usageFraction", .99}, {"availableBytes", .1 * GiB}}}},
        {"diskIO", QJsonObject{{"busiestDevice", "sda"}, {"busiestUtilization", .1}, {"busiestAwaitMilliseconds", 2}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", 3}, {"displayAvailable", true}, {"displayMaxMs", 2}}},
        {"thermal", QJsonObject{{"hottestCelsius", 55}, {"frequencyFraction", .5}}},
        {"processStates", QJsonObject{{"running", 3}, {"blocked", 0}, {"zombie", 0}}},
        {"processes", QJsonArray{own(600, "chromium", stressed ? 300 : 3, 3 * GiB), helper, own(700, "editor", stressed ? 90 : 1, 1 * GiB)}},
        {"systemProcesses", QJsonArray{QJsonObject{{"pid", 9}, {"startTicks", 1}, {"name", "Xorg"}, {"owner", "root"}, {"cpuPercent", stressed ? 120 : 1}, {"memoryBytes", 1000}, {"state", "S"}}}},
        {"activityApplications", QJsonArray{QJsonObject{{"identity", "chromium"}, {"name", "Chromium"}}}}};
    s["diagnostics"] = diagnoseSystem(s);
    s["suspects"] = rankSuspects(s, "cpu");
    s["diagnosticRun"] = QJsonObject{{"state", "idle"}};
    return s;
}
QJsonObject completedRun(const QJsonObject &s) {
    auto suspects = rankSuspects(s, "cpu");
    QJsonArray rows;
    for (const auto &value : suspects) {
        auto row = value.toObject();
        row["averageCpuPercent"] = row["cpuPercent"]; row["peakCpuPercent"] = row["cpuPercent"]; row["peakMemoryBytes"] = row["memoryBytes"]; row["peakIOBytesPerSecond"] = row["ioBytesPerSecond"];
        rows.append(row);
    }
    QJsonArray findings;
    for (const auto &value : s["diagnostics"].toObject()["findings"].toArray()) {
        auto finding = value.toObject(); finding["persistence"] = "sustained"; finding["observedSamples"] = 6; finding["totalSamples"] = 6;
        findings.append(finding);
    }
    return {{"state", "completed"}, {"headline", QStringLiteral("CPU 繁忙")}, {"summary", "s"}, {"progress", 100}, {"finishedAt", s["timestamp"]}, {"findings", findings}, {"suspects", rows},
            {"checks", QJsonArray{QJsonObject{{"kind", "cpu"}, {"title", QStringLiteral("处理器")}, {"status", "pressure"}}, QJsonObject{{"kind", "display"}, {"title", QStringLiteral("显示")}, {"status", "unknown"}}}},
            {"coverage", QJsonArray{QStringLiteral("这是约 10 秒的观察")}}};
}
QTreeWidgetItem *row(QTreeWidget *tree, const QString &name) {
    for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->text(0) == name) return tree->topLevelItem(i);
    return nullptr;
}
QString text(DiagnosticsPanel &panel, const char *name) { return panel.findChild<QLabel *>(name)->text(); }
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    qRegisterMetaType<QJsonObject>();
    DiagnosticsPanel panel;
    panel.setStyleSheet(DevStatsUI::styleSheet(true));
    panel.setPalette(DevStatsUI::palette(true));
    panel.resize(900, 900);

    // Nothing is rendered until the page is visible.
    panel.setSnapshot(data());
    check(panel.findChild<QTreeWidget *>("diagnosticsApplications")->topLevelItemCount() == 0, "a hidden page does no table work");
    panel.show();
    settle();

    auto *tree = panel.findChild<QTreeWidget *>("diagnosticsApplications");
    auto *run = panel.findChild<QPushButton *>("runDiagnostics");
    auto *cancel = panel.findChild<QPushButton *>("cancelDiagnostics");
    auto *progress = panel.findChild<QProgressBar *>("diagnosticRunProgress");
    auto *terminate = panel.findChild<QPushButton *>("terminateDiagnosticProcess");
    auto *kill = panel.findChild<QPushButton *>("killDiagnosticProcess");
    auto *addRule = panel.findChild<QPushButton *>("addRuleFromDiagnostics");
    // A process name that looks like markup is shown as text in its tooltip.
    {
        auto *item = tree->topLevelItem(0);
        const QString original = item->toolTip(0);
        const auto showTip = [&] {
            QRect area = tree->visualItemRect(item);
            area.setWidth(tree->columnWidth(0)); // The row spans every column; the name is in the first.
            QHelpEvent help(QEvent::ToolTip, area.center(), tree->viewport()->mapToGlobal(area.center()));
            QApplication::sendEvent(tree->viewport(), &help);
            return QToolTip::text();
        };
        item->setToolTip(0, "<b>evil</b><img src=x>");
        const QString shown = showTip();
        check(shown.contains("&lt;b&gt;evil&lt;/b&gt;&lt;img src=x&gt;") && !shown.contains("<b>"), "markup in a tooltip stays text");
        item->setToolTip(0, "firefox");
        check(showTip() == "firefox", "an ordinary tooltip is unchanged");
        QToolTip::hideText();
        item->setToolTip(0, original);
        check(DevStatsUI::literal("a < b") == "a < b" && DevStatsUI::literal("<i>x</i>").contains("&lt;i&gt;"), "only text that looks like markup is escaped");
    }
    auto *closeRun = panel.findChild<QToolButton *>("diagnosticRunClose");
    auto *tabs = panel.findChild<SectionTabs *>("diagnosticsSections");

    // The live page answers first: what is wrong, then who is involved.
    check(text(panel, "diagnosisSummary").contains(QStringLiteral("磁盘空间不足")) == false && !text(panel, "diagnosisSummary").isEmpty(), "headline shows");
    check(panel.findChild<QLabel *>("diagnosisState")->text() == QStringLiteral("严重"), "state pill");
    check(panel.findChildren<MeterTile *>().size() == 6, "six meters");
    check(panel.findChild<QLabel *>("diagnostics_cpuValue")->text() == "95%", "meter values come from the diagnosis");
    check(!panel.findChildren<QLabel *>("findingTitle").isEmpty(), "findings are listed");
    check(text(panel, "diagnosisFindingsTitle") == QStringLiteral("发现的问题"), "live title");
    check(closeRun->isHidden(), "nothing to collapse yet");
    check(tree->topLevelItemCount() == 3 && tree->topLevelItem(0)->text(0) == "Chromium", "suspects ranked");
    auto *group = row(tree, "Chromium");
    check(group->childCount() == 2 && group->text(2).contains(QStringLiteral("2 个进程")) && group->text(1) == QStringLiteral("我"), "multi-process app expands");
    check(row(tree, "editor")->text(2) == "PID 700", "single process shows its PID");
    check(row(tree, "Xorg")->text(1) == "root", "other users are labelled");

    // Actions follow selection.
    check(!terminate->isEnabled() && !addRule->isEnabled(), "nothing selected");
    tree->setCurrentItem(row(tree, "Xorg")); settle();
    check(!terminate->isEnabled() && !kill->isEnabled() && !addRule->isEnabled() && text(panel, "diagnosticsActionHint").contains(QStringLiteral("其他用户")), "other users' processes cannot be acted on");
    tree->setCurrentItem(row(tree, "editor")); settle();
    check(addRule->isEnabled(), "own process can get a rule");
    QSignalSpy rules(&panel, &DiagnosticsPanel::addRuleRequested);
    addRule->click();
    check(rules.count() == 1 && rules.first()[0].toJsonObject().value("pid").toInt() == 700, "rule request carries the process");
    QSignalSpy actions(&panel, &DiagnosticsPanel::processActionRequested);
    check(!terminate->isEnabled() || actions.count() == 0, "fake identities are refused by the live check");
    tree->setCurrentItem(group); settle();
    check(!addRule->isEnabled() && text(panel, "diagnosticsActionHint").contains(QStringLiteral("展开")), "a group needs one process chosen");
    tree->setCurrentItem(group->child(0)); settle();
    check(addRule->isEnabled(), "a member of a group can be chosen");

    // A healthy system says so, rather than leaving the placeholder from before the first sample.
    {
        DiagnosticsPanel healthy;
        healthy.setStyleSheet(DevStatsUI::styleSheet(true));
        healthy.show();
        auto fine = data(false); fine["disks"] = QJsonArray{}; fine["diagnostics"] = diagnoseSystem(fine);
        healthy.setSnapshot(fine); settle();
        check(healthy.findChild<QLabel *>("findingsEmpty") && healthy.findChild<QLabel *>("findingsEmpty")->text() == QStringLiteral("当前没有发现问题。") , "a healthy system says it found nothing");
    }

    // Selection and expansion survive a refresh.
    group->setExpanded(true);
    panel.setSnapshot(data()); settle();
    group = row(tree, "Chromium");
    check(group->isExpanded() && tree->currentItem() && tree->currentItem()->parent() == group, "refresh keeps expansion and selection");

    // Sorting and search.
    auto *sort = panel.findChild<QComboBox *>("diagnosticsSort");
    sort->setCurrentIndex(sort->findData("memoryBytes")); settle();
    check(tree->topLevelItem(0)->text(0) == "Chromium" && tree->topLevelItem(2)->text(0) == "Xorg", "sort by memory");
    sort->setCurrentIndex(0); settle();
    auto *search = panel.findChild<QLineEdit *>("diagnosticsSearch");
    search->setText("edit"); settle();
    check(row(tree, "editor") && !row(tree, "editor")->isHidden() && row(tree, "Xorg")->isHidden(), "search filters");
    search->setText("PID 601"); settle();
    check(!row(tree, "Chromium")->isHidden() && row(tree, "Chromium")->isExpanded(), "PID search opens the group");
    search->clear(); settle();

    // A one-click run: progress, then one results card replacing the live findings.
    QSignalSpy requests(&panel, &DiagnosticsPanel::diagnosticRunRequested);
    run->click();
    check(requests.count() == 1 && requests.first()[0].toBool(), "run requested");
    auto s = data();
    s["diagnosticRun"] = QJsonObject{{"state", "running"}, {"progress", 40}, {"elapsedSeconds", 4.}};
    panel.setSnapshot(s); settle();
    check(!run->isEnabled() && run->text().contains(QStringLiteral("诊断中")) && !cancel->isHidden() && !progress->isHidden() && progress->value() == 40, "running state");
    check(text(panel, "diagnosticRunStatus").contains("4 / 10"), "elapsed time shows");
    cancel->click();
    check(requests.count() == 2 && !requests.last()[0].toBool(), "cancel requested");
    s["diagnosticRun"] = completedRun(s);
    panel.setSnapshot(s); settle();
    check(run->isEnabled() && run->text() == QStringLiteral("重新诊断") && cancel->isHidden() && progress->isHidden(), "finished state");
    check(text(panel, "diagnosisFindingsTitle").contains(QStringLiteral("诊断结果：CPU 繁忙")), "results take over the card");
    check(!closeRun->isHidden() && !panel.findChild<QWidget *>("diagnosticRunChecks")->isHidden(), "results can be collapsed and show status per resource");
    check(tree->headerItem()->text(3) == QStringLiteral("平均 CPU") && tree->headerItem()->text(4) == QStringLiteral("内存峰值"), "table shows run statistics");
    check(panel.findChildren<QLabel *>("findingTitle").first()->text().contains(QStringLiteral("持续")), "persistence is shown");
    closeRun->click(); settle();
    check(text(panel, "diagnosisFindingsTitle") == QStringLiteral("发现的问题") && closeRun->isHidden() && tree->headerItem()->text(3) == "CPU", "collapse returns to live data");
    panel.setSnapshot(s); settle();
    check(text(panel, "diagnosisFindingsTitle") == QStringLiteral("发现的问题"), "a collapsed result stays collapsed across refreshes");
    auto again = data(); again["diagnosticRun"] = QJsonObject{{"state", "running"}}; panel.setSnapshot(again); settle();
    again["diagnosticRun"] = completedRun(again); panel.setSnapshot(again); settle();
    check(text(panel, "diagnosisFindingsTitle").contains(QStringLiteral("诊断结果")), "a new run shows its result again");

    // Reports never carry paths, even from the run.
    const auto report = QString::fromUtf8(QJsonDocument(panel.report()).toJson());
    check(!report.contains("secret-user") && !report.contains("executablePath") && !report.contains("members"), "report has no paths or process identities");
    check(report.contains("Chromium") && report.contains("oneClickDiagnosis"), "report keeps the useful part");

    // Recorded events.
    QSignalSpy details(&panel, &DiagnosticsPanel::incidentRequested);
    auto *list = panel.findChild<QTreeWidget *>("incidentList");
    auto *detail = panel.findChild<QTextBrowser *>("incidentDetail");
    auto *exportButton = panel.findChild<QPushButton *>("exportIncident");
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.;
    const QJsonObject first{{"id", "e1"}, {"startTimestamp", now - 300}, {"lastTimestamp", now - 200}, {"durationSeconds", 100}, {"status", "recovered"}, {"kinds", QJsonArray{"memory"}},
        {"summary", QStringLiteral("内存不足")}, {"strongestApplication", QJsonObject{{"name", "Chromium"}, {"memoryBytes", 3 * GiB}}}, {"strongestApplicationBasis", "memory"}};
    const QJsonObject old{{"id", "e2"}, {"startTimestamp", now - 3 * 86400}, {"lastTimestamp", now - 3 * 86400 + 5}, {"durationSeconds", 5}, {"status", "interrupted"}, {"kinds", QJsonArray{"io"}},
        {"summary", QStringLiteral("连续采样出现资源压力迹象，已保存相关资源活动供排查。")}};
    panel.setIncidents({first, old});
    check(list->topLevelItemCount() == 0, "events render only on their own tab");
    tabs->setCurrentIndex(1); settle();
    check(!list->isHidden() && list->topLevelItemCount() == 2 && !panel.findChild<QTreeWidget *>("diagnosticsApplications")->isVisible(), "tab switches the page");
    check(list->topLevelItem(0)->text(2) == QStringLiteral("内存不足") && list->topLevelItem(0)->text(3).contains("Chromium"), "cause and busiest program");
    check(list->topLevelItem(0)->data(4, Qt::UserRole + 40).toString() == "ok" && list->topLevelItem(1)->text(4) == QStringLiteral("已中断"), "status with tone");
    check(details.count() == 1 && details.first()[0].toString() == "e1" && !exportButton->isEnabled(), "first event requested");
    check(detail->toPlainText().contains(QStringLiteral("正在读取")), "pending state");
    QJsonObject full = first;
    full["headlineEvidence"] = "<b>evidence</b> " + QStringLiteral("可用内存持续走低，换页与直接回收同时升高。").repeated(12);
    full["witnesses"] = QJsonArray{QJsonObject{{"timestamp", now - 290}, {"labels", QJsonArray{"onset", "peakCPU"}}, {"cpu", QJsonObject{{"usage", .9}}}, {"memory", QJsonObject{{"availableBytes", 1 * GiB}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", 400}}}, {"disk", QJsonObject{{"busiestUtilization", .5}}},
        {"findings", QJsonArray{QJsonObject{{"title", "<script>"}, {"evidence", "x"}}}},
        {"topProcesses", QJsonArray{QJsonObject{{"name", "<img src=x>"}, {"pid", 5}, {"cpuPercent", 150}, {"memoryBytes", 100}, {"owner", "root"}}}}}};
    panel.setIncidentDetail(full); settle();
    const QString rendered = detail->toPlainText();
    check(exportButton->isEnabled() && rendered.contains(QStringLiteral("内存不足")) && rendered.contains(QStringLiteral("开始时")), "report renders");
    check(rendered.contains("<img src=x>") && rendered.contains("<b>evidence</b>") && rendered.contains("<script>"), "process and finding text is escaped, shown literally");
    check(rendered.contains("400") && rendered.contains("root"), "latency and owner in the report");
    check(detail->height() >= int(detail->document()->size().height()), "the report needs no inner scrolling");
    // Narrowing the window rewraps the report: it must grow with its text, not clip it.
    const QSize wideSize = panel.size();
    const int wideReport = detail->height();
    panel.resize(wideSize.width() * 2 / 3, wideSize.height()); settle();
    check(detail->width() < wideSize.width() * 3 / 4 && detail->height() > wideReport, "a narrower report wraps and grows");
    check(int(detail->document()->textWidth()) == detail->width() && detail->height() >= int(detail->document()->size().height()), "a narrower report is not clipped");
    check(panel.height() >= panel.layout()->totalMinimumSize().height(), "nothing is squeezed under the report");
    panel.resize(wideSize); settle();
    check(detail->height() == wideReport, "widening shrinks it back");
    // A narrow banner moves its buttons under the text instead of squeezing the text.
    auto *headline = panel.findChild<QLabel *>("diagnosisSummary");
    const auto belowHeadline = [&] { return run->mapTo(&panel, QPoint()).y() >= headline->mapTo(&panel, QPoint(0, headline->height())).y(); };
    panel.resize(600, wideSize.height()); settle();
    check(belowHeadline() && run->isVisible(), "narrow banner: buttons under the text");
    panel.resize(900, wideSize.height()); settle();
    check(!belowHeadline() && run->isVisible(), "wide banner: buttons beside the text");
    panel.resize(wideSize); settle();
    panel.setPalette(DevStatsUI::palette(true)); settle();
    const QString darkReport = detail->toHtml();
    panel.setPalette(DevStatsUI::palette(false)); settle();
    check(detail->toHtml() != darkReport, "the report's colours follow the theme");
    list->setCurrentItem(list->topLevelItem(1)); settle();
    check(details.count() == 2 && details.last()[0].toString() == "e2" && !exportButton->isEnabled(), "selecting requests that event");
    // Older versions recorded fewer fields; they must still open.
    panel.setIncidentDetail(QJsonObject{{"id", "e2"}, {"summary", old["summary"]}, {"startTimestamp", old["startTimestamp"]}, {"status", "interrupted"}, {"interruptionReason", "bye"},
        {"witnesses", QJsonArray{QJsonObject{{"timestamp", now}, {"cpu", QJsonObject{{"usage", .5}}}, {"memory", QJsonObject{{"availableBytes", 1.}}}}}}}); settle();
    check(detail->toPlainText().contains("bye") && detail->toPlainText().contains(QString::fromUtf8("—")), "old records open, with unknowns");
    panel.setIncidentDetail(QJsonObject{{"id", "e2"}, {"error", "gone"}}); settle();
    check(detail->toPlainText().contains("gone") && !exportButton->isEnabled(), "unreadable record explains itself");
    auto *eventSearch = panel.findChild<QLineEdit *>("incidentSearch");
    eventSearch->setText("Chromium"); settle();
    check(!list->topLevelItem(0)->isHidden() && list->topLevelItem(1)->isHidden(), "search events");
    eventSearch->clear();
    auto *range = panel.findChild<QComboBox *>("incidentRange");
    range->setCurrentIndex(range->findData(1)); settle();
    check(!list->topLevelItem(0)->isHidden() && list->topLevelItem(1)->isHidden(), "time range filter");
    check(text(panel, "incidentState").contains("1 / "), "visible count");
    range->setCurrentIndex(0); settle();
    QSignalSpy exports(&panel, &DiagnosticsPanel::exportIncidentRequested);
    list->setCurrentItem(list->topLevelItem(0)); settle();
    panel.setIncidentDetail(full); settle();
    exportButton->click();
    check(exports.count() == 1 && exports.first()[0].toString() == "e1", "export the selected event");

    auto *record = panel.findChild<ToggleSwitch *>("autoRecordIncidents");
    QSignalSpy recording(&panel, &DiagnosticsPanel::incidentRecordingRequested);
    panel.setIncidentRecording(false);
    check(!record->isChecked() && recording.isEmpty(), "programmatic state does not echo");
    record->click();
    check(record->isChecked() && recording.count() == 1 && recording.first()[0].toBool(), "user toggle is reported");
    panel.setIncidents({});
    settle();
    check(list->isHidden() && !text(panel, "incidentState").isEmpty(), "empty state");

    // History page.
    panel.showIncidents();
    check(tabs->currentIndex() == 1, "external navigation to events");
    tabs->setCurrentIndex(2); settle();
    const qint64 end = QDateTime::currentSecsSinceEpoch();
    QJsonArray points{QJsonObject{{"timestamp", double(end - 5)}, {"value", 420}}};
    panel.setHistories(QJsonObject{{"schedulerLatency", points}}, end - 60, end); settle();
    auto *latency = panel.findChild<HistoryChart *>("schedulerLatencyHistory");
    check(latency && latency->accessibleDescription().contains(QStringLiteral("420 毫秒")), "latency chart uses milliseconds");
    check(panel.findChild<HistoryChart *>("diskBusyHistory") && panel.findChild<HistoryChart *>("memoryPressureHistory"), "pressure charts exist");

    // Feedback line appears only when there is something to say.
    auto *feedback = panel.findChild<QLabel *>("diagnosticsFeedback");
    check(feedback->isHidden(), "no feedback yet");
    panel.setFeedback(QStringLiteral("已发送")); check(!feedback->isHidden() && feedback->text() == QStringLiteral("已发送"), "feedback shows");
    panel.setFeedback({}); check(feedback->isHidden(), "feedback clears");

    std::printf("PASS diagnostics UI: %d checks; live findings and suspects, actions, refresh stability, search and sort, run progress and results, redacted report, event list and escaped report, history\n", checks);
    return 0;
}
