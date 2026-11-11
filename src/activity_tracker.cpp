#include "activity_tracker.h"
#include "i18n.h"
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QLocale>
#include <QMap>
#include <QSet>
#include <QStandardPaths>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace {
QJsonValue unknown() { return QJsonValue(QJsonValue::Null); }
bool measured(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0;
}
QString canonical(const QString &path) {
    if (!QDir::isAbsolutePath(path)) return {};
    const auto resolved = QFileInfo(path).canonicalFilePath();
    return QDir::cleanPath(resolved.isEmpty() ? path : resolved);
}
// Desktop Exec is not shell syntax. Parse literal double-quoted/escaped tokens;
// field codes are arguments only, and nothing from metadata is ever executed.
QStringList execTokens(const QString &text) {
    QStringList tokens;
    QString token;
    bool quoted = false, escaped = false, started = false;
    for (const QChar ch : text) {
        if (escaped) { token += ch; escaped = false; started = true; }
        else if (ch == '\\') escaped = true;
        else if (ch == '"') { quoted = !quoted; started = true; }
        else if (ch.isSpace() && !quoted) {
            if (started) { tokens.append(token); token.clear(); started = false; }
        } else { token += ch; started = true; }
    }
    if (quoted || escaped) return {};
    if (started) tokens.append(token);
    return tokens;
}
QString desktopExecutable(const QString &exec) {
    auto tokens = execTokens(exec);
    if (tokens.isEmpty()) return {};
    QString command = tokens.takeFirst();
    if (QFileInfo(command).fileName() == "env") {
        while (!tokens.isEmpty() && tokens.first().contains('=') && !tokens.first().startsWith('-')) tokens.takeFirst();
        if (tokens.isEmpty() || tokens.first().startsWith('-')) return {};
        command = tokens.takeFirst();
    }
    // Generic interpreters/launchers do not establish a desktop app identity.
    const QString base = QFileInfo(command).fileName();
    const QSet<QString> generic = {"sh", "bash", "dash", "zsh", "fish", "env", "flatpak", "snap", "java", "node", "nodejs", "perl", "ruby"};
    if (generic.contains(base) || base.startsWith("python") || command.contains('%')) return {};
    if (!QDir::isAbsolutePath(command)) command = QStandardPaths::findExecutable(command);
    return canonical(command);
}
bool safeHelperRoot(const QString &root) {
    // Installed app directories are useful containment evidence. Shared binary
    // directories and Terminal=true desktops cannot absorb arbitrary descendants.
    static const QSet<QString> shared = {"/", "/usr", "/usr/bin", "/usr/sbin", "/usr/lib", "/usr/libexec",
        "/usr/share", "/usr/local", "/usr/local/bin", "/usr/local/lib", "/usr/local/share", "/bin", "/sbin", "/lib", "/lib64", "/opt"};
    return !root.isEmpty() && !shared.contains(root) && root != QDir::homePath()
        && !QSet<QString>{"bin", "sbin", "libexec"}.contains(QFileInfo(root).fileName()) && root.count('/') >= 2;
}
bool contained(const QString &path, const QString &root) { return path.startsWith(root + '/'); }
QString processKey(const QJsonObject &row) {
    return QString::number(qint64(row.value("pid").toDouble())) + ':' + QString::number(qint64(row.value("startTicks").toDouble()));
}
}

RecentActivityTracker::RecentActivityTracker(const QStringList &applicationDirectories) : directories_(applicationDirectories) {
    if (directories_.isEmpty()) {
        QString dataHome = qEnvironmentVariable("XDG_DATA_HOME");
        if (!QDir::isAbsolutePath(dataHome)) dataHome = QDir::homePath() + "/.local/share";
        directories_.append(dataHome + "/applications");
        QStringList dataDirs = qEnvironmentVariable("XDG_DATA_DIRS", "/usr/local/share:/usr/share").split(':', QString::SkipEmptyParts);
        for (const auto &path : dataDirs) if (QDir::isAbsolutePath(path)) directories_.append(path + "/applications");
        directories_.removeDuplicates();
    }
}

void RecentActivityTracker::reset() {
    entries_.clear(); applications_.clear(); previousTime_ = windowStart_ = -1;
}

