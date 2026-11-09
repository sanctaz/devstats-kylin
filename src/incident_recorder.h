#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <memory>

// All calls, including destruction, belong to the monitoring worker thread.
// Uses existing snapshots only. No timer, process scan or external service.
class IncidentRecorder {
public:
    explicit IncidentRecorder(const QString &dataRoot);
    ~IncidentRecorder();
    IncidentRecorder(const IncidentRecorder &) = delete;
    IncidentRecorder &operator=(const IncidentRecorder &) = delete;
    void setEnabled(bool enabled);
    void resetObservation();
    bool wantsFreshProcesses() const;
    // True only when a successful commit changed persisted incident data.
    bool observe(const QJsonObject &snapshot);
    QJsonArray list(int limit = 100);
    QJsonObject detail(const QString &id);
    void flush();
    QString error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
