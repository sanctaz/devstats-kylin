#include "history_store.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QSqlError>
#include <QSqlQuery>
#include <QTimer>
#include <QUuid>
#include <QVariant>
#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr qint64 RetentionSeconds = 31LL * 24 * 60 * 60;
const QStringList Metrics = {QStringLiteral("cpu"), QStringLiteral("memory"),
    QStringLiteral("diskRead"), QStringLiteral("diskWrite"),
    QStringLiteral("networkDownload"), QStringLiteral("networkUpload"), QStringLiteral("battery"),
    QStringLiteral("cpuIowait"), QStringLiteral("swapIn"), QStringLiteral("swapOut"),
    QStringLiteral("memoryPressure"), QStringLiteral("ioPressure"),
    QStringLiteral("schedulerLatency"), QStringLiteral("diskBusy")};

bool privateDirectory(const QString &path, QString &error) {
    const QByteArray encoded = QFile::encodeName(path);
    struct stat info {};
    if (lstat(encoded.constData(), &info) != 0) {
        if (errno != ENOENT || !QDir().mkpath(path) || lstat(encoded.constData(), &info) != 0) {
            error = QStringLiteral("Cannot create history directory: %1").arg(path);
            return false;
        }
    }
    if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || chmod(encoded.constData(), 0700) != 0) {
        error = QStringLiteral("History storage must be an owned directory: %1").arg(path);
        return false;
    }
    return true;
}

bool privateFile(const QString &path, bool create, QString &error) {
    const QByteArray encoded = QFile::encodeName(path);
    int fd = ::open(encoded.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | (create ? O_CREAT : 0), 0600);
    if (fd < 0) {
        if (!create && errno == ENOENT) return true;
        error = QStringLiteral("Cannot open private history file: %1 (%2)")
            .arg(path, QString::fromLocal8Bit(strerror(errno)));
        return false;
    }
    struct stat info {};
    const bool ok = fstat(fd, &info) == 0 && S_ISREG(info.st_mode) &&
        info.st_uid == getuid() && info.st_nlink == 1 && fchmod(fd, 0600) == 0;
    ::close(fd);
    if (!ok) error = QStringLiteral("History file must be an owned regular file: %1").arg(path);
    return ok;
}

QJsonObject emptyResult() {
    QJsonObject result;
    for (const QString &metric : Metrics) result.insert(metric, QJsonArray());
    return result;
}
}

HistoryStore::HistoryStore(const QString &dataRoot) : m_root(dataRoot) {}

HistoryStore::~HistoryStore() {
    m_flushTimer.reset();
    flush();
    if (m_database.isValid()) m_database.close();
    m_database = QSqlDatabase();
    if (!m_connection.isEmpty()) QSqlDatabase::removeDatabase(m_connection);
}

void HistoryStore::Aggregate::add(double value, double timestamp) {
    if (count == 0) {
        minimum = maximum = value;
        first = last = timestamp;
    } else {
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
        first = std::min(first, timestamp);
        last = std::max(last, timestamp);
    }
    total += value;
    ++count;
}

void HistoryStore::Aggregate::merge(const Aggregate &other) {
    if (!other.count) return;
    if (!count) { *this = other; return; }
    total += other.total;
    minimum = std::min(minimum, other.minimum);
    maximum = std::max(maximum, other.maximum);
    count += other.count;
    first = std::min(first, other.first);
    last = std::max(last, other.last);
}

