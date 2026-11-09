#pragma once

#include <QJsonArray>
#include <QJsonObject>

// Pure analysis of one sampler snapshot. PSI averages are percentages; CPU usage
// and iowait are fractions; process CPU uses one-core percent.
// Result: state (normal/attention/critical/unknown), headline, summary, findings
// (most important first), meters (one per dashboard tile) and coverage notes.
// A finding says what the system shows, and a suspect is whoever was busiest while
// it happened: that is strong correlation, not proof of causality.
QJsonObject diagnoseSystem(const QJsonObject &snapshot);

// Which resource to rank processes by for this diagnosis: "cpu", "memory" or "io".
QString suspectBasis(const QJsonObject &diagnosis);

// Display order among findings of equal severity: causes before their symptoms.
int findingPriority(const QString &kind);

// Applications of the current user and bounded system processes, ranked together.
// Only rows with "own": true have members that can be acted on.
QJsonArray rankSuspects(const QJsonObject &snapshot, const QString &basis, int limit = 12);
