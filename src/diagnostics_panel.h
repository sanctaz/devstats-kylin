#pragma once
#include <QWidget>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>

class HistoryFeed;

class DiagnosticsPanel final : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPanel(QWidget *parent = nullptr);
    ~DiagnosticsPanel() override;
    void setSnapshot(const QJsonObject &snapshot);
    void setHistories(const HistoryFeed &feed);
    void setFeedback(const QString &message);
    void setIncidents(const QJsonArray &incidents);
    void setIncidentDetail(const QJsonObject &incident);
    void setIncidentRecording(bool enabled);
    void showIncidents();
    QJsonObject report() const;
signals:
    // action: "terminate" (SIGTERM) or "kill" (SIGKILL). The controller confirms first.
    void processActionRequested(QJsonObject identity, QString action);
    void addRuleRequested(QJsonObject process);
    void diagnosticRunRequested(bool start);
    void exportReportRequested();
    void incidentRequested(QString id);
    void incidentRecordingRequested(bool enabled);
    void exportIncidentRequested(QString id);
    void memoryReliefRequested();
protected:
    void showEvent(QShowEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
