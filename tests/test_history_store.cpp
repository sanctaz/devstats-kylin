#include "history_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUuid>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

namespace {
void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

bool waitFor(const std::function<bool()> &predicate, int milliseconds = 4000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < milliseconds) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        usleep(1000);
    }
    return predicate();
}

QJsonObject sample(double timestamp, double value) {
    return {{QStringLiteral("timestamp"), timestamp},
        {QStringLiteral("cpu"), QJsonObject {{QStringLiteral("usage"), value}}},
        {QStringLiteral("memory"), QJsonObject {{QStringLiteral("fraction"), value / 10}}},
        {QStringLiteral("diskIO"), QJsonObject {{QStringLiteral("readBytesPerSecond"), value * 100},
                                             {QStringLiteral("writeBytesPerSecond"), value * 20}}},
        {QStringLiteral("network"), QJsonObject {{QStringLiteral("downloadBytesPerSecond"), value * 200},
                                               {QStringLiteral("uploadBytesPerSecond"), value * 10}}}};
}

void testHistory() {
    QTemporaryDir temporary;
    check(temporary.isValid(), "history temporary directory");
    const QString root = QDir(temporary.path()).filePath(QStringLiteral("项目 数据"));
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    const qint64 minute = (now / 60) * 60 - 120;
    {
        HistoryStore history(root);
        check(history.open(), qPrintable(history.error()));
        check(history.record(sample(minute + 1.25, 1)), "first sample");
        check(history.record(sample(minute + 31.5, 3)), "second sample");
        const QJsonObject result = history.query(minute, minute + 59);
        check(!result.contains(QStringLiteral("error")), "history query error");
        const QJsonArray cpu = result.value(QStringLiteral("cpu")).toArray();
        check(cpu.size() == 1, "minute aggregation count");
        const QJsonObject point = cpu.first().toObject();
        check(point.value(QStringLiteral("value")).toDouble() == 2, "mean aggregation");
        check(point.value(QStringLiteral("minimum")).toDouble() == 1, "minimum aggregation");
        check(point.value(QStringLiteral("maximum")).toDouble() == 3, "maximum aggregation");
        check(point.value(QStringLiteral("count")).toInt() == 2, "sample count");
        check(point.value(QStringLiteral("firstSample")).toDouble() == minute + 1.25, "actual first sample time");
        check(point.value(QStringLiteral("lastSample")).toDouble() == minute + 31.5, "actual last sample time");
        check(result.value(QStringLiteral("battery")).toArray().isEmpty(), "missing battery must not become zero");
        check(!history.record({{QStringLiteral("timestamp"), QStringLiteral("bad")}}), "reject bad timestamp");
        check(!history.record({{QStringLiteral("timestamp"), double(minute)}}), "reject empty metrics");
        check(history.query(10, 1).contains(QStringLiteral("error")), "reject backwards query");
        check(history.record(sample(minute + 40, 5)), "queued sample before destruction");
    }
    {
        HistoryStore history(root);
        check(history.open(), qPrintable(history.error()));
        QJsonObject point = history.query(minute, minute + 59).value(QStringLiteral("cpu")).toArray().first().toObject();
        check(point.value(QStringLiteral("count")).toInt() == 3, "restart persists pending values");
        check(point.value(QStringLiteral("value")).toDouble() == 3, "restart merges weighted mean");
        // A distant sample keeps its real timestamp and leaves the missing interval empty.
        check(history.record(sample(minute - 7200 + 7, 2)), "gap sample");
        const QJsonArray gap = history.query(minute - 7200, minute + 59).value(QStringLiteral("cpu")).toArray();
        check(gap.size() == 2, "query must not synthesize samples inside gaps");
        check(gap.first().toObject().value(QStringLiteral("timestamp")).toDouble() == minute - 7200 + 7,
              "gap timestamp is an actual observation");
        for (int index = 0; index < 1000; ++index)
            check(history.record(sample(minute - 86400 + index * 60 + 3, 1 + index % 9)), "bounded query fixtures");
        const QJsonObject bounded = history.query(minute - 90000, minute + 59, 7);
        for (auto metric = bounded.begin(); metric != bounded.end(); ++metric)
            check(metric.value().isArray() && metric.value().toArray().size() <= 7, "bounded metric arrays");
        const QJsonArray capped = history.query(minute - 90000, minute + 59, 100000).value(QStringLiteral("cpu")).toArray();
        check(capped.size() <= 600 && capped.size() > 1, "hard cap of 600 points");
        const QJsonObject absent = history.query(minute + 3600, minute + 7200);
        check(absent.value(QStringLiteral("cpu")).toArray().isEmpty(), "empty range");
        check(history.record(sample(now - 32LL * 86400, 100)), "old data is safely discarded");
        history.flush();
        check(history.error().isEmpty(), qPrintable(history.error()));
        const QString dbPath = QDir(root).filePath(QStringLiteral("History/metrics.sqlite3"));
        for (const QString &path : {root, QDir(root).filePath(QStringLiteral("History"))}) {
            struct stat info {};
            check(stat(QFile::encodeName(path).constData(), &info) == 0 && (info.st_mode & 0777) == 0700,
                  "history directories are private");
        }
        for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
            struct stat info {};
            check(stat(QFile::encodeName(dbPath + suffix).constData(), &info) == 0 && (info.st_mode & 0777) == 0600,
                  "SQLite files are private");
        }
    }
    // Simulate a database last used more than a month ago, then open it again.
    const QString dbPath = QDir(root).filePath(QStringLiteral("History/metrics.sqlite3"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("retention-fixture"));
        db.setDatabaseName(dbPath);
        check(db.open(), "open retention fixture");
        QSqlQuery query(db);
        check(query.exec(QStringLiteral("INSERT INTO minute_metrics VALUES('cpu',1,9,9,9,1,1,1)")), "insert old stored row");
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("retention-fixture"));
    {
        HistoryStore history(root);
        check(history.open(), "reopen for retention cleanup");
        check(history.query(0, now).value(QStringLiteral("cpu")).toArray().size() <= 600, "long range bounded");
    }
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("retention-check"));
        db.setDatabaseName(dbPath);
        check(db.open(), "open retention verification");
        QSqlQuery query(db);
        query.prepare(QStringLiteral("SELECT count(*) FROM minute_metrics WHERE minute < ?"));
        query.addBindValue(((now - 31LL * 86400) / 60) * 60);
        check(query.exec() && query.next() && query.value(0).toInt() == 0, "31-day retention enforced on restart");
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("retention-check"));
    const QString blocked = QDir(temporary.path()).filePath(QStringLiteral("regular-file"));
    QFile file(blocked);
    check(file.open(QIODevice::WriteOnly), "create non-directory");
    file.close();
    HistoryStore invalid(blocked);
    check(!invalid.open() && !invalid.error().isEmpty(), "report storage errors");
    const QString linkedRoot = QDir(temporary.path()).filePath(QStringLiteral("linked"));
    check(symlink(QFile::encodeName(root).constData(), QFile::encodeName(linkedRoot).constData()) == 0, "symlink fixture");
    HistoryStore linked(linkedRoot);
    check(!linked.open(), "refuse symlinked history root");
}

