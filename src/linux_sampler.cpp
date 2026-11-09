#include "linux_sampler.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace {
QJsonValue unavailable() { return QJsonValue(QJsonValue::Null); }
QByteArray readFile(const QString &path, qint64 limit = 1024 * 1024) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.read(limit);
}
// Used for the hundreds of other-user /proc/PID/stat files, where QFile's
// per-open overhead would otherwise dominate an idle scan.
QByteArray readSmall(const QByteArray &path) {
    char buffer[2048];
    const int fd = ::open(path.constData(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    const ssize_t size = ::read(fd, buffer, sizeof(buffer));
    ::close(fd);
    return size > 0 ? QByteArray(buffer, int(size)) : QByteArray();
}
QString readText(const QString &path) { return QString::fromUtf8(readFile(path)).trimmed(); }
QList<QByteArray> fields(const QByteArray &line) { return line.simplified().split(' '); }
bool number(const QByteArray &text, quint64 &result) {
    bool ok = false;
    result = text.toULongLong(&ok);
    return ok;
}
QJsonValue scalarFile(const QString &path, double divisor = 1.0) {
    bool ok = false;
    const double value = readText(path).toDouble(&ok);
    return ok && std::isfinite(value) ? QJsonValue(value / divisor) : unavailable();
}
QString normalizedPath(const QString &path) {
    if (!QDir::isAbsolutePath(path)) return {};
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? path : canonical);
}
bool inside(const QString &path, const QString &root) {
    return path == root || (root == QStringLiteral("/") ? path.startsWith('/')
                           : path.startsWith(root + '/'));
}
QString linkTarget(const QString &path, QHash<QString, QString> &paths) {
    const QByteArray encoded = QFile::encodeName(path);
    char target[16384];
    const ssize_t length = ::readlink(encoded.constData(), target, sizeof(target));
    if (length <= 0 || size_t(length) == sizeof(target)) return {};
    QString result = QFile::decodeName(QByteArray::fromRawData(target, int(length)));
    if (result.endsWith(QStringLiteral(" (deleted)"))) result.chop(10);
    const auto found = paths.constFind(result);
    if (found != paths.cend()) return found.value();
    const QString normalized = normalizedPath(result);
    paths.insert(result, normalized);
    return normalized;
}
QString unescapeMount(QString path) {
    path.replace(QStringLiteral("\\040"), QStringLiteral(" "));
    path.replace(QStringLiteral("\\011"), QStringLiteral("\t"));
    path.replace(QStringLiteral("\\012"), QStringLiteral("\n"));
    path.replace(QStringLiteral("\\134"), QStringLiteral("\\"));
    return path;
}
struct CpuCounter {
    quint64 total = 0, idle = 0, user = 0, system = 0, iowait = 0, interrupt = 0;
    bool hasIowait = false;
};
bool cpuCounter(const QList<QByteArray> &parts, CpuCounter &counter) {
    if (parts.size() < 5) return false;
    quint64 values[8] = {};
    for (int i = 1; i < qMin(parts.size(), 9); ++i)
        if (!number(parts[i], values[i - 1])) return false;
    // guest and guest_nice are already counted by user and nice in /proc/stat.
    for (quint64 value : values) counter.total += value;
    counter.idle = values[3] + values[4];
    counter.iowait = values[4];
    counter.hasIowait = parts.size() > 5;
    counter.user = values[0] + values[1];
    counter.system = values[2] + values[5] + values[6];
    counter.interrupt = values[5] + values[6];
    return true;
}
QJsonValue cpuDelta(const CpuCounter &now, const CpuCounter *before, int kind) {
    if (!before || now.total <= before->total || now.idle < before->idle ||
        now.user < before->user || now.system < before->system) return unavailable();
    if (kind == 3 && (!now.hasIowait || !before->hasIowait || now.iowait < before->iowait)) return unavailable();
    if (kind == 4 && now.interrupt < before->interrupt) return unavailable();
    const quint64 total = now.total - before->total;
    const quint64 part = kind == 0 ? total - qMin(total, now.idle - before->idle)
                         : kind == 1 ? now.user - before->user : kind == 2 ? now.system - before->system
                         : kind == 4 ? now.interrupt - before->interrupt
                         : now.iowait - before->iowait;
    return qBound(0.0, double(part) / double(total), 1.0);
}
struct ByteCounter {
    quint64 read = 0, write = 0;
    qint64 time = 0;
};
struct DiskCounter {
    quint64 read = 0, write = 0, operations = 0, waitMs = 0, busyMs = 0;
    qint64 time = 0;
};
QJsonValue byteRate(quint64 current, quint64 previous, qint64 elapsed) {
    if (elapsed <= 0 || current < previous) return unavailable();
    return double(current - previous) * 1e9 / double(elapsed);
}
struct Process {
    qint64 pid = 0, parent = 0;
    quint64 start = 0, ticks = 0, rss = 0, flags = 0, threads = 0;
    QString name, cwd, project, executable, state, waitChannel;
    quint64 majorFaults = 0;
    bool hasMajorFaults = false;
    QJsonValue memoryGrowth = unavailable();
    QJsonValue majorFaultRate = unavailable();
    QJsonValue cpu = unavailable();
    quint64 ioRead = 0, ioWrite = 0;
    bool hasRead = false, hasWrite = false;
    QJsonValue readRate = unavailable(), writeRate = unavailable();
    // Time spent waiting for block I/O (proc_pid_stat field 42). World readable, so
    // it is the only disk measure available for other users' processes.
    quint64 blkio = 0;
    bool hasBlkio = false;
    QJsonValue ioWait = unavailable();
};
// Only counters survive a scan. In particular, argv and path metadata must not
// remain allocated for every process while the next generation is collected.
struct ProcessCounters {
    quint64 start = 0, ticks = 0, majorFaults = 0, ioRead = 0, ioWrite = 0, rss = 0;
    bool hasMajorFaults = false, hasRead = false, hasWrite = false;
    quint64 blkio = 0;
    bool hasBlkio = false;
};
constexpr quint64 KernelThreadFlag = 0x00200000; // PF_KTHREAD in /proc/PID/stat flags.
bool processStat(const QByteArray &data, Process &process) {
    const int first = data.indexOf('('), last = data.lastIndexOf(')');
    if (first < 1 || last <= first) return false;
    bool valid = false;
    process.pid = data.left(first).trimmed().toLongLong(&valid);
    if (!valid || process.pid <= 0) return false;
    process.name = QString::fromLocal8Bit(data.mid(first + 1, last - first - 1));
    const auto values = fields(data.mid(last + 1));
    if (values.size() < 22) return false;
    if (values[0].size() == 1) process.state = QString::fromLatin1(values[0]);
    process.hasMajorFaults = number(values[9], process.majorFaults);
    number(values[6], process.flags);
    number(values[17], process.threads);
    process.parent = values[1].toLongLong(&valid);
    if (!valid || process.parent < 0) return false;
    quint64 user = 0, system = 0, rss = 0;
    if (!number(values[11], user) || !number(values[12], system) ||
        !number(values[19], process.start) || !number(values[21], rss)) return false;
    process.ticks = user + system;
    process.rss = rss;
    if (values.size() > 39) process.hasBlkio = number(values[39], process.blkio);
    return true;
}
QJsonObject processJson(const Process &process, long pageSize) {
    return {{"pid", double(process.pid)}, {"parentPID", double(process.parent)},
            {"startTicks", double(process.start)}, {"name", process.name},
            {"executablePath", process.executable},
            {"projectPath", process.project}, {"cpuPercent", process.cpu},
            {"state", process.state.isEmpty() ? unavailable() : QJsonValue(process.state)},
            {"majorFaultsPerSecond", process.majorFaultRate}, {"threads", double(process.threads)},
            {"ioReadBytesPerSecond", process.readRate}, {"ioWriteBytesPerSecond", process.writeRate}, {"ioWaitPercent", process.ioWait},
            {"memoryGrowthBytesPerSecond", process.memoryGrowth},
            {"waitChannel", process.waitChannel.isEmpty() ? unavailable() : QJsonValue(process.waitChannel)},
            {"memoryBytes", double(process.rss) * double(pageSize)}};
}
}

