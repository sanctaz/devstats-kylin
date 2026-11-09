// Renders every page from synthetic but realistic data, in both themes and at the
// normal and minimum window sizes, and checks layout contracts on each render.
// Usage: test_ui_preview [output directory]; without a directory it only checks.
#include "../src/charts.h"
#include "../src/diagnostic_run.h"
#include "../src/diagnostics.h"
#include "../src/diagnostics_panel.h"
#include "../src/memory_relief_dialog.h"
#include "../src/monitor_window.h"
#include "../src/process_rules_panel.h"
#include "../src/report_html.h"
#include "../src/ui_components.h"
#include "../src/ui_theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QLayout>
#include <QListWidget>
#include <QScrollBar>
#include <QTimer>
#include <QTreeWidget>
#include <QToolButton>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr double MiB = 1024.0 * 1024.0, GiB = 1024.0 * MiB;
int failures = 0;
void check(bool condition, const QString &message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message.toUtf8().constData()); ++failures; }
}
QJsonObject averages(double value) { return {{"avg10", value}, {"avg60", value * .7}, {"avg300", value * .4}}; }
QJsonObject process(int pid, const QString &name, const QString &path, double cpu, double memory, double read = 0, double write = 0, const QString &state = "S") {
    return {{"pid", pid}, {"parentPID", 1}, {"startTicks", 1000 + pid}, {"name", name}, {"executablePath", path},
            {"projectPath", QString()}, {"cpuPercent", cpu}, {"state", state}, {"memoryBytes", memory},
            {"ioReadBytesPerSecond", read}, {"ioWriteBytesPerSecond", write}, {"threads", 8}, {"memoryGrowthBytesPerSecond", 0},
            {"applicationIdentity", name}};
}
QJsonObject system(int pid, const QString &name, const QString &owner, double cpu, double memory, bool kernel, const QString &state = "S", double ioWait = -1) {
    return {{"pid", pid}, {"parentPID", kernel ? 2 : 1}, {"startTicks", 100 + pid}, {"name", name}, {"owner", owner}, {"cpuPercent", cpu},
            {"state", state}, {"memoryBytes", memory}, {"kernelThread", kernel}, {"threads", 1},
            {"ioWaitPercent", ioWait >= 0 ? QJsonValue(ioWait) : QJsonValue(QJsonValue::Null)}};
}
QJsonObject snapshot(bool stressed, bool oldKernel) {
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.;
    QJsonArray perCore;
    for (int i = 0; i < 8; ++i) perCore.append(stressed ? .98 - i * .02 : .08 + i * .03);
    QJsonObject pressure;
    if (!oldKernel) pressure = {{"cpu", QJsonObject{{"some", averages(stressed ? 34 : 1)}, {"full", QJsonValue::Null}}},
        {"memory", QJsonObject{{"some", averages(stressed ? 22 : 0)}, {"full", averages(stressed ? 8 : 0)}}},
        {"io", QJsonObject{{"some", averages(stressed ? 18 : 0)}, {"full", averages(stressed ? 6 : 0)}}}};
    else pressure = {{"cpu", QJsonValue::Null}, {"memory", QJsonValue::Null}, {"io", QJsonValue::Null}};
    QJsonArray processes{
        process(4201, "firefox", "/usr/lib/firefox/firefox", stressed ? 312 : 18, stressed ? 5.2 * GiB : 1.6 * GiB, 2 * MiB, 800 * 1024),
        process(4230, "Isolated Web Co", "/usr/lib/firefox/firefox", stressed ? 96 : 4, 900 * MiB),
        process(5102, "code", "/usr/share/code/code", stressed ? 142 : 6, 1.8 * GiB, 500 * 1024, 300 * 1024),
        process(5340, "kysec-scan", "/opt/kysec/bin/kysec-scan", stressed ? 88 : 0.2, 240 * MiB, stressed ? 64 * MiB : 0, stressed ? 12 * MiB : 0, stressed ? "D" : "S"),
        process(6001, "python3", "/usr/bin/python3", stressed ? 54 : 1, 620 * MiB),
        process(6120, "ukui-menu", "/usr/bin/ukui-menu", 0.3, 180 * MiB),
        process(6200, "peony", "/usr/bin/peony", 0.5, 240 * MiB),
        process(6900, "devstats", "/home/user/.local/opt/devstats-0.8.0/libexec/devstats", 1.1, 72 * MiB)};
    QJsonArray others{
        system(812, "Xorg", "root", stressed ? 74 : 3, 310 * MiB, false),
        system(61, "kswapd0", "root", stressed ? 46 : 0, 0, true),
        system(322, "jbd2/sda2-8", "root", stressed ? 12 : 0, 0, true, stressed ? "D" : "S", stressed ? 22 : 0),
        system(903, "kylin-security", "root", stressed ? 38 : 1, 420 * MiB, false, "S", stressed ? 41 : 0),
        system(1120, "systemd-journal", "root", 0.4, 64 * MiB, false)};
    QJsonArray apps;
    for (const auto &value : processes) {
        const auto p = value.toObject();
        apps.append(QJsonObject{{"identity", p["name"]}, {"name", p["name"]}, {"processCount", 1}, {"processes", QJsonArray{p}},
            {"cpuAveragePercent", p["cpuPercent"]}, {"cpuComplete", true}, {"cpuObservationSeconds", 60}, {"cpuCoverageFraction", 1},
            {"ioReadBytesPerSecond", p["ioReadBytesPerSecond"]}, {"ioReadBytesPerSecondComplete", true},
            {"ioWriteBytesPerSecond", p["ioWriteBytesPerSecond"]}, {"ioWriteBytesPerSecondComplete", true}});
    }
    QJsonObject result{{"timestamp", now}, {"monotonicSeconds", 500.}, {"processMonotonicSeconds", 500.}, {"processesFresh", true}, {"processSampleAgeSeconds", 0.},
        {"cpu", QJsonObject{{"usage", stressed ? .97 : .14}, {"user", stressed ? .71 : .1}, {"system", stressed ? .22 : .04}, {"iowait", stressed ? .24 : .01}, {"irq", .02},
            {"coreCount", 8}, {"runningProcesses", stressed ? 15 : 2}, {"blockedProcesses", stressed ? 3 : 0}, {"load", QJsonArray{stressed ? 13.4 : .8, stressed ? 9.1 : .7, 4.0}},
            {"perCore", perCore}, {"uptimeSeconds", 91250}, {"runQueueWait", stressed ? 1.9 : .02}, {"forksPerSecond", 3}, {"contextSwitchesPerSecond", 9000}}},
        {"memory", QJsonObject{{"totalBytes", 16 * GiB}, {"availableBytes", stressed ? .32 * GiB : 8.2 * GiB}, {"usedBytes", stressed ? 15.68 * GiB : 7.8 * GiB}, {"fraction", stressed ? .98 : .49},
            {"cachedBytes", stressed ? .4 * GiB : 4 * GiB}, {"buffersBytes", 120 * MiB}, {"swapTotalBytes", 8 * GiB}, {"swapUsedBytes", stressed ? 5.2 * GiB : .1 * GiB}}},
        {"pressure", pressure},
        {"vm", QJsonObject{{"swapInBytesPerSecond", stressed ? 38 * MiB : 0}, {"swapOutBytesPerSecond", stressed ? 52 * MiB : 0}, {"majorFaultsPerSecond", stressed ? 900 : 0},
            {"oomKillDelta", 0}, {"allocStallsPerSecond", stressed ? 80 : 0}, {"directReclaimPagesPerSecond", stressed ? 9000 : 0}, {"kswapdScanPagesPerSecond", stressed ? 20000 : 0}}},
        {"diskIO", QJsonObject{{"readBytesPerSecond", stressed ? 92 * MiB : 300 * 1024}, {"writeBytesPerSecond", stressed ? 41 * MiB : 120 * 1024},
            {"busiestDevice", "nvme0n1"}, {"busiestUtilization", stressed ? .97 : .03}, {"busiestAwaitMilliseconds", stressed ? 340 : 1.2},
            {"devices", QJsonArray{QJsonObject{{"device", "nvme0n1"}, {"readBytesPerSecond", stressed ? 92 * MiB : 300 * 1024}, {"writeBytesPerSecond", stressed ? 41 * MiB : 120 * 1024},
                {"utilization", stressed ? .97 : .03}, {"awaitMilliseconds", stressed ? 340 : 1.2}}}}}},
        {"disks", QJsonArray{QJsonObject{{"device", "/dev/nvme0n1p2"}, {"mountPoint", "/"}, {"filesystem", "ext4"}, {"totalBytes", 476 * GiB}, {"availableBytes", 180 * GiB}, {"usageFraction", .62}},
            QJsonObject{{"device", "/dev/nvme0n1p3"}, {"mountPoint", "/data"}, {"filesystem", "ext4"}, {"totalBytes", 931 * GiB}, {"availableBytes", 420 * GiB}, {"usageFraction", .55}}}},
        {"network", QJsonObject{{"interface", "wlan0"}, {"isUp", true}, {"uploadBytesPerSecond", 240 * 1024}, {"downloadBytesPerSecond", 3.4 * MiB}, {"txBytes", 12 * GiB}, {"rxBytes", 88 * GiB},
            {"ipv4", QJsonArray{"192.168.1.108"}}, {"ipv6", QJsonArray{"fe80::17b2:9af0:c451:2627"}}}},
        {"battery", QJsonObject{{"isPresent", true}, {"level", .72}, {"state", "charging"}, {"healthPercent", 94.5}, {"cycleCount", 186}, {"powerSource", "ac"}}},
        {"thermal", QJsonObject{{"hottestCelsius", stressed ? 93 : 52}, {"hottestZone", "cpu-thermal"}, {"frequencyFraction", stressed ? .58 : .35}, {"limited", false},
            {"clusters", QJsonArray{QJsonObject{{"cpus", 4}, {"currentMHz", stressed ? 1800 : 1100}, {"maximumMHz", 3100}, {"limitMHz", 3100}}, QJsonObject{{"cpus", 4}, {"currentMHz", stressed ? 1400 : 800}, {"maximumMHz", 2050}, {"limitMHz", 2050}}}}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", stressed ? 860 : 2.4}, {"schedulerP95Ms", stressed ? 500 : 2}, {"schedulerSamples", 10}, {"displayAvailable", true}, {"displayMaxMs", stressed ? 640 : 1.3}, {"displaySamples", 2}}},
        {"processes", processes}, {"systemProcesses", others}, {"processStates", QJsonObject{{"total", 214}, {"running", stressed ? 15 : 2}, {"blocked", stressed ? 3 : 0}, {"zombie", 0}}},
        {"attributedCpuPercent", stressed ? 760 : 112}, {"processCount", 8}, {"activityApplications", apps}, {"activityObservationSeconds", 60},
        {"groups", QJsonArray{QJsonObject{{"path", "/home/user/work/devstats"}, {"name", "devstats"}, {"cpuPercent", 142}, {"memoryBytes", 1.8 * GiB}, {"processes", QJsonArray{processes[2]}}}}},
        {"memoryOptimization", QJsonObject{}}, {"incidentRecording", true}};
    result["diagnostics"] = diagnoseSystem(result);
    result["suspects"] = rankSuspects(result, suspectBasis(result["diagnostics"].toObject()));
    DiagnosticRun run;
    run.start(0, now - 12);
    for (int i = 0; i < 6; ++i) { auto sample = result; sample["processMonotonicSeconds"] = 500. + i * 2; run.observe(sample, 2000 * i, now - 12 + i * 2); }
    run.finish("completed", 10000, now - 2);
    result["diagnosticRun"] = stressed ? run.result() : DiagnosticRun().result();
    const QJsonArray rules{
        QJsonObject{{"id", "r1"}, {"enabled", true}, {"match", "path"}, {"value", "/opt/kysec/bin/kysec-scan"}, {"cpuAbovePercent", 50}, {"sustainSeconds", 30}, {"forceKill", true}, {"graceSeconds", 10}},
        QJsonObject{{"id", "r2"}, {"enabled", true}, {"match", "name"}, {"value", "python3"}, {"cpuAbovePercent", 200}, {"sustainSeconds", 60}, {"forceKill", false}, {"graceSeconds", 10}},
        QJsonObject{{"id", "r3"}, {"enabled", true}, {"match", "path"}, {"value", "/usr/bin/sogou-updater"}, {"forceKill", true}, {"graceSeconds", 10}},
        QJsonObject{{"id", "r4"}, {"enabled", false}, {"match", "path"}, {"value", "/usr/bin/kylin-assistant"}, {"cpuAbovePercent", 80}, {"sustainSeconds", 120}, {"forceKill", true}, {"graceSeconds", 15}}};
    QJsonObject status{
        {"r1", QJsonObject{{"matched", 1}, {"topCpuPercent", 88}, {"armed", 1}, {"armedSeconds", 18}, {"sustainSeconds", 30}, {"averageCpuPercent", 92}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}},
        {"r2", QJsonObject{{"matched", 1}, {"topCpuPercent", 54}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}},
        {"r3", QJsonObject{{"matched", 0}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}},
        {"r4", QJsonObject{{"matched", 0}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}}};
    QJsonArray log;
    const QList<QStringList> rows{
        {"kysec-scan", "4410", "CPU 超过 50%，持续 30 秒", "exited", "已结束（3 秒）"},
        {"sogou-updater", "3982", "运行时立即结束", "killed", "未响应结束请求，已强制结束"},
        {"python3", "3711", "CPU 超过 200%，持续 60 秒", "survived", "10 秒后仍在运行，程序没有响应结束请求"},
        {"ukui-window-sw", "2201", "运行时立即结束", "refused", "监控、桌面、终端或会话基础进程受到保护，请使用系统监视器查看。"}};
    int age = 90;
    for (const auto &row : rows) {
        QJsonObject entry{{"id", row[1]}, {"timestamp", now - age}, {"name", row[0]}, {"pid", row[1].toInt()}, {"executablePath", "/usr/bin/" + row[0]},
            {"condition", row[2]}, {"phase", row[3]}, {"message", row[4]}, {"ok", row[3] != "refused"}, {"code", row[3]}};
        if (row[2].startsWith("CPU")) entry["triggerCpuPercent"] = 132;
        log.append(entry);
        age += 2600;
    }
    result["processAutomation"] = QJsonObject{{"enabled", true}, {"rules", rules}, {"status", status}, {"log", log}, {"storageError", QString()}};
    return result;
}
QJsonObject histories(qint64 start, qint64 end, bool stressed) {
    const QList<QPair<QString, double>> metrics{{"cpu", stressed ? .85 : .2}, {"memory", stressed ? .95 : .5}, {"diskRead", 40 * MiB}, {"diskWrite", 12 * MiB},
        {"networkDownload", 3 * MiB}, {"networkUpload", 300 * 1024}, {"battery", .72}, {"cpuIowait", stressed ? .2 : .02}, {"swapIn", 10 * MiB}, {"swapOut", 20 * MiB},
        {"memoryPressure", .1}, {"ioPressure", .08}, {"schedulerLatency", stressed ? 400 : 3}, {"diskBusy", stressed ? .8 : .05}};
    QJsonObject result;
    for (const auto &metric : metrics) {
        QJsonArray points;
        for (qint64 at = start; at <= end; at += 2) {
            const double phase = double(at - start) / 60.;
            const double value = metric.second * (.75 + .25 * std::sin(phase) * std::cos(phase * .37));
            points.append(QJsonObject{{"timestamp", double(at)}, {"value", value}, {"minimum", value}, {"maximum", value}, {"count", 1}, {"resolutionSeconds", 2}});
        }
        result[metric.first] = points;
    }
    return result;
}
QJsonArray incidents() {
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.;
    QJsonArray list;
    const QList<QStringList> rows{
        {"内存不足，系统正在回收内存", "firefox", "memory", "recovered"},
        {"CPU 繁忙，占用最高的是 firefox（3.1 核）", "firefox", "cpu", "recovered"},
        {"系统出现短暂冻结", "Xorg", "cpu", "recovered"},
        {"磁盘繁忙，读写响应变慢", "kysec-scan", "io", "active"}};
    int age = 1800;
    for (const auto &row : rows) {
        list.append(QJsonObject{{"id", row[0]}, {"startTimestamp", now - age}, {"lastTimestamp", now - age + 42}, {"durationSeconds", 42}, {"summary", row[0]},
            {"kinds", QJsonArray{row[2]}}, {"status", row[3]}, {"severity", "attention"}, {"strongestApplicationBasis", row[2]},
            {"strongestApplication", QJsonObject{{"name", row[1]}, {"cpuPercent", 312}, {"memoryBytes", 5.2 * GiB}, {"ioBytesPerSecond", 64 * MiB}}}});
        age += 7200;
    }
    return list;
}
QJsonObject incidentDetail() {
    const auto sample = [](const QJsonObject &base, const QStringList &labels) {
        QJsonArray marks; for (const auto &mark : labels) marks.append(mark);
        return QJsonObject{{"timestamp", base["timestamp"]}, {"labels", marks}, {"cpu", base["cpu"]}, {"memory", base["memory"]}, {"vm", base["vm"]},
            {"responsiveness", base["responsiveness"]}, {"disk", QJsonObject{{"busiestUtilization", .97}, {"device", "nvme0n1"}}},
            {"findings", base["diagnostics"].toObject()["findings"]},
            {"topProcesses", QJsonArray{QJsonObject{{"name", "firefox"}, {"pid", 4201}, {"cpuPercent", 312}, {"memoryBytes", 5.2 * GiB}},
                QJsonObject{{"name", "Xorg"}, {"owner", "root"}, {"pid", 812}, {"cpuPercent", 74}, {"memoryBytes", 310 * MiB}},
                QJsonObject{{"name", "kswapd0"}, {"owner", "root"}, {"pid", 61}, {"cpuPercent", 46}, {"memoryBytes", 0}}}}};
    };
    auto event = incidents().first().toObject();
    event["headlineEvidence"] = QStringLiteral("可用 328 MB（2%） · 换入 38 MB/s，换出 52 MB/s · 直接回收 80 次/s");
    const auto stressed = snapshot(true, false);
    event["witnesses"] = QJsonArray{sample(stressed, {"onset"}), sample(stressed, {"peakCPU", "peakMemoryPressure"}), sample(stressed, {"last"})};
    return event;
}

// Nothing in a visible page may be clipped by the window's right edge.
// Each pass delivers only the layout requests already queued, and a nested layout
// queues its parent's, so a single pass would capture a half-finished layout.
void settle() { for (int pass = 0; pass < 10; ++pass) QApplication::processEvents(); }

void checkLayout(MonitorWindow &window, const QString &name) {
    auto *pages = window.findChild<QWidget *>("modulePages");
    for (auto *widget : pages->findChildren<QWidget *>()) {
        if (!widget->isVisible()) continue;
        const QString widgetName = widget->objectName().isEmpty() ? widget->metaObject()->className() : widget->objectName();
        // A layout squeezed below its content means overlapping or clipped rows.
        if (widget->layout()) {
            const auto *layout = widget->layout();
            const int needed = layout->hasHeightForWidth() ? layout->totalHeightForWidth(widget->width()) : layout->totalMinimumSize().height();
            check(widget->height() >= needed, QStringLiteral("%1: %2 is squeezed (%3 < %4)").arg(name, widgetName).arg(widget->height()).arg(needed));
        }
        if (!qobject_cast<QLabel *>(widget) && !qobject_cast<QAbstractButton *>(widget)) continue;
        const QRect rect(widget->mapTo(&window, QPoint(0, 0)), widget->size());
        check(rect.right() <= window.width() + 1, QStringLiteral("%1: %2 overflows the window (%3 > %4)").arg(name, widgetName).arg(rect.right()).arg(window.width()));
    }
    auto *navigation = window.findChild<QListWidget *>("navigation");
    check(navigation->verticalScrollBar()->maximum() == 0, name + ": sidebar needs no scrolling");
}
}

int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", qgetenv("QT_QPA_PLATFORM").isEmpty() ? QByteArray("offscreen") : qgetenv("QT_QPA_PLATFORM"));
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    const QString output = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
    if (!output.isEmpty()) QDir().mkpath(output);
    const QStringList modules{"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics", "automation", "projects"};
    const qint64 end = QDateTime::currentSecsSinceEpoch();
    struct Variant { const char *name; int appearance; QSize size; bool stressed, oldKernel; };
    const Variant variants[] = {{"dark", 1, QSize(1100, 780), true, false}, {"light", 2, QSize(1100, 780), true, false},
                                {"dark-idle", 1, QSize(1100, 780), false, false}, {"dark-min", 1, QSize(780, 560), true, true}, {"light-min", 2, QSize(780, 560), false, true}};
    for (const auto &variant : variants) {
        MonitorWindow window;
        window.setAttribute(Qt::WA_DeleteOnClose, false);
        window.setAppearance(variant.appearance);
        const auto data = snapshot(variant.stressed, variant.oldKernel);
        const auto automation = data["processAutomation"].toObject();
        window.setPreferences(false, 0, {"/home/user/work/devstats"}, {}, true, true);
        window.rulesPanel()->setConfiguration(true, automation["rules"].toArray());
        window.setIncidents(incidents());
        window.setHistories(histories(end - 600, end, variant.stressed), end - 600, end);
        window.setSnapshot(data);
        window.resize(variant.size);
        window.show();
        settle();
        check(window.size() == variant.size, QStringLiteral("%1: window keeps its size").arg(QString::fromLatin1(variant.name)));
        for (const auto &module : modules) {
            window.setSelectedModule(module);
            settle();
            if (module == "diagnostics") {
                // Details arrive asynchronously from the worker in the real application.
                window.setIncidentDetail(incidentDetail());
            }
            settle();
            const QString name = QString::fromLatin1(variant.name) + '-' + module;
            checkLayout(window, name);
            if (module == "disk") {
                // Heaviest reader first; root processes, whose volume needs root, follow by disk wait.
                auto *disk = window.findChild<QTreeWidget *>("diskProcesses");
                const int rows = disk ? disk->topLevelItemCount() : 0;
                check(rows > 0 && disk->topLevelItem(0)->text(0) == QString::fromLatin1(variant.stressed ? "kysec-scan" : "firefox"), name + ": disk ranking starts with the heaviest reader");
                if (variant.stressed)
                    check(rows >= 2 && disk->topLevelItem(rows - 2)->text(0) == "kylin-security" && disk->topLevelItem(rows - 1)->text(0) == "jbd2/sda2-8"
                          && disk->topLevelItem(rows - 1)->text(3) == QString::fromUtf8("—") && disk->topLevelItem(rows - 2)->text(5) == "41%", name + ": system processes follow by disk wait");
                else check(rows == 2, name + ": idle processes and uncounted system processes stay out");
            }
            if (output.isEmpty()) continue;
            check(window.grab().save(output + '/' + name + ".png"), name + ": saved");
            auto *body = window.findChild<QWidget *>(module + "Page");
            check(body && body->grab().save(output + '/' + name + "-full.png"), name + ": full page saved");
        }
        window.setSelectedModule("diagnostics");
        auto *tabs = window.findChild<SectionTabs *>("diagnosticsSections");
        for (int tab = 1; tab < tabs->count(); ++tab) {
            tabs->setCurrentIndex(tab);
            settle();
            if (tab == 1) { window.setIncidentDetail(incidentDetail()); settle(); }
            const QString name = QString::fromLatin1(variant.name) + "-diagnostics-tab" + QString::number(tab);
            checkLayout(window, name);
            if (!output.isEmpty()) {
                auto *body = window.findChild<QWidget *>("diagnosticsPage");
                body->grab().save(output + '/' + name + "-full.png");
            }
        }
        if (!output.isEmpty() && QString::fromLatin1(variant.name) == "dark") {
            // The exported pages, built from the same data the window shows.
            check(DevStatsReport::writePrivate(output + "/diagnosis-report.html", DevStatsReport::diagnosisHtml(window.diagnosticReport()).toUtf8()).isEmpty(), "diagnosis report exported");
            check(DevStatsReport::writePrivate(output + "/incident-report.html", DevStatsReport::incidentHtml(incidentDetail()).toUtf8()).isEmpty(), "incident report exported");
        }
        if (!output.isEmpty() && (variant.size == QSize(1100, 780))) {
            const QString prefix = output + '/' + QString::fromLatin1(variant.name);
            window.setSelectedModule("automation");
            QTimer::singleShot(100, &app, [prefix] {
                auto *editor = QApplication::activeModalWidget();
                check(editor && editor->objectName() == "processRuleEditor", "rule editor opens");
                check(editor->grab().save(prefix + "-rule-editor.png"), "rule editor saved");
                qobject_cast<QDialog *>(editor)->reject();
            });
            window.rulesPanel()->addRuleFor(QJsonObject{{"executablePath", "/opt/kysec/bin/kysec-agent"}, {"cpuPercent", 188.}, {"pid", 77}});
            window.findChild<QToolButton *>("settingsButton")->click();
            settle();
            auto *settings = window.findChild<QDialog *>("preferencesDialog");
            check(settings && settings->grab().save(prefix + "-settings.png"), "settings saved");
            settings->close();
            MemoryReliefDialog relief(&window);
            relief.setSnapshot(data);
            relief.show();
            settle();
            check(relief.grab().save(prefix + "-relief.png"), "relief dialog saved");
            relief.close();
        }
        window.close();
    }
    std::printf(failures ? "FAIL: %d checks\n" : "PASS: every page renders in both themes and sizes without overflow\n", failures);
    return failures ? 1 : 0;
}
