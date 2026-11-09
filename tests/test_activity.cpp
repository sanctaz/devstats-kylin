#include "../src/activity_tracker.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QJsonValue>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL activity: %s\n", message); std::exit(1); }
}
void near(double actual, double expected, const char *message) {
    if (!std::isfinite(actual) || std::abs(actual - expected) > 0.000001) {
        std::fprintf(stderr, "FAIL activity: %s (actual %.12g, expected %.12g)\n", message, actual, expected);
        std::exit(1);
    }
}
QJsonObject row(int pid, int identity, QJsonValue cpu = QJsonValue(QJsonValue::Null)) {
    return {{"pid", pid}, {"startTicks", identity}, {"cpuPercent", cpu},
        {"name", "fixture"}, {"ioReadBytesPerSecond", 1234}, {"ioWriteBytesPerSecond", 5678}};
}
QJsonObject sample(RecentActivityTracker &tracker, double time, const QJsonArray &rows) {
    QJsonObject snapshot {{"processMonotonicSeconds", time}, {"processes", rows}};
    tracker.apply(snapshot);
    return snapshot;
}
QJsonObject single(RecentActivityTracker &tracker, double time, QJsonValue cpu, int identity = 100) {
    return sample(tracker, time, QJsonArray {row(1, identity, cpu)}).value("processes").toArray().first().toObject();
}
void unknown(const QJsonObject &process, const char *message) {
    check(process.value("cpuAveragePercent").isNull(), message);
    near(process.value("cpuObservationSeconds").toDouble(-1), 0, "unknown CPU has zero observation time");
}
void measured(const QJsonObject &process, double average, double seconds, const char *message) {
    check(process.value("cpuAveragePercent").isDouble(), message);
    near(process.value("cpuAveragePercent").toDouble(), average, message);
    near(process.value("cpuObservationSeconds").toDouble(-1), seconds, "measured observation duration");
}

void testWeightedIntervals() {
    RecentActivityTracker tracker;
    unknown(single(tracker, 0.0001, QJsonValue(QJsonValue::Null)), "first sample is unknown");
    measured(single(tracker, 2.0001, 10), 10, 2, "two-second interval");
    measured(single(tracker, 10.0001, 50), 42, 10, "eight-second interval is weighted by duration");
    const auto last = single(tracker, 20.0001, 90);
    measured(last, 66, 20, "irregular 2/8/10-second observations use CPU seconds, not arithmetic mean");
    near(last.value("cpuPercent").toDouble(), 90, "raw latest CPU remains unchanged");
    near(last.value("ioReadBytesPerSecond").toDouble(), 1234, "disk read indicator remains separate");
    near(last.value("ioWriteBytesPerSecond").toDouble(), 5678, "disk write indicator remains separate");
    check(!last.contains("watts") && !last.contains("energyScore"), "CPU activity must not become estimated energy");

    RecentActivityTracker multicore;
    unknown(single(multicore, 1, 250), "a finite first value still needs an observed baseline");
    measured(single(multicore, 3, 250), 250, 2, "multi-core activity may exceed 100 percent");
    measured(single(multicore, 11, 150), 170, 10, "multi-core CPU seconds remain unclamped");

    RecentActivityTracker zeroClock;
    unknown(single(zeroClock, 0, QJsonValue(QJsonValue::Null)), "a monotonic origin of zero is a valid baseline");
    measured(single(zeroClock, 2, 50), 50, 2, "the first interval after a zero clock origin is retained");
}

void testSixtySecondBoundary() {
    RecentActivityTracker tracker;
    single(tracker, 100, QJsonValue(QJsonValue::Null));
    single(tracker, 110, 10);
    single(tracker, 130, 20);
    single(tracker, 150, 40);
    measured(single(tracker, 170, 80), (20.0 + 40 + 80) / 3, 60,
        "an interval ending at the window boundary contributes nothing");
    measured(single(tracker, 175, 100), (20.0 * 15 + 40 * 20 + 80 * 20 + 100 * 5) / 60, 60,
        "an interval crossing the window boundary is proportionally clipped");
    measured(single(tracker, 190, 0), (40.0 * 20 + 80 * 20 + 100 * 5) / 60, 60,
        "old observations age out without fabricated zero samples");
}