struct LinuxSampler::Impl {
    QString proc, sys;
    bool nativeRoots;
    QElapsedTimer clock;
    const long ticksPerSecond = qMax(1L, ::sysconf(_SC_CLK_TCK));
    const long pageSize = qMax(1L, ::sysconf(_SC_PAGESIZE));
    QMap<QString, CpuCounter> previousCpu;
    QHash<QString, ByteCounter> previousNetworks;
    QHash<QString, DiskCounter> previousDisks;
    QHash<uint, QString> ownerNames;
    quint64 previousContextSwitches = 0, previousForks = 0, previousSchedWait = 0, previousSchedRun = 0;
    qint64 previousStatTime = 0, previousSchedTime = 0;
    QHash<qint64, ProcessCounters> previousProcesses;
    QHash<QByteArray, quint64> previousVm;
    qint64 previousVmTime = 0;
    qint64 previousProcessTime = 0, diskRefreshTime = -1;
    QStringList projectRoots;
    QJsonArray cachedDisks, cachedProcesses, cachedGroups, cachedSystemProcesses;
    QJsonObject cachedProcessStates, cachedThermal;
    QJsonValue cachedAttributedCpu = unavailable();
    int cachedProcessCount = 0;

    Impl(QString procRoot, QString sysRoot)
        : proc(QDir::cleanPath(procRoot)), sys(QDir::cleanPath(sysRoot)),
          nativeRoots(proc == QStringLiteral("/proc") && sys == QStringLiteral("/sys")) {
        clock.start();
    }

    QJsonValue runQueueWait(qint64 now) {
        // Field 8 of each /proc/schedstat cpu line is the total nanoseconds tasks
        // waited for that CPU. This keeps CPU contention measurable on kernels
        // built without PSI, where /proc/pressure does not exist.
        quint64 wait = 0, run = 0;
        int cpus = 0;
        for (const auto &line : readFile(proc + "/schedstat", 256 * 1024).split('\n')) {
            if (!line.startsWith("cpu")) continue;
            const auto parts = fields(line);
            quint64 running = 0, waiting = 0;
            if (parts.size() < 10 || !number(parts[7], running) || !number(parts[8], waiting)) continue;
            run += running; wait += waiting; ++cpus;
        }
        QJsonValue result = unavailable();
        if (cpus && previousSchedTime > 0 && now > previousSchedTime && wait >= previousSchedWait && run > previousSchedRun)
            result = double(wait - previousSchedWait) / double(now - previousSchedTime) / cpus;
        previousSchedWait = wait; previousSchedRun = run; previousSchedTime = cpus ? now : 0;
        return result;
    }

    QJsonObject cpu(qint64 now) {
        QMap<QString, CpuCounter> current;
        QJsonValue running = unavailable(), blocked = unavailable(), switches = unavailable(), forks = unavailable();
        quint64 contextSwitches = 0, forkCount = 0;
        bool hasContextSwitches = false, hasForks = false;
        for (const auto &line : readFile(proc + "/stat").split('\n')) {
            const auto parts = fields(line);
            if (parts.size() == 2 && (parts[0] == "procs_running" || parts[0] == "procs_blocked")) {
                quint64 value = 0;
                if (number(parts[1], value)) {
                    if (parts[0] == "procs_running") running = double(value);
                    else blocked = double(value);
                }
            }
            if (parts.size() == 2 && parts[0] == "ctxt") hasContextSwitches = number(parts[1], contextSwitches);
            if (parts.size() == 2 && parts[0] == "processes") hasForks = number(parts[1], forkCount);
            if (parts.isEmpty() || !parts[0].startsWith("cpu")) continue;
            bool core = true;
            for (int i = 3; i < parts[0].size(); ++i) core &= parts[0][i] >= '0' && parts[0][i] <= '9';
            if (!core) continue;
            CpuCounter value;
            if (cpuCounter(parts, value)) current.insert(QString::fromLatin1(parts[0]), value);
        }
        if (previousStatTime > 0 && now > previousStatTime) {
            if (hasContextSwitches) switches = byteRate(contextSwitches, previousContextSwitches, now - previousStatTime);
            if (hasForks) forks = byteRate(forkCount, previousForks, now - previousStatTime);
        }
        previousContextSwitches = contextSwitches; previousForks = forkCount; previousStatTime = now;
        QJsonObject result{{"usage", unavailable()}, {"user", unavailable()}, {"system", unavailable()},
                           {"iowait", unavailable()}, {"irq", unavailable()}, {"runningProcesses", running}, {"blockedProcesses", blocked},
                           {"contextSwitchesPerSecond", switches}, {"forksPerSecond", forks}, {"runQueueWait", runQueueWait(now)}};
        const auto aggregate = current.constFind(QStringLiteral("cpu"));
        if (aggregate != current.cend()) {
            const auto old = previousCpu.constFind(QStringLiteral("cpu"));
            const CpuCounter *before = old == previousCpu.cend() ? nullptr : &old.value();
            result["usage"] = cpuDelta(aggregate.value(), before, 0);
            result["user"] = cpuDelta(aggregate.value(), before, 1);
            result["system"] = cpuDelta(aggregate.value(), before, 2);
            result["iowait"] = cpuDelta(aggregate.value(), before, 3);
            result["irq"] = cpuDelta(aggregate.value(), before, 4);
        }
        QStringList cores = current.keys();
        cores.removeAll(QStringLiteral("cpu"));
        std::sort(cores.begin(), cores.end(), [](const QString &a, const QString &b) { return a.mid(3).toInt() < b.mid(3).toInt(); });
        QJsonArray perCore;
        for (const QString &core : cores) {
            const auto old = previousCpu.constFind(core);
            perCore.append(cpuDelta(current[core], old == previousCpu.cend() ? nullptr : &old.value(), 0));
        }
        result["perCore"] = perCore;
        result["coreCount"] = cores.size();
        QJsonArray load;
        const auto loadFields = fields(readFile(proc + "/loadavg"));
        for (int i = 0; i < 3; ++i) {
            bool valid = false;
            const double value = i < loadFields.size() ? loadFields[i].toDouble(&valid) : 0;
            load.append(valid ? QJsonValue(value) : unavailable());
        }
        result["load"] = load;
        bool valid = false;
        const double uptime = readFile(proc + "/uptime").split(' ').value(0).toDouble(&valid);
        result["uptimeSeconds"] = valid ? QJsonValue(uptime) : unavailable();
        previousCpu = current;
        return result;
    }

