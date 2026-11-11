// The charts get a complete range about once a minute and single points in
// between. What they draw, and what a report exports, must not depend on how the
// points arrived.
#include "../src/charts.h"
#include "../src/controller.h"
#include "../src/history_feed.h"
#include "../src/monitor_window.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
int checks = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL history feed: %s\n", what); std::exit(1); }
}
void drain() {
    for (int pass = 0; pass < 6; ++pass) { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); }
}
bool waitFor(const std::function<bool()> &test, int limit = 15000) {
    QElapsedTimer clock; clock.start();
    while (!test() && clock.elapsed() < limit) { QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec(); }
    return test();
}
QJsonObject live(qint64 at, double value) {
    return {{"timestamp", double(at)}, {"value", value}, {"minimum", value}, {"maximum", value}, {"count", 1}, {"resolutionSeconds", 2}};
}
QJsonObject minute(qint64 at, double value, double low, double high) {
    return {{"timestamp", double(at)}, {"firstSample", double(at - 30)}, {"lastSample", double(at + 28)}, {"value", value},
        {"minimum", low}, {"maximum", high}, {"count", 30}, {"resolutionSeconds", 60}};
}
// What a chart draws for these points when they all arrive as one complete range.
QImage drawnWhole(const QJsonArray &points, qint64 start, qint64 end, QString *description) {
    HistoryChart chart(true);
    chart.resize(640, 200);
    chart.setHistory(HistoryFeed(QJsonObject{{"cpu", points}}, start, end), "cpu");
    *description = chart.accessibleDescription();
    return chart.grab().toImage();
}

void feedKeepsTheRange() {
    HistoryFeed empty;
    check(empty.isEmpty() && empty.series("cpu").isEmpty() && empty.generation() == 0, "a new feed holds nothing");
    HistoryFeed feed(QJsonObject{{"cpu", QJsonArray{live(100, .1), live(102, .2)}}}, 0, 102);
    const auto generation = feed.generation();
    check(!feed.isEmpty() && generation != 0 && feed.series("cpu") == QJsonArray{live(100, .1), live(102, .2)}, "a complete range is returned as it came");
    feed.append(QJsonObject{{"cpu", live(104, .3)}, {"memory", live(104, .5)}}, 101, 104);
    check(feed.generation() == generation && feed.start() == 101 && feed.end() == 104, "adding points keeps the complete range and moves the range shown");
    check(feed.series("cpu") == QJsonArray{live(102, .2), live(104, .3)}, "points that left the range are dropped and new ones follow");
    check(feed.series("memory") == QJsonArray{live(104, .5)}, "a metric missing from the complete range still collects points");
    const HistoryFeed copy = feed;
    feed.append(QJsonObject{{"cpu", live(106, .4)}}, 103, 106);
    check(copy.series("cpu") == QJsonArray{live(102, .2), live(104, .3)} && feed.series("cpu") == QJsonArray{live(104, .3), live(106, .4)},
          "a copy keeps what it had when it was taken");
    feed.reset(QJsonObject{{"cpu", QJsonArray{live(200, .7)}}}, 100, 200);
    check(feed.generation() != generation && feed.recent("cpu").isEmpty() && feed.series("cpu") == QJsonArray{live(200, .7)}, "a new complete range starts over");
}

