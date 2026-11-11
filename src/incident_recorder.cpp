#include "incident_recorder.h"
#include "i18n.h"
#include "diagnostics.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMap>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>
#include <QVariant>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <deque>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 Retention = 31LL * 86400;
constexpr int PayloadLimit = 128 * 1024;
QJsonValue unknown() { return QJsonValue(QJsonValue::Null); }
bool measured(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0; }
double number(const QJsonObject &o, const char *key) { const auto v = o[QLatin1String(key)]; return measured(v) ? v.toDouble() : -1; }
QJsonObject fields(const QJsonObject &source, const QStringList &keys) {
    QJsonObject result;
    for (const auto &key : keys) result[key] = measured(source[key]) ? source[key] : unknown();
    return result;
}
QString safeName(QString name) {
    // Display names cannot carry a path, control sequence or unbounded metadata.
    name.replace('\\', '/'); name = name.section('/', -1);
    QString clean;
    for (const QChar c : name) if (c.isPrint() && c != '<' && c != '>') clean += c;
    return clean.left(64);
}
QString digest(const QString &identity) {
    return "sha256:" + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QJsonArray coverage() {
    return {T("进程排行是卡顿期间最忙的程序，相关性很强，但不等于证明。"),
        T("进程 CPU 以单核为 100%，可超过 100%；内存为 RSS，含共享页。"),
        T("每条记录保留触发前和卡顿期间的关键采样；只存程序名和 PID，不存命令行或文件路径。")};
}
bool secureDirectory(const QString &path, QString &error) {
    const auto raw = QFile::encodeName(path); struct stat st {};
    if (lstat(raw.constData(), &st) != 0 &&
        (errno != ENOENT || !QDir().mkpath(path) || lstat(raw.constData(), &st) != 0)) {
        error = T("无法创建卡顿记录目录：%1").arg(path); return false;
    }
    if (!S_ISDIR(st.st_mode) || st.st_uid != getuid() || chmod(raw.constData(), 0700) != 0) {
        error = T("卡顿记录目录必须由当前用户拥有，且不能是符号链接：%1").arg(path); return false;
    }
    return true;
}
bool secureFile(const QString &path, bool create, QString &error) {
    const auto raw = QFile::encodeName(path);
    const int fd = ::open(raw.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
    if (fd < 0) {
        if (!create && errno == ENOENT) return true;
        error = T("无法打开私有卡顿记录：%1 (%2)").arg(path, QString::fromLocal8Bit(strerror(errno))); return false;
    }
    struct stat st {};
    const bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == getuid() && st.st_nlink == 1 && fchmod(fd, 0600) == 0;
    ::close(fd);
    if (!ok) error = T("卡顿记录必须是当前用户拥有的普通文件：%1").arg(path);
    return ok;
}
// Keep only the bounded ranking candidates in native values. Building JSON for
// every process, then copying and sorting it six times, dominated idle scans.
struct EvidenceRow {
    QString identity, name, owner;
    double pid = -1, start = -1;
    double metrics[4] = {-1, -1, -1, -1};
    int processCount = 0;
    bool partial = false;
    double metric(int index) const {
        if (index < 4) return metrics[index];
        return metrics[2] >= 0 || metrics[3] >= 0 ? std::max(0.0, metrics[2]) + std::max(0.0, metrics[3]) : -1;
    }
    QJsonObject json(bool process) const {
        QJsonObject result{{"identity", identity}, {"name", name}, {"partial", partial}};
        if (!owner.isEmpty()) result["owner"] = owner;
        const char *keys[] = {"cpuPercent", "memoryBytes", "ioReadBytesPerSecond", "ioWriteBytesPerSecond", "ioBytesPerSecond"};
        for (int i = 0; i < 5; ++i) result[keys[i]] = metric(i) >= 0 && std::isfinite(metric(i)) ? QJsonValue(metric(i)) : unknown();
        if (process) { result["pid"] = pid; result["startTicks"] = start >= 0 ? QJsonValue(start) : unknown(); }
        else result["processCount"] = processCount;
        return result;
    }
};
void retainRanked(QVector<EvidenceRow> &rows, const EvidenceRow &row, int metric, int limit) {
    if (row.metric(metric) < 0 || !std::isfinite(row.metric(metric))) return;
    const auto before = [metric](const EvidenceRow &a, const EvidenceRow &b) {
        if (a.metric(metric) != b.metric(metric)) return a.metric(metric) > b.metric(metric);
        return a.identity == b.identity ? a.pid < b.pid : a.identity < b.identity;
    };
    const auto position = std::lower_bound(rows.begin(), rows.end(), row, before);
    if (position == rows.end() && rows.size() >= limit) return;
    rows.insert(position, row);
    if (rows.size() > limit) rows.removeLast();
}
QJsonArray rankedJson(const QVector<EvidenceRow> &rows) {
    QJsonArray result; for (const auto &row : rows) result.append(row.json(false)); return result;
}
QJsonObject processEvidence(const QJsonObject &snapshot) {
    QMap<QString, QString> appNames;
    for (const auto &value : snapshot["activityApplications"].toArray()) {
        const auto app = value.toObject(); appNames[app["identity"].toString()] = safeName(app["name"].toString());
    }
    struct Aggregate { EvidenceRow row; int known[4] = {}; };
    QMap<QString, Aggregate> aggregates;
    QVector<EvidenceRow> processRanks[3];
    for (auto &rows : processRanks) rows.reserve(11);
    const char *metrics[] = {"cpuPercent", "memoryBytes", "ioReadBytesPerSecond", "ioWriteBytesPerSecond"};
    const int rankMetrics[] = {0, 1, 4};
    const auto processes = snapshot["processes"].toArray();
    bool partial = processes.isEmpty();
    for (const auto &value : processes) {
        const auto source = value.toObject();
        const double pid = number(source, "pid");
        if (pid <= 0) { partial = true; continue; }
        QString key = source[QLatin1String("applicationIdentity")].toString();
        if (key.isEmpty()) key = QStringLiteral("pid:%1:%2").arg(pid, 0, 'f', 0).arg(source[QLatin1String("startTicks")].toDouble(), 0, 'f', 0);
        // DevStats stays in the process list but never becomes the incident's named
        // application: its window and diagnosis cost CPU while someone investigates.
        const bool self = source[QLatin1String("self")].toBool();
        auto app = self ? aggregates.end() : aggregates.find(key);
        if (!self && app == aggregates.end()) {
            Aggregate aggregate; aggregate.row.identity = digest(key);
            app = aggregates.insert(key, aggregate);
        }
        EvidenceRow process; process.pid = pid; process.start = number(source, "startTicks");
        process.identity = self ? digest(key) : app->row.identity; process.name = safeName(source[QLatin1String("name")].toString());
        if (!self) { app->row.name = appNames.value(key, process.name); ++app->row.processCount; }
        for (int m = 0; m < 4; ++m) {
            process.metrics[m] = number(source, metrics[m]);
            if (process.metrics[m] >= 0) {
                if (self) continue;
                if (!app->known[m]) app->row.metrics[m] = 0;
                app->row.metrics[m] += process.metrics[m]; ++app->known[m];
            } else process.partial = true;
        }
        partial |= process.partial;
        for (int i = 0; i < 3; ++i) retainRanked(processRanks[i], process, rankMetrics[i], 10);
    }
    // Root daemons and kernel threads compete in the same rankings. Their stat line
    // carries no I/O counters, so "unknown" there does not make the sample partial.
    for (const auto &value : snapshot["systemProcesses"].toArray()) {
        const auto source = value.toObject();
        const double pid = number(source, "pid");
        if (pid <= 0) continue;
        const QString name = safeName(source[QLatin1String("name")].toString());
        const QString key = QStringLiteral("system:") + name.section('/', 0, 0);
        auto app = aggregates.find(key);
        if (app == aggregates.end()) {
            Aggregate aggregate; aggregate.row.identity = digest(key);
            aggregate.row.owner = safeName(source[QLatin1String("owner")].toString());
            aggregate.row.name = name.section('/', 0, 0);
            app = aggregates.insert(key, aggregate);
        }
        EvidenceRow process; process.pid = pid; process.start = number(source, "startTicks");
        process.identity = app->row.identity; process.name = name; process.owner = app->row.owner;
        ++app->row.processCount;
        for (int m = 0; m < 2; ++m) {
            process.metrics[m] = number(source, metrics[m]);
            if (process.metrics[m] >= 0) {
                if (!app->known[m]) app->row.metrics[m] = 0;
                app->row.metrics[m] += process.metrics[m]; ++app->known[m];
            }
        }
        // I/O is unknowable for other users; mark those columns as fully "known" absent.
        app->known[2] = app->known[3] = app->row.processCount;
        for (int i = 0; i < 2; ++i) retainRanked(processRanks[i], process, rankMetrics[i], 10);
    }
    QVector<EvidenceRow> appRanks[3];
    for (auto &rows : appRanks) rows.reserve(6);
    for (auto app = aggregates.begin(); app != aggregates.end(); ++app) {
        for (int m = 0; m < 4; ++m) app->row.partial |= app->known[m] != app->row.processCount;
        for (int i = 0; i < 3; ++i) retainRanked(appRanks[i], app->row, rankMetrics[i], 5);
    }
    // Interleave the three rankings so a CPU-idle I/O writer remains visible.
    QJsonArray topProcesses; QSet<QString> seen;
    for (int i = 0; i < 10 && topProcesses.size() < 10; ++i) for (const auto &rows : processRanks) {
        if (i >= rows.size() || topProcesses.size() >= 10) continue;
        const auto &row = rows[i];
        const QString key = QString::number(row.pid, 'f', 0) + ':' + QString::number(std::max(0.0, row.start), 'f', 0);
        if (!seen.contains(key)) { seen.insert(key); topProcesses.append(row.json(true)); }
    }
    return {{"topApplications", QJsonObject{{"cpu", rankedJson(appRanks[0])}, {"memory", rankedJson(appRanks[1])}, {"io", rankedJson(appRanks[2])}}},
        {"topProcesses", topProcesses}, {"partial", partial}};
}
QJsonObject compactSample(const QJsonObject &snapshot, const QJsonArray &findings, const QJsonObject &processData) {
    QJsonObject result{{"timestamp", snapshot["timestamp"]}, {"monotonicSeconds", snapshot["monotonicSeconds"]}, {"findings", findings}};
    auto cpu = fields(snapshot["cpu"].toObject(), {"usage", "iowait", "irq", "coreCount", "runningProcesses", "blockedProcesses", "runQueueWait", "forksPerSecond"});
    QJsonArray load; const auto sourceLoad = snapshot["cpu"].toObject()["load"].toArray();
    for (int i = 0; i < 3; ++i) load.append(i < sourceLoad.size() && measured(sourceLoad[i]) ? sourceLoad[i] : unknown());
    cpu["load"] = load; result["cpu"] = cpu;
    result["memory"] = fields(snapshot["memory"].toObject(), {"totalBytes", "availableBytes", "fraction"});
    result["vm"] = fields(snapshot["vm"].toObject(), {"swapInBytesPerSecond", "swapOutBytesPerSecond", "majorFaultsPerSecond", "oomKillDelta", "allocStallsPerSecond", "directReclaimPagesPerSecond"});
    result["responsiveness"] = fields(snapshot["responsiveness"].toObject(), {"schedulerMaxMs", "schedulerP95Ms", "displayMaxMs"});
    result["thermal"] = fields(snapshot["thermal"].toObject(), {"hottestCelsius", "frequencyFraction"});
    auto diskLoad = fields(snapshot["diskIO"].toObject(), {"busiestUtilization", "busiestAwaitMilliseconds"});
    diskLoad["device"] = safeName(snapshot["diskIO"].toObject()["busiestDevice"].toString());
    result["disk"] = diskLoad;
    QJsonObject pressure;
    for (const auto &resource : {"cpu", "memory", "io"}) {
        QJsonObject item;
        for (const auto &type : {"some", "full"}) {
            if (QByteArray(resource) == "cpu" && QByteArray(type) == "full") continue;
            item[type] = fields(snapshot["pressure"].toObject()[resource].toObject()[type].toObject(), {"avg10", "avg60", "avg300"});
        }
        pressure[resource] = item;
    }
    result["pressure"] = pressure;
    for (auto it = processData.begin(); it != processData.end(); ++it) result[it.key()] = it.value();
    const bool partial = processData["partial"].toBool(); result.remove("partial");
    const double now = number(snapshot, "monotonicSeconds"), scan = number(snapshot, "processMonotonicSeconds");
    const bool ageKnown = now >= 0 && scan >= 0 && scan <= now;
    const bool fresh = snapshot["processesFresh"].toBool() && ageKnown && now - scan <= 6;
    QJsonArray notes;
    if (!fresh) notes.append(T("这次采样的进程列表是缓存的，排行可能滞后。"));
    result["processCoverage"] = QJsonObject{{"scope", "all-users"}, {"fresh", fresh}, {"ageSeconds", ageKnown ? QJsonValue(now - scan) : unknown()}, {"partial", partial}, {"notes", notes}};
    return result;
}
QJsonArray relevantFindings(const QJsonObject &diagnosis) {
    QJsonArray result;
    for (const auto &value : diagnosis["findings"].toArray()) {
        const auto finding = value.toObject(); const auto kind = finding["kind"].toString();
        if (kind == "cpu" || kind == "memory" || kind == "io" || kind == "oom" || kind == "responsiveness" || kind == "display") result.append(finding);
    }
    return result;
}
bool relevantKnown(const QJsonObject &snapshot) {
    // Recovery needs live CPU and memory figures. PSI is optional: many kernels
    // are built without it, and a missing average must not end an event as "unknown".
    const auto cpu = snapshot["cpu"].toObject(), memory = snapshot["memory"].toObject();
    return number(cpu, "usage") >= 0 && number(cpu, "coreCount") > 0 && number(memory, "totalBytes") > 0 &&
           number(memory, "availableBytes") >= 0 && number(memory, "availableBytes") <= number(memory, "totalBytes");
}
QJsonObject summary(QJsonObject payload) {
    for (const char *key : {"schemaVersion", "prelude", "witnesses", "coverage"}) payload.remove(key);
    return payload;
}
struct EncodedPayload { QJsonObject object; QByteArray bytes; };
EncodedPayload boundPayload(QJsonObject payload) {
    auto encode = [&] { return QJsonDocument(payload).toJson(QJsonDocument::Compact); };
    auto bytes = encode();
    if (bytes.size() <= PayloadLimit) return {payload, bytes};
    auto limits = payload["coverage"].toArray();
    limits.append(T("该事件接近 128 KiB 存储上限；已先缩减最旧前置快照，再按需缩短资源排行。关键见证与未知/部分数据标记仍保留。"));
    payload["coverage"] = limits;
    int removed = 0, processLimit = 10, appLimit = 5;
    auto mark = [&] {
        payload["payloadCompaction"] = QJsonObject{{"preludeSamplesRemoved", removed}, {"processRankingLimit", processLimit}, {"applicationRankingLimit", appLimit}};
    };
    mark();
    auto prelude = payload["prelude"].toArray();
    bytes = encode();
    while (bytes.size() > PayloadLimit && !prelude.isEmpty()) {
        prelude.removeFirst(); ++removed; payload["prelude"] = prelude; mark(); bytes = encode();
    }
    while (bytes.size() > PayloadLimit && (processLimit > 1 || appLimit > 1)) {
        processLimit = std::max(1, processLimit - 2); appLimit = std::max(1, appLimit - 1);
        for (const auto &key : {"prelude", "witnesses"}) {
            QJsonArray samples;
            for (const auto &value : payload[key].toArray()) {
                auto sample = value.toObject(); auto processes = sample["topProcesses"].toArray();
                while (processes.size() > processLimit) processes.removeLast();
                sample["topProcesses"] = processes;
                auto apps = sample["topApplications"].toObject();
                for (const auto &resource : {"cpu", "memory", "io"}) {
                    auto rows = apps[resource].toArray(); while (rows.size() > appLimit) rows.removeLast(); apps[resource] = rows;
                }
                sample["topApplications"] = apps; samples.append(sample);
            }
            payload[key] = samples;
        }
        mark(); bytes = encode();
    }
    return {payload, bytes};
}
double score(const QJsonObject &sample, const QString &kind) {
    if (kind == "CPU") return number(sample["cpu"].toObject(), "usage");
    if (kind == "Latency") return std::max(number(sample["responsiveness"].toObject(), "schedulerMaxMs"), number(sample["responsiveness"].toObject(), "displayMaxMs"));
    const auto pressure = sample["pressure"].toObject()[kind == "MemoryPressure" ? "memory" : "io"].toObject();
    double result = std::max(number(pressure["some"].toObject(), "avg10"), number(pressure["full"].toObject(), "avg10"));
    if (kind == "IO") result = std::max(result, number(sample["cpu"].toObject(), "iowait") * 100);
    if (kind == "MemoryPressure") {
        const auto memory = sample["memory"].toObject();
        const double total = number(memory, "totalBytes"), available = number(memory, "availableBytes");
        if (total > 0 && available >= 0 && available <= total) result = std::max(result, (1 - available / total) * 100);
        const auto vm = sample["vm"].toObject();
        const double swap = std::max(0.0, number(vm, "swapInBytesPerSecond")) + std::max(0.0, number(vm, "swapOutBytesPerSecond"));
        if (result >= 0) result += std::log1p(swap / 1048576.0) * .001;
    }
    return result;
}
}

struct IncidentRecorder::Impl {
    explicit Impl(const QString &root) : root(root) {}
    QString root, path, connection, error, persistError;
    QSqlDatabase database;
    bool enabled = true, dirty = false;
    QJsonObject active, cooldown;
    std::deque<QJsonObject> ring;
    QJsonObject suspect, processCache;
    QJsonArray suspectPrelude;
    double processStamp = -2;
    QMap<QString, QJsonObject> witnessSlots;
    double lastMono = -1, highStart = -1, lastWrite = -1, cooldownUntil = -1, cooldownHighStart = -1;
    int normalCount = 0;
    bool sidecars() {
        for (const QString &suffix : {QString(), QStringLiteral("-journal"), QStringLiteral("-wal"), QStringLiteral("-shm")})
            if (!secureFile(path + suffix, false, error)) return false;
        return true;
    }
    bool exec(const QString &sql) {
        QSqlQuery q(database); if (q.exec(sql)) return true; error = q.lastError().text(); return false;
    }
    bool prune() {
        QSqlQuery q(database); q.prepare("DELETE FROM incidents WHERE last_timestamp < ?");
        q.addBindValue(QDateTime::currentSecsSinceEpoch() - Retention);
        if (!q.exec()) { error = q.lastError().text(); return false; }
        // Every incident in the documented retention window remains available;
        // count/aggregate-byte caps used to discard recent records prematurely.
        return true;
    }
    bool write(const QJsonObject &source) {
        const auto encoded = boundPayload(source);
        const auto &payload = encoded.object; const auto &bytes = encoded.bytes;
        if (bytes.size() > PayloadLimit) { error = T("卡顿记录超过 128 KiB 上限，未写入。"); return false; }
        QSqlQuery q(database);
        if (!q.prepare("INSERT OR REPLACE INTO incidents(id,start_timestamp,last_timestamp,status,summary,payload,payload_bytes) VALUES(?,?,?,?,?,?,?)")) { error = q.lastError().text(); return false; }
        q.addBindValue(payload["id"].toString()); q.addBindValue(payload["startTimestamp"].toDouble()); q.addBindValue(payload["lastTimestamp"].toDouble());
        q.addBindValue(payload["status"].toString()); q.addBindValue(QString::fromUtf8(QJsonDocument(summary(payload)).toJson(QJsonDocument::Compact)));
        q.addBindValue(QString::fromUtf8(bytes)); q.addBindValue(bytes.size());
        if (!q.exec()) { error = q.lastError().text(); return false; } return true;
    }
    bool open() {
        if (database.isOpen()) return true;
        error.clear();
        const auto directory = QDir(root).filePath("Incidents");
        if (!secureDirectory(root, error) || !secureDirectory(directory, error)) return false;
        path = QDir(directory).filePath("incidents.sqlite3");
        if (!secureFile(path, true, error) || !sidecars()) return false;
        if (connection.isEmpty()) { connection = "devstats-incidents-" + QUuid::createUuid().toString(QUuid::WithoutBraces); database = QSqlDatabase::addDatabase("QSQLITE", connection); }
        database.setDatabaseName(path); database.setConnectOptions("QSQLITE_BUSY_TIMEOUT=1000");
        if (!database.open()) { error = database.lastError().text(); return false; }
        for (const auto &sql : {QStringLiteral("PRAGMA journal_mode=DELETE"), QStringLiteral("PRAGMA synchronous=FULL"), QStringLiteral("PRAGMA busy_timeout=1000"),
            QStringLiteral("CREATE TABLE IF NOT EXISTS incidents(id TEXT PRIMARY KEY,start_timestamp REAL NOT NULL,last_timestamp REAL NOT NULL,status TEXT NOT NULL,summary TEXT NOT NULL,payload TEXT NOT NULL,payload_bytes INTEGER NOT NULL CHECK(payload_bytes<=131072))"),
            QStringLiteral("CREATE INDEX IF NOT EXISTS incidents_time ON incidents(last_timestamp)")}) if (!exec(sql)) { database.close(); return false; }
        // A previous process cannot continue an observation across a restart.
        if (!database.transaction()) { error = database.lastError().text(); database.close(); return false; }
        QVector<QJsonObject> abandoned;
        QSqlQuery q(database);
        bool ok = q.exec("SELECT payload FROM incidents WHERE status='active' AND payload_bytes<=131072");
        if (!ok) error = q.lastError().text();
        while (ok && q.next()) {
            auto object = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
            if (object.isEmpty()) { error = T("卡顿记录损坏，无法恢复。"); ok = false; break; }
            object["status"] = "interrupted"; object["endTimestamp"] = object["lastTimestamp"];
            object["interruptionReason"] = T("应用重新启动，前次记录未确认恢复。"); abandoned.append(object);
        }
        q.finish(); for (const auto &object : abandoned) if (ok) ok = write(object);
        if (ok) ok = prune() && sidecars();
        if (ok) { if (!database.commit()) { error = database.lastError().text(); ok = false; } }
        if (!ok) { database.rollback(); database.close(); return false; }
        error.clear(); return true;
    }
    bool persist() {
        if (!dirty || active.isEmpty()) return false;
        if (!open() || !sidecars()) { persistError = error; return false; }
        if (!database.transaction()) { error = database.lastError().text(); persistError = error; return false; }
        rebuildWitnesses();
        bool ok = write(active) && prune() && sidecars();
        if (ok && !database.commit()) { error = database.lastError().text(); ok = false; }
        if (!ok) { database.rollback(); persistError = error; return false; }
        dirty = false; lastWrite = lastMono; persistError.clear(); error.clear(); return true;
    }
    void rebuildWitnesses() {
        QMap<double, QJsonObject> samples;
        for (auto it = witnessSlots.cbegin(); it != witnessSlots.cend(); ++it) {
            const double mono = it.value()["monotonicSeconds"].toDouble();
            auto sample = samples.value(mono, it.value()); auto labels = sample["labels"].toArray(); labels.append(it.key()); sample["labels"] = labels; samples[mono] = sample;
        }
        QJsonArray witnesses; for (const auto &sample : samples) witnesses.append(sample);
        active["witnesses"] = witnesses;
    }
    void update(const QJsonObject &sample, const QJsonArray &findings) {
        const double previousMono = witnessSlots.contains("last") ? witnessSlots["last"]["monotonicSeconds"].toDouble() : sample["monotonicSeconds"].toDouble();
        active["durationSeconds"] = active["durationSeconds"].toDouble() + std::max(0.0, sample["monotonicSeconds"].toDouble() - previousMono);
        active["lastTimestamp"] = sample["timestamp"];
        active["sampleCount"] = active["sampleCount"].toInt() + 1;
        auto kinds = active["kinds"].toArray();
        for (const auto &value : findings) {
            const auto finding = value.toObject(); if (!kinds.contains(finding["kind"])) kinds.append(finding["kind"]);
            if (finding["severity"] == "critical") active["severity"] = "critical";
            if (finding["kind"] == "oom") witnessSlots["oom"] = sample;
        }
        active["kinds"] = kinds;
        // The most severe finding names the event; a later milder one never replaces it.
        for (const auto &value : findings) {
            const auto finding = value.toObject();
            const bool severe = finding["severity"] == "critical";
            if (active["summary"].toString().isEmpty() || (severe && !active["headlineCritical"].toBool())) {
                active["summary"] = finding["title"];
                active["headlineEvidence"] = finding["evidence"];
                active["headlineCritical"] = severe;
            }
        }
        witnessSlots["last"] = sample;
        for (const QString &kind : {QStringLiteral("CPU"), QStringLiteral("MemoryPressure"), QStringLiteral("IO"), QStringLiteral("Latency")}) {
            const QString label = "peak" + kind;
            if (score(sample, kind) >= 0 && (!witnessSlots.contains(label) || score(sample, kind) > score(witnessSlots.value(label), kind))) witnessSlots[label] = sample;
        }
        const auto apps = sample["topApplications"].toObject();
        const auto resource = kinds.contains("io") ? "io" : (kinds.contains("memory") || kinds.contains("oom")) ? "memory" : "cpu";
        const auto candidates = apps[resource].toArray();
        const QString basis = QByteArray(resource) == "io" ? "ioBytesPerSecond" : QByteArray(resource) == "memory" ? "memoryBytes" : "cpuPercent";
        if (!findings.isEmpty() && !candidates.isEmpty()) {
            const auto candidate = candidates.first().toObject();
            const auto previous = active["strongestApplication"].toObject();
            if (active["strongestApplicationBasis"].toString() != QLatin1String(resource) || !measured(previous[basis]) || candidate[basis].toDouble() > previous[basis].toDouble()) {
                active["strongestApplication"] = candidate; active["strongestApplicationBasis"] = QLatin1String(resource);
            }
        }
        dirty = true;
    }
    void retireFinished() {
        if (active["status"] == "recovered") {
            cooldown = active;
            cooldownUntil = witnessSlots.value("last")["monotonicSeconds"].toDouble() + 60;
        } else { cooldown = {}; cooldownUntil = -1; }
        active = {}; witnessSlots.clear();
    }
    bool finish(const QString &status, const QString &reason = QString()) {
        const double previousHighStart = highStart;
        suspect = {}; suspectPrelude = {}; highStart = -1; normalCount = 0;
        if (active.isEmpty()) return false;
        active["status"] = status; active["endTimestamp"] = active["lastTimestamp"];
        if (!reason.isEmpty()) active["interruptionReason"] = reason;
        if (status == "recovered") cooldownHighStart = previousHighStart;
        dirty = true; const bool changed = persist();
        // Retain a failed final commit for flush/retry; never claim it succeeded.
        if (!changed) return false;
        retireFinished(); return true;
    }
    void begin(const QJsonObject &sample, const QJsonArray &findings) {
        witnessSlots.clear(); normalCount = 0;
        if (!cooldown.isEmpty() && lastMono <= cooldownUntil) {
            active = cooldown; active["episodeCount"] = active["episodeCount"].toInt() + 1; highStart = cooldownHighStart;
            for (const auto &value : active["witnesses"].toArray()) for (const auto &label : value.toObject()["labels"].toArray()) { auto saved = value.toObject(); saved.remove("labels"); witnessSlots[label.toString()] = saved; }
        } else {
            QJsonArray prelude = suspectPrelude;
            if (suspect.isEmpty()) for (const auto &previous : ring) prelude.append(previous);
            const auto onset = suspect.isEmpty() ? sample : suspect;
            active = {{"schemaVersion", 2}, {"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {"startTimestamp", onset["timestamp"]}, {"lastTimestamp", sample["timestamp"]}, {"durationSeconds", 0},
                {"severity", "attention"}, {"kinds", QJsonArray()}, {"sampleCount", 0}, {"episodeCount", 1},
                {"strongestApplication", QJsonObject()}, {"prelude", prelude}, {"coverage", coverage()}};
            witnessSlots["onset"] = onset;
            if (!suspect.isEmpty()) update(onset, onset["findings"].toArray());
        }
        active["status"] = "active"; active["endTimestamp"] = unknown(); active.remove("interruptionReason");
        cooldown = {}; cooldownUntil = -1;
        if (highStart < 0) highStart = lastMono;
        update(sample, findings); suspect = {}; suspectPrelude = {};
    }
};

IncidentRecorder::IncidentRecorder(const QString &dataRoot) : d(new Impl(dataRoot)) {}
IncidentRecorder::~IncidentRecorder() {
    flush(); if (d->database.isValid()) d->database.close();
    d->database = QSqlDatabase(); if (!d->connection.isEmpty()) QSqlDatabase::removeDatabase(d->connection);
}
void IncidentRecorder::setEnabled(bool enabled) {
    if (d->enabled == enabled) return;
    if (!enabled) resetObservation();
    d->enabled = enabled;
}
void IncidentRecorder::resetObservation() {
    d->finish("interrupted", T("监测被暂停、系统休眠或采样时钟重置；此次观察中断。"));
    d->ring.clear(); d->suspect = {}; d->suspectPrelude = {}; d->cooldown = {}; d->cooldownUntil = -1; d->lastMono = d->highStart = -1; d->processCache = {}; d->processStamp = -2;
}
bool IncidentRecorder::wantsFreshProcesses() const {
    return d->enabled && d->highStart >= 0 && d->lastMono >= d->highStart && d->lastMono - d->highStart < 20 &&
        (!d->suspect.isEmpty() || (!d->active.isEmpty() && d->active["status"] == "active"));
}
bool IncidentRecorder::observe(const QJsonObject &snapshot) {
    if (!d->enabled) return false;
    const double now = number(snapshot, "monotonicSeconds"), timestamp = number(snapshot, "timestamp");
    if (now < 0 || timestamp < 0 || timestamp > 1e12) { d->error = T("卡顿采样的时间戳无效。"); return d->finish("interrupted", T("采样时间未知，观察中断。")); }
    bool changed = false;
    if (d->lastMono >= 0 && now == d->lastMono) return false;
    if (d->lastMono >= 0 && (now < d->lastMono || now - d->lastMono > 15)) {
        changed = d->finish("interrupted", T("采样间隔超过 15 秒或时钟回退，观察中断。"));
        d->ring.clear(); d->suspect = {}; d->suspectPrelude = {}; d->cooldown = {}; d->cooldownUntil = -1; d->highStart = -1;
    }
    d->lastMono = now;
    // A failed terminal commit must be retried before collecting another event.
    if (!d->active.isEmpty() && d->active["status"] != "active") {
        if (d->dirty) {
            if (!d->persist()) return changed;
            changed = true;
        }
        d->retireFinished();
    }
    const auto providedDiagnosis = snapshot["diagnostics"].toObject();
    const auto diagnosis = providedDiagnosis.isEmpty() ? diagnoseSystem(snapshot) : providedDiagnosis;
    const auto findings = relevantFindings(diagnosis); const bool abnormal = !findings.isEmpty();
    const bool known = relevantKnown(snapshot);
    const double processStamp = number(snapshot, "processMonotonicSeconds");
    if (d->processCache.isEmpty() || processStamp < 0 || processStamp != d->processStamp) {
        d->processCache = processEvidence(snapshot); d->processStamp = processStamp;
    }
    const auto sample = compactSample(snapshot, findings, d->processCache);
    if (!d->active.isEmpty()) {
        if (!known && !abnormal) changed = d->finish("interrupted", T("系统压力指标缺失或无法佐证恢复，观察中断。")) || changed;
        else {
            d->update(sample, findings);
            d->normalCount = abnormal ? 0 : d->normalCount + 1;
            if (d->normalCount >= 3) changed = d->finish("recovered") || changed;
            else if (d->lastWrite < 0 || now - d->lastWrite >= 30) changed = d->persist() || changed;
        }
    } else if (abnormal) {
        // A freeze or OOM is over by the time a second sample arrives. Record it at once.
        bool immediate = false; for (const auto &value : findings) immediate |= value.toObject()["immediate"].toBool();
        if (immediate || (!d->suspect.isEmpty() && now - d->suspect["monotonicSeconds"].toDouble() >= 1 && now - d->suspect["monotonicSeconds"].toDouble() <= 6)) {
            d->begin(sample, findings); changed = d->persist() || changed;
        } else if (d->suspect.isEmpty() || now - d->suspect["monotonicSeconds"].toDouble() > 6) {
            d->suspect = sample; d->highStart = now; d->suspectPrelude = {};
            for (const auto &previous : d->ring) d->suspectPrelude.append(previous);
        }
    } else { d->suspect = {}; d->highStart = -1; }
    d->ring.push_back(sample); while (d->ring.size() > 6) d->ring.pop_front();
    return changed;
}
QJsonArray IncidentRecorder::list(int limit) {
    QJsonArray result; if (!d->open()) return result;
    QSqlQuery q(d->database);
    q.prepare("SELECT summary FROM incidents WHERE last_timestamp>=? ORDER BY last_timestamp DESC,id DESC LIMIT ?");
    q.addBindValue(QDateTime::currentSecsSinceEpoch() - Retention);
    q.addBindValue(std::max(1, std::min(500, limit)));
    if (!q.exec()) { d->error = q.lastError().text(); return result; }
    while (q.next()) { const auto object = QJsonDocument::fromJson(q.value(0).toByteArray()).object(); if (!object.isEmpty()) result.append(object); }
    d->error = d->persistError;
    return result;
}
QJsonObject IncidentRecorder::detail(const QString &id) {
    if (!d->open()) return {};
    QSqlQuery q(d->database);
    q.prepare("SELECT payload FROM incidents WHERE id=? AND last_timestamp>=? AND payload_bytes<=131072");
    q.addBindValue(id);
    q.addBindValue(QDateTime::currentSecsSinceEpoch() - Retention);
    if (!q.exec()) { d->error = q.lastError().text(); return {}; }
    if (!q.next()) { d->error = T("找不到该卡顿记录。"); return {}; }
    const auto object = QJsonDocument::fromJson(q.value(0).toByteArray()).object();
    d->error = object.isEmpty() ? T("卡顿记录损坏。") : d->persistError;
    return object;
}
void IncidentRecorder::flush() {
    if (d->dirty && d->persist() && d->active["status"] != "active") d->retireFinished();
}
QString IncidentRecorder::error() const { return d->error; }