void RecentActivityTracker::refreshMetadata(double now) {
    const bool english = DevStatsLanguage::isEnglish();
    if (metadataTime_ >= 0 && now >= metadataTime_ && now - metadataTime_ < 300 && english == metadataEnglish_) return;
    metadataTime_ = now;
    metadataEnglish_ = english;
    desktops_.clear();
    QSet<QString> seenIds;
    int examined = 0;
    // Application names follow the interface language. A desktop entry's plain Name is
    // English by convention; its Chinese name is asked for even on a system in another language.
    const QString system = QLocale().name();
    const QString locale = english ? QString() : system.startsWith(QLatin1String("zh")) ? system : QStringLiteral("zh_CN");
    for (const auto &directory : directories_) {
        QDirIterator files(directory, {"*.desktop"}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext() && examined < 4096 && desktops_.size() < 2048) {
            const QString path = files.next(); ++examined;
            QString id = QDir(directory).relativeFilePath(path); id.replace('/', '-');
            if (seenIds.contains(id)) continue;
            seenIds.insert(id);
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly) || file.size() > 65536) continue;
            QMap<QString, QString> values;
            bool desktop = false;
            for (const auto &raw : file.read(65536).split('\n')) {
                const QString line = QString::fromUtf8(raw).trimmed();
                if (line.startsWith('[')) { desktop = line == "[Desktop Entry]"; continue; }
                const int equal = line.indexOf('=');
                if (desktop && equal > 0 && !line.startsWith('#')) values.insert(line.left(equal), line.mid(equal + 1));
            }
            if (values.value("Type") != "Application" || values.value("Hidden") == "true") continue;
            const QString executable = desktopExecutable(values.value("Exec"));
            if (executable.isEmpty() || desktops_.contains(executable)) continue;
            QString name = locale.isEmpty() ? QString() : values.value("Name[" + locale + ']');
            if (name.isEmpty() && !locale.isEmpty()) name = values.value("Name[" + locale.section('_', 0, 0) + ']');
            if (name.isEmpty()) name = values.value("Name");
            if (name.isEmpty()) name = QFileInfo(executable).fileName();
            desktops_.insert(executable, {name, path, QFileInfo(executable).absolutePath(), values.value("Terminal") == "true" || values.value("Categories").split(';').contains("TerminalEmulator")});
        }
        if (examined >= 4096 || desktops_.size() >= 2048) break;
    }
}

