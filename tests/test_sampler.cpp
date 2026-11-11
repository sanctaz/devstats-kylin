#include "../src/linux_sampler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void near(double actual, double expected, const char *message) {
    check(std::abs(actual - expected) < 0.000001, message);
}
void write(const QString &path, const QByteArray &value) {
    check(QDir().mkpath(QFileInfo(path).dir().absolutePath()), "create fixture directory");
    QFile file(path);
    check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open fixture file");
    check(file.write(value) == value.size(), "write fixture file");
}
QByteArray stat(qint64 pid, qint64 parent, quint64 ticks, quint64 start, quint64 rss,
                const QByteArray &name = "node (worker)", quint64 majorFaults = 0, const QByteArray &state = "S", qint64 blkio = -1,
                quint64 childTicks = 0) {
    // Fields 3 through 24 of proc_pid_stat, or through 42 (delayacct_blkio_ticks) when
    // blkio is given; comm deliberately contains parentheses.
    QList<QByteArray> values;
    for (int field = 3; field <= (blkio >= 0 ? 42 : 24); ++field) values.append("0");
    if (blkio >= 0) values[39] = QByteArray::number(blkio);
    values[0] = state; values[1] = QByteArray::number(parent);
    values[9] = QByteArray::number(majorFaults);
    values[11] = QByteArray::number(ticks); values[19] = QByteArray::number(start);
    values[13] = QByteArray::number(childTicks);
    values[21] = QByteArray::number(rss);
    return QByteArray::number(pid) + " (" + name + ") " + values.join(' ') + '\n';
}
void process(const QString &proc, qint64 pid, qint64 parent, quint64 ticks, quint64 start,
             quint64 rss, const QString &cwd = {}, const QByteArray &command = {}) {
    const QString base = proc + '/' + QString::number(pid);
    write(base + "/stat", stat(pid, parent, ticks, start, rss));
    write(base + "/cmdline", command);
    if (!cwd.isEmpty()) {
        QFile::remove(base + "/cwd");
        check(::symlink(QFile::encodeName(cwd).constData(), QFile::encodeName(base + "/cwd").constData()) == 0, "create cwd symlink");
    }
}
QJsonObject byPid(const QJsonObject &sample, int pid) {
    for (const auto &item : sample["processes"].toArray())
        if (item.toObject()["pid"].toInt() == pid) return item.toObject();
    return {};
}
void testFreshProcessPaths() {
    QTemporaryDir temp;
    check(temp.isValid(), "fresh-path fixture");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    const QString first = temp.path() + "/first", second = temp.path() + "/second";
    write(first + "/package.json", "{}"); write(second + "/package.json", "{}");
    write(first + "/app", "first"); write(second + "/app", "second");
    // Like the kernel's proc links, each link names the real path directly.
    auto point = [&](const QString &target) {
        for (int pid : {401, 402}) {
            const QString base = proc + '/' + QString::number(pid);
            QFile::remove(base + "/cwd"); QFile::remove(base + "/exe");
            check(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(base + "/cwd").constData()) == 0 &&
                  ::symlink(QFile::encodeName(target + "/app").constData(), QFile::encodeName(base + "/exe").constData()) == 0,
                  "point proc links");
        }
    };
    process(proc, 401, 1, 1, 4000, 1);
    process(proc, 402, 401, 1, 4001, 1);
    point(first);
    LinuxSampler sampler(proc, sys);
    const auto before = sampler.sample();
    check(byPid(before, 401)["projectPath"].toString() == first &&
          byPid(before, 402)["executablePath"].toString() == first + "/app", "shared paths resolve initially");
    point(second);
    const auto after = sampler.sample();
    check(byPid(after, 401)["projectPath"].toString() == second &&
          byPid(after, 402)["executablePath"].toString() == second + "/app",
          "proc links are read again on every process scan");
    sampler.setProjectRoots({first, second});
    process(proc, 403, 1, 1, 4002, 1, {}, QByteArray("compiler\0", 9) + QByteArray(8192, 'x') +
            '\0' + QFile::encodeName(first) + '\0');
    check(byPid(sampler.sample(), 403)["projectPath"].toString() == first,
          "long command line still resolves configured projects");
    process(proc, 403, 1, 1, 4002, 1, {}, QByteArray("compiler\0--prefix=", 18) +
            QFile::encodeName(second) + '\0');
    check(byPid(sampler.sample(), 403)["projectPath"].toString() == second,
          "same process command changes are visible on the next scan");
}
// Short-lived children show up only in the CPU their parent collects when it waits
// for them. A child seen alive already showed its own CPU and is not counted twice.
void testChildAttribution() {
    QTemporaryDir temp;
    check(temp.isValid(), "child fixture");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    auto put = [&](qint64 pid, qint64 parent, quint64 ticks, quint64 start, quint64 childTicks) {
        write(proc + '/' + QString::number(pid) + "/stat", stat(pid, parent, ticks, start, 1, "worker", 0, "S", -1, childTicks));
    };
    put(600, 1, 0, 6000, 0);          // reference: its own CPU only
    put(501, 1, 10, 5010, 0);         // a script loop starting commands
    put(502, 501, 50, 5020, 0);       // one command seen alive at 50 ticks
    put(701, 1, 10, 7010, 0);         // a parent of one long job
    put(702, 701, 400, 7020, 0);      // the long job, seen alive at 400 ticks
    put(::getpid(), 1, 0, 9000, 0);   // DevStats itself
    LinuxSampler sampler(proc, sys);
    const auto first = sampler.sample();
    check(first["processCreation"].toObject().isEmpty(), "first scan has no interval to attribute");
    check(byPid(first, ::getpid())["self"].toBool() && first["selfPid"].toDouble() == ::getpid(), "DevStats marks its own process");
    QThread::msleep(30);
    put(600, 1, 100, 6000, 0);
    QDir(proc + "/502").removeRecursively(); QDir(proc + "/702").removeRecursively();
    put(501, 1, 10, 5010, 300);       // waited for 502 (50) plus never-seen children (250)
    put(701, 1, 10, 7010, 400);       // waited for the long job only
    put(503, 501, 0, 5030, 0); put(504, 501, 0, 5040, 0);
    const auto second = sampler.sample();
    const auto reference = byPid(second, 600)["cpuPercent"].toDouble();
    check(reference > 0, "reference CPU measured");
    near(byPid(second, 501)["childCpuPercent"].toDouble() / reference, 2.5,
         "parent is credited only for children that were never seen alive");
    check(!byPid(second, 701).contains("childCpuPercent"), "a long job that was seen alive does not spike its parent");
    check(byPid(second, 501)["newChildren"].toInt() == 2 && !byPid(second, 600).contains("newChildren"), "new children are counted per parent");
    const auto parents = second["processCreation"].toObject()["parents"].toArray();
    check(!parents.isEmpty() && parents[0].toObject()["pid"].toInt() == 501 && parents[0].toObject()["own"].toBool(),
          "the spawning parent leads the creation summary");
    check(second["processCreation"].toObject()["newSinceLastScan"].toInt() == 2, "new processes since the last scan are counted");
}
void testFixture() {
    QTemporaryDir temp;
    check(temp.isValid(), "temporary fixture directory");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    const QString project = temp.path() + "/project", nested = project + "/nested", sibling = project + "-other";
    QDir().mkpath(nested + "/src"); QDir().mkpath(sibling);
    const QString discovered = temp.path() + "/auto";
    QDir().mkpath(discovered + "/child");
    write(discovered + "/package.json", "{}");
    write(proc + "/stat", "cpu 100 0 50 850 0 0 0 0 40 0\ncpu0 50 0 25 425 0 0 0 0 20 0\ncpu1 50 0 25 425 0 0 0 0 20 0\n");
    write(proc + "/loadavg", "0.10 0.20 0.30 1/2 5\n");
    write(proc + "/uptime", "123.50 200.00\n");
    write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 250 kB\nSwapTotal: 100 kB\nSwapFree: 40 kB\nCached: 80 kB\nBuffers: 10 kB\nActive: 100 kB\nInactive: 50 kB\n");
    const QString mount = temp.path() + "/mount space";
    QDir().mkpath(mount);
    QByteArray escaped = QFile::encodeName(mount); escaped.replace(" ", "\\040");
    write(proc + "/self/mountinfo", "1 0 8:1 / " + escaped + " rw - ext4 /dev/sda1 rw\n2 0 0:2 / /unreachable rw - nfs server:/export rw\n");
    write(proc + "/diskstats", "8 0 sda 1 0 100 0 1 0 200 0 0 0 0\n8 1 sda1 1 0 100 0 1 0 200 0 0 0 0\n7 0 loop0 1 0 100 0 1 0 200 0 0 0 0\n253 0 dm-0 1 0 100 0 1 0 200 0 0 0 0\n");
    write(sys + "/class/block/sda1/partition", "1\n");
    write(proc + "/net/dev", "Inter-| Receive | Transmit\n eth0: 1000 0 0 0 0 0 0 0 2000 0 0 0 0 0 0 0\n eth1: 5000 0 0 0 0 0 0 0 6000 0 0 0 0 0 0 0\n");
    write(proc + "/net/route", "Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT\neth1 00000000 0100000A 0003 0 0 200 00000000 0 0 0\neth0 00000000 0100000A 0003 0 0 100 00000000 0 0 0\n");
    write(proc + "/net/if_inet6", "20010db8000000000000000000000001 02 40 00 80 eth0\n");
    write(sys + "/class/net/eth0/operstate", "up\n");
    QDir().mkpath(sys + "/class/power_supply");
    process(proc, 101, 1, 10, 100, 20, nested + "/src");
    process(proc, 102, 101, 5, 200, 10);
    process(proc, 103, 1, 1, 300, 1, sibling);
    process(proc, 104, 1, 1, 400, 3, discovered + "/child");
    process(proc, 105, 106, 1, 500, 1);
    process(proc, 106, 105, 1, 600, 1);
    process(proc, 107, 1, 1, 700, 2, {}, QByteArray("node\0--prefix=", 14) + QFile::encodeName(nested) + '\0');
    write(proc + "/999/stat", "malformed stat\n");

    const QString executable = temp.path() + "/app/main";
    QDir().mkpath(QFileInfo(executable).absolutePath());
    write(executable, "fixture");
    check(::symlink(QFile::encodeName(executable).constData(), QFile::encodeName(proc + "/101/exe").constData()) == 0, "create executable symlink");
    LinuxSampler sampler(proc, sys);
    sampler.setProjectRoots({project, nested});
    const auto first = sampler.sample();
    const auto cpu1 = first["cpu"].toObject();
    check(cpu1["usage"].isNull(), "initial CPU usage is unknown");
    check(cpu1["coreCount"].toInt() == 2, "core count comes from injected proc");
    near(cpu1["uptimeSeconds"].toDouble(), 123.5, "fixture uptime");
    near(cpu1["load"].toArray()[1].toDouble(), 0.2, "fixture load");
    near(first["memory"].toObject()["usedBytes"].toDouble(), 750 * 1024, "memory uses MemAvailable");
    near(first["memory"].toObject()["fraction"].toDouble(), .75, "memory fraction");
    near(first["memory"].toObject()["swapUsedBytes"].toDouble(), 60 * 1024, "swap used");
    check(first["disks"].toArray().size() == 1, "only local filesystem is queried");
    check(first["disks"].toArray()[0].toObject()["mountPoint"].toString() == mount, "mountinfo escaped mount path");
    check(first["diskIO"].toObject()["devices"].toArray().size() == 1, "partition and virtual disk counters excluded");
    check(first["diskIO"].toObject()["readBytesPerSecond"].isNull(), "initial disk IO rate unknown");
    check(first["network"].toObject()["interface"].toString() == "eth0", "lowest metric default route selected");
    check(first["network"].toObject()["ipv4"].toArray().isEmpty(), "fixture does not expose host IPv4 addresses");
    check(first["network"].toObject()["ipv6"].toArray()[0].toString() == "2001:db8::1", "fixture IPv6 address");
    check(first["network"].toObject()["downloadBytesPerSecond"].isNull(), "initial network rate unknown");
    check(first["battery"].toObject()["state"].toString() == "noBattery", "readable empty power supply means no battery");
    check(first["processCount"].toInt() == 7, "only valid fixture processes returned");
    check(byPid(first, 101)["executablePath"].toString() == executable, "process executable identity follows proc exe symlink");
    check(byPid(first, 102)["executablePath"].toString().isEmpty(), "unreadable executable remains unknown");
    check(byPid(first, 101)["name"].toString() == "node (worker)", "proc comm with parentheses remains intact");
    check(byPid(first, 101)["projectPath"].toString() == nested, "longest configured project wins");
    check(byPid(first, 102)["projectPath"].toString() == nested, "child inherits parent project");
    check(byPid(first, 103)["projectPath"].toString().isEmpty(), "configured roots require a path boundary");
    check(byPid(first, 104)["projectPath"].toString() == discovered, "nearest marker discovered");
    check(byPid(first, 105)["projectPath"].toString().isEmpty(), "cyclic parent links terminate");
    check(byPid(first, 107)["projectPath"].toString() == nested, "absolute command option resolves configured project");
    check(byPid(first, 101)["cpuPercent"].isNull(), "initial process CPU unknown");
    near(byPid(first, 101)["memoryBytes"].toDouble(), 20 * ::sysconf(_SC_PAGESIZE), "RSS pages converted to bytes");

    QThread::msleep(40);
    write(proc + "/stat", "cpu 150 0 75 925 0 0 0 0 70 0\ncpu0 100 0 50 500 0 0 0 0 50 0\ncpu1 50 0 25 425 0 0 0 0 20 0\n");
    write(proc + "/diskstats", "8 0 sda 1 0 104 0 1 0 208 0 0 0 0\n");
    write(proc + "/net/dev", "eth0: 1200 0 0 0 0 0 0 0 2100 0 0 0 0 0 0 0\n");
    process(proc, 101, 1, 12, 100, 21, nested + "/src");
    process(proc, 102, 101, 6, 200, 10);
    process(proc, 103, 1, 999, 999, 1, sibling); // PID reuse must not produce a spike.
    const QString bat = sys + "/class/power_supply/BAT0";
    write(bat + "/type", "Battery\n"); write(bat + "/present", "1\n");
    write(bat + "/energy_now", "20\n"); write(bat + "/energy_full", "40\n");
    write(bat + "/energy_full_design", "50\n"); write(bat + "/status", "Discharging\n");
    write(bat + "/cycle_count", "123\n");
    const auto second = sampler.sample();
    const auto cpu2 = second["cpu"].toObject();
    near(cpu2["usage"].toDouble(), .5, "CPU busy delta excludes guest double counting");
    near(cpu2["user"].toDouble(), 1.0 / 3, "CPU user delta");
    near(cpu2["system"].toDouble(), 1.0 / 6, "CPU system delta");
    near(cpu2["perCore"].toArray()[0].toDouble(), .5, "per core CPU delta");
    check(cpu2["perCore"].toArray()[1].isNull(), "zero CPU interval denominator unknown");
    const auto network = second["network"].toObject(), io = second["diskIO"].toObject();
    check(network["downloadBytesPerSecond"].toDouble() > 0, "network elapsed rate positive");
    near(network["downloadBytesPerSecond"].toDouble() / network["uploadBytesPerSecond"].toDouble(), 2, "network rates use same real interval");
    near(io["writeBytesPerSecond"].toDouble() / io["readBytesPerSecond"].toDouble(), 2, "disk sector rates use same real interval");
    near(io["readBytesPerSecond"].toDouble() / network["downloadBytesPerSecond"].toDouble(), 2048.0 / 200, "sector size fixed at 512 bytes");
    check(byPid(second, 101)["cpuPercent"].toDouble() > 0, "process CPU measured");
    near(byPid(second, 101)["cpuPercent"].toDouble() / network["downloadBytesPerSecond"].toDouble(),
         1.0 / ::sysconf(_SC_CLK_TCK), "process CPU is one-core percent over the real scan interval");
    check(byPid(second, 103)["cpuPercent"].isNull(), "PID starttime reuse guarded");
    near(second["battery"].toObject()["level"].toDouble(), .5, "battery energy fraction");
    near(second["battery"].toObject()["healthPercent"].toDouble(), 80, "battery health percent");
    check(second["battery"].toObject()["powerSource"].toString() == "battery", "discharging power source");

    QFile::remove(proc + "/101/stat");
    const auto cached = sampler.sample(false);
    check(cached["processes"] == second["processes"] && cached["groups"] == second["groups"], "disabled process scan reuses complete snapshot");
    near(cached["processMonotonicSeconds"].toDouble(), second["processMonotonicSeconds"].toDouble(),
         "cached process rows retain their actual scan timestamp");
    near(cached["scanMilliseconds"].toDouble(), 0, "disabled process scan cost reported zero");
    write(proc + "/net/dev", "eth0: 1 0 0 0 0 0 0 0 2 0 0 0 0 0 0 0\n");
    write(proc + "/diskstats", "8 0 sda 1 0 1 0 1 0 2 0 0 0 0\n");
    write(proc + "/stat", "cpu 1 0 1 1 0 0 0 0\ncpu0 1 0 1 1 0 0 0 0\n");
    const QString bat1 = sys + "/class/power_supply/BAT1";
    write(bat1 + "/type", "Battery\n"); write(bat1 + "/charge_now", "3\n");
    write(bat1 + "/charge_full", "4\n"); write(bat1 + "/charge_full_design", "5\n");
    QThread::msleep(20);
    const auto reset = sampler.sample(false);
    check(reset["cpu"].toObject()["usage"].isNull(), "reset CPU counters unknown");
    check(reset["network"].toObject()["downloadBytesPerSecond"].isNull(), "reset network counters unknown");
    check(reset["diskIO"].toObject()["readBytesPerSecond"].isNull(), "reset disk counters unknown");
    near(reset["battery"].toObject()["level"].toDouble(), .625, "mixed energy and charge units are never added");

    write(proc + "/net/route", "Iface Destination Gateway Flags RefCnt Use Metric Mask\n");
    write(proc + "/net/ipv6_route", QByteArray(32, '0') + " 00 " + QByteArray(32, '0') + " 00 " + QByteArray(32, '0') + " 00000064 00000000 00000000 00000001 eth0\n");
    check(sampler.sample(false)["network"].toObject()["interface"].toString() == "eth0", "IPv6 default route fallback");

    LinuxSampler missing(temp.path() + "/missing-proc", temp.path() + "/missing-sys");
    const auto unknown = missing.sample();
    check(unknown["battery"].toObject()["isPresent"].isNull(), "missing power supply is unknown");
    check(unknown["memory"].toObject()["totalBytes"].isNull(), "missing memory is unknown");
    check(unknown["network"].toObject()["interface"].toString().isEmpty(), "missing fixture never picks host network");
}