void testInactiveIntervals() {
    RecentActivityTracker tracker;
    single(tracker, 1, QJsonValue(QJsonValue::Null));
    single(tracker, 3, 100);
    for (int step = 1; step <= 28; ++step) single(tracker, 3 + step * 2, 0);
    measured(single(tracker, 61, 0), 100.0 * 2 / 60, 60,
        "merged inactive intervals preserve the preceding measured CPU work");
    measured(single(tracker, 63, 0), 0, 60, "inactive CPU is measured zero after old work leaves the window");
    measured(single(tracker, 65, 0), 0, 60, "a merged inactive interval clips to sixty seconds");
    measured(single(tracker, 70, 60), 5, 60, "new CPU work is weighted against a clipped merged inactive interval");
    for (int step = 1; step <= 1000; ++step) single(tracker, 70 + step * 2, 0);
    measured(single(tracker, 2072, 0), 0, 60, "long inactivity remains a bounded sixty-second observation");
}

void testDiscontinuities() {
    RecentActivityTracker tracker;
    single(tracker, 10, QJsonValue(QJsonValue::Null));
    measured(single(tracker, 12, 80), 80, 2, "activity before discontinuity");
    unknown(single(tracker, 43, 40), "a gap longer than thirty seconds clears the history");
    measured(single(tracker, 45, 60), 60, 2, "fresh observations restart after a long gap");
    measured(single(tracker, 75, 20), (60.0 * 2 + 20 * 30) / 32, 32,
        "an interval exactly thirty seconds remains measured");
    unknown(single(tracker, 77, QJsonValue(QJsonValue::Null)), "missing/reset CPU counter clears prior activity");
    measured(single(tracker, 79, 50), 50, 2, "observations restart after a CPU counter reset");
    unknown(single(tracker, 81, -1), "negative CPU is unavailable rather than negative activity");
    measured(single(tracker, 83, 0), 0, 2, "measured zero differs from unavailable CPU");
    unknown(single(tracker, 82, 20), "a backwards clock resets the activity baseline");
    measured(single(tracker, 84, 30), 30, 2, "backwards-clock recovery uses a new baseline");
}

void testIdentitiesAndRemovedProcesses() {
    RecentActivityTracker tracker;
    sample(tracker, 1, {row(1, 100), row(2, 200)});
    auto result = sample(tracker, 3, {row(1, 100, 10), row(2, 200, 80)}).value("processes").toArray();
    measured(result.at(0).toObject(), 10, 2, "first PID history");
    measured(result.at(1).toObject(), 80, 2, "independent second PID history");
    result = sample(tracker, 5, {row(1, 101, 50), row(2, 200, 20)}).value("processes").toArray();
    unknown(result.at(0).toObject(), "PID reuse with new start ticks clears the previous process history");
    measured(result.at(1).toObject(), 50, 4, "PID reuse leaves other processes unaffected");
    sample(tracker, 7, {row(2, 200, 40)});
    result = sample(tracker, 9, {row(1, 101, 30), row(2, 200, 60)}).value("processes").toArray();
    unknown(result.at(0).toObject(), "a removed PID is pruned even when it reappears with identical start ticks");
    measured(result.at(1).toObject(), 50, 8, "surviving PID history remains continuous");
    check(sample(tracker, 11, {}).value("processes").toArray().isEmpty(), "empty process list is preserved");
    unknown(single(tracker, 13, 100, 101), "empty snapshots prune all removed PID state");
}

