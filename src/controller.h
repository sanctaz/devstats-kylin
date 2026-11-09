#pragma once
#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QHash>
#include <QPointer>
#include <QSettings>
#include <QSystemTrayIcon>
#include <QThread>
#include <QElapsedTimer>
#include <QMap>
#include <QVector>
#include <memory>
#include <deque>
#include "activity_tracker.h"
#include "diagnostic_run.h"

class LinuxSampler;
class ProcessNetworkSampler;
class HistoryStore;
class IncidentRecorder;
class MemoryOptimizer;
class ApplicationActions;
class MemoryReliefDialog;
class ProcessRules;
class ResponsivenessProbe;
class MonitorWindow;
class QTimer;
class QLocalServer;
class QMenu;
class QAction;

class MonitorWorker final : public QObject {
    Q_OBJECT
public:
    explicit MonitorWorker(QString root, QStringList projects, bool incidentRecording = true);
    ~MonitorWorker() override;
public slots:
    void start();
    void stop();
    void setPresentationVisible(bool visible);
    void setNetworkObservation(bool enabled);
    void setProjects(const QStringList &projects);
    void setHistoryDuration(qint64 seconds);
    void refreshDiagnostics();
    void runDiagnostics();
    void cancelDiagnostics();
    void setIncidentRecording(bool enabled);
    void requestIncidents();
    void requestIncident(QString id);
    void exportIncident(QString id);
    void reclaimMemoryForRelief();
    void inspectApplicationWindows(QJsonArray identities, QStringList names);
    void closeApplicationWindows(QString planId);
    void cancelApplicationClose(QString planId);
    void setSleeping(bool sleeping);
    void setProcessRules(bool enabled, QJsonArray rules);
    void pauseProcessRules();
signals:
    void snapshotReady(const QJsonObject &snapshot);
    void historyReady(const QJsonObject &history, qint64 start, qint64 end);
    void actionFinished(QString message);
    void incidentsReady(QJsonArray incidents, QString error);
    void incidentDetailReady(QJsonObject incident);
    void incidentExportReady(QJsonObject incident);
    void applicationPlanReady(QJsonObject plan);
    void reliefOutcomeReady(QJsonObject outcome);
    void reliefBusyChanged(bool busy);
    void processRulesConfigured(QJsonObject result);
private:
    void sample();
    void finishDiagnostics(const QString &state);
    friend class MonitorWorkerTest;
    void updateNetworkPresentation();
    void publishHistory();
    void publishIncidents();
    void publishRelief(QJsonObject result);
    void observeRelief();
    void beginReliefObservation(QJsonObject result, const QJsonArray &identities);
    QString root_;
    QStringList projects_;
    std::unique_ptr<LinuxSampler> sampler_;
    std::unique_ptr<ResponsivenessProbe> probe_;
    std::unique_ptr<ProcessNetworkSampler> processNetwork_;
    QJsonObject cachedNetwork_, lastMeasuredNetwork_;
    bool networkObservation_ = false;
    std::unique_ptr<HistoryStore> history_;
    std::unique_ptr<IncidentRecorder> incidents_;
    std::unique_ptr<MemoryOptimizer> memoryOptimizer_;
    std::unique_ptr<ApplicationActions> applicationActions_;
    std::unique_ptr<ProcessRules> processRules_;
    bool automationEnabled_ = false;
    QJsonArray automationRules_;
    QTimer *timer_ = nullptr;
    QElapsedTimer clock_;
    qint64 lastProcessSample_ = -1;
    int processSamples_ = 0;
    bool visible_ = false, stopped_ = true;
    bool sleeping_ = false;
    bool incidentRecording_ = true, incidentsDirty_ = true;
    qint64 duration_ = 120, lastArchiveQuery_ = -1;
    QJsonObject latest_, archived_;
    QJsonObject lastMemoryOptimization_;
    QJsonObject pendingApplicationPlan_, reliefBefore_, reliefResult_, lastReliefOutcome_;
    QJsonArray reliefIdentities_;
    qint64 reliefPlanAt_ = -1, reliefObservationAt_ = -1;
    bool reliefBusy_ = false;
    QJsonObject cachedActivity_;
    QJsonArray cachedSuspects_;
    RecentActivityTracker activity_;
    DiagnosticRun diagnosticRun_;
    QJsonObject lastDiagnosticRun_;
    Qt::TimerType diagnosticTimerType_ = Qt::CoarseTimer;
    QMap<QString, std::deque<QPair<qint64, double>>> raw_;
};

class AppController final : public QObject {
    Q_OBJECT
public:
    explicit AppController(QObject *parent = nullptr);
    ~AppController() override;
    void start(bool show);
    void showModule(const QString &module = QString());
    void stop();
    QJsonObject latestSnapshot() const { return latest_; }
    MonitorWindow *window() const;
    static QString autostartPath();
    static bool autoStartEnabled();
signals:
    void snapshotUpdated();
private:
    bool eventFilter(QObject *object, QEvent *event) override;
    void ensureWindow();
    void updateTray();
    void updatePreferences();
    void setAutoStart(bool enabled);
    void applyMenuTheme();
    void notifyAutomation();
    void launchApplication(const QString &id);
    void addProject();
    void showError(const QString &message);
    void storePreference(const QString &key, const QVariant &value);
    void saveReport(const QString &html, const QString &title, const QString &prefix);
    void showMemoryRelief();
    QSettings settings_;
    QThread thread_;
    MonitorWorker *worker_ = nullptr;
    QPointer<MonitorWindow> window_;
    QTimer *windowReleaseTimer_ = nullptr;
    QPointer<MemoryReliefDialog> reliefDialog_;
    bool automationEnabled_ = false;
    bool automationNotifications_ = true;
    QString lastNotifiedAction_;
    qint64 lastNotificationAt_ = 0;
    QJsonArray automationRules_;
    bool reliefBusy_ = false;
    QJsonObject latest_, history_;
    QJsonArray incidents_;
    QJsonObject reliefOutcome_;
    qint64 historyStart_ = 0, historyEnd_ = 0;
    QString selected_ = "overview";
    QStringList projects_, customApps_;
    QVector<QSystemTrayIcon *> trays_;
    QHash<QString, QString> trayIconKeys_, trayToolTips_;
    QMenu *menu_ = nullptr;
    int trayTheme_ = 0;
    int appearance_ = 1;
    bool largeTrayText_ = true;
    bool incidentRecording_ = true;
    bool stopped_ = false;
    bool settingsWarningShown_ = false;
};
