#include "charts.h"
#include "ui_theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QDateTime>
#include <QMouseEvent>
#include <QToolTip>
#include <QJsonObject>
#include <QLinearGradient>
#include <cmath>
#include <limits>
#include <algorithm>

namespace {
constexpr int MaxChartPoints = 600;
QColor opacity(QColor color, double alpha) { color.setAlphaF(alpha); return color; }
QFont annotationFont(const QFont &base) { QFont f(base); f.setPointSizeF(9); return f; }
}

QString formatBytes(double value, bool rate) {
    if (!std::isfinite(value) || value < 0) return QString::fromUtf8("—");
    const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (value >= 1024 && unit < units.size() - 1) { value /= 1024; ++unit; }
    return QString::number(value, 'f', unit == 0 ? 0 : value < 10 ? 2 : value < 100 ? 1 : 0)
        + " " + units[unit] + (rate ? "/s" : "");
}

QString formatPercent(double value) {
    return std::isfinite(value) && value >= 0
        ? QString::number(std::clamp(value, 0.0, 1.0) * 100, 'f', 0) + "%"
        : QString::fromUtf8("—");
}

HistoryChart::HistoryChart(bool percent, QWidget *parent)
    : HistoryChart(percent ? Unit::Percent : Unit::BytesPerSecond, parent) {}

HistoryChart::HistoryChart(Unit unit, QWidget *parent) : QWidget(parent), unit_(unit) {
    setMinimumHeight(132);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QString::fromUtf8("历史趋势"));
}

void HistoryChart::setCompact(bool compact) {
    compact_ = compact;
    setMinimumHeight(compact ? 40 : 132);
    if (compact) setMaximumHeight(56);
    update();
}

QString HistoryChart::format(double value) const {
    if (unit_ == Unit::Percent) return formatPercent(value);
    if (unit_ == Unit::BytesPerSecond) return formatBytes(value, true);
    return value >= 1000 ? QString::number(value / 1000, 'f', 1) + QStringLiteral(" 秒")
                         : QString::number(value, 'f', 0) + QStringLiteral(" 毫秒");
}

void HistoryChart::setPoints(const QJsonArray &points, qint64 start, qint64 end) {
    end = std::max(start + 1, end);
    if (hasSource_ && source_ == points && start_ == start && end_ == end) return;
    source_ = points;
    hasSource_ = true;
    start_ = start;
    end_ = end;
    if (hovered_ >= 0) QToolTip::hideText();
    hovered_ = -1;
    hasLatest_ = false;
    QVector<Point> samples;
    samples.reserve(points.size());
    auto timestamp = [](const QJsonValue &value, qint64 fallback) {
        const double number = value.toDouble(-1);
        return value.isDouble() && std::isfinite(number) && number >= 0 && number <= 1.0e12
            ? qint64(number) : fallback;
    };
    for (const auto &entry : points) {
        const auto p = entry.toObject();
        const qint64 at = timestamp(p.value("timestamp"), -1);
        if (at < start || at > end) continue;
        const auto value = p.value("value");
        const double mean = value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0
            ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
        auto bound = [&](const char *name) {
            const auto v = p.value(QLatin1String(name));
            return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 ? v.toDouble() : mean;
        };
        const qint64 first = std::min(at, timestamp(p.value("firstSample"), at));
        const qint64 last = std::max(at, timestamp(p.value("lastSample"), at));
        const double count = p.value("count").toDouble(1);
        samples.append({at, std::max(start, first), std::min(end, last), mean,
            std::min(mean, bound("minimum")), std::max(mean, bound("maximum")),
            std::isfinite(count) && count > 0 ? std::min(count, 1.0e12) : 1,
            !p.value("raw").toBool() && p.value("resolutionSeconds").toDouble(60) > 2 && p.value("count").isDouble(), false});
    }
    std::stable_sort(samples.begin(), samples.end(), [](const Point &a, const Point &b) { return a.at < b.at; });
    points_.clear();
    points_.reserve(std::min(MaxChartPoints, samples.size()));
    const qint64 bucketSeconds = std::max<qint64>(60, (end_ - start_ + MaxChartPoints - 1) / MaxChartPoints);
    int currentBin = -1;
    const Point *previous = nullptr;
    bool missing = true;
    for (const auto &sample : samples) {
        // Missing readings break the line even when valid samples are close together.
        if (!std::isfinite(sample.value)) { missing = true; continue; }
        latest_ = sample;
        hasLatest_ = true;
        const qint64 allowedGap = previous && previous->aggregate && sample.aggregate ? bucketSeconds * 2 : 4;
        const bool gap = missing || !previous || sample.first - previous->last > allowedGap;
        const int bin = int(std::min<qint64>(MaxChartPoints - 1,
            (sample.at - start_) * MaxChartPoints / (end_ - start_)));
        if (bin != currentBin) {
            Point point = sample;
            point.gapBefore = gap;
            points_.append(point);
            currentBin = bin;
        } else if (gap) {
            // At sub-pixel density keep the last continuous part of a bin. Do not
            // join across an unavailable interval merely to meet the point bound.
            points_.last() = sample;
            points_.last().gapBefore = true;
        } else {
            auto &point = points_.last();
            const double total = point.count + sample.count;
            point.value = point.value * (point.count / total) + sample.value * (sample.count / total);
            point.count = total;
            point.at = sample.at;
            point.last = sample.last;
            point.minimum = std::min(point.minimum, sample.minimum);
            point.maximum = std::max(point.maximum, sample.maximum);
            point.aggregate = true;
        }
        previous = &sample;
        missing = false;
    }
    setAccessibleDescription(hasLatest_ ? QString::fromUtf8("末次采样 ") + format(latest_.value)
        : QString::fromUtf8("此时间段尚无记录"));
    update();
}

