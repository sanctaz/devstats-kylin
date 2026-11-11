#include "history_feed.h"
#include <atomic>

namespace {
quint64 nextGeneration() {
    static std::atomic<quint64> counter{0};
    return ++counter;
}
}

void HistoryFeed::reset(const QJsonObject &complete, qint64 start, qint64 end) {
    complete_ = complete;
    recent_.clear();
    start_ = start;
    end_ = end;
    generation_ = nextGeneration();
}

void HistoryFeed::append(const QJsonObject &points, qint64 start, qint64 end) {
    if (!generation_) generation_ = nextGeneration();
    for (auto it = points.constBegin(); it != points.constEnd(); ++it) recent_[it.key()].append(it.value());
    start_ = start;
    end_ = end;
}

QJsonArray HistoryFeed::series(const QString &metric) const {
    const auto recent = recent_.value(metric);
    if (recent.isEmpty()) return complete(metric);
    // The range has moved on since the complete part was built, so its oldest
    // points may now lie before the start.
    QJsonArray out;
    for (const auto &point : complete(metric))
        if (point.toObject().value(QLatin1String("timestamp")).toDouble() >= double(start_)) out.append(point);
    for (const auto &point : recent) out.append(point);
    return out;
}