void testPressureVmAndFaults() {
    QTemporaryDir temp;
    check(temp.isValid(), "pressure and VM fixture temporary directory");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    write(proc + "/stat", "cpu 100 0 50 800 50 0 0 0\ncpu0 100 0 50 800 50 0 0 0\nprocs_running 3\nprocs_blocked 2\n");
    write(proc + "/meminfo", "MemTotal: 1000 kB\nMemAvailable: 200 kB\nKReclaimable: 100 kB\nSReclaimable: 50 kB\nDirty: 5 kB\nWriteback: 3 kB\nShmem: 20 kB\nAnonPages: 400 kB\n");
    write(proc + "/pressure/cpu", "some avg10=1.25 avg60=2.00 avg300=3.00 total=100\nfull avg10=99.0 avg60=99.0 avg300=99.0 total=100\n");
    write(proc + "/pressure/memory", "some avg10=4.0 avg60=3.0 avg300=2.0 total=100\nfull avg10=1.0 avg60=0.5 avg300=0.1 total=2\n");
    write(proc + "/vmstat", "pswpin 100\npswpout 200\npgmajfault 1000\noom_kill 3\nignored 123456\n");
    write(proc + "/401/stat", stat(401, 1, 10, 123, 2, "fixture", 100));
    LinuxSampler sampler(proc, sys);
    QThread::msleep(2);
    const auto first = sampler.sample();
    check(first["cpu"].toObject()["iowait"].isNull(), "initial iowait interval is unknown");
    near(first["cpu"].toObject()["runningProcesses"].toDouble(), 3, "running count reuses proc stat");
    near(first["cpu"].toObject()["blockedProcesses"].toDouble(), 2, "blocked count reuses proc stat");
    const auto pressure = first["pressure"].toObject();
    near(pressure["cpu"].toObject()["some"].toObject()["avg10"].toDouble(), 1.25, "PSI remains a percent, not a fraction");
    near(pressure["memory"].toObject()["full"].toObject()["avg60"].toDouble(), .5, "PSI full averages parsed");
    check(pressure["cpu"].toObject()["full"].isNull(), "system CPU full is undefined even if kernel exposes a line");
    check(pressure["io"].isNull(), "unsupported PSI resource is null");
    for (const auto &key : {"swapInBytesPerSecond", "swapOutBytesPerSecond", "majorFaultsPerSecond", "oomKillDelta"})
        check(first["vm"].toObject()[key].isNull(), "initial VM counter rates/deltas are unknown");
    check(byPid(first, 401)["majorFaultsPerSecond"].isNull(), "initial process major fault rate unknown");
    check(byPid(first, 401)["state"] == "S", "process state reuses stat field");
    near(first["memory"].toObject()["reclaimableBytes"].toDouble(), 100 * 1024, "KReclaimable preferred without adding overlapping SReclaimable");
    near(first["memory"].toObject()["dirtyBytes"].toDouble(), 5 * 1024, "dirty memory bytes");
    near(first["memory"].toObject()["writebackBytes"].toDouble(), 3 * 1024, "writeback bytes");
    near(first["memory"].toObject()["sharedBytes"].toDouble(), 20 * 1024, "shared memory bytes");
    near(first["memory"].toObject()["anonymousBytes"].toDouble(), 400 * 1024, "anonymous memory bytes");
    QThread::msleep(20);
    write(proc + "/stat", "cpu 150 0 75 850 75 0 0 0\ncpu0 150 0 75 850 75 0 0 0\nprocs_running 7\nprocs_blocked 4\n");
    write(proc + "/pressure/io", "some avg10=5.0 avg60=1.0 avg300=0.5 total=1\nfull avg10=nan avg60=-1 avg300=101 total=0\n");
    write(proc + "/vmstat", "pswpin 110\npswpout 220\npgmajfault 1004\noom_kill 5\n");
    write(proc + "/401/stat", stat(401, 1, 12, 123, 2, "fixture", 104, "D"));
    const auto second = sampler.sample();
    const double elapsed = second["monotonicSeconds"].toDouble() - first["monotonicSeconds"].toDouble();
    near(second["cpu"].toObject()["iowait"].toDouble(), 1.0 / 6, "iowait uses CPU interval delta");
    near(second["cpu"].toObject()["usage"].toDouble(), .5, "busy CPU excludes idle and iowait");
    near(second["vm"].toObject()["swapInBytesPerSecond"].toDouble(), 10.0 * ::sysconf(_SC_PAGESIZE) / elapsed, "swap pages converted using runtime page size");
    near(second["vm"].toObject()["swapOutBytesPerSecond"].toDouble(), 20.0 * ::sysconf(_SC_PAGESIZE) / elapsed, "swap out rate uses same interval");
    near(second["vm"].toObject()["majorFaultsPerSecond"].toDouble(), 4.0 / elapsed, "system major faults interval");
    near(second["vm"].toObject()["oomKillDelta"].toDouble(), 2, "OOM event counter is delta, not a rate");
    near(byPid(second, 401)["majorFaultsPerSecond"].toDouble(), 4.0 / elapsed, "process major faults use existing scan interval");
    check(byPid(second, 401)["state"] == "D", "process blocking state preserved without claiming cause");
    check(second["pressure"].toObject()["io"].toObject()["full"].isNull(), "nonfinite/negative/out of range PSI is unavailable");
    const auto cached = sampler.sample(false);
    check(cached["processesFresh"] == false && cached["processes"] == second["processes"], "cached process state and rates retain explicit freshness");
    write(proc + "/vmstat", "pswpin 1\npswpout 2\npgmajfault 3\noom_kill 1\n");
    write(proc + "/401/stat", stat(401, 1, 13, 123, 2, "fixture", 1));
    QThread::msleep(5);
    const auto reset = sampler.sample();
    for (const auto &key : {"swapInBytesPerSecond", "swapOutBytesPerSecond", "majorFaultsPerSecond", "oomKillDelta"})
        check(reset["vm"].toObject()[key].isNull(), "reset VM counters remain unknown");
    check(byPid(reset, 401)["majorFaultsPerSecond"].isNull(), "reset process major faults remain unknown");
    write(proc + "/vmstat", "pswpin 2\npswpout 3\npgmajfault 4\noom_kill 1\n");
    write(proc + "/401/stat", stat(401, 1, 14, 123, 2, "fixture", 2));
    QThread::msleep(5);
    const auto recovered = sampler.sample();
    check(recovered["vm"].toObject()["swapInBytesPerSecond"].isDouble(), "VM rate recovers after reset");
    check(recovered["vm"].toObject()["oomKillDelta"].isDouble() && recovered["vm"].toObject()["oomKillDelta"].toDouble() == 0, "measured no OOM events is zero");
    check(byPid(recovered, 401)["majorFaultsPerSecond"].isDouble(), "process fault rate recovers after reset");
    check(QFile::remove(proc + "/vmstat"), "remove VM fixture");
    const auto missing = sampler.sample(false);
    check(missing["vm"].toObject()["swapInBytesPerSecond"].isNull(), "missing VM data is not zero");
    write(proc + "/vmstat", "pswpin 3\npswpout 4\npgmajfault 5\noom_kill 2\n");
    const auto restored = sampler.sample(false);
    check(restored["vm"].toObject()["swapInBytesPerSecond"].isNull() && restored["vm"].toObject()["oomKillDelta"].isNull(), "restored VM counters need a fresh baseline");
    write(proc + "/401/stat", stat(401, 1, 1000, 456, 2, "fixture", 10000));
    QThread::msleep(5);
    check(byPid(sampler.sample(), 401)["majorFaultsPerSecond"].isNull(), "PID reuse does not create a major fault spike");
    write(proc + "/meminfo", "SReclaimable: 25 kB\n");
    near(sampler.sample(false)["memory"].toObject()["reclaimableBytes"].toDouble(), 25 * 1024, "SReclaimable fallback for older kernels");
    write(proc + "/stat", "cpu 200 0 100 1000\ncpu0 200 0 100 1000\n");
    const auto shortCpu = sampler.sample(false)["cpu"].toObject();
    check(shortCpu["iowait"].isNull(), "absent iowait counter stays unknown despite valid busy counters");
    check(shortCpu["runningProcesses"].isNull() && shortCpu["blockedProcesses"].isNull(), "absent scheduler counts stay unknown");
}

