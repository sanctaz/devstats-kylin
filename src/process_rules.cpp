#include "process_rules.h"
#include "process_actions.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <cerrno>
#include <cmath>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr int MaximumRules = 32, MaximumLog = 100, MaximumCandidates = 4096;
constexpr int MaximumActions = 8;
constexpr qint64 IdentityCooldown = 60000, MaximumBackoff = 300000;
constexpr qint64 MaximumLogBytes = 2 * 1024 * 1024;
constexpr qint64 KillVerifyMs = 5000, MaximumMeasuredIntervalMs = 15000;
constexpr int MaximumPending = 64;
const QFileDevice::Permissions PrivateFile = QFileDevice::ReadOwner | QFileDevice::WriteOwner;

QJsonObject invalid(const QString &message) {
    return {{"ok", false}, {"code", "invalidRules"}, {"message", message}};
}

bool plainText(const QString &text, int maximum) {
    if (text.isEmpty() || text.size() > maximum) return false;
    for (const auto character : text)
        if (character.isNull() || character.isSpace() || character.category() == QChar::Other_Control)
            return false;
    return true;
}

bool nativeSource() {
    // An injected sampler fixture must never become an action source. Every
    // actual attempt is additionally revalidated against native /proc by the
    // existing ProcessActions policy immediately before descriptor-based SIGTERM.
    return QDir::cleanPath(qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc")) == "/proc" &&
           QDir::cleanPath(qEnvironmentVariable("DEVSTATS_SYS_ROOT", "/sys")) == "/sys";
}

QString identityKey(const QJsonObject &process) {
    return QString::number(process.value("pid").toDouble(), 'f', 0) + '/' +
           QString::number(process.value("startTicks").toDouble(), 'f', 0) + '/' +
           process.value("executablePath").toString();
}

QJsonObject candidate(const QJsonObject &process, const QString &id, const QString &value) {
    return {{"ruleId", id}, {"ruleValue", value}, {"pid", process.value("pid")},
            {"startTicks", process.value("startTicks")}, {"executablePath", process.value("executablePath")},
            {"cpuPercent", process.value("cpuPercent")},
            {"name", QFileInfo(process.value("executablePath").toString()).fileName()}};
}

QString seconds(double value) { return QString::number(value, 'f', 0) + QStringLiteral(" 秒"); }
}

ProcessRules::ProcessRules(QString dataRoot) : dataRoot_(std::move(dataRoot)) {}

