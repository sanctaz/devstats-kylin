#include "tray_icon.h"
#include "i18n.h"
#include "charts.h"
#include <QApplication>
#include <QFontMetricsF>
#include <QJsonArray>
#include <QStringList>
#include <QVector>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {
double number(const QJsonValue &v) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 ? v.toDouble() : -1;
}
int percentage(const QJsonValue &v) {
    const double n = number(v); return n >= 0 && n <= 1 ? qRound(n * 100) : -1;
}
QString percentText(int n) { return n < 0 ? QString::fromUtf8("—") : QString::number(n) + '%'; }
QString iconPercent(int n) { return n < 0 ? QString::fromUtf8("—") : QString::number(n); }
QString batteryTitle(const QString &state) {
    if (state == "charging") return T("正在充电");
    if (state == "full") return T("已充满");
    if (state == "pluggedIn") return T("接电未充电");
    if (state == "discharging") return T("使用电池");
    if (state == "noBattery") return T("无内置电池");
    return T("尚未读取到电池状态");
}
struct Sample { int value; bool gap; };
QVector<Sample> cpuSamples(const QJsonObject &snapshot) {
    const auto points = snapshot.value("menuCPUHistory").toArray();
    QVector<Sample> result; result.reserve(20);
    double previous = -1; bool missing = true;
    for (int i = std::max(0, points.size() - 20); i < points.size(); ++i) {
        const auto point = points[i].toObject(); const double timestamp = number(point.value("timestamp"));
        const int value = percentage(point.value("value"));
        const bool gap = missing || (timestamp >= 0 && previous >= 0 && (timestamp - previous > 4 || timestamp <= previous));
        result.append({value, gap}); missing = value < 0; previous = timestamp;
    }
    return result;
}
void text(QPainter &p, const QString &value, const QRectF &rect, int pixels,
          Qt::Alignment alignment = Qt::AlignCenter) {
    QFont font = QApplication::font(); font.setWeight(QFont::DemiBold); font.setKerning(false);
    font.setHintingPreference(QFont::PreferFullHinting); font.setPixelSize(pixels);
    // Preserve numeral height even at 100: condense only as much as the fixed
    // 22px host slot needs, rather than falling back to an 8px font.
    const qreal advance = QFontMetricsF(font).horizontalAdvance(value);
    if (advance > rect.width()) font.setStretch(std::max(1, int(std::floor(rect.width() / advance * 100))));
    p.setFont(font);
    const QRectF ink = QFontMetricsF(font).tightBoundingRect(value);
    const qreal horizontal = std::min(1., rect.width() / std::max(1., ink.width()));
    const qreal x = alignment.testFlag(Qt::AlignRight) ? rect.right() - horizontal * ink.right()
                    : rect.center().x() - horizontal * ink.center().x();
    // Center actual glyph ink, not full ascent/descent, so 13px network text
    // remains inside its own 11px row.
    p.save(); p.setClipRect(rect, Qt::IntersectClip);
    p.translate(x, rect.center().y() - ink.center().y()); p.scale(horizontal, 1);
    p.drawText(QPointF(0, 0), value); p.restore();
}
QString rateText(double value, bool largeText) {
    if (!largeText) return DevStatsTray::compactRate(value);
    if (!std::isfinite(value) || value < 0) return QString::fromUtf8("—");
    const QStringList units = {"", "K", "M", "G", "T", "P", "E"};
    int unit = 0;
    while (value >= 999.5 && unit < units.size() - 1) { value /= 1024; ++unit; }
    // At most three glyphs: 100M becomes .1G, rather than squeezing four
    // glyphs or falsely showing zero in the next binary unit.
    if (unit && value >= 99.5) {
        if (unit == units.size() - 1) return QStringLiteral(">9E");
        value /= 1024; ++unit;
    }
    QString result = QString::number(value, 'f', unit && value < .95 ? 1 : 0);
    if (result.startsWith("0.")) result.remove(0, 1);
    return result + units[unit];
}
void overlay(QPainter &p, const QPainterPath &path, const QColor &ink) {
    p.save(); p.setCompositionMode(QPainter::CompositionMode_Clear);
    p.setPen(QPen(Qt::black, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::black); p.drawPath(path);
    p.restore(); p.fillPath(path, ink);
}
}