bool HistoryStore::open() {
    if (m_database.isOpen()) return true;
    m_error.clear();
    const QString directory = QDir(m_root).filePath(QStringLiteral("History"));
    if (!privateDirectory(m_root, m_error) || !privateDirectory(directory, m_error)) return false;
    m_path = QDir(directory).filePath(QStringLiteral("metrics.sqlite3"));
    if (!privateFile(m_path, true, m_error) ||
        !privateFile(m_path + QStringLiteral("-wal"), false, m_error) ||
        !privateFile(m_path + QStringLiteral("-shm"), false, m_error)) return false;
    if (m_connection.isEmpty()) {
        m_connection = QStringLiteral("devstats-history-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connection);
    }
    m_database.setDatabaseName(m_path);
    m_database.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=3000"));
    if (!m_database.open()) {
        m_error = m_database.lastError().text();
        return false;
    }
    const QStringList statements = {
        QStringLiteral("PRAGMA journal_mode=WAL"),
        QStringLiteral("PRAGMA synchronous=NORMAL"),
        QStringLiteral("PRAGMA busy_timeout=3000"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS minute_metrics ("
            "metric TEXT NOT NULL, minute INTEGER NOT NULL, total REAL NOT NULL,"
            "minimum REAL NOT NULL, maximum REAL NOT NULL, count INTEGER NOT NULL,"
            "first_sample REAL NOT NULL, last_sample REAL NOT NULL, PRIMARY KEY(metric,minute))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS minute_metrics_time ON minute_metrics(minute)")
    };
    for (const QString &statement : statements) {
        QSqlQuery query(m_database);
        if (!query.exec(statement)) {
            m_error = query.lastError().text();
            m_database.close();
            return false;
        }
    }
    secureSidecars();
    if (!m_error.isEmpty() || !commit()) { m_database.close(); return false; }
    if (!m_flushTimer) {
        m_flushTimer.reset(new QTimer());
        m_flushTimer->setInterval(20000);
        QObject::connect(m_flushTimer.get(), &QTimer::timeout, m_flushTimer.get(), [this] { flush(); });
    }
    m_flushTimer->start();
    return true;
}

bool HistoryStore::record(const QJsonObject &snapshot) {
    if (!m_database.isOpen()) { m_error = QStringLiteral("History database is not open"); return false; }
    const QJsonValue rawTimestamp = snapshot.value(QStringLiteral("timestamp"));
    const double timestamp = rawTimestamp.toDouble(-1);
    if (!rawTimestamp.isDouble() || !std::isfinite(timestamp) || timestamp < 0 || timestamp > 1.0e12) {
        m_error = QStringLiteral("Snapshot has an invalid timestamp");
        return false;
    }
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - RetentionSeconds;
    if (timestamp < cutoff) return true;
    const qint64 minute = qint64(std::floor(timestamp / 60.0)) * 60;
    struct Field { const char *metric; const char *group; const char *key; };
    static const Field fields[] = {
        {"cpu", "cpu", "usage"}, {"memory", "memory", "fraction"},
        {"diskRead", "diskIO", "readBytesPerSecond"}, {"diskWrite", "diskIO", "writeBytesPerSecond"},
        {"networkDownload", "network", "downloadBytesPerSecond"},
        {"networkUpload", "network", "uploadBytesPerSecond"}, {"battery", "battery", "level"},
        {"cpuIowait", "cpu", "iowait"}, {"swapIn", "vm", "swapInBytesPerSecond"}, {"swapOut", "vm", "swapOutBytesPerSecond"},
        {"schedulerLatency", "responsiveness", "schedulerMaxMs"}, {"diskBusy", "diskIO", "busiestUtilization"}
    };
    bool recorded = false;
    for (const Field &field : fields) {
        const QJsonValue value = snapshot.value(QLatin1String(field.group)).toObject().value(QLatin1String(field.key));
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0) continue;
        m_pending[minute][QLatin1String(field.metric)].add(value.toDouble(), timestamp);
        recorded = true;
    }
    for (const QString resource : {QStringLiteral("memory"), QStringLiteral("io")}) {
        const auto value = snapshot.value("pressure").toObject().value(resource).toObject().value("some").toObject().value("avg10");
        if (!value.isDouble() || !std::isfinite(value.toDouble()) || value.toDouble() < 0 || value.toDouble() > 100) continue;
        m_pending[minute][resource == "memory" ? "memoryPressure" : "ioPressure"].add(value.toDouble() / 100., timestamp);
        recorded = true;
    }
    if (!recorded) { m_error = QStringLiteral("Snapshot has no numeric metrics"); return false; }
    // Bound memory even when importing samples without an event loop.
    if (m_pending.size() >= 240) return commit();
    return true;
}

void HistoryStore::secureSidecars() {
    for (const QString &suffix : {QString(), QStringLiteral("-wal"), QStringLiteral("-shm")}) {
        QString permissionError;
        if (!privateFile(m_path + suffix, false, permissionError)) m_error = permissionError;
    }
}

bool HistoryStore::commit() {
    if (!m_database.isOpen()) return false;
    if (!m_database.transaction()) { m_error = m_database.lastError().text(); return false; }
    QSqlQuery select(m_database);
    QSqlQuery write(m_database);
    // SQLite shipped with Qt 5.12 may predate UPSERT; explicit merge keeps its contract small.
    if (!select.prepare(QStringLiteral("SELECT total,minimum,maximum,count,first_sample,last_sample "
            "FROM minute_metrics WHERE metric=? AND minute=?")) ||
        !write.prepare(QStringLiteral("INSERT OR REPLACE INTO minute_metrics "
            "(metric,minute,total,minimum,maximum,count,first_sample,last_sample) VALUES(?,?,?,?,?,?,?,?)"))) {
        m_error = select.lastError().isValid() ? select.lastError().text() : write.lastError().text();
        m_database.rollback();
        return false;
    }
    const qint64 cutoff = QDateTime::currentSecsSinceEpoch() - RetentionSeconds;
    for (auto minute = m_pending.cbegin(); minute != m_pending.cend(); ++minute) {
        if (minute.key() + 60 <= cutoff) continue;
        for (auto metric = minute.value().cbegin(); metric != minute.value().cend(); ++metric) {
            Aggregate combined = metric.value();
            select.bindValue(0, metric.key());
            select.bindValue(1, minute.key());
            if (!select.exec()) { m_error = select.lastError().text(); m_database.rollback(); return false; }
            if (select.next()) {
                Aggregate previous;
                previous.total = select.value(0).toDouble();
                previous.minimum = select.value(1).toDouble();
                previous.maximum = select.value(2).toDouble();
                previous.count = select.value(3).toLongLong();
                previous.first = select.value(4).toDouble();
                previous.last = select.value(5).toDouble();
                combined.merge(previous);
            }
            select.finish();
            write.bindValue(0, metric.key());
            write.bindValue(1, minute.key());
            write.bindValue(2, combined.total);
            write.bindValue(3, combined.minimum);
            write.bindValue(4, combined.maximum);
            write.bindValue(5, combined.count);
            write.bindValue(6, combined.first);
            write.bindValue(7, combined.last);
            if (!write.exec()) { m_error = write.lastError().text(); m_database.rollback(); return false; }
        }
    }
    QSqlQuery prune(m_database);
    prune.prepare(QStringLiteral("DELETE FROM minute_metrics WHERE minute < ?"));
    prune.addBindValue((cutoff / 60) * 60);
    if (!prune.exec() || !m_database.commit()) {
        m_error = prune.lastError().isValid() ? prune.lastError().text() : m_database.lastError().text();
        m_database.rollback();
        return false;
    }
    m_pending.clear();
    m_error.clear();
    secureSidecars();
    return m_error.isEmpty();
}

void HistoryStore::flush() { if (m_database.isOpen()) commit(); }
QString HistoryStore::error() const { return m_error; }

QJsonObject HistoryStore::query(qint64 startEpochSeconds, qint64 endEpochSeconds, int maxPoints) {
    QJsonObject result = emptyResult();
    if (startEpochSeconds < 0 || endEpochSeconds < startEpochSeconds || endEpochSeconds > 1000000000000LL) {
        m_error = QStringLiteral("Invalid history time range");
        result.insert(QStringLiteral("error"), m_error);
        return result;
    }
    if (!m_database.isOpen() || !commit()) {
        if (m_error.isEmpty()) m_error = QStringLiteral("History database is not open");
        result.insert(QStringLiteral("error"), m_error);
        return result;
    }
    maxPoints = std::max(1, std::min(600, maxPoints));
    const qint64 width = std::max<qint64>(60, (endEpochSeconds - startEpochSeconds + maxPoints) / maxPoints);
    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("SELECT metric,minute,total,minimum,maximum,count,first_sample,last_sample "
        "FROM minute_metrics WHERE minute>=? AND minute<=? AND last_sample>=? AND first_sample<=? "
        "ORDER BY minute"));
    query.addBindValue((startEpochSeconds / 60) * 60);
    query.addBindValue((endEpochSeconds / 60) * 60);
    query.addBindValue(startEpochSeconds);
    query.addBindValue(endEpochSeconds);
    if (!query.exec()) {
        m_error = query.lastError().text();
        result.insert(QStringLiteral("error"), m_error);
        return result;
    }
    QMap<QString, QMap<qint64, Aggregate>> bins;
    while (query.next()) {
        const QString metric = query.value(0).toString();
        if (!Metrics.contains(metric)) continue;
        const qint64 minute = query.value(1).toLongLong();
        const qint64 bucket = std::min<qint64>(maxPoints - 1, std::max<qint64>(0, (minute - startEpochSeconds) / width));
        Aggregate aggregate;
        aggregate.total = query.value(2).toDouble();
        aggregate.minimum = query.value(3).toDouble();
        aggregate.maximum = query.value(4).toDouble();
        aggregate.count = query.value(5).toLongLong();
        aggregate.first = query.value(6).toDouble();
        aggregate.last = query.value(7).toDouble();
        if (aggregate.count > 0) bins[metric][bucket].merge(aggregate);
    }
    for (const QString &metric : Metrics) {
        QJsonArray points;
        const auto values = bins.value(metric);
        for (auto value = values.cbegin(); value != values.cend(); ++value) {
            const Aggregate &aggregate = value.value();
            points.append(QJsonObject {
                {QStringLiteral("timestamp"), aggregate.last},
                {QStringLiteral("value"), aggregate.total / aggregate.count},
                {QStringLiteral("minimum"), aggregate.minimum}, {QStringLiteral("maximum"), aggregate.maximum},
                {QStringLiteral("count"), double(aggregate.count)},
                {QStringLiteral("firstSample"), aggregate.first}, {QStringLiteral("lastSample"), aggregate.last}
            });
        }
        result.insert(metric, points);
    }
    return result;
}