void testFreshScanContract() {
    RecentActivityTracker tracker;
    single(tracker, 1, QJsonValue(QJsonValue::Null));
    auto fresh = sample(tracker, 3, {row(1, 100, 50)});
    measured(fresh.value("processes").toArray().first().toObject(), 50, 2, "fresh process scan establishes activity");
    // The controller keeps cached process rows and their scan timestamp intact, and
    // invokes apply only after a fresh process scan. Presentation refreshes add no time.
    for (double wallTime : {5.0, 7.0, 9.0}) {
        auto cached = fresh;
        cached.insert("timestamp", wallTime);
        near(cached.value("processMonotonicSeconds").toDouble(), 3, "cached scan timestamp stays fixed");
        measured(cached.value("processes").toArray().first().toObject(), 50, 2, "cached rows do not extend the CPU window");
    }
    const auto next = single(tracker, 13, 20);
    measured(next, (50.0 * 2 + 20 * 10) / 12, 12,
        "the next fresh scan uses its actual elapsed interval without cached-row duplicates");
}
QJsonObject appRow(int pid, int parent, int identity, const QString &executable, QJsonValue cpu = QJsonValue(QJsonValue::Null)) {
    auto result = row(pid, identity, cpu);
    result["parentPID"] = parent; result["executablePath"] = executable;
    return result;
}
QJsonObject application(const QJsonObject &snapshot, const QString &executable) {
    for (const auto &value : snapshot.value("activityApplications").toArray()) {
        const auto app = value.toObject();
        if (app.value("executablePath").toString() == executable) return app;
    }
    return {};
}
void fixtureFile(const QString &path, const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "create metadata fixture directory");
    QFile file(path); check(file.open(QIODevice::WriteOnly), "open metadata fixture");
    check(file.write(bytes) == bytes.size(), "write metadata fixture");
}
void testApplicationWindow() {
    RecentActivityTracker tracker({"/nonexistent-desktop-fixture"});
    sample(tracker, 0, {appRow(1, 0, 100, "/opt/editor/editor"), appRow(2, 1, 200, "/opt/editor/editor")});
    auto snapshot = sample(tracker, 2, {appRow(1, 0, 100, "/opt/editor/editor", 10), appRow(2, 1, 200, "/opt/editor/editor", 20)});
    auto app = application(snapshot, "/opt/editor/editor");
    near(app.value("cpuAveragePercent").toDouble(-1), 30, "same-executable processes sum CPU seconds");
    check(app.value("processCount").toInt() == 2 && app.value("processes").toArray().size() == 2, "application contains expandable process details");
    check(app.value("cpuComplete").toBool(), "stable members have complete CPU");
    near(app.value("cpuCoverageFraction").toDouble(-1), 1, "complete CPU interval coverage");
    // A fresh child has no baseline. Its unknown activity must not be a zero.
    auto child = appRow(3, 1, 300, "/opt/editor/editor"); child["ioReadBytesPerSecond"] = QJsonValue(QJsonValue::Null);
    snapshot = sample(tracker, 10, {appRow(1, 0, 100, "/opt/editor/editor", 50), appRow(2, 1, 200, "/opt/editor/editor", 25), child});
    app = application(snapshot, "/opt/editor/editor");
    near(app.value("cpuAveragePercent").toDouble(-1), (30.0 * 2 + 75 * 8) / 10, "app uses common window instead of sum of mismatched process averages");
    check(!app.value("cpuComplete").toBool(), "new member marks average as partial");
    near(app.value("cpuCoverageFraction").toDouble(-1), .2, "new-member unknown interval reduces coverage");
    check(!app.value("ioReadBytesPerSecondComplete").toBool(), "one missing IO member marks summed IO partial");
    near(app.value("ioReadBytesPerSecond").toDouble(-1), 2468, "partial IO retains only measured rates");
    check(app.value("ioWriteBytesPerSecondComplete").toBool(), "read/write completeness is independent");
    snapshot = sample(tracker, 20, {appRow(1, 0, 100, "/opt/editor/editor", 0)});
    app = application(snapshot, "/opt/editor/editor");
    near(app.value("cpuAveragePercent").toDouble(-1), 660.0 / 20, "exited children retain prior CPU contribution in app window");
    check(!app.value("cpuComplete").toBool(), "exit interval is partial");
    snapshot = sample(tracker, 30, {});
    app = application(snapshot, "/opt/editor/editor");
    check(app.value("noReadableProcesses").toBool() && app.value("processCount").toInt(-1) == 0, "departed application remains as recent activity with zero running processes");
    near(app.value("cpuAveragePercent").toDouble(-1), 22, "departed activity decays against same actual window");
    sample(tracker, 50, {}); sample(tracker, 70, {});
    check(application(sample(tracker, 81, {}), "/opt/editor/editor").isEmpty(), "expired departed application is pruned");

    RecentActivityTracker identities({"/nonexistent-desktop-fixture"});
    sample(identities, 1, {appRow(1, 0, 100, "/opt/old/app")});
    sample(identities, 3, {appRow(1, 0, 100, "/opt/old/app", 80)});
    snapshot = sample(identities, 5, {appRow(1, 0, 101, "/opt/new/app", 100)});
    check(!application(snapshot, "/opt/old/app").value("cpuComplete").toBool(), "PID reuse marks former application interval partial");
    check(application(snapshot, "/opt/new/app").value("cpuAveragePercent").isNull(), "reused PID never leaks old application CPU into new identity");
    near(application(snapshot, "/opt/old/app").value("cpuAveragePercent").toDouble(-1), 40, "former identity retains measured work until expiration");
}
void testDesktopIdentity() {
    QTemporaryDir temp; check(temp.isValid(), "desktop fixture temporary directory");
    const QString apps = temp.path() + "/applications";
    const QString editor = temp.path() + "/Editor App/editor";
    const QString helper = temp.path() + "/Editor App/helper";
    const QString other = temp.path() + "/Other App/editor";
    const QString terminal = temp.path() + "/Terminal App/terminal";
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Fixture Editor\nExec=\"" + editor.toUtf8() + "\" %F\n");
    fixtureFile(apps + "/terminal.desktop", "[Desktop Entry]\nType=Application\nName=Fixture Terminal\nTerminal=true\nExec=\"" + terminal.toUtf8() + "\"\n");
    fixtureFile(apps + "/unsafe.desktop", "[Desktop Entry]\nType=Application\nName=Wrong Shell Identity\nExec=/bin/sh -c something\n");
    RecentActivityTracker tracker({apps});
    const auto snapshot = sample(tracker, 1, {appRow(1, 0, 10, editor), appRow(2, 1, 20, editor), appRow(3, 1, 30, helper),
        appRow(4, 1, 40, other), appRow(5, 0, 50, terminal), appRow(6, 5, 60, temp.path() + "/Terminal App/arbitrary"),
        appRow(7, 0, 70, QString()), appRow(8, 0, 80, QString()), appRow(9, 0, 90, "/bin/sh")});
    const auto app = application(snapshot, editor);
    check(app.value("name").toString() == "Fixture Editor", "quoted literal desktop Exec matches exact executable");
    check(app.value("processCount").toInt() == 3, "same binary and contained helper join exact recognized desktop ancestor");
    check(application(snapshot, other).value("name").toString() == "editor", "same basename in different directory never gets another app desktop name");
    check(application(snapshot, other).value("processCount").toInt() == 1, "unrelated launched binary is not absorbed by parentage");
    check(application(snapshot, terminal).value("processCount").toInt() == 1, "terminal never absorbs arbitrary executable descendants");
    check(application(snapshot, "/bin/sh").value("name").toString() != "Wrong Shell Identity", "generic shell desktop launcher is not executable identity");
    check(snapshot.value("activityApplications").toArray().size() == 7, "unreadable executables remain distinct PID identities");
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Changed Editor\nExec=\"" + editor.toUtf8() + "\" %F\n");
    const auto cached = sample(tracker, 3, {appRow(1, 0, 10, editor, 10)});
    check(application(cached, editor).value("name").toString() == "Fixture Editor", "desktop metadata is cached across frequent scans");
    const auto refreshed = sample(tracker, 303, {appRow(1, 0, 10, editor, 10)});
    check(application(refreshed, editor).value("name").toString() == "Changed Editor", "metadata refreshes after bounded cache lifetime");
}

