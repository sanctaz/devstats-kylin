#pragma once
#include <QJsonArray>
#include <QJsonObject>

// Measurements are observations of concurrent activity, never "bytes freed".
namespace DevStatsRelief {
QJsonObject baseline(const QJsonObject &snapshot, const QJsonArray &identities);
QJsonObject outcome(const QJsonObject &before, const QJsonObject &after,
                    const QJsonArray &identities, QJsonObject operation);
}