void RecentActivityTracker::apply(QJsonObject &snapshot) {
    const double now = snapshot.value("processMonotonicSeconds").toDouble(-1);
    if (!std::isfinite(now) || now < 0) {
        reset(); snapshot["activityApplications"] = QJsonArray(); snapshot["activityObservationSeconds"] = 0; return;
    }
    const double elapsed = now - previousTime_;
    // A duplicate cached scan adds no interval. The controller normally avoids
    // calling apply for cached rows; retaining the contract here prevents reset.
    if (previousTime_ >= 0 && elapsed == 0) return;
    if (previousTime_ >= 0 && (elapsed < 0 || elapsed > 30)) reset();
    refreshMetadata(now);
    if (windowStart_ < 0) windowStart_ = now;
    const double start = std::max(windowStart_, now - 60);
    const double duration = now - start;
    const auto rows = snapshot.value("processes").toArray();
    struct Path { QString executable, name; bool shell = false; };
    struct Process { qint64 parent; QString executable, application; bool shell; };
    // Paths and ancestry belong to this scan only. Resolve each distinct input
    // path once, so helpers sharing a binary and repeated ancestors do not repeat
    // filesystem lookups. A later scan must observe changed symlink targets.
    QHash<QString, Path> paths;
    QHash<qint64, Process> byPid;
    byPid.reserve(rows.size());
    QHash<QString, QJsonObject> metadata;
    static const QSet<QString> shells = {"sh", "bash", "dash", "zsh", "fish", "ksh", "csh", "tcsh"};
    for (const auto &value : rows) {
        const auto process = value.toObject();
        const qint64 pid = qint64(process.value(QLatin1String("pid")).toDouble());
        if (pid <= 0) continue;
        const QString rawPath = process.value(QLatin1String("executablePath")).toString();
        auto path = paths.constFind(rawPath);
        if (path == paths.cend()) {
            const QString executable = canonical(rawPath);
            const QString name = QFileInfo(executable).fileName();
            path = paths.insert(rawPath, {executable, name, shells.contains(name)});
        }
        const QString &executable = path->executable;
        const QString identity = executable.isEmpty() ? "pid:" + processKey(process) : "exe:" + executable;
        byPid.insert(pid, {qint64(process.value(QLatin1String("parentPID")).toDouble()), executable, identity, path->shell});
        if (metadata.contains(identity)) continue;
        const auto desktop = desktops_.constFind(executable);
        metadata.insert(identity, {{"identity", identity}, {"name", desktop == desktops_.cend()
            ? (executable.isEmpty() ? process.value(QLatin1String("name")).toString() : path->name) : desktop->name},
            {"executablePath", executable}, {"desktopFile", desktop == desktops_.cend() ? QString() : desktop->file}});
    }
    // Only an exact desktop executable ancestor plus an app-private directory
    // can establish a helper relationship. Names and generic parentage never do.
    for (auto it = byPid.begin(); it != byPid.end(); ++it) {
        const QString &executable = it->executable;
        if (executable.isEmpty() || desktops_.contains(executable)) continue;
        qint64 parent = it->parent;
        QSet<qint64> visited;
        for (int depth = 0; depth < 64 && !visited.contains(parent); ++depth) {
            const auto ancestor = byPid.constFind(parent);
            if (ancestor == byPid.cend()) break;
            visited.insert(parent);
            const QString &parentExe = ancestor->executable;
            const auto desktop = desktops_.constFind(parentExe);
            if (desktop != desktops_.cend()) {
                if (!desktop->terminal && safeHelperRoot(desktop->root) && contained(executable, desktop->root))
                    it->application = "exe:" + parentExe;
                break;
            }
            // A shell/interpreter in between is not evidence of app ownership.
            if (ancestor->shell) break;
            parent = ancestor->parent;
        }
    }
    struct Scan { double cpuSeconds = 0; bool measured = false, complete = true; QJsonArray processes; };
    QHash<QString, Scan> scans;
    QSet<qint64> alive;
    QJsonArray result;
    for (const auto &value : rows) {
        auto process = value.toObject();
        const qint64 pid = qint64(process.value(QLatin1String("pid")).toDouble());
        const auto indexed = byPid.constFind(pid);
        if (indexed == byPid.cend()) continue;
        const qint64 identity = qint64(process.value(QLatin1String("startTicks")).toDouble());
        const QString application = indexed->application;
        alive.insert(pid);
        auto &entry = entries_[pid];
        if (entry.time >= 0 && entry.application != application) scans[entry.application].complete = false;
        auto &scan = scans[application];
        const bool stable = entry.time >= 0 && entry.identity == identity && entry.application == application;
        const auto cpu = process.value(QLatin1String("cpuPercent"));
        const bool valid = stable && now > entry.time && now - entry.time <= 30 && measured(cpu);
        if (!valid) {
            entry = Entry(); entry.identity = identity;
            scan.complete = false;
        } else {
            const double seconds = cpu.toDouble() / 100 * (now - entry.time);
            entry.intervals.push_back({entry.time, now, seconds});
            scan.cpuSeconds += seconds; scan.measured = true;
        }
        entry.time = now; entry.application = application;
        while (!entry.intervals.empty() && entry.intervals.front().end <= now - 60) entry.intervals.pop_front();
        double processDuration = 0, processSeconds = 0;
        for (const auto &interval : entry.intervals) {
            const double included = interval.end - std::max(interval.start, now - 60);
            if (included > 0) { processDuration += included; processSeconds += interval.cpuSeconds * included / (interval.end - interval.start); }
        }
        // insert() writes the field once; operator[] would first add a null and then replace it.
        process.insert(QStringLiteral("cpuAveragePercent"), processDuration > 0 ? QJsonValue(processSeconds / processDuration * 100) : unknown());
        process.insert(QStringLiteral("cpuObservationSeconds"), processDuration);
        process.insert(QStringLiteral("applicationIdentity"), application);
        scan.processes.append(process); result.append(process);
        auto &app = applications_[application];
        app.metadata = metadata.value(application); app.lastSeen = now;
    }
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!alive.contains(it.key())) { scans[it->application].complete = false; it = entries_.erase(it); }
        else ++it;
    }
    // Reused PIDs and changed executable identities also leave a partial interval
    // in their former application; no unknown work is silently counted as zero.
    if (previousTime_ >= 0) {
        for (auto it = scans.cbegin(); it != scans.cend(); ++it)
            applications_[it.key()].intervals.push_back({previousTime_, now, it->cpuSeconds, it->measured, it->complete});
    }
    previousTime_ = now;
    QJsonArray apps;
    for (auto it = applications_.begin(); it != applications_.end();) {
        auto &app = it.value();
        while (!app.intervals.empty() && app.intervals.front().end <= start) app.intervals.pop_front();
        const auto scan = scans.value(it.key());
        if (scan.processes.isEmpty() && (app.intervals.empty() || app.lastSeen <= now - 60)) { it = applications_.erase(it); continue; }
        double cpuSeconds = 0, incomplete = 0;
        bool hasMeasurement = false;
        for (const auto &interval : app.intervals) {
            const double included = interval.end - std::max(interval.start, start);
            if (included <= 0) continue;
            cpuSeconds += interval.cpuSeconds * included / (interval.end - interval.start);
            if (!interval.complete) incomplete += included;
            hasMeasurement |= interval.measured;
        }
        auto object = app.metadata;
        object.insert(QStringLiteral("processes"), scan.processes); object.insert(QStringLiteral("processCount"), scan.processes.size());
        object.insert(QStringLiteral("cpuAveragePercent"), duration > 0 && hasMeasurement ? QJsonValue(cpuSeconds / duration * 100) : unknown());
        object.insert(QStringLiteral("cpuObservationSeconds"), duration);
        object.insert(QStringLiteral("cpuCoverageFraction"), duration > 0 ? std::max(0.0, 1 - incomplete / duration) : 0);
        object.insert(QStringLiteral("cpuComplete"), duration > 0 && incomplete == 0);
        object.insert(QStringLiteral("noReadableProcesses"), scan.processes.isEmpty());
        for (const QString &field : {QStringLiteral("ioReadBytesPerSecond"), QStringLiteral("ioWriteBytesPerSecond")}) {
            double total = 0; bool complete = !scan.processes.isEmpty(), any = false;
            for (const auto &process : scan.processes) {
                const auto rate = process.toObject().value(field);
                complete &= measured(rate);
                if (measured(rate)) { total += rate.toDouble(); any = true; }
            }
            object.insert(field, any ? QJsonValue(total) : unknown()); object.insert(field + QStringLiteral("Complete"), complete);
        }
        apps.append(object); ++it;
    }
    // Inactive metadata cannot grow without bound during rapid process churn.
    if (applications_.size() > scans.size() + 4096) {
        QStringList inactive;
        for (auto it = applications_.cbegin(); it != applications_.cend(); ++it) if (!scans.contains(it.key())) inactive.append(it.key());
        std::sort(inactive.begin(), inactive.end(), [this](const QString &a, const QString &b) { return applications_[a].lastSeen < applications_[b].lastSeen; });
        for (const auto &key : inactive) { if (applications_.size() <= scans.size() + 4096) break; applications_.remove(key); }
    }
    QVector<QJsonObject> ordered;
    for (const auto &value : apps) ordered.append(value.toObject());
    std::sort(ordered.begin(), ordered.end(), [](const QJsonObject &left, const QJsonObject &right) {
        const double l = left.value("cpuAveragePercent").toDouble(-1), r = right.value("cpuAveragePercent").toDouble(-1);
        return l == r ? left.value("identity").toString() < right.value("identity").toString() : l > r;
    });
    apps = QJsonArray();
    for (const auto &object : ordered) apps.append(object);
    snapshot["processes"] = result;
    snapshot["activityApplications"] = apps;
    snapshot["activityObservationSeconds"] = duration;
    snapshot["activityBasis"] = "Application sums of measured CPU seconds over one shared recent window; partial values are lower bounds. Disk IO is separate, not energy.";
}