QJsonObject ProcessRules::configure(bool enabled, const QJsonArray &rules) {
    if (rules.size() > MaximumRules) return invalid(QStringLiteral("最多可设置 32 条自动查杀规则。"));
    QVector<Rule> parsed;
    QJsonArray normalized;
    QSet<QString> ids, targets;
    bool active = false;
    for (const auto &entry : rules) {
        if (!entry.isObject()) return invalid(QStringLiteral("规则格式无效。"));
        const auto object = entry.toObject();
        Rule rule{object.value("id").toString(), object.value("match").toString(),
                  object.value("value").toString(), object.value("enabled").toBool()};
        if (!plainText(rule.id, 64) || ids.contains(rule.id) || !object.value("enabled").isBool())
            return invalid(QStringLiteral("每条规则需要唯一标识及明确的启用状态。"));
        if (rule.match == "path") {
            if (rule.value.isEmpty() || rule.value.size() > 4096 || rule.value.toUtf8().size() > 4096 || rule.value.contains(QChar::Null) ||
                !QDir::isAbsolutePath(rule.value) || rule.value != QDir::cleanPath(rule.value))
                return invalid(QStringLiteral("请选择完整、规范的可执行文件绝对路径。"));
            const QFileInfo file(rule.value);
            const QString canonical = file.canonicalFilePath();
            if (file.exists()) {
                if (canonical.isEmpty() || !file.isFile() || !file.isExecutable())
                    return invalid(QStringLiteral("现有路径必须指向可执行文件。"));
                rule.value = canonical;
            }
        } else if (rule.match == "name") {
            if (!plainText(rule.value, 255) || rule.value.toUtf8().size() > 255 ||
                rule.value == "." || rule.value == ".." || rule.value.contains('/') ||
                rule.value.contains('\\'))
                return invalid(QStringLiteral("程序名只接受完整文件名，不能包含路径、空白或参数。"));
            for (const auto character : QStringLiteral("*?[]{}()^$|"))
                if (rule.value.contains(character))
                    return invalid(QStringLiteral("程序名必须精确匹配，不能使用通配符或正则表达式。"));
        } else {
            return invalid(QStringLiteral("请选择完整程序路径或精确程序名。"));
        }
        const QString target = rule.match + ':' + rule.value;
        if (targets.contains(target)) return invalid(QStringLiteral("同一个程序无需添加重复规则。"));
        QJsonObject normal{{"id", rule.id}, {"enabled", rule.enabled}, {"match", rule.match}, {"value", rule.value}};
        const auto limit = object.value("cpuAbovePercent");
        if (!limit.isUndefined() && !limit.isNull()) {
            const double value = limit.toDouble(-1);
            if (!limit.isDouble() || !std::isfinite(value) || value < 1 || value > 100000)
                return invalid(QStringLiteral("CPU 阈值需在 1% 到 100000% 之间（100% 为占满一个核心）。"));
            rule.cpuAbove = value;
            normal["cpuAbovePercent"] = value;
            const auto sustain = object.value("sustainSeconds");
            if (!sustain.isUndefined()) {
                if (!sustain.isDouble() || sustain.toDouble() < 0 || sustain.toDouble() > 3600)
                    return invalid(QStringLiteral("持续时间需在 0 到 3600 秒之间。"));
                rule.sustainSeconds = sustain.toInt();
            }
            normal["sustainSeconds"] = rule.sustainSeconds;
        }
        rule.forceKill = object.value("forceKill").toBool(false);
        normal["forceKill"] = rule.forceKill;
        const auto grace = object.value("graceSeconds");
        if (!grace.isUndefined()) {
            if (!grace.isDouble() || grace.toDouble() < 1 || grace.toDouble() > 120)
                return invalid(QStringLiteral("等待时间需在 1 到 120 秒之间。"));
            rule.graceSeconds = grace.toInt();
        }
        normal["graceSeconds"] = rule.graceSeconds;
        ids.insert(rule.id); targets.insert(target);
        active |= rule.enabled;
        parsed.append(rule);
        normalized.append(normal);
    }
    // Reapplying settings does not erase cooldowns or executable respawn history.
    enabled_ = enabled; activeRules_ = active;
    rules_ = std::move(parsed); ruleJson_ = normalized;
    // A changed rule starts counting afresh; it never inherits a partial countdown.
    watches_.clear();
    unresponsive_.clear();
    if (!logLoaded_) loadLog();
    auto result = state(); result["ok"] = true; result["code"] = "configured";
    return result;
}

bool ProcessRules::usableSnapshot(const QJsonObject &snapshot) const {
    const auto stamp = snapshot.value("processMonotonicSeconds");
    if (!nativeSource() || !snapshot.value("processesFresh").toBool() ||
        !stamp.isDouble() || !std::isfinite(stamp.toDouble()) || stamp.toDouble() < 0 ||
        !snapshot.value("processes").isArray()) return false;
    if (snapshot.contains("processSampleAgeSeconds")) {
        const auto age = snapshot.value("processSampleAgeSeconds");
        if (!age.isDouble() || !std::isfinite(age.toDouble()) || age.toDouble() < 0 || age.toDouble() > 5) return false;
    }
    return true;
}

const ProcessRules::Rule *ProcessRules::matching(const QJsonObject &process) const {
    const QString executable = process.value("executablePath").toString();
    if (executable.isEmpty() || executable.size() > 4096 || executable.toUtf8().size() > 4096 || !QDir::isAbsolutePath(executable) ||
        executable != QDir::cleanPath(executable) || executable.endsWith(" (deleted)")) return nullptr;
    QString name;
    for (const auto &rule : rules_) {
        if (!rule.enabled) continue;
        if (rule.match == "path" && rule.value == executable) return &rule;
        if (rule.match == "name") {
            if (name.isEmpty()) name = QFileInfo(executable).fileName();
            if (rule.value == name) return &rule;
        }
    }
    return nullptr;
}

QString ProcessRules::conditionText(const Rule &rule) const {
    if (rule.cpuAbove < 0) return QStringLiteral("运行时立即结束");
    const QString limit = QStringLiteral("CPU 超过 %1%").arg(rule.cpuAbove, 0, 'f', 0);
    return rule.sustainSeconds ? limit + QStringLiteral("，持续 %1").arg(seconds(rule.sustainSeconds)) : limit;
}