void testScanPathResolution() {
    QTemporaryDir temp; check(temp.isValid(), "symlink fixture temporary directory");
    const QString apps = temp.path() + "/applications";
    const QString first = temp.path() + "/First/editor";
    const QString second = temp.path() + "/Second/editor";
    const QString link = temp.path() + "/current";
    fixtureFile(first, "first"); fixtureFile(second, "second");
    check(QFile::link(first, link), "create executable symlink");
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Linked Editor\nExec=" + link.toUtf8() + "\n");
    RecentActivityTracker tracker({apps});
    auto snapshot = sample(tracker, 1, {appRow(1, 0, 10, link), appRow(2, 0, 20, first)});
    check(application(snapshot, first).value("processCount").toInt() == 2,
        "different input paths resolving to one real executable share identity");
    snapshot = sample(tracker, 3, {appRow(1, 0, 10, link, 40), appRow(2, 0, 20, first, 20)});
    near(application(snapshot, first).value("cpuAveragePercent").toDouble(-1), 60, "canonical aliases retain measured CPU");
    check(QFile::remove(link) && QFile::link(second, link), "redirect executable symlink");
    snapshot = sample(tracker, 5, {appRow(1, 0, 10, link, 90), appRow(2, 0, 20, first, 20)});
    const auto redirected = application(snapshot, second);
    check(redirected.value("processCount").toInt() == 1 && redirected.value("desktopFile").toString().isEmpty(),
        "the next scan observes a redirected symlink without stale desktop ownership");
    check(redirected.value("cpuAveragePercent").isNull(), "changed executable cannot reuse old path CPU history");
    near(application(snapshot, first).value("cpuAveragePercent").toDouble(-1), 40,
        "previous canonical application keeps only its measured CPU contribution");
    tracker.reset();
    snapshot = sample(tracker, 7, {appRow(1, 0, 10, link, 10)});
    check(snapshot.value("activityApplications").toArray().size() == 1 && application(snapshot, second).value("cpuAveragePercent").isNull(),
        "reset removes process/application observations and resolves current executable path");
    check(application(snapshot, second).value("desktopFile").toString().isEmpty(), "reset preserves existing desktop metadata refresh cadence");
    snapshot = sample(tracker, 301, {appRow(1, 0, 10, link, 10)});
    check(application(snapshot, second).value("name").toString() == "Linked Editor",
        "desktop Exec symlink target refreshes at the original three-hundred-second boundary");
}