void testDiskWait() {
    const double hz = double(qMax(1L, ::sysconf(_SC_CLK_TCK)));
    QTemporaryDir temp;
    check(temp.isValid(), "disk wait temporary directory");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    write(proc + "/601/stat", stat(601, 1, 10, 500, 1, "reader", 0, "D", 200));
    write(proc + "/602/stat", stat(602, 1, 10, 501, 1, "idle", 0, "S", 0));
    write(proc + "/603/stat", stat(603, 1, 10, 502, 1, "old kernel line"));
    LinuxSampler sampler(proc, sys);
    const auto first = sampler.sample();
    check(byPid(first, 601)["ioWaitPercent"].isNull(), "disk wait needs two readings");
    QThread::msleep(60);
    write(proc + "/601/stat", stat(601, 1, 10, 500, 1, "reader", 0, "D", 203));
    const auto second = sampler.sample();
    const double elapsed = second["processMonotonicSeconds"].toDouble() - first["processMonotonicSeconds"].toDouble();
    near(byPid(second, 601)["ioWaitPercent"].toDouble(), qMin(100.0, 3.0 / hz / elapsed * 100.0), "disk wait is the share of the interval spent waiting");
    check(byPid(second, 602)["ioWaitPercent"].isDouble() && byPid(second, 602)["ioWaitPercent"].toDouble() == 0, "a process that did not wait reads 0%");
    check(byPid(second, 603)["ioWaitPercent"].isNull(), "a stat line without the field stays unknown");
    QThread::msleep(5);
    write(proc + "/601/stat", stat(601, 1, 10, 500, 1, "reader", 0, "D", 150));
    check(byPid(sampler.sample(), 601)["ioWaitPercent"].isNull(), "a counter that goes backwards is unknown, never negative");

    // Delay accounting is off by default since Linux 5.14: every process then reports
    // zero, which means "not counted", not "no waiting".
    QTemporaryDir off;
    check(off.isValid(), "disk wait off temporary directory");
    const QString offProc = off.path() + "/proc";
    write(offProc + "/701/stat", stat(701, 1, 10, 500, 1, "reader", 0, "D", 0));
    write(offProc + "/702/stat", stat(702, 1, 10, 501, 1, "writer", 0, "S", 0));
    LinuxSampler uncounted(offProc, off.path() + "/sys");
    uncounted.sample();
    QThread::msleep(20);
    const auto zero = uncounted.sample();
    check(byPid(zero, 701)["ioWaitPercent"].isNull() && byPid(zero, 702)["ioWaitPercent"].isNull(), "all-zero disk wait counters are reported as unknown");
}

