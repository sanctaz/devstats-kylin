#include "incident_recorder.h"
#include "diagnostics.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSet>
#include <QSysInfo>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <QVariant>
#include <cmath>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>

namespace {
int failures = 0, assertions = 0;
void check(bool ok, const QString &message) {
    ++assertions;
    if (!ok) { ++failures; QTextStream(stderr) << "FAIL: " << message << '\n'; }
}
const double epoch = QDateTime::currentSecsSinceEpoch() - 20000;
QJsonObject psi(double v = 0) { return {{"avg10", v}, {"avg60", v}, {"avg300", v}}; }
QJsonObject process(int pid, const QString &identity, double cpu, double rss, QJsonValue read = 1, QJsonValue write = 2) {
    return {{"pid", pid}, {"startTicks", pid * 13}, {"applicationIdentity", identity}, {"name", "worker"},
        {"cpuPercent", cpu}, {"memoryBytes", rss}, {"ioReadBytesPerSecond", read}, {"ioWriteBytesPerSecond", write},
        {"commandLine", "/private/token/secret"}, {"executablePath", "/private/token/exe"}, {"projectPath", "/private/token/project"}};
}
QJsonObject sample(double mono, const QString &kind = QString(), int processCount = 0) {
    QJsonObject pressure;
    for (const char *resource : {"cpu", "memory", "io"}) pressure[resource] = QJsonObject{{"some", psi()}, {"full", psi()}};
    QJsonObject result{{"timestamp", epoch + mono}, {"monotonicSeconds", mono}, {"processMonotonicSeconds", mono}, {"processesFresh", true},
        {"cpu", QJsonObject{{"usage", .2}, {"iowait", .01}, {"coreCount", 4}, {"load", QJsonArray{.5,.5,.5}}, {"runningProcesses", 1}, {"blockedProcesses", 0}}},
        {"memory", QJsonObject{{"totalBytes", 1000000000.0}, {"availableBytes", 500000000.0}, {"fraction", .5}}},
        {"vm", QJsonObject{{"swapInBytesPerSecond", 0}, {"swapOutBytesPerSecond", 0}, {"majorFaultsPerSecond", 0}, {"oomKillDelta", 0}}},
        {"pressure", pressure}, {"disks", QJsonArray{QJsonObject{{"mountPoint", "/private/token/mount"}, {"usageFraction", .5}, {"availableBytes", 3000000000.0}}}},
        {"endpoint", "/private/token/endpoint"}};
    if (kind == "cpu") { auto cpu = result["cpu"].toObject(); cpu["usage"] = .96; cpu["runningProcesses"] = 8; cpu["load"] = QJsonArray{8,8,8}; result["cpu"] = cpu; }
    if (kind == "memory") {
        auto memory = result["memory"].toObject(); memory["availableBytes"] = 20000000; memory["fraction"] = .98; result["memory"] = memory;
        pressure["memory"] = QJsonObject{{"some", psi(35)}, {"full", psi(15)}}; result["pressure"] = pressure;
    }
    if (kind == "io") {
        auto cpu = result["cpu"].toObject(); cpu["iowait"] = .3; cpu["blockedProcesses"] = 2; result["cpu"] = cpu;
        pressure["io"] = QJsonObject{{"some", psi(55)}, {"full", psi(25)}}; result["pressure"] = pressure;
    }
    if (kind == "oom") { auto vm = result["vm"].toObject(); vm["oomKillDelta"] = 1; result["vm"] = vm; }
    QJsonArray processes;
    for (int i = 0; i < processCount; ++i) processes.append(process(i + 1, QString("exe:/private/token/app-%1").arg(i % 100), (i % 80) + 1, (i + 1) * 1024));
    result["processes"] = processes; return result;
}
QString dbPath(const QString &root) { return QDir(root).filePath("Incidents/incidents.sqlite3"); }
struct Database {
    QString name = "test-" + QUuid::createUuid().toString(); QSqlDatabase db;
    explicit Database(const QString &path) { db = QSqlDatabase::addDatabase("QSQLITE", name); db.setDatabaseName(path); check(db.open(), "test SQLite connection opens"); }
    ~Database() { db.close(); db = QSqlDatabase(); QSqlDatabase::removeDatabase(name); }
    qint64 scalar(const QString &sql) { QSqlQuery q(db); check(q.exec(sql) && q.next(), "SQL query: " + sql); return q.value(0).toLongLong(); }
    void exec(const QString &sql) { QSqlQuery q(db); check(q.exec(sql), "SQL execution: " + q.lastError().text()); }
};
QJsonObject only(IncidentRecorder &r) { const auto rows = r.list(); check(rows.size() == 1, "exactly one incident"); return rows.isEmpty() ? QJsonObject() : rows.first().toObject(); }
void testTriggerAndEvidence() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    for (int i = 0; i < 8; ++i) check(!r.observe(sample(i * 2)), "normal snapshot has no persisted change");
    check(!QFile::exists(dbPath(temp.path())), "normal observation does not open a database");
    check(!r.observe(sample(16, "cpu", 25)) && r.wantsFreshProcesses(), "first suspicion requests fresh process scan");
    check(!r.observe(sample(16.5, "cpu", 25)), "manual subsecond refresh cannot confirm incident");
    auto confirmed = sample(18, "cpu");
    confirmed["processes"] = QJsonArray{process(1, "exe:/private/token/a", 20, 100), process(2, "exe:/private/token/a", 30, 200),
        process(3, "exe:/private/token/b", 45, 400), process(4, "exe:/private/token/io", 0, 50, 5000000, 10000000),
        process(5, "exe:/private/token/partial", 10, 100, QJsonValue(QJsonValue::Null), 3)};
    check(r.observe(confirmed), "two separated abnormal observations persist immediately");
    auto row = only(r); check(row["status"] == "active" && row["severity"] == "critical", "active severity persisted");
    const auto id = row["id"].toString(); const auto detail = r.detail(id);
    check(!row.contains("prelude") && !row.contains("witnesses"), "list includes summaries only");
    check(detail["prelude"].toArray().size() == 6, "bounded six prelude snapshots");
    const auto witnesses = detail["witnesses"].toArray(); check(!witnesses.isEmpty() && witnesses.size() <= 6, "bounded key witnesses");
    const auto current = witnesses.last().toObject(); const auto top = current["topApplications"].toObject()["cpu"].toArray();
    check(top.first().toObject()["cpuPercent"].toDouble() == 50, "exact application identities sum all measured CPU");
    check(top.size() == 4, "same process names in different identities remain separate applications");
    check(current["topApplications"].toObject()["io"].toArray().first().toObject()["ioBytesPerSecond"].toDouble() == 15000000, "IO applications aggregated directly from processes");
    check(current["processCoverage"].toObject()["partial"].toBool(), "unknown process IO is explicitly partial");
    bool partialIO = false;
    for (const auto &value : current["topProcesses"].toArray()) {
        const auto p = value.toObject(); if (p["pid"].toInt() == 5) partialIO = p["ioBytesPerSecond"].toDouble(-1) == 3 && p["partial"].toBool();
    }
    check(partialIO, "one-sided process IO remains a ranked measured lower bound");
    check(!r.observe(confirmed) && only(r)["status"] == "active", "duplicate sample timestamp is ignored without interruption");
    check(current["topProcesses"].toArray().size() <= 10, "bounded process evidence");
    const auto bytes = QJsonDocument(detail).toJson(QJsonDocument::Compact);
    check(!bytes.contains("/private/token") && !bytes.contains("executablePath") && !bytes.contains("commandLine") && !bytes.contains("projectPath") && !bytes.contains("mountPoint") && !bytes.contains("endpoint"), "whitelisted evidence never persists private paths");
    check(bytes.size() <= 131072, "bounded payload");
    Database db(dbPath(temp.path()));
    db.exec("CREATE TABLE writes(count INTEGER)"); db.exec("INSERT INTO writes VALUES(0)");
    db.exec("CREATE TRIGGER count_writes AFTER INSERT ON incidents BEGIN UPDATE writes SET count=count+1; END");
    for (int t = 20; t < 48; t += 2) {
        check(!r.observe(sample(t, t == 30 ? "memory" : t == 32 ? "io" : "cpu", 20)), "no intermediate write before thirty seconds");
        if (t == 34) check(r.wantsFreshProcesses(), "process boost remains enabled before the twenty-second boundary");
        if (t == 36) check(!r.wantsFreshProcesses(), "process boost ends exactly twenty seconds after first suspicion");
    }
    check(db.scalar("SELECT count FROM writes") == 0, "no per-sample disk write");
    check(r.observe(sample(48, "cpu", 20)), "ongoing event commits after thirty seconds");
    check(db.scalar("SELECT count FROM writes") == 1, "exactly one thirty-second update");
    for (int t = 50; t <= 78; t += 2) r.observe(sample(t, "cpu", 20));
    check(!r.wantsFreshProcesses(), "high-frequency process requests remain stopped during sustained pressure");
    check(!r.observe(sample(80)) && !r.observe(sample(82)) && r.observe(sample(84)), "three known normal samples close event");
    row = only(r); check(row["status"] == "recovered" && !r.wantsFreshProcesses(), "recovery disables scan boost");
    check(row["strongestApplication"].toObject()["cpuPercent"].toDouble() > 0 && !row["strongestApplicationBasis"].toString().isEmpty(), "strongest abnormal activity retains explicit resource basis after recovery");
    check(!r.observe(sample(86, "io")) && r.observe(sample(88, "io")), "new pressure inside cooldown confirmed");
    row = only(r); check(row["id"].toString() == id && row["episodeCount"].toInt() == 2, "cooldown merges repeat into same event");
    check(!r.wantsFreshProcesses(), "cooldown merge does not restart an expired high-frequency budget");
    r.observe(sample(90)); r.observe(sample(92)); r.observe(sample(94));
    check(r.observe(sample(96, "oom")), "OOM immediately reopens even during cooldown");
    row = only(r); check(row["kinds"].toArray().contains("oom") && row["episodeCount"].toInt() == 3, "OOM is retained in merged event");
    r.setEnabled(false); check(!r.wantsFreshProcesses() && !r.observe(sample(98, "oom")), "disabled capture stops observations and boost");
    check(only(r)["status"] == "interrupted", "disabling records an interruption and retains history");
    struct stat st {}; check(stat(dbPath(temp.path()).toUtf8().constData(), &st) == 0 && (st.st_mode & 0777) == 0600, "database owner-only mode");
    check(stat(QDir(temp.path()).filePath("Incidents").toUtf8().constData(), &st) == 0 && (st.st_mode & 0777) == 0700, "directory owner-only mode");
}
void testGapsUnknownRestart() {
    QTemporaryDir temp;
    QString id;
    {
        IncidentRecorder r(temp.path());
        check(r.observe(sample(0, "oom")), "OOM first observation persists"); id = only(r)["id"].toString();
        auto missing = sample(2); missing["memory"] = QJsonObject();
        check(r.observe(missing) && only(r)["status"] == "interrupted", "unknown system metrics end observation as interrupted");
        r.observe(sample(4, "cpu")); check(r.observe(sample(6, "cpu")), "new event after interruption");
        check(r.observe(sample(24, "cpu")), "long gap persists interruption");
        check(r.list().size() == 2 && r.list().first().toObject()["status"] == "interrupted", "gap does not stitch separate observations");
        check(r.observe(sample(26, "cpu")), "new pair after gap opens separate event");
        r.resetObservation(); check(!r.wantsFreshProcesses(), "explicit reset clears scan request");
        check(r.observe(sample(0, "oom")), "clock zero after resume is accepted as a new event");
        id = r.list().last().toObject()["id"].toString();
    }
    {
        IncidentRecorder r(temp.path()); const auto rows = r.list();
        bool abandoned = false; for (const auto &value : rows) {
            const auto row = value.toObject();
            check(row["status"] != "active", "restart leaves no abandoned active incident");
            abandoned |= row["interruptionReason"].toString().contains(QStringLiteral("重新启动"));
        }
        check(abandoned, "restart explicitly marks interrupted session");
        check(!r.wantsFreshProcesses(), "restart has no process scan boost");
        check(r.observe(sample(2, "oom")), "restart OOM starts a fresh incident");
    }
    QTemporaryDir noPSI; IncidentRecorder oldKernel(noPSI.path());
    auto swap = sample(0, "memory", 3); swap["pressure"] = QJsonObject();
    auto vm = swap["vm"].toObject(); vm["swapInBytesPerSecond"] = 2000000; swap["vm"] = vm;
    oldKernel.observe(swap); swap["monotonicSeconds"] = 2; swap["timestamp"] = epoch + 2; swap["processMonotonicSeconds"] = 2;
    check(oldKernel.observe(swap), "low-memory swap pressure captures on kernels without PSI");
    auto oldRow = only(oldKernel); auto oldDetail = oldKernel.detail(oldRow["id"].toString()); bool memoryPeak = false;
    for (const auto &value : oldDetail["witnesses"].toArray()) memoryPeak |= value.toObject()["labels"].toArray().contains("peakMemoryPressure");
    check(memoryPeak, "memory pressure peak falls back to capacity/swap when PSI unavailable");
    for (int t : {4, 6, 8}) { auto recovered = sample(t); recovered["pressure"] = QJsonObject(); oldKernel.observe(recovered); }
    check(only(oldKernel)["status"] == "recovered", "a kernel without PSI still recovers normally");
    QTemporaryDir diskOnly; IncidentRecorder r(diskOnly.path()); auto disk = sample(0);
    disk["disks"] = QJsonArray{QJsonObject{{"mountPoint", "/"}, {"usageFraction", .999}, {"availableBytes", 1}}};
    r.observe(disk); disk["monotonicSeconds"] = 2; disk["timestamp"] = epoch + 2;
    check(!r.observe(disk) && !r.wantsFreshProcesses() && !QFile::exists(dbPath(diskOnly.path())), "disk capacity alone never triggers capture");
}
void testStorageFailures() {
    QTemporaryDir temp, outside;
    check(::symlink(outside.path().toUtf8().constData(), QDir(temp.path()).filePath("Incidents").toUtf8().constData()) == 0, "create symlink fixture");
    IncidentRecorder denied(temp.path());
    check(!denied.observe(sample(0, "oom")) && !denied.error().isEmpty(), "symlinked storage fails visibly");
    check(!QFile::exists(QDir(outside.path()).filePath("incidents.sqlite3")), "rejected symlink never writes outside target");
    QTemporaryDir locked; IncidentRecorder r(locked.path()); r.list(); Database db(dbPath(locked.path()));
    db.exec("BEGIN EXCLUSIVE");
    check(!r.observe(sample(0, "oom")) && !r.error().isEmpty(), "locked SQLite reports failure without false success");
    db.exec("ROLLBACK"); const auto savedError = r.error(); r.list();
    check(r.error() == savedError && !savedError.isEmpty(), "successful list does not erase pending persistence failure");
    r.flush(); check(r.error().isEmpty() && r.list().size() == 1, "flush retries failed event and clears error only after commit");
    check(r.detail("missing").isEmpty() && !r.error().isEmpty(), "missing detail has explicit error");
    r.list(); check(r.error().isEmpty(), "successful read clears a previous read error");
    QTemporaryDir invalid; IncidentRecorder bad(invalid.path()); auto object = sample(0, "oom"); object["timestamp"] = QJsonValue(QJsonValue::Null);
    check(!bad.observe(object) && !bad.error().isEmpty(), "invalid time never reports successful capture");
}
void testTerminalCommitRecovery() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    check(r.observe(sample(0, "oom", 2)), "terminal retry fixture starts event");
    const auto id = only(r)["id"].toString();
    r.observe(sample(2)); r.observe(sample(4));
    Database db(dbPath(temp.path())); db.exec("BEGIN EXCLUSIVE");
    check(!r.observe(sample(6)) && !r.error().isEmpty(), "failed recovery commit remains pending and reports failure");
    db.exec("ROLLBACK"); r.flush();
    check(r.error().isEmpty() && only(r)["status"] == "recovered", "flush retries and completes the pending recovery commit");
    check(r.observe(sample(8, "oom", 2)), "capture resumes after a successful terminal retry");
    const auto row = only(r);
    check(row["id"] == id && row["episodeCount"].toInt() == 2 && row["status"] == "active", "terminal retry preserves cooldown merge and event identity");
}
void testRetentionAndBounds() {
    QTemporaryDir temp; IncidentRecorder r(temp.path()); r.list();
    Database db(dbPath(temp.path())); check(db.db.transaction(), "retention fixture transaction");
    QSqlQuery write(db.db); write.prepare("INSERT INTO incidents VALUES(?,?,?,?,?,?,?)");
    for (int i = 0; i < 601; ++i) {
        const double when = QDateTime::currentSecsSinceEpoch() - (i == 0 ? 32 * 86400 : 1000 - i);
        const auto payload = QJsonDocument(QJsonObject{{"id", QString("fixture-%1").arg(i)}, {"padding", QString(120000, 'x')}}).toJson(QJsonDocument::Compact);
        write.bindValue(0, QString("fixture-%1").arg(i)); write.bindValue(1, when); write.bindValue(2, when); write.bindValue(3, "recovered");
        write.bindValue(4, QString::fromUtf8(QJsonDocument(QJsonObject{{"id", QString("fixture-%1").arg(i)}}).toJson(QJsonDocument::Compact)));
        write.bindValue(5, QString::fromUtf8(payload)); write.bindValue(6, payload.size()); check(write.exec(), "seed bounded retention row");
    }
    check(db.db.commit(), "retention fixture commit");
    check(r.observe(sample(0, "oom", 1000)), "new record atomically enforces retention");
    check(db.scalar("SELECT COUNT(*) FROM incidents") == 601, "all six hundred recent rows plus the new event survive pruning");
    check(db.scalar("SELECT SUM(payload_bytes) FROM incidents") > 32LL * 1024 * 1024, "aggregate bytes never shorten the thirty-one-day retention window");
    check(r.detail("fixture-1")["id"] == "fixture-1" && r.detail("fixture-600")["id"] == "fixture-600", "oldest and newest recent fixtures remain readable");
    check(db.scalar("SELECT COUNT(*) FROM incidents WHERE id='fixture-0'") == 0, "thirty-one-day retention");
    check(db.scalar("SELECT MAX(payload_bytes) FROM incidents") <= 131072, "all rows within payload limit");
    check(r.list(4).size() == 4, "list respects bounded requested limit");
    // A full six-prelude/six-witness worst-case is still bounded at 1000 processes.
    QTemporaryDir large; IncidentRecorder stress(large.path());
    for (int i = 0; i < 6; ++i) stress.observe(sample(i * 2, "", 1000));
    stress.observe(sample(12, "cpu", 1000)); stress.observe(sample(14, "cpu", 1000));
    stress.observe(sample(16, "memory", 1000)); stress.observe(sample(18, "io", 1000)); stress.observe(sample(20, "oom", 1000)); stress.observe(sample(22, "cpu", 1000)); stress.flush();
    const auto row = only(stress); const auto payload = stress.detail(row["id"].toString());
    check(stress.error().isEmpty() && QJsonDocument(payload).toJson(QJsonDocument::Compact).size() <= 131072, "worst-case process evidence stays within payload budget");
    check(payload["witnesses"].toArray().size() <= 6 && payload["prelude"].toArray().size() <= 6, "worst-case witness and prelude count bounds");
}
void testLongPressureCheckpoints() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    check(r.observe(sample(0, "oom", 8)), "long pressure persists onset immediately");
    Database db(dbPath(temp.path()));
    db.exec("CREATE TABLE writes(count INTEGER)"); db.exec("INSERT INTO writes VALUES(0)");
    db.exec("CREATE TRIGGER count_writes AFTER INSERT ON incidents BEGIN UPDATE writes SET count=count+1; END");
    for (int t = 2; t <= 180; t += 2) {
        const bool changed = r.observe(sample(t, "cpu", 8));
        check(changed == (t % 30 == 0), "sustained pressure keeps exact thirty-second checkpoint cadence");
        if (t >= 20) check(!r.wantsFreshProcesses(), "long pressure never extends the process boost beyond twenty seconds");
    }
    check(db.scalar("SELECT count FROM writes") == 6, "three-minute pressure retains six checkpoints beyond boost expiry");
    const auto row = only(r), detail = r.detail(row["id"].toString());
    check(row["status"] == "active" && detail["durationSeconds"].toDouble() == 180, "long pressure remains active with full monotonic duration");
    check(detail["witnesses"].toArray().last().toObject()["monotonicSeconds"].toDouble() == 180, "checkpoint materializes the current last witness");
    check(detail["witnesses"].toArray().size() <= 6 && detail["sampleCount"].toInt() == 91, "bounded witnesses retain the complete observation count");
}
void testLegacyDatabase() {
    QTemporaryDir temp;
    check(QDir().mkpath(QDir(temp.path()).filePath("Incidents")), "legacy database directory");
    {
        Database db(dbPath(temp.path()));
        db.exec("CREATE TABLE incidents(id TEXT PRIMARY KEY,start_timestamp REAL NOT NULL,last_timestamp REAL NOT NULL,status TEXT NOT NULL,summary TEXT NOT NULL,payload TEXT NOT NULL,payload_bytes INTEGER NOT NULL CHECK(payload_bytes<=131072))");
        db.exec("CREATE TABLE settings(name TEXT PRIMARY KEY,value TEXT)");
        db.exec("INSERT INTO settings VALUES('incidentRecording','false')");
        const QJsonObject legacy{{"schemaVersion", 1}, {"id", "legacy-077"}, {"status", "recovered"}, {"startTimestamp", epoch}, {"lastTimestamp", epoch + 10},
            {"durationSeconds", 10}, {"kinds", QJsonArray{"cpu"}}, {"coverage", QJsonArray{"legacy coverage"}},
            {"witnesses", QJsonArray{QJsonObject{{"timestamp", epoch}, {"processCoverage", QJsonObject{{"fresh", false}, {"ageSeconds", QJsonValue(QJsonValue::Null)}, {"partial", true}}}}}}};
        const auto bytes = QJsonDocument(legacy).toJson(QJsonDocument::Compact);
        QSqlQuery q(db.db); q.prepare("INSERT INTO incidents VALUES(?,?,?,?,?,?,?)");
        q.addBindValue("legacy-077"); q.addBindValue(epoch); q.addBindValue(epoch + 10); q.addBindValue("recovered");
        q.addBindValue(QString::fromUtf8(bytes)); q.addBindValue(QString::fromUtf8(bytes)); q.addBindValue(bytes.size());
        check(q.exec(), "seed previous schema-version-one row");
    }
    IncidentRecorder r(temp.path());
    check(only(r)["id"] == "legacy-077", "previous database opens without migration");
    const auto detail = r.detail("legacy-077");
    check(detail["coverage"].toArray().first() == "legacy coverage" && detail["witnesses"].toArray().first().toObject()["processCoverage"].toObject()["ageSeconds"].isNull(), "previous evidence and unknown markers remain intact");
    check(r.observe(sample(0, "oom", 2)) && r.list().size() == 2, "new events append to the same previous database");
    Database db(dbPath(temp.path()));
    check(db.scalar("SELECT COUNT(*) FROM settings WHERE name='incidentRecording' AND value='false'") == 1, "unrelated previous settings are untouched");
}
void testProcessEvidenceCacheAndUnknowns() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    auto onset = sample(0, "oom");
    onset["processes"] = QJsonArray{process(1, "app-a", 100, 500, QJsonValue(QJsonValue::Null), 10), process(2, "app-b", 50, 1000, 100, 200)};
    check(r.observe(onset), "process cache fixture starts event");
    auto cached = sample(2, "cpu"); cached["processMonotonicSeconds"] = 0; cached["processesFresh"] = false;
    cached["processes"] = QJsonArray{process(3, "different", 999, 99999)};
    r.observe(cached); r.flush();
    auto payload = r.detail(only(r)["id"].toString()); auto last = payload["witnesses"].toArray().last().toObject();
    check(last["topProcesses"].toArray().first().toObject()["pid"].toInt() == 1, "unchanged process scan reuses measured evidence");
    check(!last["processCoverage"].toObject()["fresh"].toBool() && last["processCoverage"].toObject()["ageSeconds"].toDouble() == 2, "cached evidence still updates real age and stale flag");
    auto unknown = sample(4, "cpu"); unknown["processMonotonicSeconds"] = QJsonValue(QJsonValue::Null);
    unknown["processes"] = QJsonArray{process(4, "unknown-timing", 1, 2, QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null))};
    r.observe(unknown); r.flush(); payload = r.detail(only(r)["id"].toString()); last = payload["witnesses"].toArray().last().toObject();
    const auto row = last["topProcesses"].toArray().first().toObject();
    check(row["pid"].toInt() == 4 && row["ioBytesPerSecond"].isNull() && row["partial"].toBool(), "unknown scan time never reuses previous process identity or replaces unknown IO with zero");
    check(last["processCoverage"].toObject()["ageSeconds"].isNull() && !last["processCoverage"].toObject()["fresh"].toBool(), "unknown process age remains unknown");
}
void testRankedBoundaries() {
    QJsonArray rows;
    for (int pid = 1; pid <= 100; ++pid)
        rows.append(process(pid, QString("exact-app-%1").arg(pid), pid, 101 - pid, pid == 50 ? 10000 : pid * 10, 0));
    rows.append(process(101, "all-unknown", -1, -1, QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)));
    auto evidence = [&](const QJsonArray &processes) {
        QTemporaryDir temp; IncidentRecorder r(temp.path()); auto snapshot = sample(0, "oom"); snapshot["processes"] = processes;
        check(r.observe(snapshot), "ranking boundary fixture persists");
        return r.detail(only(r)["id"].toString())["witnesses"].toArray().first().toObject();
    };
    const auto forward = evidence(rows); const auto top = forward["topProcesses"].toArray();
    check(top.size() == 10 && top[0].toObject()["pid"].toInt() == 100 && top[1].toObject()["pid"].toInt() == 1 && top[2].toObject()["pid"].toInt() == 50,
        "bounded rankings interleave independently strongest CPU, memory and IO processes");
    const auto apps = forward["topApplications"].toObject();
    check(apps["cpu"].toArray().size() == 5 && apps["cpu"].toArray().first().toObject()["cpuPercent"].toDouble() == 100 &&
        apps["memory"].toArray().first().toObject()["memoryBytes"].toDouble() == 100 && apps["io"].toArray().first().toObject()["ioBytesPerSecond"].toDouble() == 10000,
        "application resource rankings each retain their own exact top five");
    QJsonArray reverse; for (int i = rows.size() - 1; i >= 0; --i) reverse.append(rows[i]);
    const auto backward = evidence(reverse);
    check(forward["topApplications"] == backward["topApplications"] && forward["topProcesses"] == backward["topProcesses"], "bounded ranking output is deterministic under reversed process enumeration");
}
void testExpiredQueriesOnOpenConnection() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    check(r.list().isEmpty() && r.error().isEmpty(), "open recorder connection before retention fixtures");
    Database db(dbPath(temp.path()));
    QSqlQuery write(db.db);
    check(write.prepare("INSERT INTO incidents VALUES(?,?,?,?,?,?,?)"), "prepare read-retention fixture");
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    for (const auto &id : {QStringLiteral("expired"), QStringLiteral("recent")}) {
        const qint64 timestamp = id == "expired" ? now - 31LL * 86400 - 60 : now;
        const auto payload = QJsonDocument(QJsonObject{{"id", id}, {"lastTimestamp", double(timestamp)}}).toJson(QJsonDocument::Compact);
        write.bindValue(0, id); write.bindValue(1, timestamp); write.bindValue(2, timestamp); write.bindValue(3, "recovered");
        write.bindValue(4, QString::fromUtf8(payload)); write.bindValue(5, QString::fromUtf8(payload)); write.bindValue(6, payload.size());
        check(write.exec(), "insert record after recorder connection is already open");
    }
    const auto version = db.scalar("PRAGMA data_version");
    const auto rows = r.list();
    check(rows.size() == 1 && rows.first().toObject()["id"] == "recent", "already-open connection hides expired rows from list");
    check(r.detail("expired").isEmpty() && !r.error().isEmpty(), "already-open connection rejects expired detail");
    check(r.detail("recent")["id"] == "recent" && r.error().isEmpty(), "recent detail remains readable after expired lookup");
    check(db.scalar("SELECT COUNT(*) FROM incidents") == 2, "retention queries do not physically delete rows");
    check(db.scalar("PRAGMA data_version") == version, "retention queries cause no SQLite write");
}
void testLongNamesAndWallClock() {
    QTemporaryDir temp; IncidentRecorder r(temp.path());
    auto noisy = [](double mono, const QString &kind) {
        auto snapshot = sample(mono, kind, 40); auto rows = snapshot["processes"].toArray();
        for (int i = 0; i < rows.size(); ++i) { auto row = rows[i].toObject(); row["name"] = QString(64, QChar(0x6d4b)); rows[i] = row; }
        snapshot["processes"] = rows; return snapshot;
    };
    for (int i = 0; i < 6; ++i) r.observe(noisy(i * 2, ""));
    r.observe(noisy(12, "cpu")); check(r.observe(noisy(14, "cpu")), "long UTF8 names capture starts");
    auto cpuPeak = noisy(16, "cpu"); auto cpu = cpuPeak["cpu"].toObject(); cpu["usage"] = .99; cpuPeak["cpu"] = cpu; r.observe(cpuPeak);
    r.observe(noisy(18, "memory")); r.observe(noisy(20, "io")); r.observe(noisy(22, "oom"));
    auto last = noisy(24, "cpu"); last["timestamp"] = epoch - 1000; r.observe(last); r.flush();
    const auto row = only(r); const auto detail = r.detail(row["id"].toString());
    check(r.error().isEmpty() && QJsonDocument(detail).toJson(QJsonDocument::Compact).size() <= 131072, "six distinct witnesses plus six UTF8-name preludes remain persistable");
    check(detail["durationSeconds"].toDouble() == 12, "duration follows monotonic elapsed time across backward wall clock");
    QSet<QString> labels;
    for (const auto &value : detail["witnesses"].toArray()) for (const auto &label : value.toObject()["labels"].toArray()) labels.insert(label.toString());
    check(labels.contains("onset") && labels.contains("peakCPU") && labels.contains("peakMemoryPressure") && labels.contains("peakIO") && labels.contains("oom") && labels.contains("last"), "budget compaction retains every key witness label");
    check(detail["payloadCompaction"].isObject(), "UTF8 worst case records explicit payload compaction coverage");
    QTemporaryDir peakTemp; IncidentRecorder peak(peakTemp.path());
    auto first = sample(0, "cpu"); first["processes"] = QJsonArray{process(1,"exact-app",200,500)};
    peak.observe(first); first["timestamp"] = epoch + 2; first["monotonicSeconds"] = 2; first["processMonotonicSeconds"] = 2; peak.observe(first);
    for (int i = 2; i < 5; ++i) { auto normal = sample(i * 2); normal["processes"] = QJsonArray{process(2,"idle-recovery",1,10)}; peak.observe(normal); }
    check(only(peak)["strongestApplication"].toObject()["cpuPercent"].toDouble() == 200, "idle recovery application never replaces abnormal peak activity");
    // DevStats' own window and diagnosis cost CPU while someone investigates; the
    // incident keeps it in the process list but never names it.
    QTemporaryDir selfTemp; IncidentRecorder selfRecorder(selfTemp.path());
    for (int i = 0; i < 2; ++i) {
        auto busy = sample(i * 2, "cpu"); auto self = process(7, "exe:/opt/devstats/devstats", 300, 500); self["self"] = true;
        busy["processes"] = QJsonArray{self, process(8, "exact-app", 150, 500)}; selfRecorder.observe(busy);
    }
    selfRecorder.flush();
    const auto selfRow = only(selfRecorder);
    check(selfRow["strongestApplication"].toObject()["cpuPercent"].toDouble() == 150, "DevStats is never the incident's named application");
    bool listed = false;
    for (const auto &value : selfRecorder.detail(selfRow["id"].toString())["witnesses"].toArray())
        for (const auto &row : value.toObject()["topProcesses"].toArray()) listed |= row.toObject()["pid"].toDouble() == 7;
    check(listed, "DevStats remains visible in the incident's process list");
}
qint64 statusKiB(const QByteArray &key) {
    QFile file("/proc/self/status");
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const auto &raw : file.readAll().split('\n')) {
        const auto line = raw.simplified();
        if (line.startsWith(key)) return line.split(' ').value(1).toLongLong();
    }
    return -1;
}
double cpuSeconds() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return -1;
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6 + usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6;
}
QJsonObject testNormalPerformance(bool active = false) {
    QTemporaryDir temp; IncidentRecorder r(temp.path()); r.list(); Database db(dbPath(temp.path()));
    const auto initialVersion = db.scalar("PRAGMA data_version");
    auto object = sample(0, active ? "cpu" : "", 1600);
    object["diagnostics"] = diagnoseSystem(object);
    qint64 sum = 0, maximum = 0, cachedSum = 0, cachedMax = 0, rebuildSum = 0, rebuildMax = 0;
    const auto residentBefore = statusKiB("VmRSS:"); qint64 residentMaximum = residentBefore;
    const auto cpuBefore = cpuSeconds();
    for (int i = 0; i < 120; ++i) {
        const double mono = i * 2; object["timestamp"] = epoch + mono; object["monotonicSeconds"] = mono;
        object["processMonotonicSeconds"] = (i / 5) * 10; object["processesFresh"] = i % 5 == 0;
        QElapsedTimer timer; timer.start(); const bool changed = r.observe(object); const auto elapsed = timer.nsecsElapsed();
        sum += elapsed; maximum = std::max(maximum, elapsed);
        if (i % 5 != 0) { cachedSum += elapsed; cachedMax = std::max(cachedMax, elapsed); }
        else { rebuildSum += elapsed; rebuildMax = std::max(rebuildMax, elapsed); }
        if (!active) check(!changed, "normal process-heavy sample never changes persisted list");
        if (i % 5 == 0) residentMaximum = std::max(residentMaximum, statusKiB("VmRSS:"));
    }
    const auto loopCpuSeconds = cpuSeconds() - cpuBefore;
    const auto residentAfter = statusKiB("VmRSS:");
    const auto incidentRows = db.scalar("SELECT COUNT(*) FROM incidents");
    const bool unchanged = db.scalar("PRAGMA data_version") == initialVersion;
    if (!active) {
        check(incidentRows == 0, "normal process-heavy run creates no SQL incident rows");
        check(unchanged, "normal process-heavy run performs no SQLite write");
    }
    qint64 preludeBytes = 0;
    if (!active) {
        auto onset = sample(240, "oom"); onset["processes"] = object["processes"]; onset["processMonotonicSeconds"] = object["processMonotonicSeconds"];
        onset["processesFresh"] = false;
        check(r.observe(onset), "post-benchmark event persists the retained lightweight prelude");
        const auto payload = r.detail(only(r)["id"].toString()); const auto prelude = payload["prelude"].toArray();
        preludeBytes = QJsonDocument(prelude).toJson(QJsonDocument::Compact).size();
        check(prelude.size() == 6 && preludeBytes < 120 * 1024, "six retained healthy samples contain only bounded process evidence");
        for (const auto &entry : prelude) {
            const auto sample = entry.toObject();
            check(!sample.contains("processes") && !sample.contains("activityApplications") && sample["topProcesses"].toArray().size() <= 10, "healthy prelude never retains full process arrays");
        }
    }
    return {{"state", active ? "active" : "healthy"}, {"samples", 120}, {"processes", 1600}, {"processScanUpdates", 24},
        {"meanObserveMilliseconds", sum / 120.0 / 1e6}, {"maxObserveMilliseconds", maximum / 1e6},
        {"cachedMeanObserveMilliseconds", cachedSum / 96.0 / 1e6}, {"cachedMaxObserveMilliseconds", cachedMax / 1e6},
        {"rebuildMeanObserveMilliseconds", rebuildSum / 24.0 / 1e6}, {"rebuildMaxObserveMilliseconds", rebuildMax / 1e6},
        {"loopCpuSeconds", loopCpuSeconds}, {"residentBeforeKiB", double(residentBefore)}, {"residentAfterKiB", double(residentAfter)}, {"residentSampledMaximumKiB", double(residentMaximum)}, {"residentHighWaterKiB", double(statusKiB("VmHWM:"))},
        {"retainedPreludeEncodedBytes", double(preludeBytes)}, {"incidentRows", double(incidentRows)}, {"databaseDataVersionUnchanged", unchanged},
        {"environment", QString("Qt %1 / %2 on the build host; native host CPU/RSS when run directly; target-device performance remains unverified").arg(qVersion(), QSysInfo::buildCpuArchitecture())}};
}
}
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (app.arguments().contains("--benchmark-only")) {
        const QJsonObject result{{"healthy", testNormalPerformance()}, {"active", testNormalPerformance(true)}, {"failures", failures}};
        QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented); return failures ? 1 : 0;
    }
    testTriggerAndEvidence(); testGapsUnknownRestart(); testStorageFailures(); testTerminalCommitRecovery(); testRetentionAndBounds(); testLongPressureCheckpoints(); testLegacyDatabase(); testProcessEvidenceCacheAndUnknowns(); testRankedBoundaries(); testExpiredQueriesOnOpenConnection(); testLongNamesAndWallClock();
    const auto performance = app.arguments().contains("--regression-only") ? QJsonObject() : testNormalPerformance();
    const QJsonObject result{{"suite", "incident-recorder"}, {"qtVersion", qVersion()}, {"assertions", assertions}, {"failures", failures}, {"passed", failures == 0}, {"performance", performance}};
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Indented);
    return failures ? 1 : 0;
}
