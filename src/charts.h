#pragma once
#include <QWidget>
#include <QJsonArray>
#include <QVector>

QString formatBytes(double value, bool rate = false);
QString formatPercent(double value);

class HistoryChart final : public QWidget {
public:
    enum class Unit { Percent, BytesPerSecond, Milliseconds };
    explicit HistoryChart(bool percent, QWidget *parent = nullptr);
    explicit HistoryChart(Unit unit, QWidget *parent = nullptr);
    void setPoints(const QJsonArray &points, qint64 start, qint64 end);
    // A compact chart is a plain sparkline for tiles: no axis or scale text.
    void setCompact(bool compact);
protected:
    void paintEvent(QPaintEvent *) override;
    void mouseMoveEvent(QMouseEvent *) override;
    void leaveEvent(QEvent *) override;
private:
    struct Point {
        qint64 at, first, last;
        double value, minimum, maximum, count;
        bool aggregate, gapBefore;
    };
    QString format(double value) const;
    QVector<Point> points_;
    QJsonArray source_;
    Unit unit_;
    bool compact_ = false;
    bool hasSource_ = false;
    qint64 start_ = 0, end_ = 1;
    int hovered_ = -1;
    Point latest_{};
    bool hasLatest_ = false;
    QRectF plotRect() const;
    double ceiling() const;
};