QRectF HistoryChart::plotRect() const {
    if (compact_) return QRectF(1, 3, std::max(1, width() - 2), std::max(1, height() - 5));
    return QRectF(4, 22, std::max(1, width() - 8), std::max(1, height() - 44));
}

double HistoryChart::ceiling() const {
    if (unit_ == Unit::Percent) return 1;
    double peak = 0;
    for (const auto &point : points_) peak = std::max(peak, point.maximum);
    if (peak <= 0) return 1;
    const double padded = peak * 1.15;
    const double magnitude = std::pow(10., std::floor(std::log10(padded)));
    const double scaled = padded / magnitude;
    return (scaled <= 2 ? 2 : scaled <= 5 ? 5 : 10) * magnitude;
}

void HistoryChart::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto colors = DevStatsUI::colors(palette());
    const QRectF plot = plotRect();
    const double maximum = ceiling();
    auto position = [&](qint64 at, double value) {
        return QPointF(plot.left() + double(at - start_) / double(end_ - start_) * plot.width(),
            plot.bottom() - std::clamp(value / maximum, 0.0, 1.0) * plot.height());
    };
    painter.setPen(QPen(opacity(colors.secondary, .14), 1, Qt::DashLine));
    if (!compact_) for (int i = 0; i <= 2; ++i) {
        const qreal y = std::round(plot.top() + plot.height() * i / 2) + .5;
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    painter.save();
    painter.setClipRect(plot.adjusted(-3, -3, 3, 3));
    QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
    gradient.setColorAt(0, opacity(colors.accent, .22));
    gradient.setColorAt(1, opacity(colors.accent, .025));
    int begin = 0;
    while (begin < points_.size()) {
        int finish = begin + 1;
        while (finish < points_.size() && !points_[finish].gapBefore) ++finish;
        const auto &first = points_[begin];
        const auto &last = points_[finish - 1];
        QPainterPath line;
        line.moveTo(position(first.first, first.value));
        for (int i = begin; i < finish; ++i) line.lineTo(position(points_[i].at, points_[i].value));
        line.lineTo(position(last.last, last.value));
        QPainterPath area = line;
        area.lineTo(position(last.last, 0));
        area.lineTo(position(first.first, 0));
        area.closeSubpath();
        painter.fillPath(area, gradient);
        // Minute summaries retain their full observed range above the mean area.
        QPainterPath band;
        band.moveTo(position(first.first, first.minimum));
        for (int i = begin; i < finish; ++i) band.lineTo(position(points_[i].at, points_[i].minimum));
        band.lineTo(position(last.last, last.minimum));
        band.lineTo(position(last.last, last.maximum));
        for (int i = finish - 1; i >= begin; --i) band.lineTo(position(points_[i].at, points_[i].maximum));
        band.lineTo(position(first.first, first.maximum));
        band.closeSubpath();
        painter.fillPath(band, opacity(colors.accent, .13));
        painter.setPen(QPen(colors.accent, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.drawPath(line);
        if (finish == begin + 1 && first.first == first.last) {
            if (first.minimum != first.maximum)
                painter.drawLine(position(first.at, first.minimum), position(first.at, first.maximum));
            painter.setBrush(colors.accent);
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(position(first.at, first.value), 2.3, 2.3);
        }
        begin = finish;
    }
    if (!points_.isEmpty()) {
        const int selected = hovered_ >= 0 ? hovered_ : points_.size() - 1;
        const QPointF dot = position(points_[selected].at, points_[selected].value);
        if (hovered_ >= 0) {
            painter.setPen(QPen(opacity(colors.secondary, .55), 1, Qt::DashLine));
            painter.drawLine(QPointF(dot.x(), plot.top()), QPointF(dot.x(), plot.bottom()));
        }
        painter.setBrush(colors.accent);
        painter.setPen(QPen(colors.surface, 2));
        painter.drawEllipse(dot, hovered_ >= 0 ? 4 : 3, hovered_ >= 0 ? 4 : 3);
    }
    painter.restore();
    if (compact_) return;
    painter.setFont(annotationFont(font()));
    painter.setPen(colors.muted);
    const QString scale = unit_ == Unit::Percent ? QStringLiteral("0–100%") : QStringLiteral("0 – ") + format(maximum);
    const QString stamp = hovered_ >= 0 ? QDateTime::fromSecsSinceEpoch(points_[hovered_].at).toString(
        end_ - start_ >= 86400 ? QStringLiteral("MM-dd hh:mm") : QStringLiteral("hh:mm:ss")) : scale;
    const QString detail = hovered_ >= 0
        ? (points_[hovered_].aggregate ? QString::fromUtf8("均值 ") : QString()) + format(points_[hovered_].value)
        : hasLatest_ ? (latest_.aggregate ? QString::fromUtf8("末次均值 ") : QString::fromUtf8("末次 ")) + format(latest_.value)
        : QString::fromUtf8("—");
    const qreal detailWidth = std::min(plot.width() * .58, painter.fontMetrics().horizontalAdvance(detail) + 2.);
    painter.drawText(QRectF(plot.left(), 0, std::max(0., plot.width() - detailWidth - 12), 20),
        Qt::AlignLeft | Qt::AlignVCenter, painter.fontMetrics().elidedText(stamp, Qt::ElideRight, int(std::max(0., plot.width() - detailWidth - 12))));
    painter.setPen(hovered_ >= 0 ? colors.text : colors.secondary);
    painter.drawText(QRectF(plot.right() - detailWidth, 0, detailWidth, 20), Qt::AlignRight | Qt::AlignVCenter, detail);
    painter.setPen(colors.muted);
    const QString dateFormat = end_ - start_ >= 86400 ? QStringLiteral("MM-dd hh:mm") : QStringLiteral("hh:mm");
    const QRectF axis(plot.left(), plot.bottom() + 4, plot.width(), 18);
    painter.drawText(QRectF(axis.left(), axis.top(), axis.width() / 2, axis.height()), Qt::AlignLeft | Qt::AlignVCenter,
        QDateTime::fromSecsSinceEpoch(start_).toString(dateFormat));
    painter.drawText(QRectF(axis.center().x(), axis.top(), axis.width() / 2, axis.height()), Qt::AlignRight | Qt::AlignVCenter,
        QDateTime::fromSecsSinceEpoch(end_).toString(dateFormat));
    if (points_.isEmpty()) {
        painter.setPen(colors.secondary);
        painter.drawText(plot, Qt::AlignCenter, QString::fromUtf8("此时间段尚无记录"));
    }
}

void HistoryChart::mouseMoveEvent(QMouseEvent *event) {
    int selected = -1;
    const auto plot = plotRect();
    if (!points_.isEmpty() && plot.contains(event->pos())) {
        const double at = start_ + (event->pos().x() - plot.left()) / plot.width() * (end_ - start_);
        double nearest = std::numeric_limits<double>::max();
        const double tolerance = std::max(4., double(end_ - start_) / plot.width() * 7);
        for (int i = 0; i < points_.size(); ++i) {
            const auto &point = points_[i];
            const double distance = at < point.first ? point.first - at : at > point.last ? at - point.last : 0;
            if (distance < nearest && distance <= tolerance) { nearest = distance; selected = i; }
        }
    }
    if (selected == hovered_) return;
    hovered_ = selected;
    update();
    if (selected < 0) { QToolTip::hideText(); return; }
    const auto &point = points_[selected];
    QString detail = format(point.value);
    if (point.aggregate) detail = QString::fromUtf8("均值 ") + detail + QString::fromUtf8("\n范围 ")
        + format(point.minimum) + QStringLiteral(" – ") + format(point.maximum);
    QToolTip::showText(event->globalPos(), QDateTime::fromSecsSinceEpoch(point.at).toString("MM-dd hh:mm:ss")
        + QStringLiteral("\n") + detail, this);
}

void HistoryChart::leaveEvent(QEvent *) {
    if (hovered_ >= 0) { hovered_ = -1; update(); }
    QToolTip::hideText();
}
