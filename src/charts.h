#pragma once
#include <QWidget>
#include <QJsonValue>
#include <QPointF>
#include <QRectF>
#include <QVector>

class HistoryFeed;

QString formatBytes(double value, bool rate = false);
QString formatPercent(double value);

class QBrush;
class QColor;
class QPainter;
class QPen;

// One unbroken stretch of a chart in widget coordinates, left to right: the line,
// and the lowest and highest value at each of its points.
struct ChartRun {
    QVector<QPointF> mean, low, high;
};
// Fills the area between the line and `baseline`, the band between low and high
// where they differ, and strokes the line, all inside `clip`. The stretch is drawn
// in vertical strips `stripWidth` wide; the pixels are the same as in one pass.
void drawChartRun(QPainter &painter, const ChartRun &run, const QRectF &clip, qreal baseline,
                  const QBrush &area, const QColor &band, const QPen &line, int stripWidth = 24);

class HistoryChart final : public QWidget {
public:
    enum class Unit { Percent, BytesPerSecond, Milliseconds };
    explicit HistoryChart(bool percent, QWidget *parent = nullptr);
    explicit HistoryChart(Unit unit, QWidget *parent = nullptr);
    // Shows one metric of a feed. When the chart already shows this feed's complete
    // range, only the points added since are read.
    void setHistory(const HistoryFeed &feed, const QString &metric);
    // A compact chart is a plain sparkline for tiles: no axis or scale text.
    void setCompact(bool compact);
protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    struct Point {
        qint64 at, first, last;
        double value, minimum, maximum, count;
        bool aggregate, gapBefore;
    };
    QString format(double value) const;
    static Point parse(const QJsonValue &entry);
    void rebuild();
    // Every point of the feed, in time order and not yet clipped to the range.
    QVector<Point> samples_;
    // What is drawn: samples_ within the range, at most two per pixel column.
    QVector<Point> points_;
    Unit unit_;
    bool compact_ = false;
    // The feed generation shown and how many of its recent points are in samples_.
    quint64 generation_ = 0;
    int applied_ = 0;
    qint64 start_ = 0, end_ = 1;
    int hovered_ = -1;
    Point latest_{};
    bool hasLatest_ = false;
    QRectF plotRect() const;
    double ceiling() const;
};
