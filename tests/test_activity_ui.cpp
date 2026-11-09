#include "../src/monitor_window.h"
#include <QApplication>
#include <QJsonArray>
#include <QScrollBar>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL activity UI: %s\n", message); std::exit(1); }
}
QJsonObject app(int identity, int processes = 2) {
    QJsonArray rows;
    for (int i = 0; i < processes; ++i) rows.append(QJsonObject{{"name", "worker"}, {"pid", identity * 100 + i},
        {"startTicks", identity * 1000 + i}, {"cpuAveragePercent", 20 + i}, {"cpuObservationSeconds", 10},
        {"ioReadBytesPerSecond", 10}, {"ioWriteBytesPerSecond", QJsonValue(QJsonValue::Null)}});
    return {{"identity", QString::number(identity)}, {"name", "App " + QString::number(identity)},
        {"processes", rows}, {"processCount", processes}, {"cpuAveragePercent", 80 - identity},
        {"cpuObservationSeconds", 20}, {"cpuCoverageFraction", .5}, {"cpuComplete", false},
        {"ioReadBytesPerSecond", 20}, {"ioReadBytesPerSecondComplete", true},
        {"ioWriteBytesPerSecond", QJsonValue(QJsonValue::Null)}, {"ioWriteBytesPerSecondComplete", false}};
}
}
int main(int argc, char **argv) {
    QApplication application(argc, argv);
    MonitorWindow window;
    window.setSelectedModule("battery");
    QJsonArray apps;
    for (int i = 1; i <= 12; ++i) apps.append(app(i));
    QJsonObject snapshot{{"activityApplications", apps}, {"activityObservationSeconds", 20}};
    window.setSnapshot(snapshot); window.show(); application.processEvents();
    auto *tree = window.findChild<QTreeWidget *>("batteryImpactTable");
    check(tree && tree->topLevelItemCount() == 10, "rank at most ten application rows");
    check(tree->rootIsDecorated(), "application rows expose expansion controls");
    auto *first = tree->topLevelItem(0);
    check(first->childCount() == 2 && first->text(1) == "2", "application count and child details");
    check(first->text(2).endsWith(" *"), "partial application activity is visibly marked");
    check(first->child(0)->text(1) == "PID 100", "expanded process detail includes PID");
    check(first->child(0)->text(4) == QString::fromUtf8("—"), "unknown IO remains unknown in details");
    first->setExpanded(true); first->child(1)->setSelected(true);
    application.processEvents();
    tree->verticalScrollBar()->setValue(2); const int scroll = tree->verticalScrollBar()->value();
    window.setSnapshot(snapshot); application.processEvents();
    first = tree->topLevelItem(0);
    check(first->isExpanded(), "refresh preserves expanded application identity");
    check(first->child(1)->isSelected(), "refresh preserves selected PID identity");
    check(tree->verticalScrollBar()->value() == scroll, "refresh preserves tree scroll position");
    auto exited = app(1, 0); exited["noReadableProcesses"] = true;
    snapshot["activityApplications"] = QJsonArray{exited}; window.setSnapshot(snapshot);
    check(tree->topLevelItem(0)->text(1).contains(QString::fromUtf8("最近结束")), "departed application is explicitly labeled");
    std::puts("PASS activity UI: ten applications, counts, expandable PID details, partial/unknown labels, expansion/selection/scroll refresh, recent exit label");
}
