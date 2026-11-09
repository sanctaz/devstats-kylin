#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

// Exported reports are single HTML pages: inline styles, no scripts and no remote
// resources, every value escaped. The data a page was built from is appended to it,
// so the numbers can still be checked or processed.
namespace DevStatsReport {
QString diagnosisHtml(const QJsonObject &report);
QString incidentHtml(const QJsonObject &incident);
// Adds ".html" when the chosen name has no suffix.
QString htmlPath(const QString &chosen);
// Writes atomically with owner-only permissions. Returns an error message, or nothing.
QString writePrivate(const QString &path, const QByteArray &data);
}
