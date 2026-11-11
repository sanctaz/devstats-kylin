#include "charts.h"
#include "i18n.h"
#include "history_feed.h"
#include "ui_theme.h"
#include <QPainter>
#include <QDateTime>
#include <QMouseEvent>
#include <QResizeEvent>
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

// Qt's antialiasing rasterizer keeps the cells a shape covers in a small pool. When
// the pool overflows it halves the band of scan lines it works on and walks the whole
// outline again. A long jagged line overflows again and again, so one path for a whole
// chart was measured at two and a half to five times the cost of the same pixels drawn
// in pieces. Each strip is clipped at whole pixels and drawn from only the points that
// can reach into it, which keeps every outline small and leaves every pixel as it was.
void drawChartRun(QPainter &painter, const ChartRun &run, const QRectF &clip, qreal baseline,
                  const QBrush &area, const QColor &band, const QPen &line, int stripWidth) {
    const int count = run.mean.size();
    if (count < 2 || stripWidth < 1) return;
    bool spread = false;
    for (int i = 0; i < count && !spread; ++i) spread = run.low[i].y() != run.high[i].y();
    // How far outside a strip a point can still paint into it: half the pen, plus the
    // rounding of the strip's edges to whole pixels.
    const qreal reach = line.widthF() / 2 + 1.5;
    const auto leftOf = [](const QPointF &point, qreal x) { return point.x() < x; };
    const auto rightOf = [](qreal x, const QPointF &point) { return x < point.x(); };
    QVector<QPointF> polygon;
    for (qreal left = clip.left(); left < clip.right(); left += stripWidth) {
        const qreal right = std::min(left + stripWidth, clip.right());
        if (run.mean.last().x() < left - reach || run.mean.first().x() > right + reach) continue;
        // One point beyond the strip on each side, so the joins nearest its edges are
        // made with the same neighbours as in the whole line.
        const int lo = std::max(0, int(std::lower_bound(run.mean.cbegin(), run.mean.cend(), left - reach, leftOf) - run.mean.cbegin()) - 1);
        const int hi = std::min(count - 1, int(std::upper_bound(run.mean.cbegin(), run.mean.cend(), right + reach, rightOf) - run.mean.cbegin()));
        painter.setClipRect(QRectF(left, clip.top(), right - left, clip.height()));
        painter.setPen(Qt::NoPen);
        polygon.resize(0);
        for (int i = lo; i <= hi; ++i) polygon.append(run.mean[i]);
        polygon.append(QPointF(run.mean[hi].x(), baseline));
        polygon.append(QPointF(run.mean[lo].x(), baseline));
        painter.setBrush(area);
        painter.drawPolygon(polygon.constData(), polygon.size());
        if (spread) {
            polygon.resize(0);
            for (int i = lo; i <= hi; ++i) polygon.append(run.low[i]);
            for (int i = hi; i >= lo; --i) polygon.append(run.high[i]);
            painter.setBrush(band);
            painter.drawPolygon(polygon.constData(), polygon.size());
        }
        painter.setBrush(Qt::NoBrush);
        painter.setPen(line);
        painter.drawPolyline(run.mean.constData() + lo, hi - lo + 1);
    }
    painter.setClipRect(clip);
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
    setAccessibleName(T("历史趋势"));
}

void HistoryChart::setCompact(bool compact) {
    compact_ = compact;
    setMinimumHeight(compact ? 40 : 132);
    if (compact) setMaximumHeight(56);
    rebuild();
}

// The number of points drawn follows the width.
void HistoryChart::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (event->size().width() != event->oldSize().width()) rebuild();
}

QString HistoryChart::format(double value) const {
    if (unit_ == Unit::Percent) return formatPercent(value);
    if (unit_ == Unit::BytesPerSecond) return formatBytes(value, true);
    return value >= 1000 ? T("%1 秒").arg(QString::number(value / 1000, 'f', 1))
                         : T("%1 毫秒").arg(QString::number(value, 'f', 0));
}

HistoryChart::Point HistoryChart::parse(const QJsonValue &entry) {
    const auto timestamp = [](const QJsonValue &value, qint64 fallback) {
        const double number = value.toDouble(-1);
        return value.isDouble() && std::isfinite(number) && number >= 0 && number <= 1.0e12
            ? qint64(number) : fallback;
    };
    const auto p = entry.toObject();
    const qint64 at = timestamp(p.value(QLatin1String("timestamp")), -1);
    const auto value = p.value(QLatin1String("value"));
    const double mean = value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0
        ? value.toDouble() : std::numeric_limits<double>::quiet_NaN();
    const auto bound = [&](QLatin1String name) {
        const auto v = p.value(name);
        return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 ? v.toDouble() : mean;
    };
    const auto countValue = p.value(QLatin1String("count"));
    const double count = countValue.toDouble(1);
    return {at, std::min(at, timestamp(p.value(QLatin1String("firstSample")), at)),
        std::max(at, timestamp(p.value(QLatin1String("lastSample")), at)), mean,
        std::min(mean, bound(QLatin1String("minimum"))), std::max(mean, bound(QLatin1String("maximum"))),
        std::isfinite(count) && count > 0 ? std::min(count, 1.0e12) : 1,
        !p.value(QLatin1String("raw")).toBool() && p.value(QLatin1String("resolutionSeconds")).toDouble(60) > 2 && countValue.isDouble(), false};
}