namespace DevStatsTray {
QString compactRate(double value) {
    if (!std::isfinite(value) || value < 0) return QString::fromUtf8("—");
    const QStringList units = {"", "K", "M", "G", "T", "P", "E"};
    int unit = 0;
    while (value >= 999.5 && unit < units.size() - 1) { value /= 1024; ++unit; }
    if (value >= 999.5) return QStringLiteral(">9E");
    QString result = QString::number(value, 'f', value < 9.95 && unit ? 1 : 0);
    if (result.endsWith(".0")) result.chop(2);
    return result + units[unit];
}

QString cacheKey(const QString &metric, const QJsonObject &snapshot, const QColor &ink, bool largeText) {
    QString key = metric + ':' + QString::number(ink.rgba(), 16) + ':' + QApplication::font().toString() + (largeText ? ":large" : ":standard");
    const auto data = snapshot.value(metric == "disk" ? "diskIO" : metric).toObject();
    if (metric == "cpu" || metric == "memory") {
        key += ':' + QString::number(percentage(data.value(metric == "cpu" ? "usage" : "fraction")));
        if (metric == "cpu") for (const auto &point : cpuSamples(snapshot)) key += QStringLiteral("/%1%2").arg(point.value).arg(point.gap ? 'g' : 'c');
    } else if (metric == "network" || metric == "disk") {
        key += ':' + rateText(number(data.value(metric == "network" ? "uploadBytesPerSecond" : "readBytesPerSecond")), largeText)
            + '/' + rateText(number(data.value(metric == "network" ? "downloadBytesPerSecond" : "writeBytesPerSecond")), largeText);
    } else if (metric == "battery") key += ':' + data.value("state").toString() + ':' + QString::number(percentage(data.value("level")));
    return key;
}

QString toolTip(const QString &metric, const QJsonObject &snapshot) {
    const auto data = snapshot.value(metric == "disk" ? "diskIO" : metric).toObject();
    QString result;
    if (metric == "cpu") result = T("处理器 %1\n用户 %2 · 系统 %3\n单击查看处理器详情")
        .arg(percentText(percentage(data.value("usage"))), percentText(percentage(data.value("user"))), percentText(percentage(data.value("system"))));
    else if (metric == "memory") result = T("内存 %1\n已用 %2 / 总计 %3\n单击查看内存详情")
        .arg(percentText(percentage(data.value("fraction"))), formatBytes(number(data.value("usedBytes"))), formatBytes(number(data.value("totalBytes"))));
    else if (metric == "network") result = T("网络%1\n↑ 上传 %2\n↓ 下载 %3\n单击查看网络详情")
        .arg(data.value("interface").toString().isEmpty() ? QString() : " · " + data.value("interface").toString(),
            formatBytes(number(data.value("uploadBytesPerSecond")), true), formatBytes(number(data.value("downloadBytesPerSecond")), true));
    else if (metric == "disk") result = T("磁盘读写\n读取 %1\n写入 %2\n单击查看磁盘详情")
        .arg(formatBytes(number(data.value("readBytesPerSecond")), true), formatBytes(number(data.value("writeBytesPerSecond")), true));
    else if (metric == "battery") {
        const QString state = data.value("state").toString();
        result = T("电池 / 电源\n%1%2\n单击查看电池与应用影响")
            .arg(state == "noBattery" ? QString() : percentText(percentage(data.value("level"))) + " · ", batteryTitle(state));
    }
    return result;
}

QImage render(const QString &metric, const QJsonObject &snapshot, const QColor &ink, int scale, bool largeText) {
    scale = std::clamp(scale, 1, 3);
    QImage image(IconSize * scale, IconSize * scale, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
    QPainter p(&image); p.setRenderHint(QPainter::Antialiasing); p.setPen(ink);
    const auto data = snapshot.value(metric == "disk" ? "diskIO" : metric).toObject();
    const QRectF label(0, 8, 22, 14);
    const int labelPixels = largeText ? 14 : 12, ratePixels = largeText ? 13 : 11;
    if (metric == "cpu") {
        const auto points = cpuSamples(snapshot); const int current = percentage(data.value("usage"));
        QColor faint = ink; faint.setAlphaF(ink.alphaF() * .18);
        if (points.isEmpty()) {
            p.setPen(QPen(faint, 1)); p.drawRoundedRect(QRectF(1.5, 1.5, 19, 5), 1, 1);
            if (current >= 0) p.fillRect(QRectF(2, 2, 18 * current / 100., 4), ink);
        } else {
            int begin = 0;
            auto at = [&](int i) { return QPointF(points.size() == 1 ? 20 : 1.5 + 19. * i / (points.size() - 1), 6.5 - points[i].value * .05); };
            while (begin < points.size()) {
                if (points[begin].value < 0) { ++begin; continue; }
                int end = begin + 1; while (end < points.size() && points[end].value >= 0 && !points[end].gap) ++end;
                QPainterPath line; line.moveTo(at(begin));
                for (int i = begin + 1; i < end; ++i) line.lineTo(at(i));
                QPainterPath area(line); area.lineTo(at(end - 1).x(), 7); area.lineTo(at(begin).x(), 7); area.closeSubpath();
                p.fillPath(area, faint); p.setBrush(Qt::NoBrush); p.setPen(QPen(ink, 1.1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.drawPath(line);
                if (end == begin + 1) { p.setPen(Qt::NoPen); p.setBrush(ink); p.drawEllipse(at(begin), .7, .7); }
                begin = end;
            }
        }
        p.setPen(ink); text(p, iconPercent(current), label, labelPixels);
    } else if (metric == "memory") {
        const int used = percentage(data.value("fraction"));
        // A DIMM keeps its three chips, contact fingers and keyed notch at every
        // usage level. A variable bar inside this small outline reads as a battery.
        p.setPen(QPen(ink, 1, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin)); p.setBrush(Qt::NoBrush);
        QPainterPath board; board.moveTo(2.5, .5); board.lineTo(19.5, .5); board.lineTo(19.5, 5.5);
        board.lineTo(12.5, 5.5); board.lineTo(12.5, 4.5); board.lineTo(10.5, 4.5); board.lineTo(10.5, 5.5);
        board.lineTo(2.5, 5.5); board.closeSubpath(); p.drawPath(board);
        for (qreal x : {4., 9., 14.}) p.fillRect(QRectF(x, 2, 3, 2), ink);
        for (qreal x : {4.5, 6.5, 8.5, 13.5, 15.5, 17.5}) p.drawLine(QPointF(x, 5.5), QPointF(x, 7));
        text(p, iconPercent(used), label, labelPixels);
    } else if (metric == "network" || metric == "disk") {
        for (int row = 0; row < 2; ++row) {
            p.setPen(QPen(ink, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const qreal top = row * 11;
            if (metric == "network") {
                const qreal tip = top + (row == 0 ? 2 : 9), tail = top + (row == 0 ? 9 : 2), wing = tip + (row == 0 ? 2 : -2);
                p.drawLine(QPointF(2, tail), QPointF(2, tip));
                p.drawLine(QPointF(.5, wing), QPointF(2, tip)); p.drawLine(QPointF(3.5, wing), QPointF(2, tip));
            } else text(p, row == 0 ? "R" : "W", QRectF(0, top, 4, 11), 9);
            const QString name = metric == "network" ? (row == 0 ? "uploadBytesPerSecond" : "downloadBytesPerSecond") : (row == 0 ? "readBytesPerSecond" : "writeBytesPerSecond");
            text(p, rateText(number(data.value(name)), largeText), QRectF(4.5, top, 17.5, 11), ratePixels, Qt::AlignRight | Qt::AlignVCenter);
        }
    } else if (metric == "battery") {
        const QString state = data.value("state").toString(); const int level = percentage(data.value("level"));
        p.setPen(QPen(ink, 1, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin)); p.setBrush(Qt::NoBrush);
        if (state == "noBattery") {
            p.drawRoundedRect(QRectF(7.5, 2.5, 7, 3.5), 1, 1); p.drawLine(QPointF(9.5, .5), QPointF(9.5, 2.5));
            p.drawLine(QPointF(12.5, .5), QPointF(12.5, 2.5)); p.drawLine(QPointF(11, 6), QPointF(11, 7));
        } else {
            p.drawRoundedRect(QRectF(1.5, 1, 17, 5.5), 1.5, 1.5); p.fillRect(QRectF(19.5, 2.5, 1, 2.5), ink);
            if (level >= 0 && state != "unknown") p.fillRect(QRectF(3, 2, 14 * level / 100., 3.5), ink);
            if (state == "charging") {
                QPainterPath bolt; bolt.moveTo(11.5, .5); bolt.lineTo(7.5, 4); bolt.lineTo(10, 4); bolt.lineTo(9, 7.5);
                bolt.lineTo(13.8, 3); bolt.lineTo(11.3, 3); bolt.closeSubpath(); overlay(p, bolt, ink);
            } else if (state == "pluggedIn") {
                QPainterPath pause; pause.addRect(QRectF(8, 2, 1, 3.5)); pause.addRect(QRectF(11, 2, 1, 3.5)); overlay(p, pause, ink);
            } else if (state == "unknown") text(p, "?", QRectF(5, 0, 11, 8), 10);
        }
        p.setPen(ink); text(p, state == "noBattery" ? QString::fromUtf8("—") : iconPercent(level), label, labelPixels);
    }
    p.end(); return image;
}
}
