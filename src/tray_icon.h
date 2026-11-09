#pragma once
#include <QColor>
#include <QImage>
#include <QJsonObject>
#include <QString>

namespace DevStatsTray {
constexpr int IconSize = 22;
QString compactRate(double bytesPerSecond);
QString cacheKey(const QString &metric, const QJsonObject &snapshot, const QColor &ink, bool largeText = false);
QString toolTip(const QString &metric, const QJsonObject &snapshot);
QImage render(const QString &metric, const QJsonObject &snapshot, const QColor &ink, int scale = 1, bool largeText = false);
}