void chartDrawsTheSameEitherWay() {
    const qint64 t0 = 1770000000;
    // Minute averages for the older part, then two-second samples with a stretch
    // when nothing ran, and one unavailable reading listed out of order.
    QJsonArray all;
    for (int i = 0; i < 6; ++i) all.append(minute(t0 - 570 + i * 60, .2 + .05 * i, .1, .5 + .05 * i));
    for (qint64 at = t0 - 240; at <= t0; at += 2)
        if (at <= t0 - 120 || at >= t0 - 100) all.append(live(at, .3 + .2 * std::sin(at / 7.0)));
    all.append(QJsonObject{{"timestamp", double(t0 - 50)}, {"value", QJsonValue::Null}});

    HistoryFeed feed(QJsonObject{{"cpu", all}}, t0 - 600, t0);
    HistoryChart shown(true), hidden(true);
    shown.resize(640, 200); hidden.resize(640, 200);
    shown.setHistory(feed, "cpu");
    QString description;
    const QImage first = drawnWhole(all, t0 - 600, t0, &description);
    check(shown.grab().toImage() == first && shown.accessibleDescription() == description, "a complete range draws the same in any chart");
    check(description.startsWith(QStringLiteral("末次采样 ")), "the fixture has something to draw");

    for (int step = 1; step <= 45; ++step) {
        const qint64 at = t0 + step * 2;
        QJsonObject point = live(at, .5 + .4 * std::sin(step / 3.0));
        if (step == 7) point = live(at - 2, .9);                          // a second sample within the same second
        if (step == 12) point = live(at - 5, .05);                        // older than the newest point
        if (step == 20) point.insert("value", QJsonValue::Null);          // an unavailable reading
        if (step == 30) {
            feed.append(QJsonObject{}, at - 600, at);                     // a sample without this metric still moves the range
        } else {
            feed.append(QJsonObject{{"cpu", point}}, at - 600, at);
            all.append(point);
        }
        shown.setHistory(feed, "cpu");
        const QImage whole = drawnWhole(all, at - 600, at, &description);
        check(shown.grab().toImage() == whole, "a chart that gets each point draws what the complete range draws");
        check(shown.accessibleDescription() == description, "a chart that gets each point reports the same latest value");
        if (step == 10 || step == 45) {
            hidden.setHistory(feed, "cpu");
            check(hidden.grab().toImage() == whole && hidden.accessibleDescription() == description, "a chart that was not shown catches up in one step");
        }
        if (step == 45) check(whole != first, "the added points change what is drawn");
    }
    check(feed.series("cpu").size() < all.size(), "the oldest points have left the range by now");
    check(shown.grab().toImage() == drawnWhole(feed.series("cpu"), feed.start(), feed.end(), &description), "the exported series draws the same chart");

    const QJsonArray replacement{live(t0, .1)};
    shown.setHistory(HistoryFeed(QJsonObject{{"cpu", replacement}}, t0 - 600, t0), "cpu");
    check(shown.grab().toImage() == drawnWhole(replacement, t0 - 600, t0, &description) && shown.accessibleDescription() == description,
          "a new complete range replaces everything shown before");
    shown.setHistory(HistoryFeed(), "cpu");
    check(shown.accessibleDescription() == QStringLiteral("此时间段尚无记录"), "an empty feed clears the chart");
}

void windowShowsAddedPoints() {
    const qint64 t0 = 1770000000;
    MonitorWindow window;
    window.setSelectedModule("overview");
    HistoryFeed feed(QJsonObject{{"cpu", QJsonArray{live(t0, .4)}}}, t0 - 120, t0);
    window.setHistories(feed);
    const auto charts = window.findChildren<HistoryChart *>("cpuHistory");
    check(charts.size() == 1, "the CPU page has its chart");
    feed.append(QJsonObject{{"cpu", live(t0 + 2, .9)}}, t0 - 118, t0 + 2);
    window.setHistories(feed);
    window.setSelectedModule("cpu");
    check(charts.first()->accessibleDescription().contains("90%"), "a chart on another page catches the points added while it was hidden");
    feed.append(QJsonObject{{"cpu", live(t0 + 4, .25)}}, t0 - 116, t0 + 4);
    window.setHistories(feed);
    check(charts.first()->accessibleDescription().contains("25%"), "the chart in view follows each added point");
    window.setSelectedModule("diagnostics");
    const auto report = window.diagnosticReport();
    check(report.value("history").toObject().value("cpu").toArray() == QJsonArray{live(t0, .4), live(t0 + 2, .9), live(t0 + 4, .25)}
          && report.value("historyStart").toDouble() == double(t0 - 116) && report.value("historyEnd").toDouble() == double(t0 + 4),
          "an exported report includes the points added since the last complete range");
}

// The real worker on this machine, through the controller, into a real window.
void controllerDeliversAddedPoints() {
    AppController controller;
    controller.start(true); drain();
    controller.showModule("diagnostics"); drain();
    int snapshots = 0;
    QObject::connect(&controller, &AppController::snapshotUpdated, [&] { ++snapshots; });
    check(waitFor([&] { return snapshots >= 3; }), "the worker keeps sampling");
    const auto current = [&] {
        const auto report = controller.window()->diagnosticReport();
        const auto cpu = report.value("history").toObject().value("cpu").toArray();
        const double newest = std::floor(controller.latestSnapshot().value("timestamp").toDouble());
        return cpu.size() >= 3 && report.value("historyEnd").toDouble() == newest && cpu.last().toObject().value("timestamp").toDouble() == newest;
    };
    check(waitFor(current), "the window holds every sample taken since it opened, up to the newest");
    controller.stop();
}
}

int main(int argc, char **argv) {
    QTemporaryDir root;
    check(root.isValid(), "isolated root");
    qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8()); qputenv("XDG_DATA_HOME", (root.path() + "/share").toUtf8());
    qputenv("DEVSTATS_DATA_DIR", (root.path() + "/data").toUtf8());
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    feedKeepsTheRange();
    chartDrawsTheSameEitherWay();
    windowShowsAddedPoints();
    controllerDeliversAddedPoints();
    drain();
    std::printf("PASS history feed: %d checks\n", checks);
    return 0;
}
