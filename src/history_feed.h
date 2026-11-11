#pragma once
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>

// What the charts draw: the last complete range the worker published, plus the
// points sampled since. A complete range arrives when the window opens, when the
// time range changes and about once a minute. Every other sample adds one point
// per metric, so no work proportional to the length of the history is repeated
// every two seconds.
class HistoryFeed {
public:
    HistoryFeed() = default;
    HistoryFeed(const QJsonObject &complete, qint64 start, qint64 end) { reset(complete, start, end); }
    void reset(const QJsonObject &complete, qint64 start, qint64 end);
    // points: one point per metric, keyed by metric. start/end: the range now shown.
    void append(const QJsonObject &points, qint64 start, qint64 end);
    bool isEmpty() const { return generation_ == 0; }
    // Names the complete range. Feeds with the same generation hold the same
    // complete range, and recent() only grows while the generation stays the same.
    quint64 generation() const { return generation_; }
    qint64 start() const { return start_; }
    qint64 end() const { return end_; }
    QJsonArray complete(const QString &metric) const { return complete_.value(metric).toArray(); }
    QJsonArray recent(const QString &metric) const { return recent_.value(metric); }
    // One metric's points in the range now shown, as a single array.
    QJsonArray series(const QString &metric) const;
private:
    QJsonObject complete_;
    QHash<QString, QJsonArray> recent_;
    qint64 start_ = 0, end_ = 0;
    quint64 generation_ = 0;
};