void near(double actual, double expected, const char *message) {
    if (std::abs(actual - expected) > 1e-6) throw std::runtime_error(message);
}
QJsonObject metricSample(double timestamp, double value, bool withPressure = true) {
    QJsonObject sample{{"timestamp", timestamp}, {"cpu", QJsonObject{{"usage", .2}, {"iowait", value / 100}}},
        {"vm", QJsonObject{{"swapInBytesPerSecond", value * 100}, {"swapOutBytesPerSecond", value * 200}}}};
    if (withPressure) sample["pressure"] = QJsonObject{{"memory", QJsonObject{{"some", QJsonObject{{"avg10", value}}}}},
        {"io", QJsonObject{{"some", QJsonObject{{"avg10", value * 2}}}}}};
    return sample;
}
void withDatabase(const QString &path, const std::function<void(QSqlDatabase &)> &run) {
    const QString connection = "optimization-fixture-" + QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlDatabase database = QSqlDatabase::addDatabase("QSQLITE", connection);
        database.setDatabaseName(path); check(database.open(), qPrintable(database.lastError().text()));
        run(database); database.close();
    }
    QSqlDatabase::removeDatabase(connection);
}
void testHistoryMetrics() {
    QTemporaryDir temporary; check(temporary.isValid(), "history metrics isolated directory");
    const QString root = temporary.path() + "/old-profile", path = root + "/History/metrics.sqlite3";
    check(QDir().mkpath(root + "/History"), "create old database fixture path");
    const qint64 now = QDateTime::currentSecsSinceEpoch(), minute = now / 60 * 60 - 180;
    const qint64 expired = (now - 32LL * 86400) / 60 * 60, retained = (now - 30LL * 86400) / 60 * 60;
    withDatabase(path, [&](QSqlDatabase &database) {
        QSqlQuery query(database);
        check(query.exec("CREATE TABLE minute_metrics (metric TEXT NOT NULL, minute INTEGER NOT NULL, total REAL NOT NULL, minimum REAL NOT NULL, maximum REAL NOT NULL, count INTEGER NOT NULL, first_sample REAL NOT NULL, last_sample REAL NOT NULL, PRIMARY KEY(metric,minute))"), "create pre-existing 0.7.4 schema");
        query.prepare("INSERT INTO minute_metrics VALUES('cpu',?,0.75,0.75,0.75,1,?,?)");
        query.addBindValue(minute - 600); query.addBindValue(minute - 595); query.addBindValue(minute - 595);
        check(query.exec(), "pre-existing legacy metric");
        for (const auto &metric : {"cpu", "memoryPressure", "ioPressure", "cpuIowait", "swapIn", "swapOut"}) {
            query.prepare("INSERT INTO minute_metrics VALUES(?,?,0.5,0.5,0.5,1,?,?)");
            query.addBindValue(metric); query.addBindValue(expired); query.addBindValue(expired + 5); query.addBindValue(expired + 5);
            check(query.exec(), "expired legacy/new metric fixture");
        }
        query.prepare("INSERT INTO minute_metrics VALUES('memoryPressure',?,0.6,0.6,0.6,1,?,?)");
        query.addBindValue(retained); query.addBindValue(retained + 5); query.addBindValue(retained + 5);
        check(query.exec(), "inside retention boundary fixture");
    });
    {
        HistoryStore history(root); check(history.open(), qPrintable(history.error()));
        const auto legacy = history.query(minute - 600, minute - 540)["cpu"].toArray();
        check(legacy.size() == 1 && legacy.first().toObject()["value"].toDouble() == .75, "opening new version preserves old database metrics");
        check(history.record(metricSample(minute + 1.25, 10)), "first pressure metrics sample");
        check(history.record(metricSample(minute + 31.5, 30)), "second pressure metrics sample");
        check(history.record(metricSample(minute + 45, 80, false)), "unknown PSI sample still records other numeric metrics");
        auto invalidPressure = metricSample(minute + 50, 40, false);
        invalidPressure["pressure"] = QJsonObject{{"memory", QJsonValue::Null}, {"io", QJsonObject{{"some", QJsonObject{{"avg10", -1}}}}}};
        check(history.record(invalidPressure), "unsupported/malformed PSI preserves unknown values");
        check(history.record(QJsonObject{{"timestamp", double(minute + 55)}, {"cpu", QJsonObject{{"usage", .2}}},
            {"pressure", QJsonObject{{"memory", QJsonObject{{"some", QJsonObject{{"avg10", 101}}}}},
                {"io", QJsonObject{{"some", QJsonObject{{"avg10", "bad"}}}}}}}}), "out-of-range or nonnumeric PSI excluded while legacy metric records");
        const auto result = history.query(minute, minute + 59);
        const auto memoryPoint = result["memoryPressure"].toArray().first().toObject();
        near(memoryPoint["value"].toDouble(), .2, "memory PSI percentage is normalized once to fraction");
        near(memoryPoint["minimum"].toDouble(), .1, "memory pressure minimum"); near(memoryPoint["maximum"].toDouble(), .3, "memory pressure maximum");
        check(memoryPoint["count"].toInt() == 2, "unknown PSI never contributes artificial zeros or samples");
        near(memoryPoint["firstSample"].toDouble(), minute + 1.25, "pressure first observation timestamp");
        near(memoryPoint["lastSample"].toDouble(), minute + 31.5, "pressure last observation timestamp excludes unknowns");
        const auto ioPoint = result["ioPressure"].toArray().first().toObject();
        near(ioPoint["value"].toDouble(), .4, "IO pressure mean"); near(ioPoint["minimum"].toDouble(), .2, "IO pressure minimum"); near(ioPoint["maximum"].toDouble(), .6, "IO pressure maximum");
        for (const auto &metric : {"cpuIowait", "swapIn", "swapOut"}) {
            const auto point = result[metric].toArray().first().toObject();
            const double factor = QByteArray(metric) == "cpuIowait" ? .01 : QByteArray(metric) == "swapIn" ? 100 : 200;
            near(point["value"].toDouble(), 40 * factor, "iowait/swap minute mean uses all measured samples");
            near(point["minimum"].toDouble(), 10 * factor, "iowait/swap minute minimum");
            near(point["maximum"].toDouble(), 80 * factor, "iowait/swap minute maximum");
            check(point["count"].toInt() == 4, "iowait/swap measured interval count");
        }
        check(history.record(metricSample(minute + 60 + 5, 20, false)), "PSI unknown minute fixture");
        check(history.record(metricSample(minute + 120 + 7, 0)), "measured zero pressure after unknown minute");
        const auto gap = history.query(minute, minute + 179)["memoryPressure"].toArray();
        check(gap.size() == 2 && gap.last().toObject()["timestamp"].toDouble() == minute + 127, "unknown minute remains a history gap with true observation time");
        check(gap.last().toObject()["value"].toDouble() == 0, "measured zero PSI is retained rather than confused with unknown");
        check(history.record(metricSample(now - 32LL * 86400, 99)), "older samples are safely discarded");
        history.flush(); check(history.error().isEmpty(), qPrintable(history.error()));
    }
    {
        HistoryStore history(root); check(history.open(), qPrintable(history.error()));
        const auto result = history.query(minute, minute + 179);
        for (const auto &metric : {"memoryPressure", "ioPressure", "cpuIowait", "swapIn", "swapOut"})
            check(result.contains(metric) && !result[metric].toArray().isEmpty(), "new history keys persisted and survive restart");
        near(result["memoryPressure"].toArray().first().toObject()["value"].toDouble(), .2, "persisted pressure average survives restart");
        check(history.query(retained, retained + 59)["memoryPressure"].toArray().size() == 1, "inside 31-day pressure retention stays present");
        check(history.query(0, expired + 59)["memoryPressure"].toArray().isEmpty(), "expired pressure samples are gone");
    }
    withDatabase(path, [&](QSqlDatabase &database) {
        QSqlQuery query(database); query.prepare("SELECT COUNT(*) FROM minute_metrics WHERE minute < ?");
        query.addBindValue(((now - 31LL * 86400) / 60) * 60);
        check(query.exec() && query.next() && query.value(0).toInt() == 0, "same 31-day pruning applies to every metric");
        check(query.exec("SELECT COUNT(DISTINCT metric) FROM minute_metrics WHERE metric IN ('memoryPressure','ioPressure','cpuIowait','swapIn','swapOut')") &&
            query.next() && query.value(0).toInt() == 5, "new metrics physically persist in existing SQLite table");
    });
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        testHistory();
        QTextStream(stdout) << "PASS history: aggregation, restart, 31-day retention, gaps, bounds, private storage, errors\n";
        testHistoryMetrics();
        QTextStream(stdout) << "PASS pressure history: legacy database, normalized PSI/iowait/swap aggregation, unknown gaps, persistence, 31-day retention\n";
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << "FAIL history: " << error.what() << '\n';
        return 1;
    }
}