    QJsonObject memory() const {
        QHash<QByteArray, quint64> values;
        for (const auto &line : readFile(proc + "/meminfo").split('\n')) {
            const auto parts = fields(line);
            if (parts.size() < 2) continue;
            quint64 value = 0;
            if (!number(parts[1], value)) continue;
            QByteArray key = parts[0];
            if (key.endsWith(':')) key.chop(1);
            values.insert(key, value * (parts.value(2) == "kB" ? 1024ULL : 1ULL));
        }
        auto get = [&values](const char *key) -> QJsonValue {
            const auto found = values.constFind(key);
            return found == values.cend() ? unavailable() : QJsonValue(double(found.value()));
        };
        QJsonObject result{{"totalBytes", get("MemTotal")}, {"availableBytes", get("MemAvailable")},
                           {"usedBytes", unavailable()}, {"fraction", unavailable()},
                           {"swapTotalBytes", get("SwapTotal")}, {"swapUsedBytes", unavailable()},
                           {"cachedBytes", get("Cached")}, {"buffersBytes", get("Buffers")},
                           {"activeBytes", get("Active")}, {"inactiveBytes", get("Inactive")},
                           {"reclaimableBytes", get(values.contains("KReclaimable") ? "KReclaimable" : "SReclaimable")},
                           {"dirtyBytes", get("Dirty")}, {"writebackBytes", get("Writeback")},
                           {"sharedBytes", get("Shmem")}, {"anonymousBytes", get("AnonPages")}};
        if (values.contains("MemTotal") && values.contains("MemAvailable")) {
            const quint64 total = values["MemTotal"], available = qMin(total, values["MemAvailable"]);
            result["availableBytes"] = double(available);
            result["usedBytes"] = double(total - available);
            if (total) result["fraction"] = double(total - available) / double(total);
        }
        if (values.contains("SwapTotal") && values.contains("SwapFree"))
            result["swapUsedBytes"] = double(values["SwapTotal"] - qMin(values["SwapTotal"], values["SwapFree"]));
        return result;
    }

    QJsonObject pressure() const {
        QJsonObject result;
        for (const auto &resource : {"cpu", "memory", "io"}) {
            QJsonObject parsed{{"some", unavailable()}, {"full", unavailable()}};
            bool supported = false;
            for (const auto &line : readFile(proc + "/pressure/" + resource, 4096).split('\n')) {
                const auto parts = fields(line);
                if (parts.isEmpty() || (parts[0] != "some" && parts[0] != "full")) continue;
                // CPU full is undefined at system level, including kernels that
                // expose an all-zero compatibility line. Do not call it healthy.
                if (QByteArray(resource) == "cpu" && parts[0] == "full") continue;
                QJsonObject averages{{"avg10", unavailable()}, {"avg60", unavailable()}, {"avg300", unavailable()}};
                bool measured = false;
                for (int i = 1; i < parts.size(); ++i) {
                    const int equal = parts[i].indexOf('=');
                    if (equal < 0) continue;
                    const QByteArray key = parts[i].left(equal);
                    if (key != "avg10" && key != "avg60" && key != "avg300") continue;
                    bool valid = false;
                    const double value = parts[i].mid(equal + 1).toDouble(&valid);
                    if (valid && std::isfinite(value) && value >= 0 && value <= 100) {
                        averages[QString::fromLatin1(key)] = value;
                        measured = true;
                    }
                }
                if (measured) { parsed[QString::fromLatin1(parts[0])] = averages; supported = true; }
            }
            result[resource] = supported ? QJsonValue(parsed) : unavailable();
        }
        return result;
    }

    QJsonObject vm(qint64 now) {
        QHash<QByteArray, quint64> current;
        for (const auto &line : readFile(proc + "/vmstat").split('\n')) {
            const auto parts = fields(line);
            if (parts.size() != 2) continue;
            quint64 value = 0;
            if (!number(parts[1], value)) continue;
            if (parts[0].startsWith("allocstall_")) current["allocstall"] += value;
            else if (parts[0] == "pswpin" || parts[0] == "pswpout" || parts[0] == "pgmajfault" || parts[0] == "oom_kill" ||
                     parts[0] == "pgscan_direct" || parts[0] == "pgscan_kswapd") current.insert(parts[0], value);
        }
        QJsonObject result;
        const QList<QByteArray> counters{"pswpin", "pswpout", "pgmajfault", "oom_kill", "allocstall", "pgscan_direct", "pgscan_kswapd"};
        const QStringList keys{"swapInBytesPerSecond", "swapOutBytesPerSecond", "majorFaultsPerSecond", "oomKillDelta",
                               "allocStallsPerSecond", "directReclaimPagesPerSecond", "kswapdScanPagesPerSecond"};
        for (int i = 0; i < counters.size(); ++i) {
            const auto key = counters[i];
            QJsonValue value = unavailable();
            if (previousVmTime > 0 && now > previousVmTime && current.contains(key) && previousVm.contains(key)) {
                if (i == 3) {
                    if (current[key] >= previousVm[key]) value = double(current[key] - previousVm[key]);
                } else {
                    value = byteRate(current[key], previousVm[key], now - previousVmTime);
                    if (value.isDouble() && i < 2) value = value.toDouble() * double(pageSize);
                }
            }
            result[keys[i]] = value;
        }
        previousVm = current;
        previousVmTime = now;
        return result;
    }