namespace {
// Ownership, protection, kernel support, and whether this exact identity still exists
// do not change for one process identity, so those answers are kept. A failed read can:
// /proc could not be read in that moment, or an ancestor exited while the lineage was
// walked. Remembering that would protect the process for its whole life.
bool lasting(const QJsonObject &result) {
    static const QSet<QString> passing{"identityUnavailable", "ancestryUnavailable", "operationFailed"};
    return result.value("ok").toBool() || !passing.contains(result.value("code").toString());
}
}

QJsonObject ProcessRules::inspection(const QJsonObject &process, const QString &identity) {
    const auto found = inspections_.constFind(identity);
    if (found != inspections_.cend()) return found.value();
    const auto result = inspector_ ? inspector_(process) : ProcessActions().inspectIdentity(process);
    if (lasting(result)) inspections_.insert(identity, result);
    return result;
}

void ProcessRules::trigger(const Rule &rule, const QJsonObject &process, const QString &identity, qint64 now, double average,
                           QHash<QString, ExecutableAttempt> &waves, int &attempts) {
    const QString executable = process.value("executablePath").toString();
    if (rule.cpuAbove < 0) {
        // Repeated restarts of an always-ended program back off. A CPU rule has its
        // own limiter: a fresh process must first exceed the limit all over again.
        const auto prior = executableAttempts_.constFind(executable);
        auto wave = waves.find(executable);
        if (wave == waves.end()) {
            if (prior != executableAttempts_.cend() && now - prior->at < prior->delay) return;
            ExecutableAttempt next;
            next.at = now;
            if (prior != executableAttempts_.cend()) next.delay = prior->delay;
            wave = waves.insert(executable, next);
        }
        if (prior != executableAttempts_.cend() && !prior->identities.contains(identity))
            wave->delay = qMin(MaximumBackoff, prior->delay * 2);
        wave->identities.insert(identity);
    }
    identityAttempts_.insert(identity, now);
    ++attempts;
    const auto result = ProcessActions().execute(process, QStringLiteral("terminate"));
    QJsonObject fields{{"condition", conditionText(rule)}, {"ok", result.value("ok").toBool()}, {"code", result.value("code")}};
    if (average >= 0) fields["triggerCpuPercent"] = average;
    if (result.contains("signal")) fields["signal"] = result.value("signal");
    if (!result.value("ok").toBool()) {
        fields["phase"] = "refused";
        fields["message"] = result.value("message");
        appendLog(rule, process, fields);
        // A passing failure is retried after the usual cooldown.
        if (lasting(result)) refused_.insert(identity);
        return;
    }
    fields["phase"] = "requested";
    fields["message"] = QStringLiteral("已发送结束请求，等待程序退出…");
    const auto row = appendLog(rule, process, fields);
    if (pending_.size() >= MaximumPending) pending_.removeFirst();
    Pending pending;
    pending.identity = QJsonObject{{"pid", process.value("pid")}, {"startTicks", process.value("startTicks")}, {"executablePath", process.value("executablePath")}};
    pending.logId = row.value("id").toString(); pending.ruleId = rule.id;
    pending.termAt = now; pending.graceSeconds = rule.graceSeconds; pending.forceKill = rule.forceKill;
    pending_.append(pending);
}

void ProcessRules::verifyPending(qint64 now) {
    ProcessActions actions;
    for (int i = pending_.size() - 1; i >= 0; --i) {
        auto &item = pending_[i];
        const double waited = double(now - item.termAt) / 1000.;
        if (!actions.isRunning(item.identity)) {
            QJsonObject changes{{"ok", true}, {"exitedAfterSeconds", waited}};
            changes["phase"] = item.killAt >= 0 ? "killed" : "exited";
            changes["code"] = item.killAt >= 0 ? "killed" : "exited";
            changes["message"] = item.killAt >= 0 ? QStringLiteral("未响应结束请求，已强制结束")
                                                  : QStringLiteral("已结束（%1）").arg(waited < 1 ? QStringLiteral("不到 1 秒") : seconds(waited));
            updateLog(item.logId, changes);
            pending_.removeAt(i);
            continue;
        }
        if (item.killAt >= 0) {
            if (now - item.killAt >= KillVerifyMs) {
                updateLog(item.logId, {{"ok", false}, {"phase", "survived"}, {"code", "survived"},
                    {"message", QStringLiteral("强制结束后仍未退出，可能卡在内核等待（D 状态）")}});
                unresponsive_.insert(identityKey(item.identity));
                pending_.removeAt(i);
            }
            continue;
        }
        if (now - item.termAt < qint64(item.graceSeconds) * 1000) continue;
        if (!enabled_ || !item.forceKill) {
            updateLog(item.logId, {{"ok", false}, {"phase", "survived"}, {"code", "survived"},
                {"message", !enabled_ ? QStringLiteral("自动查杀已暂停，进程仍在运行")
                                      : QStringLiteral("%1后仍在运行，程序没有响应结束请求").arg(seconds(item.graceSeconds))}});
            // Repeating a request this process just ignored would only fill the log.
            // The live status keeps saying so until it exits or the rule changes.
            if (enabled_) unresponsive_.insert(identityKey(item.identity));
            pending_.removeAt(i);
            continue;
        }
        const auto result = actions.execute(item.identity, QStringLiteral("kill"));
        if (!result.value("ok").toBool()) {
            updateLog(item.logId, {{"ok", false}, {"phase", "survived"}, {"code", result.value("code")}, {"message", result.value("message")}});
            unresponsive_.insert(identityKey(item.identity));
            pending_.removeAt(i);
            continue;
        }
        updateLog(item.logId, {{"phase", "forcing"}, {"signal", result.value("signal")},
            {"message", QStringLiteral("%1未退出，已发送强制结束").arg(seconds(item.graceSeconds))}});
        item.killAt = now;
    }
}

