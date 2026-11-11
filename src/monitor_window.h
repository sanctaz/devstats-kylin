#pragma once

#include <QMainWindow>
#include <QJsonObject>
#include <QJsonArray>
#include <QStringList>
#include <memory>

class HistoryFeed;
class ProcessRulesPanel;

class MonitorWindow final : public QMainWindow {
    Q_OBJECT
public:
    // appearance: 0 follows the system, 1 dark, 2 light.
    explicit MonitorWindow(int appearance = 1, QWidget *parent = nullptr);
    ~MonitorWindow() override;
    void setSnapshot(const QJsonObject &snapshot);
    void setHistories(const HistoryFeed &feed);
    void setSelectedModule(const QString &module);
    QString selectedModule() const;
    qint64 historyDuration() const;
    void setPreferences(bool autoStart, int trayTheme, const QStringList &manualProjects,
                        const QStringList &customApps, bool largeTrayText = true, bool automationNotifications = true,
                        int language = 0);
    // Opens the settings dialog, or brings it forward.
    void showSettings();
    bool settingsVisible() const;
    void setAppearance(int mode);
    QJsonObject diagnosticReport() const;
    void setDiagnosticFeedback(const QString &message);
    void setIncidents(const QJsonArray &incidents);
    void setIncidentDetail(const QJsonObject &incident);
    void setIncidentRecording(bool enabled);
    void showIncidents();
    ProcessRulesPanel *rulesPanel() const;
signals:
    void selectionChanged(QString module);
    void durationChanged(qint64 seconds);
    void visibilityChanged(bool visible);
    void networkObservationRequested(bool enabled);
    void suspectRankingRequested(bool enabled);
    void addProjectRequested();
    void removeProjectRequested(QString path);
    void autoStartRequested(bool enabled);
    void trayThemeRequested(int theme);
    void largeTrayTextRequested(bool enabled);
    void automationNotificationsRequested(bool enabled);
    void appearanceRequested(int mode);
    // 0 follows the system, 1 Chinese, 2 English.
    void languageRequested(int mode);
    void quickLaunchRequested(QString application);
    void diagnosticRunRequested(bool start);
    void processActionRequested(QJsonObject identity, QString action);
    void diagnosticExportRequested();
    void incidentRequested(QString id);
    void incidentRecordingRequested(bool enabled);
    void exportIncidentRequested(QString id);
    void memoryReliefRequested();
protected:
    void closeEvent(QCloseEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
    void renderCurrentSnapshot();
    void renderCurrentHistories();
    void publishVisibility(bool visible);
    void publishNetworkObservation();
};
