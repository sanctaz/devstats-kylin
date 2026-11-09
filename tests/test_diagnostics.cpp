#include "../src/diagnostics.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr double MiB = 1024.0 * 1024.0, GiB = 1024.0 * MiB;
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
QJsonObject averages(double amount) { return {{"avg10", amount}, {"avg60", amount / 2}, {"avg300", amount / 4}}; }
QJsonObject pressure(double some = 0, double full = 0) { return {{"some", averages(some)}, {"full", averages(full)}}; }
QJsonObject healthy() {
    return {{"cpu", QJsonObject{{"usage", .2}, {"iowait", .01}, {"irq", .01}, {"coreCount", 8}, {"runningProcesses", 2},
        {"blockedProcesses", 0}, {"load", QJsonArray{1, 1, 1}}, {"runQueueWait", .01}, {"forksPerSecond", 2}}},
        {"memory", QJsonObject{{"totalBytes", 16 * GiB}, {"availableBytes", 10 * GiB}, {"fraction", .375}}},
        {"pressure", QJsonObject{{"cpu", QJsonObject{{"some", averages(0)}, {"full", QJsonValue::Null}}}, {"memory", pressure()}, {"io", pressure()}}},
        {"vm", QJsonObject{{"swapInBytesPerSecond", 0}, {"swapOutBytesPerSecond", 0}, {"majorFaultsPerSecond", 0}, {"oomKillDelta", 0},
            {"allocStallsPerSecond", 0}, {"directReclaimPagesPerSecond", 0}}},
        {"diskIO", QJsonObject{{"busiestDevice", "nvme0n1"}, {"busiestUtilization", .02}, {"busiestAwaitMilliseconds", 1}}},
        {"disks", QJsonArray{QJsonObject{{"mountPoint", "/"}, {"usageFraction", .3}, {"availableBytes", 100 * GiB}}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", 2}, {"schedulerP95Ms", 2}, {"schedulerSamples", 10}, {"displayAvailable", true}, {"displayMaxMs", 1}}},
        {"thermal", QJsonObject{{"hottestCelsius", 50}, {"hottestZone", "cpu-thermal"}, {"frequencyFraction", .4}, {"limited", false}}},
        {"processStates", QJsonObject{{"running", 2}, {"blocked", 0}, {"zombie", 0}}},
        {"processes", QJsonArray{}}, {"systemProcesses", QJsonArray{}},
        {"processesFresh", true}};
}
void set(QJsonObject &snapshot, const char *section, const char *key, QJsonValue value) {
    auto object = snapshot[section].toObject(); object[key] = value; snapshot[section] = object;
}
QJsonObject finding(const QJsonObject &diagnosis, const char *kind) {
    for (const auto &value : diagnosis["findings"].toArray()) if (value.toObject()["kind"].toString() == kind) return value.toObject();
    return {};
}
QJsonObject process(const QString &name, double cpu, double memory = 0, double read = 0, const QString &state = "S") {
    return {{"pid", 100}, {"startTicks", 5}, {"name", name}, {"executablePath", "/usr/bin/" + name}, {"cpuPercent", cpu}, {"memoryBytes", memory},
            {"ioReadBytesPerSecond", read}, {"ioWriteBytesPerSecond", 0}, {"state", state}, {"applicationIdentity", name}};
}
QJsonObject system(const QString &name, double cpu, const QString &owner = "root", bool kernel = false, const QString &state = "S") {
    return {{"pid", 1}, {"startTicks", 1}, {"name", name}, {"owner", owner}, {"kernelThread", kernel}, {"cpuPercent", cpu}, {"memoryBytes", 1024}, {"state", state}};
}

void states() {
    auto d = diagnoseSystem(healthy());
    check(d["state"] == "normal" && d["findings"].toArray().isEmpty() && d["headline"] == QStringLiteral("系统运行流畅"), "healthy system is normal");
    check(d["summary"].toString().contains(QStringLiteral("CPU 20%")), "normal summary carries real figures");
    check(d["meters"].toArray().size() == 6, "six meters");

    // A kernel without PSI must still give an answer: this was "unknown" forever.
    auto noPsi = healthy();
    noPsi["pressure"] = QJsonObject{{"cpu", QJsonValue::Null}, {"memory", QJsonValue::Null}, {"io", QJsonValue::Null}};
    d = diagnoseSystem(noPsi);
    check(d["state"] == "normal", "missing PSI does not make the system state unknown");
    bool mentioned = false;
    for (const auto &value : d["coverage"].toArray()) mentioned |= value.toString().contains("PSI");
    check(mentioned, "missing PSI is disclosed");

    auto missing = healthy(); missing.remove("cpu");
    check(diagnoseSystem(missing)["state"] == "unknown", "no CPU figure means unknown");
    missing = healthy(); missing.remove("memory");
    check(diagnoseSystem(missing)["state"] == "unknown", "no memory figure means unknown");
    check(diagnoseSystem(QJsonObject{})["headline"] == QStringLiteral("正在收集数据"), "empty snapshot is still collecting");
}

void cpu() {
    auto s = healthy(); set(s, "cpu", "usage", .9); set(s, "cpu", "load", QJsonArray{8});
    auto d = diagnoseSystem(s);
    check(!finding(d, "cpu").isEmpty() && d["state"] == "attention", "busy CPU with full load");
    check(finding(d, "cpu")["action"] == "suspect", "CPU finding points at processes");
    s["processes"] = QJsonArray{process("firefox", 320)};
    d = diagnoseSystem(s);
    check(finding(d, "cpu")["title"].toString().contains("firefox") && finding(d, "cpu")["title"].toString().contains(QStringLiteral("3.2 核")), "busy CPU names the top process");
    set(s, "cpu", "usage", .97); set(s, "cpu", "load", QJsonArray{13});
    check(finding(diagnoseSystem(s), "cpu")["severity"] == "critical", "saturated CPU with a long queue is serious");

    // Run-queue wait replaces CPU PSI where the kernel has none.
    auto noPsi = healthy(); noPsi["pressure"] = QJsonObject{};
    set(noPsi, "cpu", "usage", .7); set(noPsi, "cpu", "load", QJsonArray{2}); set(noPsi, "cpu", "runQueueWait", .6);
    check(!finding(diagnoseSystem(noPsi), "cpu").isEmpty(), "scheduler wait shows contention without PSI");
    set(noPsi, "cpu", "runQueueWait", .1);
    check(finding(diagnoseSystem(noPsi), "cpu").isEmpty(), "low wait is not contention");
    auto lagging = healthy(); set(lagging, "pressure", "cpu", QJsonObject{{"some", averages(40)}});
    check(finding(diagnoseSystem(lagging), "cpu").isEmpty(), "the PSI average of a load that already ended is not a current problem");
    auto half = healthy(); set(half, "cpu", "usage", .55); set(half, "thermal", "frequencyFraction", .5);
    check(finding(diagnoseSystem(half), "thermal").isEmpty(), "a governor may keep frequency low at moderate load");

    // One pegged thread is a hint, not a system problem.
    auto single = healthy(); single["processes"] = QJsonArray{process("worker", 99)};
    d = diagnoseSystem(single);
    check(finding(d, "singleCore")["severity"] == "info" && d["state"] == "normal" && d["headline"] == QStringLiteral("系统运行流畅"), "single busy thread stays a hint");
}

void memory() {
    auto s = healthy(); set(s, "memory", "availableBytes", 1 * GiB); set(s, "vm", "swapOutBytesPerSecond", 5 * MiB);
    auto d = diagnoseSystem(s);
    check(!finding(d, "memory").isEmpty() && finding(d, "memory")["action"] == "relief", "low memory with swapping");
    s["processes"] = QJsonArray{process("chrome", 1, 6 * GiB)};
    check(finding(diagnoseSystem(s), "memory")["suggestion"].toString().contains("chrome"), "memory finding names the largest process");
    set(s, "memory", "availableBytes", .2 * GiB);
    check(finding(diagnoseSystem(s), "memory")["severity"] == "critical", "nearly no memory with swapping is serious");

    // Direct reclaim is the stall itself, even without any swap configured or PSI.
    auto reclaim = healthy(); reclaim["pressure"] = QJsonObject{};
    set(reclaim, "memory", "availableBytes", 1 * GiB); set(reclaim, "vm", "allocStallsPerSecond", 5);
    check(!finding(diagnoseSystem(reclaim), "memory").isEmpty(), "direct reclaim counts as pressure");

    auto plenty = healthy(); set(plenty, "vm", "swapOutBytesPerSecond", 5 * MiB);
    check(finding(diagnoseSystem(plenty), "memory").isEmpty(), "moderate swapping with plenty of memory is fine");
    auto storm = healthy(); set(storm, "vm", "swapInBytesPerSecond", 40 * MiB);
    check(!finding(diagnoseSystem(storm), "memory").isEmpty(), "a swap-in storm stalls whatever memory is free");
    auto psiOnly = healthy(); set(psiOnly, "pressure", "memory", pressure(8));
    check(!finding(diagnoseSystem(psiOnly), "memory").isEmpty(), "memory stall time alone is reported, not hidden as unknown");

    auto oom = healthy(); set(oom, "vm", "oomKillDelta", 1);
    d = diagnoseSystem(oom);
    check(finding(d, "oom")["severity"] == "critical" && finding(d, "oom")["immediate"].toBool(), "OOM is serious and immediate");
}

void disk() {
    auto s = healthy(); set(s, "diskIO", "busiestUtilization", .95); set(s, "diskIO", "busiestAwaitMilliseconds", 80);
    auto d = diagnoseSystem(s);
    check(!finding(d, "io").isEmpty() && finding(d, "io")["evidence"].toString().contains("nvme0n1"), "slow device names it");
    set(s, "diskIO", "busiestAwaitMilliseconds", 400);
    check(finding(diagnoseSystem(s), "io")["severity"] == "critical", "very slow device is serious");
    auto fast = healthy(); set(fast, "diskIO", "busiestUtilization", .95); set(fast, "diskIO", "busiestAwaitMilliseconds", 2);
    check(finding(diagnoseSystem(fast), "io").isEmpty(), "a busy but fast SSD is not a stall");
    auto waiting = healthy(); set(waiting, "cpu", "iowait", .3); set(waiting, "cpu", "blockedProcesses", 2);
    waiting["systemProcesses"] = QJsonArray{system("jbd2/sda2-8", 0, "root", true, "D")};
    d = diagnoseSystem(waiting);
    check(finding(d, "io")["evidence"].toString().contains("jbd2/sda2-8"), "blocked processes are named");
    auto iowaitOnly = healthy(); set(iowaitOnly, "cpu", "iowait", .5);
    check(finding(diagnoseSystem(iowaitOnly), "io").isEmpty(), "iowait with nobody blocked proves nothing");
}

void direct() {
    auto s = healthy(); set(s, "responsiveness", "schedulerMaxMs", 400);
    auto d = diagnoseSystem(s);
    check(finding(d, "responsiveness")["severity"] == "attention" && !finding(d, "responsiveness")["immediate"].toBool(), "delayed wake-up is reported");
    set(s, "responsiveness", "schedulerMaxMs", 1500);
    d = diagnoseSystem(s);
    check(finding(d, "responsiveness")["severity"] == "critical" && finding(d, "responsiveness")["immediate"].toBool(), "a second of delay is a freeze");
    check(finding(d, "responsiveness")["evidence"].toString().contains(QStringLiteral("1.5 秒")), "freeze evidence has the duration");
    set(s, "responsiveness", "schedulerMaxMs", 50);
    check(finding(diagnoseSystem(s), "responsiveness").isEmpty(), "ordinary jitter is ignored");

    auto display = healthy(); set(display, "responsiveness", "displayMaxMs", 700);
    check(finding(diagnoseSystem(display), "display")["severity"] == "attention", "slow display server is reported while CPU is idle");
    set(display, "responsiveness", "displayMaxMs", 3500);
    d = diagnoseSystem(display);
    check(finding(d, "display")["severity"] == "critical" && finding(d, "display")["immediate"].toBool(), "hung display server records at once");
    auto noDisplay = healthy(); set(noDisplay, "responsiveness", "displayAvailable", false); set(noDisplay, "responsiveness", "displayMaxMs", QJsonValue());
    bool mentioned = false;
    for (const auto &value : diagnoseSystem(noDisplay)["coverage"].toArray()) mentioned |= value.toString().contains(QStringLiteral("显示"));
    check(mentioned && diagnoseSystem(noDisplay)["state"] == "normal", "no display connection is disclosed, not a problem");
}

void others() {
    auto hot = healthy(); set(hot, "thermal", "hottestCelsius", 92);
    check(finding(diagnoseSystem(hot), "thermal")["severity"] == "attention", "hot CPU");
    set(hot, "thermal", "hottestCelsius", 101);
    check(finding(diagnoseSystem(hot), "thermal")["severity"] == "critical", "very hot CPU");
    auto slow = healthy(); set(slow, "cpu", "usage", .8); set(slow, "thermal", "frequencyFraction", .5);
    check(!finding(diagnoseSystem(slow), "thermal").isEmpty(), "low frequency under load");
    auto idle = healthy(); set(idle, "cpu", "usage", .05); set(idle, "thermal", "frequencyFraction", .2);
    check(finding(diagnoseSystem(idle), "thermal").isEmpty(), "low frequency while idle is normal power saving");

    auto irq = healthy(); set(irq, "cpu", "irq", .3);
    check(!finding(diagnoseSystem(irq), "interrupt").isEmpty(), "interrupt storm");

    auto forks = healthy(); set(forks, "cpu", "usage", .6); set(forks, "cpu", "forksPerSecond", 300); forks["attributedCpuPercent"] = 80;
    check(!finding(diagnoseSystem(forks), "forks").isEmpty(), "CPU no live process explains points at short-lived processes");
    forks["attributedCpuPercent"] = 470;
    check(finding(diagnoseSystem(forks), "forks").isEmpty(), "forks with fully attributed CPU are fine");

    auto compositor = healthy(); compositor["systemProcesses"] = QJsonArray{system("Xorg", 90)};
    check(finding(diagnoseSystem(compositor), "compositor")["title"].toString().contains("Xorg"), "busy display server is named");
    compositor["processes"] = QJsonArray{process("ukui-kwin_x11", 180)};
    check(finding(diagnoseSystem(compositor), "compositor")["severity"] == "critical", "very busy compositor is serious");

    auto full = healthy(); full["disks"] = QJsonArray{QJsonObject{{"mountPoint", "/home"}, {"usageFraction", .96}, {"availableBytes", 1 * GiB}}};
    check(finding(diagnoseSystem(full), "disk")["title"].toString().contains("/home"), "full disk is named");
}

void ordering() {
    auto s = healthy();
    set(s, "cpu", "usage", .9); set(s, "cpu", "load", QJsonArray{8});
    set(s, "memory", "availableBytes", 1 * GiB); set(s, "vm", "swapOutBytesPerSecond", 5 * MiB);
    set(s, "responsiveness", "schedulerMaxMs", 400);
    set(s, "thermal", "frequencyFraction", .9);
    auto d = diagnoseSystem(s);
    const auto list = d["findings"].toArray();
    check(list[0].toObject()["kind"] == "memory" && list[1].toObject()["kind"] == "cpu" && list.last().toObject()["kind"] == "responsiveness", "causes come before symptoms");
    check(d["headline"] == list[0].toObject()["title"], "headline is the first finding");
    check(d["summary"].toString().contains(QStringLiteral("另有 2 项")), "summary counts the rest");
    set(s, "vm", "swapOutBytesPerSecond", 0); set(s, "memory", "availableBytes", 10 * GiB); set(s, "responsiveness", "schedulerMaxMs", 1500);
    d = diagnoseSystem(s);
    check(d["findings"].toArray()[0].toObject()["kind"] == "responsiveness", "a critical symptom outranks an attention cause");
    check(d["state"] == "critical", "state follows the worst finding");
    const auto meters = d["meters"].toArray();
    check(meters[0].toObject()["tone"] == "warn" && meters[3].toObject()["tone"] == "critical" && meters[1].toObject()["tone"] == "ok", "meters carry their own severity");
}

void suspects() {
    auto s = healthy();
    s["activityApplications"] = QJsonArray{QJsonObject{{"identity", "firefox"}, {"name", "Firefox"}}};
    auto helper = process("firefox", 60, 1 * GiB); helper["pid"] = 101;
    s["processes"] = QJsonArray{process("firefox", 200, 4 * GiB), helper, process("tar", 5, 100 * MiB, 80 * MiB, "D")};
    s["systemProcesses"] = QJsonArray{system("kworker/u8:2", 30, "root", true), system("kworker/3:1", 40, "root", true), system("Xorg", 120)};
    auto rows = rankSuspects(s, "cpu");
    auto first = rows[0].toObject();
    check(first["name"] == "Firefox" && first["processCount"] == 2 && first["cpuPercent"].toDouble() == 260 && first["own"].toBool(), "application processes merge under their app name");
    check(first["members"].toArray().size() == 2 && first["members"].toArray()[0].toObject()["cpuPercent"].toDouble() == 200, "members are ranked and carry identity");
    check(first["reason"].toString().contains(QStringLiteral("全机")), "reason explains the rank");
    check(rows[1].toObject()["name"] == "Xorg" && !rows[1].toObject()["own"].toBool() && rows[1].toObject()["owner"] == "root", "other users rank in the same list, marked");
    check(rows[2].toObject()["name"] == "kworker" && rows[2].toObject()["processCount"] == 2 && rows[2].toObject()["kernelThread"].toBool(), "kernel workers merge by family");
    check(rankSuspects(s, "memory")[0].toObject()["memoryBytes"].toDouble() == 5 * GiB, "memory basis ranks by RSS");
    auto io = rankSuspects(s, "io");
    check(io[0].toObject()["name"] == "tar" && io[0].toObject()["blockedCount"] == 1 && io[0].toObject()["reason"].toString().contains(QStringLiteral("等待磁盘")), "whoever waits on the disk leads an I/O ranking");
    check(rankSuspects(s, "cpu", 2).size() == 2, "limit is respected");
    QJsonArray many;
    for (int i = 0; i < 12; ++i) { auto p = process("chromium", i); p["pid"] = 200 + i; many.append(p); }
    s["processes"] = many; s["systemProcesses"] = QJsonArray{};
    check(rankSuspects(s, "cpu")[0].toObject()["members"].toArray().size() == 8, "members are bounded");

    auto d = healthy(); set(d, "memory", "availableBytes", 1 * GiB); set(d, "vm", "swapOutBytesPerSecond", 5 * MiB);
    check(suspectBasis(diagnoseSystem(d)) == "memory", "memory problem ranks by memory");
    auto b = healthy(); set(b, "diskIO", "busiestUtilization", .95); set(b, "diskIO", "busiestAwaitMilliseconds", 80);
    check(suspectBasis(diagnoseSystem(b)) == "io", "disk problem ranks by I/O");
    check(suspectBasis(diagnoseSystem(healthy())) == "cpu", "default ranks by CPU");
    check(findingPriority("memory") < findingPriority("cpu") && findingPriority("cpu") < findingPriority("responsiveness") && findingPriority("unknown-kind") > findingPriority("singleCore"), "priority is ordered and total");
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    states(); cpu(); memory(); disk(); direct(); others(); ordering(); suspects();
    std::printf("PASS diagnostics: %d checks; works without PSI, device latency, direct reclaim, scheduler and display latency, thermal, forks, ordering, all-user suspects\n", checks);
    return 0;
}