void ProcessRules::updateStatus(const QJsonObject &snapshot, qint64 now) {
    // One pass over the processes, then rules only compare strings: this runs on
    // every scan for up to 32 rules, enabled or not.
    struct Row { QString executable, name; int index; };
    QVector<Row> running;
    QHash<QString, QString> names;
    const auto processes = snapshot.value("processes").toArray();
    for (int i = 0; i < processes.size(); ++i) {
        const auto process = processes[i].toObject();
        const QString executable = process.value("executablePath").toString();
        if (executable.isEmpty() || process.value("state").toString() == "Z") continue;
        auto name = names.constFind(executable);
        if (name == names.cend()) name = names.insert(executable, QFileInfo(executable).fileName());
        running.append({executable, name.value(), i});
    }
    const auto others = snapshot.value("systemProcesses").toArray();
    QJsonObject status;
    for (const auto &rule : rules_) {
        int matched = 0, armed = 0, protectedCount = 0, waiting = 0, unresponsive = 0;
        double topCpu = -1, armedAverage = -1, longestMs = 0;
        QString protection;
        for (const auto &row : running) {
            if (rule.match == "path" ? rule.value != row.executable : rule.value != row.name) continue;
            ++matched;
            const auto process = processes[row.index].toObject();
            topCpu = qMax(topCpu, process.value("cpuPercent").toDouble(-1));
            const QString identity = identityKey(process);
            if (unresponsive_.contains(identity)) ++unresponsive;
            const auto check = inspections_.constFind(identity);
            if (check != inspections_.cend() && !check->value("ok").toBool()) { ++protectedCount; protection = check->value("message").toString(); }
            const auto watch = watches_.constFind(identity);
            if (watch != watches_.cend() && now > watch->since) {
                ++armed;
                longestMs = qMax(longestMs, double(now - watch->since));
                armedAverage = qMax(armedAverage, watch->cpuMs / double(now - watch->since));
            }
            for (const auto &item : pending_) if (item.ruleId == rule.id && identityKey(item.identity) == identity) ++waiting;
        }
        // Other users' processes are visible but out of reach: say so rather than
        // leaving a rule that silently never fires. Their names are kernel comm values,
        // which stop at 15 characters.
        int otherUser = 0;
        if (rule.match == "name") {
            const QString comm = rule.value.left(15);
            for (const auto &entry : others) if (entry.toObject().value("name").toString() == comm) ++otherUser;
        }
        status.insert(rule.id, QJsonObject{{"matched", matched}, {"topCpuPercent", topCpu >= 0 ? QJsonValue(topCpu) : QJsonValue(QJsonValue::Null)},
            {"armed", armed}, {"armedSeconds", longestMs / 1000.}, {"sustainSeconds", rule.sustainSeconds},
            {"averageCpuPercent", armedAverage >= 0 ? QJsonValue(armedAverage) : QJsonValue(QJsonValue::Null)},
            {"protected", protectedCount}, {"protectedReason", protection}, {"waitingExit", waiting}, {"unresponsive", unresponsive}, {"otherUser", otherUser}});
    }
    status_ = status;
}