    QJsonArray disks(qint64 now) {
        if (diskRefreshTime >= 0 && now - diskRefreshTime < 60000000000LL) return cachedDisks;
        diskRefreshTime = now;
        cachedDisks = {};
        const QSet<QByteArray> local = {"ext2", "ext3", "ext4", "btrfs", "xfs", "zfs", "f2fs", "vfat", "exfat", "ntfs", "ntfs3", "hfsplus", "ufs", "jfs", "reiserfs", "bcachefs", "overlay"};
        QSet<QString> mounted;
        for (const auto &line : readFile(proc + "/self/mountinfo", 4 * 1024 * 1024).split('\n')) {
            const int separator = line.indexOf(" - ");
            if (separator < 0) continue;
            const auto before = fields(line.left(separator)), after = fields(line.mid(separator + 3));
            if (before.size() < 6 || after.size() < 2 || !local.contains(after[0])) continue;
            const QString path = unescapeMount(QString::fromLocal8Bit(before[4]));
            if (mounted.contains(path)) continue;
            struct statvfs stat {};
            if (::statvfs(QFile::encodeName(path).constData(), &stat) != 0 || !stat.f_blocks) continue;
            const double total = double(stat.f_blocks) * double(stat.f_frsize);
            const double available = double(stat.f_bavail) * double(stat.f_frsize);
            mounted.insert(path);
            cachedDisks.append(QJsonObject{{"device", unescapeMount(QString::fromLocal8Bit(after[1]))},
                {"mountPoint", path}, {"filesystem", QString::fromLatin1(after[0])},
                {"totalBytes", total}, {"availableBytes", available},
                {"usageFraction", qBound(0.0, (total - available) / total, 1.0)}});
        }
        return cachedDisks;
    }

    bool wholeDevice(const QString &name) const {
        if (name.startsWith("loop") || name.startsWith("ram") || name.startsWith("zram") ||
            name.startsWith("dm-") || name.startsWith("md") || name.startsWith("sr") || name.startsWith("fd")) return false;
        const QString classPath = sys + "/class/block/" + name, blockPath = sys + "/block/" + name;
        if (QFileInfo::exists(classPath + "/partition") || QFileInfo::exists(blockPath + "/partition")) return false;
        if (QFileInfo::exists(classPath + "/device") || QFileInfo::exists(blockPath + "/device")) return true;
        // Some container mounts expose diskstats without the corresponding sysfs
        // device links. Restrict that case to unambiguous whole-device names.
        static const QRegularExpression physical(QStringLiteral("^(sd[a-z]+|hd[a-z]+|vd[a-z]+|xvd[a-z]+|nvme[0-9]+n[0-9]+|mmcblk[0-9]+)$"));
        return physical.match(name).hasMatch();
    }

    QJsonObject diskIO(qint64 now) {
        QHash<QString, DiskCounter> current;
        QJsonArray devices;
        double readTotal = 0, writeTotal = 0, busiest = -1, busiestWait = -1;
        QString busiestDevice;
        bool allValid = true;
        for (const auto &line : readFile(proc + "/diskstats").split('\n')) {
            const auto parts = fields(line);
            if (parts.size() < 14) continue;
            const QString device = QString::fromLatin1(parts[2]);
            if (!wholeDevice(device)) continue;
            quint64 readSectors = 0, writeSectors = 0, reads = 0, writes = 0, readMs = 0, writeMs = 0, busyMs = 0, inFlight = 0;
            if (!number(parts[5], readSectors) || !number(parts[9], writeSectors) ||
                readSectors > UINT64_MAX / 512 || writeSectors > UINT64_MAX / 512) continue;
            const bool timing = number(parts[3], reads) && number(parts[6], readMs) && number(parts[7], writes) &&
                                number(parts[10], writeMs) && number(parts[11], inFlight) && number(parts[12], busyMs);
            DiskCounter value{readSectors * 512, writeSectors * 512, reads + writes, readMs + writeMs, busyMs, now};
            current.insert(device, value);
            QJsonValue read = unavailable(), write = unavailable(), utilization = unavailable(), wait = unavailable();
            const auto old = previousDisks.constFind(device);
            if (old != previousDisks.cend()) {
                read = byteRate(value.read, old->read, now - old->time);
                write = byteRate(value.write, old->write, now - old->time);
                if (timing && now > old->time && value.busyMs >= old->busyMs) {
                    // Device latency, unlike throughput, is what a stalled desktop feels.
                    utilization = qBound(0.0, double(value.busyMs - old->busyMs) * 1e6 / double(now - old->time), 1.0);
                    if (value.operations > old->operations && value.waitMs >= old->waitMs)
                        wait = double(value.waitMs - old->waitMs) / double(value.operations - old->operations);
                    else if (value.operations == old->operations) wait = 0.0;
                }
            }
            allValid &= read.isDouble() && write.isDouble();
            if (read.isDouble()) readTotal += read.toDouble();
            if (write.isDouble()) writeTotal += write.toDouble();
            if (utilization.isDouble() && utilization.toDouble() > busiest) {
                busiest = utilization.toDouble(); busiestDevice = device; busiestWait = wait.toDouble(-1);
            }
            devices.append(QJsonObject{{"device", device}, {"readBytesPerSecond", read},
                {"writeBytesPerSecond", write}, {"readBytes", double(value.read)}, {"writeBytes", double(value.write)},
                {"utilization", utilization}, {"awaitMilliseconds", wait}, {"inFlight", timing ? QJsonValue(double(inFlight)) : unavailable()}});
        }
        previousDisks = current;
        const bool measured = !devices.isEmpty() && allValid;
        return {{"readBytesPerSecond", measured ? QJsonValue(readTotal) : unavailable()},
                {"writeBytesPerSecond", measured ? QJsonValue(writeTotal) : unavailable()}, {"devices", devices},
                {"busiestDevice", busiestDevice}, {"busiestUtilization", busiest >= 0 ? QJsonValue(busiest) : unavailable()},
                {"busiestAwaitMilliseconds", busiestWait >= 0 ? QJsonValue(busiestWait) : unavailable()}};
    }