void HistoryChart::setHistory(const HistoryFeed &feed, const QString &metric) {
    const qint64 start = feed.start(), end = std::max(start + 1, feed.end());
    const auto recent = feed.recent(metric);
    const auto earlier = [](const Point &a, const Point &b) { return a.at < b.at; };
    if (feed.generation() && feed.generation() == generation_ && applied_ <= recent.size()) {
        if (applied_ == recent.size() && start_ == start && end_ == end) return;
        // New points nearly always belong at the end. Placing each after the points
        // of the same second gives the order a complete sort would.
        for (int i = applied_; i < recent.size(); ++i) {
            const Point sample = parse(recent.at(i));
            samples_.insert(std::upper_bound(samples_.begin(), samples_.end(), sample, earlier), sample);
        }
    } else {
        const auto complete = feed.complete(metric);
        samples_.clear();
        samples_.reserve(complete.size() + recent.size());
        for (const auto &entry : complete) samples_.append(parse(entry));
        for (const auto &entry : recent) samples_.append(parse(entry));
        std::stable_sort(samples_.begin(), samples_.end(), earlier);
    }
    generation_ = feed.generation();
    applied_ = recent.size();
    start_ = start;
    end_ = end;
    rebuild();
}

void HistoryChart::rebuild() {
    if (hovered_ >= 0) QToolTip::hideText();
    hovered_ = -1;
    hasLatest_ = false;
    points_.clear();
    // At most two points per pixel column. Two neighbours still show as the line going
    // up and down; more than that cannot be told apart, and every point costs time to
    // draw. Samples that share a slot become one point with their mean and full range.
    const int columns = std::clamp(2 * int(plotRect().width()), 1, MaxChartPoints);
    points_.reserve(std::min(columns, samples_.size()));
    const qint64 bucketSeconds = std::max<qint64>(60, (end_ - start_ + MaxChartPoints - 1) / MaxChartPoints);
    int currentBin = -1;
    Point previous{};
    bool hasPrevious = false, missing = true;
    for (Point sample : samples_) {
        if (sample.at < start_ || sample.at > end_) continue;
        sample.first = std::max(start_, sample.first);
        sample.last = std::min(end_, sample.last);
        // Missing readings break the line even when valid samples are close together.
        if (!std::isfinite(sample.value)) { missing = true; continue; }
        latest_ = sample;
        hasLatest_ = true;
        const qint64 allowedGap = hasPrevious && previous.aggregate && sample.aggregate ? bucketSeconds * 2 : 4;
        const bool gap = missing || !hasPrevious || sample.first - previous.last > allowedGap;
        const int bin = int(std::min<qint64>(columns - 1, (sample.at - start_) * columns / (end_ - start_)));
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
        previous = sample;
        hasPrevious = true;
        missing = false;
    }
    setAccessibleDescription(hasLatest_ ? T("末次采样 %1").arg(format(latest_.value))
        : T("此时间段尚无记录"));
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
    const QRectF clip = plot.adjusted(-3, -3, 3, 3);
    painter.setClipRect(clip);
    QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
    gradient.setColorAt(0, opacity(colors.accent, .22));
    gradient.setColorAt(1, opacity(colors.accent, .025));
    const qreal lineWidth = 1.7;
    ChartRun run;
    int begin = 0;
    while (begin < points_.size()) {
        int finish = begin + 1;
        while (finish < points_.size() && !points_[finish].gapBefore) ++finish;
        const auto &first = points_[begin];
        const auto &last = points_[finish - 1];
        // The stretch starts where its first point's samples began and ends where its
        // last point's samples ended. Minute summaries keep their full observed range
        // as a band around the mean.
        run.mean.resize(0); run.low.resize(0); run.high.resize(0);
        const auto add = [&](qint64 at, const Point &point) {
            run.mean.append(position(at, point.value));
            run.low.append(position(at, point.minimum));
            run.high.append(position(at, point.maximum));
        };
        add(first.first, first);
        for (int i = begin; i < finish; ++i) add(points_[i].at, points_[i]);
        add(last.last, last);
        // Rounding every corner is the costliest part of a dense line, and where the
        // points lie closer together than the line is wide the rounding cannot be seen.
        const bool dense = run.mean.last().x() - run.mean.first().x() < lineWidth * (finish - begin);
        const QPen linePen(colors.accent, lineWidth, Qt::SolidLine, Qt::RoundCap, dense ? Qt::BevelJoin : Qt::RoundJoin);
        drawChartRun(painter, run, clip, plot.bottom(), gradient, opacity(colors.accent, .13), linePen);
        if (finish == begin + 1 && first.first == first.last) {
            painter.setPen(linePen);
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
        ? (points_[hovered_].aggregate ? T("均值 ") : QString()) + format(points_[hovered_].value)
        : hasLatest_ ? (latest_.aggregate ? T("末次均值 ") : T("末次 ")) + format(latest_.value)
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
        painter.drawText(plot, Qt::AlignCenter, T("此时间段尚无记录"));
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
    if (point.aggregate) detail = T("均值 %1\n范围 %2 – %3").arg(detail, format(point.minimum), format(point.maximum));
    QToolTip::showText(event->globalPos(), QDateTime::fromSecsSinceEpoch(point.at).toString("MM-dd hh:mm:ss")
        + QStringLiteral("\n") + detail, this);
}

void HistoryChart::leaveEvent(QEvent *) {
    if (hovered_ >= 0) { hovered_ = -1; update(); }
    QToolTip::hideText();
}
