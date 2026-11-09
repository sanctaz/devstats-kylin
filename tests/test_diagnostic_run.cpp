#include "../src/diagnostic_run.h"
#include "../src/diagnostics.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
void check(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL diagnostic run: %s\n", what); std::exit(1); } }
constexpr double GiB = 1024. * 1024 * 1024;
QJsonObject snapshot(int milliseconds, bool busy = false, double cpu = 20, double latency = 2) {
    QJsonObject s{{"cpu", QJsonObject{{"usage", busy ? .95 : .1}, {"iowait", 0}, {"coreCount", 1}, {"runningProcesses", busy ? 2 : 0}, {"blockedProcesses", 0}, {"load", QJsonArray{busy ? 3 : 0}}}},
        {"memory", QJsonObject{{"totalBytes", 8 * GiB}, {"availableBytes", 6 * GiB}}},
        {"vm", QJsonObject{{"swapInBytesPerSecond", 0}, {"swapOutBytesPerSecond", 0}, {"oomKillDelta", 0}}},
        {"diskIO", QJsonObject{{"busiestUtilization", .01}, {"busiestAwaitMilliseconds", 1}, {"busiestDevice", "sda"}}},
        {"disks", QJsonArray{QJsonObject{{"mountPoint", "/private/project-secret"}, {"usageFraction", .2}, {"availableBytes", 80 * GiB}}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", latency}, {"displayAvailable", false}}},
        {"monotonicSeconds", milliseconds / 1000.}, {"processMonotonicSeconds", milliseconds / 1000.}, {"processesFresh", true},
        {"processes", QJsonArray{QJsonObject{{"pid", 42}, {"startTicks", 100}, {"executablePath", "/private/secretapp"}, {"name", "Fixture app"},
            {"applicationIdentity", "exe"}, {"cpuPercent", cpu}, {"memoryBytes", 1234}, {"ioReadBytesPerSecond", 100}, {"ioWriteBytesPerSecond", 50}}}},
        {"systemProcesses", QJsonArray{QJsonObject{{"pid", 7}, {"startTicks", 1}, {"name", "kswapd0"}, {"owner", "root"}, {"kernelThread", true}, {"cpuPercent", 3}, {"memoryBytes", 0}}}},
        {"activityApplications", QJsonArray{QJsonObject{{"identity", "exe"}, {"name", "Fixture app"}}}}};
    s["diagnostics"] = diagnoseSystem(s);
    return s;
}
QJsonObject find(const QJsonObject &r, const char *key, const char *field, const QString &value) {
    for (const auto &v : r.value(key).toArray()) if (v.toObject().value(field).toString() == value) return v.toObject();
    return {};
}
void run(DiagnosticRun &r, const std::function<QJsonObject(int)> &sample) {
    for (int i = 0; i < 6; ++i) r.observe(sample(i * 2000), i * 2000, 100 + i * 2);
}

void lifecycle() {
    DiagnosticRun r;
    check(r.result().value("state") == "idle" && !r.running(), "idle first");
    check(r.start(0, 100) && r.running() && !r.start(10, 100), "starts once");
    check(!r.observe(snapshot(0), 0, 100), "first sample does not finish");
    check(r.result().value("sampleCount") == 1 && r.result().value("progress") == 0, "first sample counted");
    r.observe(snapshot(500), 500, 100.5);
    check(r.result().value("sampleCount") == 1, "a republish inside the interval is not another sample");
    for (int t = 2000; t < 10000; t += 2000) check(!r.observe(snapshot(t), t, 100 + t / 1000.), "still running");
    check(r.running() && r.result().value("progress") == 80, "progress follows elapsed time");
    check(r.observe(snapshot(10000), 10000, 110), "reaches its ten seconds");
    const auto result = r.result();
    check(result.value("state") == "completed" && result.value("progress") == 100 && result.value("sampleCount") == 6, "completes with six samples");
    check(result.value("finishedAt").toDouble() == 110 && result.value("startedAt").toDouble() == 100, "wall times kept");
    check(!r.observe(snapshot(12000), 12000, 112), "a finished run ignores samples");
    check(r.result() == result, "result is stable");
    check(r.start(20000, 200) && r.result().value("sampleCount") == 0, "a rerun starts clean");

    DiagnosticRun cancelled;
    cancelled.start(0, 0); cancelled.observe(snapshot(0), 0, 0);
    check(!cancelled.finish("nonsense", 1000, 1) && cancelled.finish("cancelled", 1000, 1), "only known end states");
    check(cancelled.result().value("state") == "cancelled" && cancelled.result().value("headline") == QStringLiteral("已取消诊断"), "cancel is explicit");
    DiagnosticRun interrupted;
    interrupted.start(0, 0); interrupted.finish("interrupted", 1000, 1);
    check(interrupted.result().value("headline") == QStringLiteral("诊断被中断"), "interruption is explicit");
}

void findings() {
    DiagnosticRun quiet; quiet.start(0, 0);
    run(quiet, [](int t) { return snapshot(t); });
    quiet.finish("completed", 10000, 10);
    auto result = quiet.result();
    check(result.value("headline") == QStringLiteral("本轮未发现明显压力") && result.value("findings").toArray().isEmpty(), "quiet run says so");
    check(find(result, "checks", "kind", "cpu").value("status") == "ok", "measured quiet resource is ok");
    check(find(result, "checks", "kind", "memory").value("status") == "ok", "works with no PSI at all");
    check(find(result, "checks", "kind", "display").value("status") == "unknown", "unmeasurable resource is unknown, not ok");
    check(find(result, "checks", "kind", "thermal").value("status") == "unknown", "no thermal data is unknown");

    DiagnosticRun sustained; sustained.start(0, 0);
    run(sustained, [](int t) { return snapshot(t, true); });
    sustained.finish("completed", 10000, 10);
    result = sustained.result();
    auto finding = find(result, "findings", "kind", "cpu");
    check(finding.value("persistence") == "sustained" && finding.value("observedSamples") == 6 && finding.value("totalSamples") == 6, "six of six is sustained");
    check(result.value("headline") == finding.value("title"), "headline comes from the top finding");
    check(find(result, "checks", "kind", "cpu").value("status") == "pressure", "check reflects pressure");

    DiagnosticRun blip; blip.start(0, 0);
    run(blip, [](int t) { return snapshot(t, t == 4000); });
    blip.finish("completed", 10000, 10);
    result = blip.result();
    check(find(result, "findings", "kind", "cpu").value("persistence") == "transient", "one sample is transient");
    check(result.value("headline").toString().startsWith(QStringLiteral("只有短暂压力")), "transient pressure is labelled as such");

    DiagnosticRun gap; gap.start(0, 0);
    for (int t : {0, 2000, 8000, 10000}) gap.observe(snapshot(t, true), t, t / 1000.);
    result = gap.result();
    check(find(result, "findings", "kind", "cpu").value("persistence") == "transient", "a sampling gap breaks persistence");
    bool mentioned = false;
    for (const auto &value : result.value("coverage").toArray()) mentioned |= value.toString().contains(QStringLiteral("3 秒"));
    check(mentioned, "a gap in sampling is itself reported as a likely stall");

    DiagnosticRun latency; latency.start(0, 0);
    latency.observe(snapshot(0, false, 20, 5), 0, 0); latency.observe(snapshot(2000, false, 20, 1500), 2000, 2); latency.observe(snapshot(4000, false, 20, 8), 4000, 4);
    result = latency.result();
    check(result.value("schedulerMaxMs").toDouble() == 1500, "the worst latency of the run is kept");
    check(find(result, "findings", "kind", "responsiveness").value("severity") == "critical", "the freeze remains in the result");
}

void suspects() {
    DiagnosticRun r; r.start(0, 0);
    const QList<double> cpu{10, 20, 30, 40, 50, 60};
    for (int i = 0; i < 6; ++i) r.observe(snapshot(i * 2000, false, cpu[i]), i * 2000, i * 2);
    r.finish("completed", 10000, 10);
    auto result = r.result();
    auto rows = result.value("suspects").toArray();
    auto top = rows[0].toObject();
    check(top.value("name") == "Fixture app" && top.value("own").toBool(), "the busiest application leads");
    check(top.value("averageCpuPercent").toDouble() == 35 && top.value("peakCpuPercent").toDouble() == 60, "average and peak are both real");
    check(top.value("peakMemoryBytes").toDouble() == 1234 && top.value("peakIOBytesPerSecond").toDouble() == 150, "memory and I/O peaks");
    check(top.value("members").toArray().first().toObject().value("pid").toInt() == 42, "members keep an identity to act on");
    check(rows[1].toObject().value("name") == "kswapd0" && !rows[1].toObject().value("own").toBool(), "system processes are included, marked");
    check(result.value("rankingBasis") == "cpu", "basis recorded");

    // Cached process lists repeat old rates and must not be counted again.
    DiagnosticRun cached; cached.start(0, 0);
    auto first = snapshot(0, false, 100); cached.observe(first, 0, 0);
    auto second = snapshot(2000, false, 100); second["processesFresh"] = false; cached.observe(second, 2000, 2);
    auto third = snapshot(4000, false, 100); third["processMonotonicSeconds"] = 0.; cached.observe(third, 4000, 4);
    check(cached.result().value("suspects").toArray()[0].toObject().value("averageCpuPercent").toDouble() == 100, "stale scans excluded");
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    lifecycle(); findings(); suspects();
    std::puts("PASS diagnostic run: lifecycle, sample dedupe, sustained/transient/gap, unknown vs ok, worst latency, average and peak suspects including system processes");
    return 0;
}