    QJsonObject thermal() const {
        QString hottestZone;
        double hottest = -1;
        const auto zones = QDir(sys + "/class/thermal").entryList({QStringLiteral("thermal_zone*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (int i = 0; i < qMin(32, zones.size()); ++i) {
            const QString path = sys + "/class/thermal/" + zones[i];
            const double value = scalarFile(path + "/temp", 1000.).toDouble(-1);
            // Absent sensors commonly report 0, or a large sentinel; neither is a temperature.
            if (value > 1 && value < 130 && value > hottest) { hottest = value; hottestZone = readText(path + "/type"); }
        }
        QJsonArray clusters;
        double current = 0, maximum = 0;
        bool limited = false;
        const QString cpufreq = sys + "/devices/system/cpu/cpufreq";
        const auto policies = QDir(cpufreq).entryList({QStringLiteral("policy*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (int i = 0; i < qMin(16, policies.size()); ++i) {
            const QString path = cpufreq + '/' + policies[i];
            const double cur = scalarFile(path + "/scaling_cur_freq").toDouble(-1);
            const double top = scalarFile(path + "/cpuinfo_max_freq").toDouble(-1);
            const double limit = scalarFile(path + "/scaling_max_freq").toDouble(-1);
            if (cur <= 0 || top <= 0) continue;
            const int cpus = qMax(1, readText(path + "/affected_cpus").split(' ', QString::SkipEmptyParts).size());
            current += cur * cpus; maximum += top * cpus;
            limited |= limit > 0 && limit < top * .97;
            clusters.append(QJsonObject{{"cpus", cpus}, {"currentMHz", cur / 1000.}, {"maximumMHz", top / 1000.},
                                        {"limitMHz", limit > 0 ? QJsonValue(limit / 1000.) : unavailable()}});
        }
        return {{"hottestCelsius", hottest > 0 ? QJsonValue(hottest) : unavailable()}, {"hottestZone", hottestZone},
                {"frequencyFraction", maximum > 0 ? QJsonValue(current / maximum) : unavailable()},
                {"limited", limited}, {"clusters", clusters}};
    }

    QString ownerName(uint uid) {
        const auto found = ownerNames.constFind(uid);
        if (found != ownerNames.cend()) return found.value();
        struct passwd entry {}, *result = nullptr;
        char buffer[4096];
        const QString name = ::getpwuid_r(uid, &entry, buffer, sizeof(buffer), &result) == 0 && result
            ? QString::fromLocal8Bit(result->pw_name) : QString::number(uid);
        ownerNames.insert(uid, name);
        return name;
    }

    QString defaultInterface(const QSet<QString> &available) const {
        QString selected;
        quint64 metric = UINT64_MAX;
        for (const auto &line : readFile(proc + "/net/route").split('\n')) {
            const auto p = fields(line);
            if (p.size() < 8 || p[1] != "00000000" || p[7] != "00000000") continue;
            bool validFlags = false, validMetric = false;
            const auto flags = p[3].toULongLong(&validFlags, 16), candidateMetric = p[6].toULongLong(&validMetric);
            const QString name = QString::fromLatin1(p[0]);
            if (validFlags && (flags & 1) && validMetric && available.contains(name) && candidateMetric < metric) {
                selected = name; metric = candidateMetric;
            }
        }
        if (!selected.isEmpty()) return selected;
        metric = UINT64_MAX;
        for (const auto &line : readFile(proc + "/net/ipv6_route").split('\n')) {
            const auto p = fields(line);
            if (p.size() < 10 || p[0] != QByteArray(32, '0') || p[1] != "00") continue;
            bool validMetric = false, validFlags = false;
            const quint64 candidateMetric = p[5].toULongLong(&validMetric, 16), flags = p[8].toULongLong(&validFlags, 16);
            const QString name = QString::fromLatin1(p[9]);
            if (validMetric && validFlags && (flags & 1) && available.contains(name) && candidateMetric < metric) {
                selected = name; metric = candidateMetric;
            }
        }
        if (!selected.isEmpty()) return selected;
        QStringList candidates = available.values();
        candidates.sort();
        for (const auto &name : candidates)
            if (name != "lo" && readText(sys + "/class/net/" + name + "/operstate") == "up") return name;
        return {};
    }

    QJsonObject network(qint64 now) {
        QHash<QString, ByteCounter> current;
        QSet<QString> names;
        for (const auto &line : readFile(proc + "/net/dev").split('\n')) {
            const int colon = line.indexOf(':');
            if (colon < 0) continue;
            const QString name = QString::fromLatin1(line.left(colon).trimmed());
            const auto p = fields(line.mid(colon + 1));
            ByteCounter value; value.time = now;
            if (p.size() < 16 || !number(p[0], value.read) || !number(p[8], value.write)) continue;
            current.insert(name, value); names.insert(name);
        }
        const QString name = defaultInterface(names);
        QJsonObject result{{"interface", name}, {"downloadBytesPerSecond", unavailable()},
            {"uploadBytesPerSecond", unavailable()}, {"rxBytes", unavailable()}, {"txBytes", unavailable()},
            {"ipv4", QJsonArray()}, {"ipv6", QJsonArray()}, {"isUp", unavailable()}};
        if (!name.isEmpty() && current.contains(name)) {
            const auto value = current[name];
            result["rxBytes"] = double(value.read); result["txBytes"] = double(value.write);
            const auto old = previousNetworks.constFind(name);
            if (old != previousNetworks.cend()) {
                result["downloadBytesPerSecond"] = byteRate(value.read, old->read, now - old->time);
                result["uploadBytesPerSecond"] = byteRate(value.write, old->write, now - old->time);
            }
            const QString state = readText(sys + "/class/net/" + name + "/operstate");
            if (!state.isEmpty()) result["isUp"] = state == "up";
            QJsonArray ipv4, ipv6;
            // getifaddrs is host-global; fixtures must never consult it.
            if (nativeRoots) {
                struct ifaddrs *addresses = nullptr;
                if (::getifaddrs(&addresses) == 0) {
                    QSet<QString> seen4, seen6;
                    for (auto *address = addresses; address; address = address->ifa_next) {
                        if (!address->ifa_addr || QString::fromLocal8Bit(address->ifa_name) != name) continue;
                        result["isUp"] = bool(address->ifa_flags & IFF_UP);
                        char text[INET6_ADDRSTRLEN] = {};
                        const int family = address->ifa_addr->sa_family;
                        if (family == AF_INET && ::inet_ntop(family, &reinterpret_cast<sockaddr_in *>(address->ifa_addr)->sin_addr, text, sizeof(text))) seen4.insert(QString::fromLatin1(text));
                        if (family == AF_INET6 && ::inet_ntop(family, &reinterpret_cast<sockaddr_in6 *>(address->ifa_addr)->sin6_addr, text, sizeof(text))) seen6.insert(QString::fromLatin1(text));
                    }
                    ::freeifaddrs(addresses);
                    QStringList v4 = seen4.values(), v6 = seen6.values(); v4.sort(); v6.sort();
                    for (const auto &address : v4) ipv4.append(address);
                    for (const auto &address : v6) ipv6.append(address);
                }
            } else {
                for (const auto &line : readFile(proc + "/net/if_inet6").split('\n')) {
                    const auto p = fields(line);
                    if (p.size() < 6 || QString::fromLatin1(p[5]) != name || p[0].size() != 32) continue;
                    const auto bytes = QByteArray::fromHex(p[0]);
                    char text[INET6_ADDRSTRLEN] = {};
                    if (bytes.size() == 16 && ::inet_ntop(AF_INET6, bytes.constData(), text, sizeof(text))) ipv6.append(QString::fromLatin1(text));
                }
            }
            result["ipv4"] = ipv4; result["ipv6"] = ipv6;
        }
        previousNetworks = current;
        return result;
    }

    QJsonObject battery() const {
        QJsonObject result{{"isPresent", unavailable()}, {"level", unavailable()}, {"state", "unknown"},
            {"healthPercent", unavailable()}, {"cycleCount", unavailable()}, {"powerSource", "unknown"}};
        const QString power = sys + "/class/power_supply";
        if (!QFileInfo(power).isDir() || !QFileInfo(power).isReadable()) return result;
        bool sawBattery = false, present = false, mainsKnown = false, online = false, unreadableType = false;
        double levelSum = 0, healthSum = 0, cycleSum = 0;
        int levels = 0, healths = 0, cycles = 0;
        double energyNow = 0, energyFull = 0, chargeNow = 0, chargeFull = 0;
        int energyCount = 0, chargeCount = 0;
        bool charging = false, discharging = false, full = false;
        for (const auto &name : QDir(power).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const QString path = power + '/' + name, type = readText(path + "/type");
            if (type.isEmpty()) { unreadableType = true; continue; }
            if (type != "Battery") {
                const auto value = scalarFile(path + "/online");
                if (value.isDouble()) { mainsKnown = true; online |= value.toDouble() > 0; }
                continue;
            }
            sawBattery = true;
            const auto installed = scalarFile(path + "/present");
            if (installed.isDouble() && installed.toDouble() == 0) continue;
            present = true;
            const QString status = readText(path + "/status").toLower();
            charging |= status == "charging"; discharging |= status == "discharging"; full |= status == "full";
            const auto capacity = scalarFile(path + "/capacity");
            if (capacity.isDouble()) { levelSum += qBound(0.0, capacity.toDouble() / 100.0, 1.0); ++levels; }
            const auto enow = scalarFile(path + "/energy_now"), efull = scalarFile(path + "/energy_full");
            const auto cnow = scalarFile(path + "/charge_now"), cfull = scalarFile(path + "/charge_full");
            const auto edesign = scalarFile(path + "/energy_full_design"), cdesign = scalarFile(path + "/charge_full_design");
            if (enow.isDouble() && efull.toDouble() > 0) { energyNow += enow.toDouble(); energyFull += efull.toDouble(); ++energyCount; }
            else if (cnow.isDouble() && cfull.toDouble() > 0) { chargeNow += cnow.toDouble(); chargeFull += cfull.toDouble(); ++chargeCount; }
            double health = -1;
            if (efull.isDouble() && edesign.toDouble() > 0) health = efull.toDouble() / edesign.toDouble() * 100;
            else if (cfull.isDouble() && cdesign.toDouble() > 0) health = cfull.toDouble() / cdesign.toDouble() * 100;
            if (health >= 0) { healthSum += health; ++healths; }
            const auto cycle = scalarFile(path + "/cycle_count");
            if (cycle.isDouble()) { cycleSum += cycle.toDouble(); ++cycles; }
        }
        if (!present && unreadableType) return result;
        result["isPresent"] = present;
        if (!present) {
            result["state"] = "noBattery";
            result["powerSource"] = online || !sawBattery ? "ac" : "unknown";
            return result;
        }
        // Never add microamp-hours to microwatt-hours. Mixed battery types use
        // the mean of per-battery percentages where capacity is available.
        if (levels > 0) result["level"] = levelSum / levels;
        if (energyCount > 0 && chargeCount == 0) result["level"] = qBound(0.0, energyNow / energyFull, 1.0);
        if (chargeCount > 0 && energyCount == 0) result["level"] = qBound(0.0, chargeNow / chargeFull, 1.0);
        if (energyCount > 0 && chargeCount > 0 && !levels)
            result["level"] = qBound(0.0, (energyNow / energyFull + chargeNow / chargeFull) / 2, 1.0);
        if (healths) result["healthPercent"] = healthSum / healths;
        if (cycles) result["cycleCount"] = cycleSum / cycles;
        result["state"] = charging ? "charging" : discharging ? "discharging" : full ? "full" : online ? "pluggedIn" : "unknown";
        result["powerSource"] = online || charging || full ? "ac" : discharging ? "battery" : mainsKnown && !online ? "battery" : "unknown";
        return result;
    }

    QString explicitProject(const QString &cwd, const QByteArray &command) const {
        QStringList paths{cwd};
        for (const auto &arg : command.split('\0')) {
            QString path = QString::fromLocal8Bit(arg);
            const int equals = path.indexOf('=');
            if (equals >= 0) path = path.mid(equals + 1);
            if (QDir::isAbsolutePath(path)) paths.append(QDir::cleanPath(path));
        }
        for (const auto &root : projectRoots)
            for (const auto &path : paths) if (inside(path, root)) return root;
        return {};
    }
    QString discoveredProject(const QString &cwd, QHash<QString, QString> &cache) const {
        if (cwd.isEmpty()) return {};
        const auto found = cache.constFind(cwd);
        if (found != cache.cend()) return found.value();
        QString path = cwd, result;
        const QStringList markers{".git", "package.json", "CMakeLists.txt", "pyproject.toml", "Cargo.toml", "go.mod", "build.gradle", "build.gradle.kts", "pom.xml", "Package.swift"};
        for (int depth = 0; depth < 12 && path != "/"; ++depth) {
            for (const auto &marker : markers) {
                // Exact marker probes only; no recursive directory traversal.
                if (QFileInfo::exists(path + '/' + marker)) { result = path; break; }
            }
            if (!result.isEmpty()) break;
            const QString parent = QFileInfo(path).dir().absolutePath();
            if (parent == path) break;
            path = parent;
        }
        cache.insert(cwd, result);
        return result;
    }

    double processes(qint64 now) {
        QElapsedTimer duration; duration.start();
        QHash<qint64, Process> current;
        QHash<qint64, ProcessCounters> counters;
        // Read every proc symlink afresh; only share canonicalization within
        // this scan so executable replacements and cwd changes stay visible.
        QHash<QString, QString> projectCache, paths;
        struct Other { Process process; uint uid; };
        QVector<Other> others;
        int running = 0, blocked = 0, zombie = 0;
        double attributed = 0;
        bool anyCpu = false;
        auto cpuSince = [&](const Process &process, const ProcessCounters *old) -> QJsonValue {
            if (previousProcessTime <= 0 || now <= previousProcessTime || !old || old->start != process.start || process.ticks < old->ticks)
                return unavailable();
            return double(process.ticks - old->ticks) * 1e11 / (double(ticksPerSecond) * double(now - previousProcessTime));
        };
        // Share of the interval the process spent waiting for disk, 0-100.
        auto waitSince = [&](const Process &process, const ProcessCounters *old) -> QJsonValue {
            if (previousProcessTime <= 0 || now <= previousProcessTime || !old || old->start != process.start
                || !process.hasBlkio || !old->hasBlkio || process.blkio < old->blkio) return unavailable();
            return qBound(0.0, double(process.blkio - old->blkio) * 1e11 / (double(ticksPerSecond) * double(now - previousProcessTime)), 100.0);
        };
        // Kernels without delay accounting (off by default since 5.14) report zero for
        // every process; a zero there means "not measured", never "no waiting".
        bool anyBlkio = false;
        auto tally = [&](const Process &process) {
            anyBlkio |= process.hasBlkio && process.blkio > 0;
            if (process.state == "R") ++running; else if (process.state == "D") ++blocked; else if (process.state == "Z") ++zombie;
            if (process.cpu.isDouble()) { attributed += process.cpu.toDouble(); anyCpu = true; }
        };
        const auto pids = QDir(proc).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const auto &entry : pids) {
            bool numeric = false;
            const qint64 pid = entry.toLongLong(&numeric);
            if (!numeric || pid <= 0) continue;
            const QString path = proc + '/' + entry;
            const QByteArray encodedPath = QFile::encodeName(path);
            struct stat owner {};
            if (::stat(encodedPath.constData(), &owner)) continue;
            if (owner.st_uid != ::getuid()) {
                // /proc/PID/stat is world readable, so root daemons and kernel threads
                // (Xorg, kswapd0, jbd2, security agents) still appear as stall suspects.
                // Only the counters are read; their cwd, exe and argv stay private.
                Other other{{}, uint(owner.st_uid)};
                if (!processStat(readSmall(encodedPath + "/stat"), other.process) || other.process.pid != pid) continue;
                const auto old = previousProcesses.constFind(pid);
                other.process.cpu = cpuSince(other.process, old == previousProcesses.cend() ? nullptr : &old.value());
                other.process.ioWait = waitSince(other.process, old == previousProcesses.cend() ? nullptr : &old.value());
                counters.insert(pid, {other.process.start, other.process.ticks, 0, 0, 0, other.process.rss, false, false, false,
                                      other.process.blkio, other.process.hasBlkio});
                tally(other.process);
                others.append(other);
                continue;
            }
            Process process;
            if (!processStat(readFile(path + "/stat", 16384), process) || process.pid != pid) continue;
            process.cwd = linkTarget(path + "/cwd", paths);
            process.executable = linkTarget(path + "/exe", paths);
            const QByteArray command = projectRoots.isEmpty() ? QByteArray() : readFile(path + "/cmdline", 65536);
            for (const auto &line : readFile(path + "/io", 4096).split('\n')) {
                const auto parts = fields(line);
                if (parts.size() != 2) continue;
                if (parts[0] == "read_bytes:") process.hasRead = number(parts[1], process.ioRead);
                if (parts[0] == "write_bytes:") process.hasWrite = number(parts[1], process.ioWrite);
            }
            Process checked;
            if (!processStat(readFile(path + "/stat", 16384), checked) || checked.start != process.start || checked.pid != pid) continue;
            const auto old = previousProcesses.constFind(pid);
            process.cpu = cpuSince(process, old == previousProcesses.cend() ? nullptr : &old.value());
            process.ioWait = waitSince(process, old == previousProcesses.cend() ? nullptr : &old.value());
            if (process.state == "D") process.waitChannel = readText(path + "/wchan");
            if (previousProcessTime > 0 && now > previousProcessTime && old != previousProcesses.cend() && old->start == process.start)
                process.memoryGrowth = (double(process.rss) - double(old->rss)) * double(pageSize) * 1e9 / double(now - previousProcessTime);
            if (previousProcessTime > 0 && old != previousProcesses.cend() && old->start == process.start) {
                if (process.hasMajorFaults && old->hasMajorFaults)
                    process.majorFaultRate = byteRate(process.majorFaults, old->majorFaults, now - previousProcessTime);
                if (process.hasRead && old->hasRead) process.readRate = byteRate(process.ioRead, old->ioRead, now - previousProcessTime);
                if (process.hasWrite && old->hasWrite) process.writeRate = byteRate(process.ioWrite, old->ioWrite, now - previousProcessTime);
            }
            process.project = projectRoots.isEmpty() ? QString() : explicitProject(process.cwd, command);
            if (process.project.isEmpty()) process.project = discoveredProject(process.cwd, projectCache);
            counters.insert(pid, {process.start, process.ticks, process.majorFaults, process.ioRead, process.ioWrite, process.rss,
                                  process.hasMajorFaults, process.hasRead, process.hasWrite, process.blkio, process.hasBlkio});
            tally(process);
            current.insert(pid, process);
        }
        // Bound inherited chains independently of process-tree depth to avoid a
        // malicious proc fixture (or a very deep tree) exhausting the C++ stack.
        for (auto it = current.begin(); it != current.end(); ++it) {
            qint64 ancestor = it.key();
            QSet<qint64> seen;
            QVector<qint64> chain;
            QString project;
            while (current.contains(ancestor) && !seen.contains(ancestor) && chain.size() < 256) {
                seen.insert(ancestor); chain.append(ancestor);
                const auto &node = current[ancestor];
                if (!node.project.isEmpty()) { project = node.project; break; }
                ancestor = node.parent;
            }
            if (!project.isEmpty()) for (qint64 member : chain) current[member].project = project;
        }
        if (!anyBlkio) {
            for (auto &process : current) process.ioWait = unavailable();
            for (auto &other : others) other.process.ioWait = unavailable();
        }
        auto ordered = current.values();
        std::sort(ordered.begin(), ordered.end(), [](const Process &a, const Process &b) {
            if (a.cpu.toDouble(-1) != b.cpu.toDouble(-1)) return a.cpu.toDouble(-1) > b.cpu.toDouble(-1);
            if (a.rss != b.rss) return a.rss > b.rss;
            return a.pid < b.pid;
        });
        cachedProcesses = {}; cachedGroups = {};
        QMap<QString, QJsonArray> grouped;
        for (const auto &process : ordered) {
            const auto json = processJson(process, pageSize);
            cachedProcesses.append(json);
            if (!process.project.isEmpty()) grouped[process.project].append(json);
        }
        for (auto it = grouped.cbegin(); it != grouped.cend(); ++it) {
            double memory = 0, cpu = 0;
            bool allMeasured = true;
            for (const auto &value : it.value()) {
                const auto object = value.toObject();
                memory += object["memoryBytes"].toDouble();
                allMeasured &= object["cpuPercent"].isDouble();
                cpu += object["cpuPercent"].toDouble();
            }
            cachedGroups.append(QJsonObject{{"path", it.key()}, {"name", QFileInfo(it.key()).fileName()},
                {"cpuPercent", allMeasured ? QJsonValue(cpu) : unavailable()}, {"memoryBytes", memory}, {"processes", it.value()}});
        }
        cachedProcessCount = current.size();
        // Keep the bounded set a stall diagnosis can use: busiest by CPU and memory,
        // plus everything blocked on I/O. The rest of the other-user scan is dropped.
        QSet<qint64> kept;
        cachedSystemProcesses = {};
        auto keep = [&](const Other &other) {
            if (kept.contains(other.process.pid)) return;
            kept.insert(other.process.pid);
            auto json = processJson(other.process, pageSize);
            json["owner"] = ownerName(other.uid);
            json["kernelThread"] = bool(other.process.flags & KernelThreadFlag);
            cachedSystemProcesses.append(json);
        };
        std::sort(others.begin(), others.end(), [](const Other &a, const Other &b) {
            return a.process.cpu.toDouble(-1) > b.process.cpu.toDouble(-1);
        });
        for (int i = 0; i < qMin(16, others.size()); ++i) keep(others[i]);
        int waiting = 0;
        for (const auto &other : others) if (other.process.state == "D" && waiting++ < 16) keep(other);
        std::sort(others.begin(), others.end(), [](const Other &a, const Other &b) { return a.process.rss > b.process.rss; });
        for (int i = 0; i < qMin(8, others.size()); ++i) keep(others[i]);
        // Their read and write volume needs root; waiting for disk is what can be seen.
        std::sort(others.begin(), others.end(), [](const Other &a, const Other &b) {
            return a.process.ioWait.toDouble(-1) > b.process.ioWait.toDouble(-1);
        });
        for (int i = 0; i < qMin(8, others.size()) && others[i].process.ioWait.toDouble(-1) >= 1; ++i) keep(others[i]);
        cachedProcessStates = {{"total", cachedProcessCount + others.size()}, {"running", running}, {"blocked", blocked}, {"zombie", zombie}};
        cachedAttributedCpu = anyCpu ? QJsonValue(attributed) : unavailable();
        cachedThermal = thermal();
        previousProcesses = std::move(counters); previousProcessTime = now;
        return double(duration.nsecsElapsed()) / 1e6;
    }
};

LinuxSampler::LinuxSampler(const QString &procRoot, const QString &sysRoot)
    : d(new Impl(procRoot, sysRoot)) {}
LinuxSampler::~LinuxSampler() = default;
void LinuxSampler::setProjectRoots(const QStringList &roots) {
    d->projectRoots.clear();
    for (const auto &path : roots) {
        const QString normalized = normalizedPath(path);
        if (!normalized.isEmpty() && !d->projectRoots.contains(normalized)) d->projectRoots.append(normalized);
    }
    std::sort(d->projectRoots.begin(), d->projectRoots.end(), [](const QString &a, const QString &b) {
        return a.size() == b.size() ? a < b : a.size() > b.size();
    });
}
QJsonObject LinuxSampler::sample(bool scanProcesses) {
    const qint64 now = d->clock.nsecsElapsed();
    const double scanTime = scanProcesses ? d->processes(now) : 0;
    return {{"timestamp", double(QDateTime::currentMSecsSinceEpoch()) / 1000.0}, {"cpu", d->cpu(now)},
        {"memory", d->memory()}, {"pressure", d->pressure()}, {"vm", d->vm(now)},
        {"disks", d->disks(now)}, {"diskIO", d->diskIO(now)},
        {"monotonicSeconds", double(now) / 1e9}, {"processesFresh", scanProcesses},
        {"processMonotonicSeconds", double(d->previousProcessTime) / 1e9},
        {"network", d->network(now)}, {"battery", d->battery()}, {"processes", d->cachedProcesses},
        {"groups", d->cachedGroups}, {"processCount", d->cachedProcessCount}, {"scanMilliseconds", scanTime},
        {"systemProcesses", d->cachedSystemProcesses}, {"processStates", d->cachedProcessStates},
        {"attributedCpuPercent", d->cachedAttributedCpu}, {"thermal", d->cachedThermal}};
}
