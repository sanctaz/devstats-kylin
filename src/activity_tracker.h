#pragma once
#include <QJsonObject>
#include <QHash>
#include <QStringList>
#include <deque>

// One owner monitoring thread. CPU activity is a resource clue, not an energy meter.
class RecentActivityTracker {
public:
    // An explicit directory list is useful for deterministic desktop metadata tests.
    explicit RecentActivityTracker(const QStringList &applicationDirectories = QStringList());
    void apply(QJsonObject &snapshot);
    void reset();
private:
    struct Interval {
        double start, end, cpuSeconds;
        bool measured = true, complete = true;
    };
    struct Entry {
        qint64 identity = 0;
        double time = -1;
        QString application;
        std::deque<Interval> intervals;
    };
    struct Application {
        QJsonObject metadata;
        double lastSeen = -1;
        std::deque<Interval> intervals;
    };
    struct Desktop { QString name, file, root; bool terminal = false; };
    QHash<qint64, Entry> entries_;
    QHash<QString, Application> applications_;
    QHash<QString, Desktop> desktops_;
    QStringList directories_;
    double metadataTime_ = -1, previousTime_ = -1, windowStart_ = -1;
    void refreshMetadata(double now);
};