QJsonObject ProcessRules::observe(const QJsonObject &snapshot, qint64 monotonicMs) {
    if (rules_.isEmpty() && pending_.isEmpty()) { status_ = {}; return state(); }
    // Keep cached and duplicate samples free of log writes and native inspection.
    // The worker owns the existing scan cadence.
    if (monotonicMs < 0 || monotonicMs < clockHighWater_ || !usableSnapshot(snapshot)) return state();
    const double sampled = snapshot.value("processMonotonicSeconds").toDouble();
    if (sampled <= lastSample_) return state();
    lastSample_ = sampled; clockHighWater_ = monotonicMs;
    const qint64 interval = lastScan_ >= 0 ? qMin(monotonicMs - lastScan_, MaximumMeasuredIntervalMs) : 0;
    lastScan_ = monotonicMs;
    verifyPending(monotonicMs);

    QHash<QString, ExecutableAttempt> waves;
    QSet<QString> seen;
    int checked = 0, attempts = 0;
    for (const auto &entry : snapshot.value("processes").toArray()) {
        if (++checked > MaximumCandidates) break;
        const auto process = entry.toObject();
        if (process.value("state").toString() == "Z") continue;
        const Rule *rule = matching(process);
        if (!rule) continue;
        const QString identity = identityKey(process);
        seen.insert(identity);
        const auto check = inspection(process, identity);
        if (!check.value("ok").toBool()) {
            // A passing read failure is simply asked again on the next scan.
            if (enabled_ && lasting(check) && !refused_.contains(identity) && rule->cpuAbove < 0) {
                refused_.insert(identity);
                appendLog(*rule, process, {{"phase", "refused"}, {"ok", false}, {"condition", conditionText(*rule)},
                    {"code", check.value("code")}, {"message", check.value("message")}});
            }
            continue;
        }
        // Nothing more will be sent to these, so they must not keep the rule armed:
        // that would hold the faster scan for as long as the process keeps running.
        if (refused_.contains(identity) || unresponsive_.contains(identity)) { watches_.remove(identity); continue; }
        double average = -1;
        if (rule->cpuAbove >= 0) {
            const auto cpuValue = process.value("cpuPercent");
            if (!cpuValue.isDouble()) continue; // A first sample has no rate; never treat that as "below".
            const double cpu = cpuValue.toDouble();
            auto watch = watches_.find(identity);
            if (watch == watches_.end()) {
                if (cpu < rule->cpuAbove) continue;
                // The reading already covers the interval since the previous scan.
                Watch fresh; fresh.since = monotonicMs - interval; fresh.cpuMs = cpu * double(interval);
                watch = watches_.insert(identity, fresh);
            } else {
                watch->cpuMs += cpu * double(interval);
            }
            const qint64 elapsed = monotonicMs - watch->since;
            average = elapsed > 0 ? watch->cpuMs / double(elapsed) : cpu;
            // A brief dip does not reset the clock while the average stays above the limit.
            if (cpu < rule->cpuAbove && average < rule->cpuAbove) { watches_.erase(watch); continue; }
            if (elapsed < qint64(rule->sustainSeconds) * 1000 || average < rule->cpuAbove) continue;
        }
        if (!enabled_ || !rule->enabled || attempts >= MaximumActions) continue;
        const auto prior = identityAttempts_.constFind(identity);
        if (prior != identityAttempts_.cend() && monotonicMs - prior.value() < IdentityCooldown) continue;
        bool waiting = false;
        for (const auto &item : pending_) waiting |= identityKey(item.identity) == identity;
        if (waiting) continue;
        trigger(*rule, process, identity, monotonicMs, average, waves, attempts);
        watches_.remove(identity);
    }
    for (auto it = watches_.begin(); it != watches_.end();)
        if (!seen.contains(it.key())) it = watches_.erase(it); else ++it;
    for (auto it = inspections_.begin(); it != inspections_.end();)
        if (!seen.contains(it.key())) it = inspections_.erase(it); else ++it;
    refused_.intersect(seen);
    unresponsive_.intersect(seen);
    if (!waves.isEmpty())
        for (auto it = waves.cbegin(); it != waves.cend(); ++it) executableAttempts_.insert(it.key(), it.value());
    updateStatus(snapshot, monotonicMs);
    if (logDirty_) { pruneAttempts(monotonicMs); saveLog(); }
    return state();
}

