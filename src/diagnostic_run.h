#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

// A bounded, in-memory observation. The worker supplies its existing sampler
// snapshots and monotonic clock; this class owns no timer and performs no action.
class DiagnosticRun final {
public:
    bool start(qint64 monotonicMilliseconds, double wallSeconds);
    bool observe(const QJsonObject &snapshot, qint64 monotonicMilliseconds, double wallSeconds);
    bool finish(const QString &state, qint64 monotonicMilliseconds, double wallSeconds);
    bool running() const { return state_ == QStringLiteral("running"); }
    QJsonObject result() const;
private:
    struct Finding {
        QJsonObject representative;
        int observed = 0, consecutive = 0, longest = 0;
        qint64 consecutiveAt = -1, longestMilliseconds = 0;
    };
    struct Check { int known = 0, unknown = 0, pressure = 0; };
    struct Suspect {
        QJsonObject latest;
        double cpuSum = 0, cpuPeak = -1, memoryPeak = -1, ioPeak = -1;
        int cpuSamples = 0, blockedSamples = 0;
    };
    void observeSuspects(const QJsonObject &snapshot);
    QString rankingBasis() const;
    QString state_ = QStringLiteral("idle");
    qint64 started_ = -1, elapsed_ = 0, lastObservation_ = -1;
    double startedAt_ = 0, finishedAt_ = 0, lastProcessStamp_ = -1;
    double schedulerMax_ = -1, displayMax_ = -1;
    int samples_ = 0;
    QHash<QString, Finding> findings_;
    QHash<QString, Check> checks_;
    QHash<QString, Suspect> suspects_;
    QStringList coverage_;
};
