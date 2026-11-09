#pragma once
#include <QStyledItemDelegate>
#include <QVector>
#include <QWidget>

class ResourceBreakdown final : public QWidget {
public:
    struct Segment { QString label; double fraction; QString detail; };
    explicit ResourceBreakdown(QWidget *parent = nullptr);
    void setSegments(const QVector<Segment> &segments);
protected:
    void paintEvent(QPaintEvent *) override;
private:
    QVector<Segment> segments_;
};

// Paints percentages and task states without adding per-row widgets or timers.
class MetricItemDelegate final : public QStyledItemDelegate {
public:
    // A column's data under this role ("ok", "warn", "critical", ...) draws it as a pill.
    static constexpr int ToneRole = Qt::UserRole + 40;
    explicit MetricItemDelegate(QObject *parent = nullptr) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;
};
