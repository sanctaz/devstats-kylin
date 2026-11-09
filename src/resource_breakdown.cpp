#include "resource_breakdown.h"
#include "charts.h"
#include "ui_theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyle>
#include <QApplication>
#include <QStringList>
#include <cmath>
#include <algorithm>

namespace {
QColor blend(const QColor &a, const QColor &b, double weight) {
    return QColor::fromRgbF(a.redF() * weight + b.redF() * (1 - weight),
        a.greenF() * weight + b.greenF() * (1 - weight), a.blueF() * weight + b.blueF() * (1 - weight));
}
}

ResourceBreakdown::ResourceBreakdown(QWidget *parent) : QWidget(parent) {
    setFixedHeight(46);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAccessibleName(QStringLiteral("资源分布"));
    setSegments({});
}

void ResourceBreakdown::setSegments(const QVector<Segment> &segments) {
    segments_.clear();
    double sum = 0;
    for (const auto &segment : segments) {
        if (!std::isfinite(segment.fraction) || segment.fraction < 0 || segment.fraction > 1) {
            segments_.clear(); break;
        }
        sum += segment.fraction;
        segments_.append(segment);
    }
    if (std::abs(sum - 1) > .002) segments_.clear();
    QStringList descriptions;
    for (const auto &segment : segments_)
        descriptions.append(segment.label + " " + (segment.detail.isEmpty() ? formatPercent(segment.fraction) : segment.detail));
    const auto description = descriptions.isEmpty() ? QStringLiteral("暂无有效采样") : descriptions.join(QStringLiteral(" · "));
    setAccessibleDescription(description); setToolTip(description);
    setProperty("segmentCount", segments_.size());
    update();
}

void ResourceBreakdown::paintEvent(QPaintEvent *) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const auto c = DevStatsUI::colors(palette());
    const QRectF bar(0, 5, width(), 8);
    p.setPen(Qt::NoPen); p.setBrush(c.raised); p.drawRoundedRect(bar, 4, 4);
    QFont annotation = font(); annotation.setPointSizeF(9.5); p.setFont(annotation);
    if (segments_.isEmpty()) {
        p.setPen(c.muted); p.drawText(QRectF(0, 20, width(), 22), Qt::AlignLeft | Qt::AlignVCenter, accessibleDescription()); return;
    }
    QVector<QColor> colors;
    for (int i = 0; i < segments_.size(); ++i)
        colors.append(i == segments_.size() - 1 ? blend(c.secondary, c.surface, .25)
            : blend(c.accent, c.surface, i == 0 ? 1 : i == 1 ? .62 : .4));
    QPainterPath clip; clip.addRoundedRect(bar, 4, 4); p.save(); p.setClipPath(clip);
    qreal x = 0;
    for (int i = 0; i < segments_.size(); ++i) {
        const qreal size = segments_[i].fraction * bar.width();
        p.fillRect(QRectF(x, bar.top(), size, bar.height()), colors[i]); x += size;
    }
    p.restore();
    qreal left = 0, top = 20;
    for (int i = 0; i < segments_.size(); ++i) {
        const auto &segment = segments_[i];
        const QString text = segment.label + " " + (segment.detail.isEmpty() ? formatPercent(segment.fraction) : segment.detail);
        const int extent = p.fontMetrics().horizontalAdvance(text) + 18;
        if (left > 0 && left + extent > width()) { left = 0; top += 22; }
        p.setBrush(colors[i]); p.setPen(Qt::NoPen); p.drawRoundedRect(QRectF(left, top + 7, 7, 7), 2, 2);
        p.setPen(c.secondary);
        p.drawText(QRectF(left + 13, top, std::max(0., width() - left - 13), 21), Qt::AlignLeft | Qt::AlignVCenter,
            p.fontMetrics().elidedText(text, Qt::ElideRight, std::max(0, width() - int(left) - 13)));
        left += extent + 20;
    }
}

void MetricItemDelegate::paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const {
    QStyleOptionViewItem item(option); initStyleOption(&item, index);
    const QWidget *window = option.widget ? option.widget->window() : nullptr;
    const auto c = DevStatsUI::colors(window ? window->palette() : option.palette);
    const QString text = item.text;
    // A cell may carry a status tone: paint its text as a small coloured pill.
    const QString tone = index.data(ToneRole).toString();
    if (!tone.isEmpty() && !text.isEmpty()) {
        item.text.clear();
        const auto *style = option.widget ? option.widget->style() : QApplication::style();
        style->drawControl(QStyle::CE_ItemViewItem, &item, p, option.widget);
        p->save(); p->setRenderHint(QPainter::Antialiasing); p->setFont(option.font);
        const int size = std::max(0, std::min(item.rect.width() - 4, p->fontMetrics().horizontalAdvance(text) + 14));
        const bool right = Qt::Alignment(index.data(Qt::TextAlignmentRole).toInt()) & Qt::AlignRight;
        const QRectF pill(right ? item.rect.right() - size - 2 : item.rect.left() + 4, item.rect.center().y() - 10, size, 20);
        const QColor color = DevStatsUI::toneColor(c, tone);
        QColor soft = color; soft.setAlphaF(.16);
        p->setPen(Qt::NoPen); p->setBrush(soft); p->drawRoundedRect(pill, 4, 4);
        p->setPen(color); p->drawText(pill, Qt::AlignCenter, p->fontMetrics().elidedText(text, Qt::ElideRight, int(pill.width()) - 8)); p->restore(); return;
    }
    QStyledItemDelegate::paint(p, item, index);
    QString percentage = text; if (percentage.endsWith('*')) percentage.chop(1);
    if (!percentage.endsWith('%') || index.column() == 0) return;
    percentage.chop(1); bool valid = false; const double value = percentage.toDouble(&valid);
    if (!valid || !std::isfinite(value) || value < 0) return;
    p->save(); p->setRenderHint(QPainter::Antialiasing); p->setPen(Qt::NoPen);
    const QRectF track(item.rect.left() + 7, item.rect.bottom() - 4, std::max(0, item.rect.width() - 14), 2);
    QColor base = c.secondary; base.setAlphaF(.12); p->setBrush(base); p->drawRoundedRect(track, 1, 1);
    QColor fill = c.accent; fill.setAlphaF(.7); p->setBrush(fill);
    p->drawRoundedRect(QRectF(track.left(), track.top(), track.width() * std::clamp(value / 100., 0., 1.), track.height()), 1, 1);
    p->restore();
}