void ProcessRules::pruneAttempts(qint64 now) {
    for (auto it = identityAttempts_.begin(); it != identityAttempts_.end();)
        if (now - it.value() > MaximumBackoff * 2) it = identityAttempts_.erase(it); else ++it;
    for (auto it = executableAttempts_.begin(); it != executableAttempts_.end();)
        if (now - it->at > MaximumBackoff * 2) it = executableAttempts_.erase(it); else ++it;
    while (identityAttempts_.size() > 1024) {
        auto oldest = identityAttempts_.begin();
        for (auto it = identityAttempts_.begin(); it != identityAttempts_.end(); ++it)
            if (it.value() < oldest.value()) oldest = it;
        identityAttempts_.erase(oldest);
    }
    while (executableAttempts_.size() > 128) {
        auto oldest = executableAttempts_.begin();
        for (auto it = executableAttempts_.begin(); it != executableAttempts_.end(); ++it)
            if (it->at < oldest->at) oldest = it;
        executableAttempts_.erase(oldest);
    }
}

QJsonObject ProcessRules::appendLog(const Rule &rule, const QJsonObject &process, const QJsonObject &fields) {
    auto row = candidate(process, rule.id, rule.value);
    row["id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
    row["timestamp"] = double(QDateTime::currentMSecsSinceEpoch()) / 1000.;
    row["ruleMatch"] = rule.match;
    for (auto it = fields.begin(); it != fields.end(); ++it) row[it.key()] = it.value();
    log_.prepend(row);
    while (log_.size() > MaximumLog) log_.removeLast();
    logDirty_ = true;
    return row;
}

void ProcessRules::updateLog(const QString &id, const QJsonObject &changes) {
    for (int i = 0; i < log_.size(); ++i) {
        auto row = log_[i].toObject();
        if (row.value("id").toString() != id) continue;
        for (auto it = changes.begin(); it != changes.end(); ++it) row[it.key()] = it.value();
        log_[i] = row;
        logDirty_ = true;
        return;
    }
}

void ProcessRules::loadLog() {
    logLoaded_ = true;
    if (dataRoot_.isEmpty()) return;
    QFile file(QDir(dataRoot_).filePath("process-rules-log.json"));
    if (!file.exists()) return;
    struct stat info{};
    if (::lstat(QFile::encodeName(file.fileName()).constData(), &info) != 0 || !S_ISREG(info.st_mode) ||
        info.st_uid != ::getuid() || (info.st_mode & 0077) || file.size() > MaximumLogBytes || !file.open(QIODevice::ReadOnly)) {
        storageError_ = QStringLiteral("自动查杀记录文件无法安全读取。"); return;
    }
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(file.read(MaximumLogBytes + 1), &error);
    if (error.error != QJsonParseError::NoError || !document.isArray()) {
        storageError_ = QStringLiteral("自动查杀记录文件格式无效。"); return;
    }
    const auto entries = document.array();
    for (const auto &value : entries) {
        if (log_.size() >= MaximumLog) break;
        if (!value.isObject()) continue;
        const auto row = value.toObject();
        if (!row.value("timestamp").isDouble() || !row.value("ok").isBool() ||
            !row.value("code").isString() || row.value("code").toString().size() > 64 ||
            row.value("message").toString().size() > 2048 || row.value("executablePath").toString().size() > 4096)
            continue;
        log_.append(row);
    }
}

void ProcessRules::saveLog() {
    if (dataRoot_.isEmpty()) return;
    QDir directory(dataRoot_);
    if (!directory.exists() && !QDir().mkpath(dataRoot_)) {
        storageError_ = QStringLiteral("无法创建自动查杀记录目录。"); return;
    }
    const QString path = directory.filePath("process-rules-log.json");
    struct stat info{};
    const int existing = ::lstat(QFile::encodeName(path).constData(), &info);
    if ((existing == 0 && (!S_ISREG(info.st_mode) || info.st_uid != ::getuid() || (info.st_mode & 0077))) ||
        (existing != 0 && errno != ENOENT)) {
        storageError_ = QStringLiteral("自动查杀记录路径或权限不安全，未覆盖现有文件。"); return;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    const QByteArray bytes = QJsonDocument(log_).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || !file.setPermissions(PrivateFile) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        storageError_ = QStringLiteral("无法保存自动查杀记录；本次结果仍保留在内存中。"); return;
    }
    logDirty_ = false;
    storageError_.clear();
}

QJsonObject ProcessRules::state() const {
    return {{"enabled", enabled_}, {"rules", ruleJson_}, {"log", log_}, {"status", status_}, {"storageError", storageError_}};
}

void ProcessRules::resetObservation() {
    lastSample_ = -1; lastScan_ = -1;
    watches_.clear();
}