void testProcessIO() {
    QTemporaryDir temp;
    check(temp.isValid(), "process IO temporary directory");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    process(proc, 301, 1, 10, 1000, 1);
    const QString ioPath = proc + "/301/io";
    write(ioPath, "rchar: 9000000000\nwchar: 8000000000\nread_bytes: 1000\nwrite_bytes: 5000\ncancelled_write_bytes: 123\n");
    LinuxSampler sampler(proc, sys);
    QThread::msleep(2);
    const auto first = sampler.sample();
    check(byPid(first, 301)["ioReadBytesPerSecond"].isNull(), "initial process disk read rate unknown");
    check(byPid(first, 301)["ioWriteBytesPerSecond"].isNull(), "initial process disk write rate unknown");
    QThread::msleep(15);
    write(ioPath, "rchar: 19000000000\nwchar: 0\nread_bytes: 1300\nwrite_bytes: 5600\ncancelled_write_bytes: 999999\n");
    const auto second = sampler.sample();
    const double elapsed = second["processMonotonicSeconds"].toDouble() - first["processMonotonicSeconds"].toDouble();
    check(elapsed > 0, "process IO uses a positive monotonic scan interval");
    near(byPid(second, 301)["ioReadBytesPerSecond"].toDouble(), 300.0 / elapsed,
         "process disk reads use read_bytes delta, not rchar");
    near(byPid(second, 301)["ioWriteBytesPerSecond"].toDouble(), 600.0 / elapsed,
         "process disk writes use write_bytes delta, not wchar/cancelled_write_bytes");

    QThread::msleep(5);
    write(ioPath, "read_bytes: 1600\nwrite_bytes: 6200\n");
    const auto cached = sampler.sample(false);
    check(cached["processes"] == second["processes"], "cached scans leave process IO rates intact");
    near(cached["processMonotonicSeconds"].toDouble(), second["processMonotonicSeconds"].toDouble(),
         "cached process IO does not advance the process time baseline");
    QThread::msleep(5);
    const auto third = sampler.sample();
    const double nextElapsed = third["processMonotonicSeconds"].toDouble() - second["processMonotonicSeconds"].toDouble();
    near(byPid(third, 301)["ioReadBytesPerSecond"].toDouble(), 300.0 / nextElapsed,
         "fresh process IO spans cached sample intervals once");

    QThread::msleep(5);
    write(ioPath, "read_bytes: 3\nwrite_bytes: 10\n");
    const auto reset = sampler.sample();
    check(byPid(reset, 301)["ioReadBytesPerSecond"].isNull() && byPid(reset, 301)["ioWriteBytesPerSecond"].isNull(),
          "decreasing process IO counters are unavailable");
    QThread::msleep(5);
    write(ioPath, "read_bytes: 8\nwrite_bytes: 20\n");
    const auto recovered = sampler.sample();
    check(byPid(recovered, 301)["ioReadBytesPerSecond"].isDouble() && byPid(recovered, 301)["ioWriteBytesPerSecond"].isDouble(),
          "process IO recovers from the new counter baseline");

    check(QFile::remove(ioPath), "remove process IO fixture");
    QThread::msleep(5);
    const auto missing = sampler.sample();
    check(byPid(missing, 301)["ioReadBytesPerSecond"].isNull() && byPid(missing, 301)["ioWriteBytesPerSecond"].isNull(),
          "missing process IO never becomes a zero rate");
    QThread::msleep(5);
    write(ioPath, "read_bytes: 13\n");
    const auto partial = sampler.sample();
    check(byPid(partial, 301)["ioReadBytesPerSecond"].isNull() && byPid(partial, 301)["ioWriteBytesPerSecond"].isNull(),
          "restored IO requires a fresh baseline and keeps absent counters unknown");
    QThread::msleep(5);
    write(ioPath, "read_bytes: 18\nwrite_bytes: 100\n");
    const auto partialRecovery = sampler.sample();
    check(byPid(partialRecovery, 301)["ioReadBytesPerSecond"].isDouble() && byPid(partialRecovery, 301)["ioWriteBytesPerSecond"].isNull(),
          "read/write IO availability and baselines are independent");
    QThread::msleep(5);
    write(ioPath, "read_bytes: invalid\nwrite_bytes: 120\n");
    const auto malformed = sampler.sample();
    check(byPid(malformed, 301)["ioReadBytesPerSecond"].isNull() && byPid(malformed, 301)["ioWriteBytesPerSecond"].isDouble(),
          "malformed IO counters invalidate only that indicator");

    QThread::msleep(5);
    process(proc, 301, 1, 10000, 2000, 1);
    write(ioPath, "read_bytes: 1000000000\nwrite_bytes: 2000000000\n");
    const auto reused = sampler.sample();
    check(byPid(reused, 301)["ioReadBytesPerSecond"].isNull() && byPid(reused, 301)["ioWriteBytesPerSecond"].isNull(),
          "PID reuse never turns previous-process IO into a spike");
    check(byPid(reused, 301)["cpuPercent"].isNull(), "PID reuse resets both CPU and disk IO baselines");
    check(QFile::remove(proc + "/301/stat"), "remove process fixture");
    QThread::msleep(5);
    check(sampler.sample()["processes"].toArray().isEmpty(), "removed process IO state is pruned");
    process(proc, 301, 1, 10001, 2000, 1);
    write(ioPath, "rchar: 9000000000\nwchar: 8000000000\n");
    QThread::msleep(5);
    const auto onlyCharacters = sampler.sample();
    check(byPid(onlyCharacters, 301)["ioReadBytesPerSecond"].isNull() && byPid(onlyCharacters, 301)["ioWriteBytesPerSecond"].isNull(),
          "character IO counters are never substituted for unavailable disk byte counters");
}
}