void testMetadataRefreshBoundary() {
    QTemporaryDir temp; check(temp.isValid(), "metadata boundary temporary directory");
    const QString apps = temp.path() + "/applications";
    const QString editor = temp.path() + "/Editor/editor";
    const QString added = temp.path() + "/Added/added";
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Original Editor\nExec=" + editor.toUtf8() + "\n");
    RecentActivityTracker tracker({apps});
    sample(tracker, 1, {appRow(1, 0, 10, editor), appRow(2, 0, 20, added)});
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Updated Editor\nExec=" + editor.toUtf8() + "\n");
    fixtureFile(apps + "/added.desktop", "[Desktop Entry]\nType=Application\nName=New Application\nExec=" + added.toUtf8() + "\n");
    auto snapshot = sample(tracker, 300, {appRow(1, 0, 10, editor, 10), appRow(2, 0, 20, added, 20)});
    check(application(snapshot, editor).value("name").toString() == "Original Editor", "updated desktop stays cached before boundary");
    check(application(snapshot, added).value("desktopFile").toString().isEmpty(), "new desktop stays undiscovered before boundary");
    snapshot = sample(tracker, 301, {appRow(1, 0, 10, editor, 10), appRow(2, 0, 20, added, 20)});
    check(application(snapshot, editor).value("name").toString() == "Updated Editor", "updated desktop refreshes exactly at boundary");
    check(application(snapshot, added).value("name").toString() == "New Application", "new desktop is discovered exactly at boundary");
}

void testAncestryBoundaries() {
    QTemporaryDir temp; check(temp.isValid(), "ancestry fixture temporary directory");
    const QString apps = temp.path() + "/applications";
    const QString editor = temp.path() + "/Editor/editor";
    const QString helper = temp.path() + "/Editor/helper";
    fixtureFile(apps + "/editor.desktop", "[Desktop Entry]\nType=Application\nName=Editor\nExec=" + editor.toUtf8() + "\n");
    RecentActivityTracker tracker({apps});
    const auto snapshot = sample(tracker, 1, {appRow(1, 0, 10, editor), appRow(2, 1, 20, helper),
        appRow(3, 2, 30, helper), appRow(4, 1, 40, "/fixture/bin/bash"), appRow(5, 4, 50, temp.path() + "/Editor/through-shell"),
        appRow(6, 7, 60, temp.path() + "/Editor/cycle-a"), appRow(7, 6, 70, temp.path() + "/Editor/cycle-b"),
        appRow(8, 999, 80, temp.path() + "/Editor/missing-parent")});
    check(application(snapshot, editor).value("processCount").toInt() == 3, "deep contained helper ancestry joins exact desktop ancestor");
    check(application(snapshot, temp.path() + "/Editor/through-shell").value("processCount").toInt() == 1,
        "shell ancestry blocks app ownership");
    check(application(snapshot, temp.path() + "/Editor/cycle-a").value("processCount").toInt() == 1
        && application(snapshot, temp.path() + "/Editor/cycle-b").value("processCount").toInt() == 1,
        "cyclic parent indexes terminate without fabricated ownership");
    check(application(snapshot, temp.path() + "/Editor/missing-parent").value("processCount").toInt() == 1,
        "missing ancestors leave executable identity independent");
}

}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    testWeightedIntervals();
    testSixtySecondBoundary();
    testInactiveIntervals();
    testDiscontinuities();
    testIdentitiesAndRemovedProcesses();
    testFreshScanContract();
    testApplicationWindow();
    testDesktopIdentity();
    testScanPathResolution();
    testMetadataRefreshBoundary();
    testAncestryBoundaries();
    std::puts("PASS activity: duration-weighted CPU, sixty-second clipping, measured zero, gaps/resets, PID identities, pruning, fresh-scan contract, app common-window sums/partial IO/new+exit/PID reuse, exact desktop/helper identity/cache, symlink redirection, metadata refresh boundary, ancestry boundaries");
    return 0;
}
