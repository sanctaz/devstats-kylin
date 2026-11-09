#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QVector>

// Owned by MonitorWorker. Consumes its existing fresh process snapshots; this
// class has no timer, process enumeration, shell command, or numeric-PID signal.
//
// A rule says which program, when, and how far to go:
//   {id, enabled, match: "path" | "name", value,
//    cpuAbovePercent?: number,   // absent: end the program whenever it runs.
//    sustainSeconds?: 0..3600,   // average CPU must stay above the limit this long.
//    forceKill?: bool, graceSeconds?: 1..120}  // SIGKILL if SIGTERM was ignored.
// CPU is judged for each process on its own; 100% is one fully used logical core.
class ProcessRules final {
public:
    explicit ProcessRules(QString dataRoot = {});

    // Atomic validation. A failed configuration preserves the previous rules.
    QJsonObject configure(bool enabled, const QJsonArray &rules);
    // An immediate global pause never validates draft or changed executables.
    // Keep configured rules, retry limits and reports for a later explicit resume.
    void pause() { enabled_ = false; }
    QJsonObject observe(const QJsonObject &snapshot, qint64 monotonicMs);
    // enabled, rules, status (rule id -> live state) and a newest-first log of
    // at most 100 actions.
    QJsonObject state() const;
    // A recreated sampler has a new relative process clock. Preserve retries.
    void resetObservation();
    // True while a process is over its limit or an exit is being verified, so
    // the worker samples at its faster cadence only for as long as it matters.
    bool wantsFreshProcesses() const { return enabled_ && (!watches_.isEmpty() || !pending_.isEmpty()); }

private:
    struct Rule {
        QString id, match, value;
        bool enabled = false, forceKill = false;
        double cpuAbove = -1; // Negative: no CPU condition.
        int sustainSeconds = 0, graceSeconds = 10;
    };
    struct ExecutableAttempt { qint64 at = 0; qint64 delay = 30000; QSet<QString> identities; };
    struct Watch { qint64 since = 0; double cpuMs = 0; };
    struct Pending {
        QJsonObject identity;
        QString logId, ruleId;
        qint64 termAt = 0, killAt = -1;
        int graceSeconds = 10;
        bool forceKill = false;
    };
    const Rule *matching(const QJsonObject &process) const;
    bool usableSnapshot(const QJsonObject &snapshot) const;
    QString conditionText(const Rule &rule) const;
    QJsonObject inspection(const QJsonObject &process, const QString &identity);
    void trigger(const Rule &rule, const QJsonObject &process, const QString &identity, qint64 now, double average, QHash<QString, ExecutableAttempt> &waves, int &attempts);
    void verifyPending(qint64 now);
    void updateStatus(const QJsonObject &snapshot, qint64 now);
    void updateLog(const QString &id, const QJsonObject &changes);
    QJsonObject appendLog(const Rule &rule, const QJsonObject &process, const QJsonObject &fields);
    void loadLog();
    void saveLog();
    void pruneAttempts(qint64 now);

    QString dataRoot_, storageError_;
    bool enabled_ = false, activeRules_ = false, logLoaded_ = false, logDirty_ = false;
    QVector<Rule> rules_;
    QJsonArray ruleJson_, log_;
    QJsonObject status_;
    QHash<QString, qint64> identityAttempts_;
    QHash<QString, ExecutableAttempt> executableAttempts_;
    QHash<QString, Watch> watches_;
    QHash<QString, QJsonObject> inspections_;
    QSet<QString> refused_, unresponsive_;
    QVector<Pending> pending_;
    qint64 clockHighWater_ = -1, lastScan_ = -1;
    double lastSample_ = -1;
};