// A middle process and its child can both end within one interval; their CPU then
// reaches the nearest surviving ancestor. What could only predate the interval is
// bounded by how long the whole machine was busy.
void testChildSubtreeAndCap() {
    QTemporaryDir temp;
    check(temp.isValid(), "subtree fixture");
    const QString proc = temp.path() + "/proc", sys = temp.path() + "/sys";
    auto put = [&](qint64 pid, qint64 parent, quint64 ticks, quint64 start, quint64 childTicks) {
        write(proc + '/' + QString::number(pid) + "/stat", stat(pid, parent, ticks, start, 1, "worker", 0, "S", -1, childTicks));
    };
    auto machine = [&](quint64 busy) { write(proc + "/stat", "cpu  " + QByteArray::number(busy) + " 0 0 100000 0 0 0 0 0 0\n"); };
    machine(1000);
    put(600, 1, 0, 6000, 0);        // reference
    put(801, 1, 5, 8010, 0);        // survives
    put(802, 801, 10, 8020, 0);     // a make: ends within the interval
    put(803, 802, 400, 8030, 0);    // its compiler, seen at 400 ticks, ends too
    put(901, 1, 5, 9010, 0);        // collects something that ran before the interval
    LinuxSampler sampler(proc, sys);
    sampler.sample();
    QThread::msleep(30);
    machine(1600);
    put(600, 1, 100, 6000, 0);
    QDir(proc + "/802").removeRecursively(); QDir(proc + "/803").removeRecursively();
    put(801, 1, 5, 8010, 10 + 450);  // the make (10) and the compiler's whole 450
    put(901, 1, 5, 9010, 100000);
    const auto second = sampler.sample();
    const double reference = byPid(second, 600)["cpuPercent"].toDouble();
    near(byPid(second, 801)["childCpuPercent"].toDouble() / reference, .5,
         "a subtree ending together credits its surviving ancestor only with what was never seen");
    near(byPid(second, 901)["childCpuPercent"].toDouble() / reference, 6,
         "children's CPU is bounded by how long the whole machine was busy in the interval");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testFixture();
    testFreshProcessPaths();
    testChildAttribution();
    testChildSubtreeAndCap();
    testProcessIO();
    testDiskWait();
    testPressureVmAndFaults();
    std::puts("Sampler fixture tests passed (process IO/faults, disk wait share and unknown-when-uncounted, pressure, VM rates, resets, missing data, reuse and cache)");
    return 0;
}
