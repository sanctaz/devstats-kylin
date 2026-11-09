#include "controller.h"
#include "tray_icon.h"
#include "linux_sampler.h"
#include "process_network_sampler.h"
#include "history_store.h"
#include "responsiveness_probe.h"
#include "runtime_env.h"
#include "monitor_window.h"
#include "charts.h"
#include <QApplication>
#include <QTimer>
#include <QDateTime>
#include <QJsonArray>
#include <QMenu>
#include <QAction>
#include <QFileDialog>
#include <QMessageBox>
#include <QSaveFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QProcess>
#include <QDesktopServices>
#include <QUrl>
#include <QDBusConnection>
#include <QJsonDocument>
#include <QPushButton>
#include <cmath>
#include "diagnostics.h"
#include "process_actions.h"
#include "incident_recorder.h"
#include "memory_optimizer.h"
#include "application_actions.h"
#include "memory_relief_dialog.h"
#include "relief_observation.h"
#include "process_rules.h"
#include "process_rules_panel.h"
#include "ui_theme.h"
#include <QUuid>

namespace {
QJsonObject inactiveNetwork(const QString &reason) {
    return {{"state", "inactive"}, {"observing", false}, {"reason", reason}, {"applications", QJsonArray{}}};
}
QString configFile() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/devstats/settings.ini";
}
QJsonValue pressureFraction(const QJsonObject &snapshot, const QString &resource) {
    const auto v = snapshot.value("pressure").toObject().value(resource).toObject().value("some").toObject().value("avg10");
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 && v.toDouble() <= 100 ? QJsonValue(v.toDouble() / 100) : QJsonValue(QJsonValue::Null);
}
const QStringList metricKeys = {"cpu", "memory", "network", "battery", "disk"};
const QStringList metricTitles = {QString::fromUtf8("CPU"), QString::fromUtf8("内存"),
    QString::fromUtf8("网络"), QString::fromUtf8("电池"), QString::fromUtf8("磁盘")};
bool identitiesCurrent(const QJsonObject &snapshot, const QJsonArray &identities) {
    if (identities.isEmpty() || identities.size()>64 || snapshot.value("processSampleAgeSeconds").toDouble(999)>5 ||
        std::abs(QDateTime::currentMSecsSinceEpoch()/1000.-snapshot.value("timestamp").toDouble())>6) return false;
    for(const auto &value:identities) {
        const auto id=value.toObject(); bool found=false;
        for(const auto &entry:snapshot.value("processes").toArray()) {
            const auto p=entry.toObject();
            if(p.value("pid")==id.value("pid") && p.value("startTicks")==id.value("startTicks") &&
                !id.value("executablePath").toString().isEmpty() && p.value("executablePath")==id.value("executablePath")) {found=true;break;}
        }
        if(!found)return false;
    }
    return true;
}
}

MonitorWorker::MonitorWorker(QString root, QStringList projects, bool incidentRecording)
    : root_(std::move(root)), projects_(std::move(projects)), incidentRecording_(incidentRecording) {}
MonitorWorker::~MonitorWorker() = default;

void MonitorWorker::start() {
    if (!stopped_) return;
    stopped_ = false;
    clock_.start();
    if (lastDiagnosticRun_.isEmpty()) lastDiagnosticRun_ = diagnosticRun_.result();
    sampler_.reset(new LinuxSampler(qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc"),
                                    qEnvironmentVariable("DEVSTATS_SYS_ROOT", "/sys")));
    sampler_->setProjectRoots(projects_);
    processNetwork_.reset(new ProcessNetworkSampler(qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc")));
    cachedNetwork_ = inactiveNetwork(QStringLiteral("仅在网络页可见时采集应用流量。"));
    history_.reset(new HistoryStore(root_));
    history_->open();
    incidents_.reset(new IncidentRecorder(root_));
    incidents_->setEnabled(incidentRecording_);
    memoryOptimizer_.reset(new MemoryOptimizer());
    applicationActions_.reset(new ApplicationActions());
    processRules_.reset(new ProcessRules(root_));
    processRules_->configure(automationEnabled_, automationRules_);
    // Fixtures replay recorded /proc data; this machine's real latency would not match it.
    if (qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc") == "/proc" && qEnvironmentVariableIsEmpty("DEVSTATS_DISABLE_PROBES")) {
        probe_.reset(new ResponsivenessProbe);
        probe_->start();
    }
    timer_ = new QTimer(this);
    timer_->setInterval(2000);
    timer_->setTimerType(Qt::CoarseTimer);
    connect(timer_, &QTimer::timeout, this, &MonitorWorker::sample);
    timer_->start();
    sample();
}

void MonitorWorker::stop() {
    if (stopped_) return;
    finishDiagnostics(QStringLiteral("interrupted"));
    stopped_ = true;
    if (processNetwork_) processNetwork_->reset();
    processNetwork_.reset();
    cachedNetwork_ = inactiveNetwork(QStringLiteral("监控已停止。"));
    cancelApplicationClose(QString());
    if (timer_) { timer_->stop(); delete timer_; timer_ = nullptr; }
    probe_.reset();
    if (history_) { history_->flush(); history_.reset(); }
    if (incidents_) { incidents_->resetObservation(); incidents_->flush(); incidents_.reset(); }
    memoryOptimizer_.reset();
    applicationActions_.reset();
    processRules_.reset();
    pendingApplicationPlan_ = {};
    if(reliefBusy_) publishRelief({{"ok",false},{"message",QStringLiteral("监控已停止，后续内存观测中断。")}});
    sampler_.reset();
}

void MonitorWorker::setPresentationVisible(bool visible) {
    if (visible_ == visible) return;
    visible_ = visible;
    if (visible && !stopped_ && !sleeping_) { lastProcessSample_ = -1; sample(); requestIncidents(); }
}

void MonitorWorker::setNetworkObservation(bool enabled) {
    if (networkObservation_ == enabled) return;
    networkObservation_ = enabled;
    if (processNetwork_) processNetwork_->reset();
    cachedNetwork_ = inactiveNetwork(enabled ? QStringLiteral("正在等待网络采样。") : QStringLiteral("网络页已隐藏，应用流量采样暂停。"));
    if (enabled && !stopped_ && !sleeping_ && processNetwork_) {
        // Showing the window may have just obtained fresh process metadata.
        // Reuse that scan for the first TCP baseline instead of scanning twice.
        if (lastProcessSample_ >= 0 && clock_.elapsed() - lastProcessSample_ < 500 && latest_.value("processesFresh").toBool()) {
            cachedNetwork_ = processNetwork_->sample(latest_); cachedNetwork_["observing"] = true;
        } else { lastProcessSample_ = -1; sample(); return; }
    }
    updateNetworkPresentation();
    if (!latest_.isEmpty()) emit snapshotReady(latest_);
}

void MonitorWorker::setProjects(const QStringList &projects) {
    projects_ = projects;
    if (sampler_) sampler_->setProjectRoots(projects_);
    lastProcessSample_ = -1;
    if (!stopped_ && !sleeping_) sample();
}

void MonitorWorker::setHistoryDuration(qint64 seconds) {
    duration_ = qBound<qint64>(120, seconds, 30 * 86400);
    lastArchiveQuery_ = -1;
    if (visible_ && !stopped_) publishHistory();
}

void MonitorWorker::refreshDiagnostics() {
    if (!stopped_ && !sleeping_ && (lastProcessSample_ < 0 || clock_.elapsed() - lastProcessSample_ >= 750)) {
        lastProcessSample_ = -1; sample();
    }
}
void MonitorWorker::runDiagnostics() {
    if (stopped_ || sleeping_ || !sampler_ || diagnosticRun_.running()) return;
    if (!diagnosticRun_.start(clock_.elapsed(), QDateTime::currentMSecsSinceEpoch() / 1000.)) return;
    memoryOptimizer_->resetObservation();
    diagnosticTimerType_ = timer_->timerType();
    timer_->setTimerType(Qt::PreciseTimer);
    timer_->start(); // Reuse the existing timer with a fresh two-second phase.
    lastProcessSample_ = -1;
    if (probe_) probe_->setFast(true);
    sample();
}

void MonitorWorker::finishDiagnostics(const QString &state) {
    if (!diagnosticRun_.finish(state, clock_.elapsed(), QDateTime::currentMSecsSinceEpoch() / 1000.)) return;
    if (memoryOptimizer_) memoryOptimizer_->resetObservation();
    if (probe_) probe_->setFast(false);
    if (timer_) { timer_->setTimerType(diagnosticTimerType_); timer_->start(); }
    lastDiagnosticRun_ = diagnosticRun_.result();
    latest_["diagnosticRun"] = lastDiagnosticRun_;
    emit snapshotReady(latest_);
}

void MonitorWorker::cancelDiagnostics() { finishDiagnostics(QStringLiteral("cancelled")); }

void MonitorWorker::setIncidentRecording(bool enabled) {
    incidentRecording_ = enabled;
    if (incidents_) incidents_->setEnabled(enabled);
    incidentsDirty_ = true;
    if (visible_) publishIncidents();
}

void MonitorWorker::setProcessRules(bool enabled, QJsonArray rules) {
    ProcessRules validator;
    auto result = processRules_ ? processRules_->configure(enabled, rules) : validator.configure(enabled, rules);
    if (result.value("ok").toBool()) {
        automationEnabled_ = enabled;
        automationRules_ = result.value("rules").toArray();
        result["enabled"] = enabled;
        if (!latest_.isEmpty() && processRules_) {
            latest_["processAutomation"] = processRules_->state();
            emit snapshotReady(latest_);
        }
    }
    emit processRulesConfigured(result);
}

void MonitorWorker::pauseProcessRules() {
    automationEnabled_ = false;
    if (processRules_) processRules_->pause();
    const auto state = processRules_ ? processRules_->state() : QJsonObject{{"enabled", false}, {"rules", automationRules_}};
    auto result = state;
    result["ok"] = true; result["pauseOnly"] = true;
    emit processRulesConfigured(result);
    if (!latest_.isEmpty()) { latest_["processAutomation"] = state; emit snapshotReady(latest_); }
}

void MonitorWorker::requestIncidents() { incidentsDirty_ = true; publishIncidents(); }

void MonitorWorker::publishIncidents() {
    if (!incidents_ || !incidentsDirty_) return;
    const auto result = incidents_->list(100);
    emit incidentsReady(result, incidents_->error());
    incidentsDirty_ = false;
}

void MonitorWorker::requestIncident(QString id) {
    if (!incidents_) return;
    auto result = incidents_->detail(id);
    if (result.isEmpty()) result = QJsonObject{{"id", id}, {"error", incidents_->error().isEmpty()
        ? QStringLiteral("事件已超出保留范围，或记录不存在。") : incidents_->error()}};
    emit incidentDetailReady(result);
}

void MonitorWorker::exportIncident(QString id) {
    if (!incidents_) return;
    const auto result = incidents_->detail(id);
    if (result.isEmpty()) emit actionFinished(incidents_->error().isEmpty() ? QStringLiteral("事件记录不存在，未导出。") : incidents_->error());
    else emit incidentExportReady(result);
}

void MonitorWorker::publishRelief(QJsonObject result) {
    if (!result.contains("beforeAvailableBytes")) result = DevStatsRelief::outcome(reliefBefore_, {}, reliefIdentities_, result);
    if (!result.value("timestamp").isDouble()) result["timestamp"] = QDateTime::currentMSecsSinceEpoch()/1000.;
    lastReliefOutcome_ = result;
    reliefObservationAt_ = -1; reliefBusy_ = false;
    reliefBefore_ = {}; reliefIdentities_ = {}; reliefResult_ = {};
    emit reliefOutcomeReady(result); emit reliefBusyChanged(false);
}

void MonitorWorker::beginReliefObservation(QJsonObject result, const QJsonArray &identities) {
    reliefResult_ = result; reliefIdentities_ = identities;
    reliefObservationAt_ = clock_.elapsed();
    result["phase"] = "awaitingObservation";
    result["message"] = result.value("message").toString()+QStringLiteral(" 正在利用后续采样观察内存变化…");
    emit reliefOutcomeReady(DevStatsRelief::outcome(reliefBefore_, {}, identities, result));
}

void MonitorWorker::observeRelief() {
    if (reliefObservationAt_<0 || clock_.elapsed()-reliefObservationAt_<4000) return;
    if (!latest_.value("processesFresh").toBool() && clock_.elapsed()-reliefObservationAt_<12000) return;
    auto result=DevStatsRelief::outcome(reliefBefore_,latest_,reliefIdentities_,reliefResult_);
    result["phase"]="observed";
    publishRelief(result);
}

void MonitorWorker::reclaimMemoryForRelief() {
    if (reliefBusy_) { emit actionFinished(QStringLiteral("正在处理其他减压操作，请等待结果。")); return; }
    if (!memoryOptimizer_ || stopped_ || sleeping_) {
        publishRelief({{"action","self"},{"ok",false},{"message",QStringLiteral("监控未运行，未执行回收。")}}); return;
    }
    reliefBusy_=true; emit reliefBusyChanged(true);
    reliefBefore_=DevStatsRelief::baseline(latest_,{});
    lastMemoryOptimization_=memoryOptimizer_->reclaim();
    publishRelief({{"action","self"},{"ok",lastMemoryOptimization_.value("attempted")},
        {"message",lastMemoryOptimization_.value("message")},{"selfResult",lastMemoryOptimization_}});
}

void MonitorWorker::inspectApplicationWindows(QJsonArray identities, QStringList names) {
    if(reliefBusy_) {emit actionFinished(QStringLiteral("正在处理其他减压操作，请等待结果。"));return;}
    if(stopped_ || sleeping_ || !applicationActions_) {
        publishRelief({{"action","applications"},{"ok",false},{"message",QStringLiteral("监控未运行，请恢复后重试。")}});return;
    }
    reliefBusy_=true;emit reliefBusyChanged(true);
    refreshDiagnostics();
    if(!identitiesCurrent(latest_,identities)) {
        publishRelief({{"action","applications"},{"ok",false},{"message",QStringLiteral("应用进程已变化或超过64个，请刷新后重新选择。")}});return;
    }
    const auto inspected=applicationActions_->inspect(identities);
    if(!inspected.value("canClose").toBool()) {
        publishRelief({{"action","applications"},{"ok",false},{"message",inspected.value("message")}});return;
    }
    pendingApplicationPlan_=inspected;
    pendingApplicationPlan_["planId"]=QUuid::createUuid().toString(QUuid::WithoutBraces);
    pendingApplicationPlan_["identities"]=identities;
    pendingApplicationPlan_["applicationNames"]=QJsonArray::fromStringList(names);
    reliefPlanAt_=clock_.elapsed();
    emit applicationPlanReady(pendingApplicationPlan_);
}

void MonitorWorker::cancelApplicationClose(QString planId) {
    if(pendingApplicationPlan_.isEmpty()) return;
    if(!planId.isEmpty() && pendingApplicationPlan_.value("planId").toString()!=planId) return;
    pendingApplicationPlan_={}; reliefPlanAt_=-1;
    applicationActions_.reset(new ApplicationActions());
    publishRelief({{"action","applications"},{"ok",false},{"phase","cancelled"},
        {"message",QStringLiteral("已取消正常关窗请求，未发送关闭消息。")}});
}

void MonitorWorker::closeApplicationWindows(QString planId) {
    if(pendingApplicationPlan_.isEmpty() || pendingApplicationPlan_.value("planId").toString()!=planId) return;
    const auto plan=pendingApplicationPlan_; pendingApplicationPlan_={};
    if(stopped_ || sleeping_ || !applicationActions_ || clock_.elapsed()-reliefPlanAt_>60000) {
        publishRelief({{"action","applications"},{"ok",false},{"message",QStringLiteral("确认已过期或监控暂停，请刷新后重新确认。")}});return;
    }
    const auto identities=plan.value("identities").toArray();
    reliefBefore_=DevStatsRelief::baseline(latest_,identities);
    auto result=applicationActions_->close(identities,plan.value("windows").toArray());
    result["action"]="applications";
    // Only retain summary and counts; internal window/process tokens stay private.
    result.remove("windows"); result.remove("identities");
    if(result.value("requestedCount").toInt()>0) beginReliefObservation(result,identities);
    else publishRelief(result);
}

void MonitorWorker::setSleeping(bool sleeping) {
    if (sleeping_ == sleeping || stopped_) return;
    sleeping_ = sleeping;
    if (sleeping) {
        if (processNetwork_) processNetwork_->reset();
        cachedNetwork_ = inactiveNetwork(QStringLiteral("系统休眠，应用流量采样暂停。"));
        updateNetworkPresentation();
        finishDiagnostics(QStringLiteral("interrupted"));
        cancelApplicationClose(QString());
        timer_->stop();
        if (probe_) probe_->stop();
        history_->flush();
        incidents_->resetObservation();
        incidentsDirty_ = true;
        memoryOptimizer_->resetObservation();
        if(reliefBusy_) publishRelief({{"ok",false},{"message",QStringLiteral("系统进入休眠，后续内存观测中断。")}});
        if (networkObservation_ && !latest_.isEmpty()) emit snapshotReady(latest_);
    } else {
        sampler_.reset(new LinuxSampler(qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc"),
                                        qEnvironmentVariable("DEVSTATS_SYS_ROOT", "/sys")));
        sampler_->setProjectRoots(projects_);
        activity_.reset();
        if (processRules_) processRules_->resetObservation();
        cachedActivity_ = QJsonObject();
        lastProcessSample_ = -1;
        processSamples_ = 0;
        lastArchiveQuery_ = -1;
        if (probe_) probe_->start();
        timer_->start();
        sample();
    }
}

void MonitorWorker::sample() {
    if (stopped_ || sleeping_) return;
    const qint64 now = clock_.elapsed();
    const qint64 processInterval = visible_ || networkObservation_ || processSamples_ < 2 || incidents_->wantsFreshProcesses() || reliefObservationAt_>=0 || diagnosticRun_.running() || processRules_->wantsFreshProcesses() ? 2000 : 10000;
    const bool scan = lastProcessSample_ < 0 || now - lastProcessSample_ >= processInterval - 100;
    latest_ = sampler_->sample(scan);
    if (scan) {
        lastProcessSample_ = now;
        ++processSamples_;
        activity_.apply(latest_);
    } else {
        // Retain the previously measured rolling CPU window without extending it
        // with duplicate cached samples.
        for (auto it = cachedActivity_.constBegin(); it != cachedActivity_.constEnd(); ++it)
            latest_[it.key()] = it.value();
    }
    if (scan) {
        for (const auto &key : {"processes", "activityApplications", "activityObservationSeconds", "activityBasis"})
            cachedActivity_[key] = latest_.value(key);
    }
    if (probe_) latest_["responsiveness"] = probe_->takeWindow();
    latest_["processSampleAgeSeconds"] = double(now - lastProcessSample_) / 1000;
    if (processRules_) latest_["processAutomation"] = scan ? processRules_->observe(latest_, now) : processRules_->state();
    if (networkObservation_ && scan && processNetwork_) { cachedNetwork_ = processNetwork_->sample(latest_); cachedNetwork_["observing"] = true; }
    updateNetworkPresentation();
    latest_["diagnostics"] = diagnoseSystem(latest_);
    // Ranking walks every process, so it runs only when someone will read the result.
    if (scan && (visible_ || diagnosticRun_.running())) cachedSuspects_ = rankSuspects(latest_, suspectBasis(latest_["diagnostics"].toObject()));
    latest_["suspects"] = cachedSuspects_;
    // A diagnostic run is observation only, including its final sample. Do not
    // feed its additional scans into the existing automatic heap-reclaim policy.
    const bool observing = diagnosticRun_.running();
    if (observing && diagnosticRun_.observe(latest_, now, QDateTime::currentMSecsSinceEpoch() / 1000.)) {
        memoryOptimizer_->resetObservation();
        if (probe_) probe_->setFast(false);
        timer_->setTimerType(diagnosticTimerType_); timer_->start();
    }
    if (observing) lastDiagnosticRun_ = diagnosticRun_.result();
    latest_["diagnosticRun"] = lastDiagnosticRun_;
    const auto optimization = observing ? QJsonObject() : memoryOptimizer_->observe(latest_);
    if (!optimization.isEmpty()) {
        lastMemoryOptimization_ = optimization;
        emit actionFinished(optimization.value("message").toString());
    }
    latest_["memoryOptimization"] = lastMemoryOptimization_;
    observeRelief();
    latest_["memoryReliefOutcome"] = lastReliefOutcome_;
    latest_["memoryReliefBusy"] = reliefBusy_;
    latest_["incidentRecording"] = incidentRecording_;
    if (incidents_->observe(latest_)) incidentsDirty_ = true;
    if (!incidents_->error().isEmpty()) latest_["incidentError"] = incidents_->error();
    if (!history_->record(latest_)) latest_["historyError"] = history_->error();
    const qint64 at = qint64(latest_.value("timestamp").toDouble());
    const auto cpu = latest_.value("cpu").toObject(), memory = latest_.value("memory").toObject();
    const auto disk = latest_.value("diskIO").toObject(), network = latest_.value("network").toObject();
    const auto battery = latest_.value("battery").toObject();
    const QMap<QString, QJsonValue> metrics{{"cpu", cpu.value("usage")}, {"memory", memory.value("fraction")},
        {"diskRead", disk.value("readBytesPerSecond")}, {"diskWrite", disk.value("writeBytesPerSecond")},
        {"networkDownload", network.value("downloadBytesPerSecond")}, {"networkUpload", network.value("uploadBytesPerSecond")},
        {"battery", battery.value("level")}, {"cpuIowait", cpu.value("iowait")},
        {"swapIn", latest_.value("vm").toObject().value("swapInBytesPerSecond")},
        {"swapOut", latest_.value("vm").toObject().value("swapOutBytesPerSecond")},
        {"memoryPressure", pressureFraction(latest_, "memory")}, {"ioPressure", pressureFraction(latest_, "io")},
        {"schedulerLatency", latest_.value("responsiveness").toObject().value("schedulerMaxMs")}, {"diskBusy", disk.value("busiestUtilization")}};
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it) {
        auto &points = raw_[it.key()];
        if (it.value().isDouble() && std::isfinite(it.value().toDouble()))
            points.push_back(qMakePair(at, it.value().toDouble()));
        while (!points.empty() && (points.front().first < at - 3600 || points.size() > 1801))
            points.pop_front();
    }
    QJsonArray menu;
    const auto &cpuPoints = raw_["cpu"];
    const int first = qMax(0, int(cpuPoints.size()) - 20);
    for (int i = first; i < int(cpuPoints.size()); ++i)
        menu.append(QJsonObject{{"timestamp", double(cpuPoints[size_t(i)].first)}, {"value", cpuPoints[size_t(i)].second}});
    latest_["menuCPUHistory"] = menu;
    emit snapshotReady(latest_);
    if (visible_) { publishHistory(); publishIncidents(); }
}

void MonitorWorker::updateNetworkPresentation() {
    lastMeasuredNetwork_ = ProcessNetworkSampler::retainedObservation(cachedNetwork_, lastMeasuredNetwork_);
    if (latest_.isEmpty()) return;
    latest_["networkApplications"] = cachedNetwork_;
    if (cachedNetwork_.value("state").toString() == "ready" || cachedNetwork_.value("state").toString() == "partial") latest_.remove("networkApplicationsPrevious");
    else latest_["networkApplicationsPrevious"] = lastMeasuredNetwork_;
}

void MonitorWorker::publishHistory() {
    if (!history_ || latest_.isEmpty()) return;
    const qint64 end = qint64(latest_.value("timestamp").toDouble());
    const qint64 start = end - duration_;
    qint64 firstRaw = end;
    for (auto it = raw_.cbegin(); it != raw_.cend(); ++it)
        if (!it.value().empty()) firstRaw = qMin(firstRaw, it.value().front().first);
    if (lastArchiveQuery_ < 0 || clock_.elapsed() - lastArchiveQuery_ >= 60000) {
        archived_ = history_->query(start, qMin(end, firstRaw - 1), 600);
        lastArchiveQuery_ = clock_.elapsed();
    }
    QJsonObject result;
    for (auto it = raw_.cbegin(); it != raw_.cend(); ++it) {
        QJsonArray points;
        const qint64 liveStart = it.value().empty() ? end + 1 : it.value().front().first;
        for (const auto &value : archived_.value(it.key()).toArray()) {
            const auto point = value.toObject();
            if (point.value("lastSample").toDouble(point.value("timestamp").toDouble()) < liveStart)
                points.append(point);
        }
        for (const auto &point : it.value()) if (point.first >= start && point.first <= end) {
            points.append(QJsonObject{{"timestamp", double(point.first)}, {"value", point.second},
                {"minimum", point.second}, {"maximum", point.second}, {"count", 1}, {"resolutionSeconds", 2}});
        }
        result.insert(it.key(), points);
    }
    emit historyReady(result, start, end);
}

AppController::AppController(QObject *parent)
    : QObject(parent), settings_(configFile(), QSettings::IniFormat) {
    windowReleaseTimer_ = new QTimer(this);
    windowReleaseTimer_->setObjectName("windowReleaseTimer");
    windowReleaseTimer_->setSingleShot(true); windowReleaseTimer_->setInterval(30000);
    connect(windowReleaseTimer_, &QTimer::timeout, this, [this] {
        if (!window_ || window_->isVisible()) return;
        // Dialogs are top-level windows even when their owner is hidden. Never
        // discard an open settings/relief/confirmation interaction with its owner.
        for (auto *child : window_->findChildren<QWidget *>()) {
            if (child->isWindow() && child->isVisible()) { windowReleaseTimer_->start(); return; }
        }
        delete window_.data();
    });
    projects_ = settings_.value("projects").toStringList();
    customApps_ = settings_.value("customApps").toStringList();
    trayTheme_ = settings_.value("trayTheme", 0).toInt();
    largeTrayText_ = settings_.value("trayLargeText", true).toBool();
    incidentRecording_ = settings_.value("incidentRecording", true).toBool();
    automationNotifications_ = settings_.value("automationNotifications", true).toBool();
    ProcessRules automationValidator;
    const auto automationConfig = automationValidator.configure(settings_.value("automationEnabled", false).toBool(),
        QJsonDocument::fromJson(settings_.value("automationRules").toByteArray()).array());
    if (automationConfig.value("ok").toBool()) {
        automationEnabled_ = automationConfig.value("enabled").toBool(settings_.value("automationEnabled", false).toBool());
        automationRules_ = automationConfig.value("rules").toArray();
    }
    appearance_ = qBound(0, settings_.value("appearance", 1).toInt(), 2);
    worker_ = new MonitorWorker(devStatsDataRoot(), projects_, incidentRecording_);
    worker_->setProcessRules(automationEnabled_, automationRules_);
    worker_->moveToThread(&thread_);
    connect(&thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(&thread_, &QThread::started, worker_, &MonitorWorker::start);
    connect(worker_, &MonitorWorker::snapshotReady, this, [this](const QJsonObject &snapshot) {
        latest_ = snapshot;
        updateTray();
        if (window_ && window_->isVisible() && !window_->isMinimized()) window_->setSnapshot(latest_);
        if (reliefDialog_ && reliefDialog_->isVisible()) reliefDialog_->setSnapshot(latest_);
        notifyAutomation();
        emit snapshotUpdated();
    });
    connect(worker_, &MonitorWorker::historyReady, this, [this](const QJsonObject &history, qint64 start, qint64 end) {
        history_ = history; historyStart_ = start; historyEnd_ = end;
        if (window_ && window_->isVisible() && !window_->isMinimized()) window_->setHistories(history, start, end);
    });
    connect(worker_, &MonitorWorker::incidentsReady, this, [this](const QJsonArray &incidents, const QString &error) {
        if (!error.isEmpty()) {
            if (window_) window_->setDiagnosticFeedback(QStringLiteral("事件记录：%1").arg(error));
            return;
        }
        incidents_ = incidents;
        if (window_) window_->setIncidents(incidents_);
    });
    connect(worker_, &MonitorWorker::incidentDetailReady, this, [this](const QJsonObject &incident) {
        if (window_) window_->setIncidentDetail(incident);
    });
    connect(worker_, &MonitorWorker::incidentExportReady, this, [this](const QJsonObject &incident) {
        if (!window_) return;
        saveReport(incident, QStringLiteral("导出卡顿事件（含当时应用名、PID 和资源指标）"), "DevStats-incident-");
    });
    connect(worker_, &MonitorWorker::reliefBusyChanged, this, [this](bool busy) {
        reliefBusy_=busy;
        if(reliefDialog_) reliefDialog_->setBusy(busy);
    });
    connect(worker_, &MonitorWorker::processRulesConfigured, this, [this](QJsonObject result) {
        if (result.value("ok").toBool()) {
            automationEnabled_ = result.value("enabled").toBool(); automationRules_ = result.value("rules").toArray();
            settings_.setValue("automationEnabled", automationEnabled_);
            settings_.setValue("automationRules", QJsonDocument(automationRules_).toJson(QJsonDocument::Compact));
            settings_.sync();
            if (settings_.status() != QSettings::NoError) {
                result["message"] = QStringLiteral("规则已在本次运行生效，但设置文件保存失败；请检查个人配置目录的写入权限。");
            } else result["message"] = automationEnabled_ ? QStringLiteral("已保存，规则已生效。") : QStringLiteral("已保存。自动查杀目前处于暂停。");
        }
        if (window_) window_->rulesPanel()->configurationFinished(result);
    });
    connect(worker_, &MonitorWorker::reliefOutcomeReady, this, [this](const QJsonObject &result) {
        reliefOutcome_=result;
        if(reliefDialog_) reliefDialog_->setOutcome(result);
    });
    connect(worker_, &MonitorWorker::actionFinished, this, [this](const QString &message) {
        if(reliefDialog_) reliefDialog_->setFeedback(message);
    });
    connect(worker_, &MonitorWorker::applicationPlanReady, this, [this](const QJsonObject &plan) {
        const QString id=plan.value("planId").toString();
        if(!reliefDialog_ || !reliefDialog_->isVisible()) {
            QMetaObject::invokeMethod(worker_,"cancelApplicationClose",Qt::QueuedConnection,Q_ARG(QString,id));return;
        }
        QStringList names; for(const auto &v:plan.value("applicationNames").toArray()) names.append(v.toString());
        QStringList details;for(const auto &v:plan.value("windows").toArray()) {
            const auto w=v.toObject();details.append(QStringLiteral("PID %1 · %2").arg(QString::number(w.value("pid").toDouble(),'f',0),w.value("title").toString()));
        }
        QMessageBox confirm(QMessageBox::Warning,QStringLiteral("正常关闭所选应用窗口"),QString(),QMessageBox::Cancel,reliefDialog_);
        confirm.setObjectName("confirmMemoryReliefApplications");confirm.setTextFormat(Qt::PlainText);
        confirm.setText(QStringLiteral("将请求正常关闭 %1 个已确认窗口：\n%2\n\n应用可以弹出保存询问，也可能保留后台进程。请先保存工作；窗口关闭不代表整个应用已退出，实际内存变化将在之后采样观察。")
            .arg(plan.value("windowCount").toInt()).arg(names.mid(0,8).join(QStringLiteral("、"))));
        confirm.setDetailedText(details.join('\n'));
        auto *proceed=confirm.addButton(QStringLiteral("请求正常关闭窗口"),QMessageBox::AcceptRole);confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        const char *slot=confirm.clickedButton()==proceed?"closeApplicationWindows":"cancelApplicationClose";
        QMetaObject::invokeMethod(worker_,slot,Qt::QueuedConnection,Q_ARG(QString,id));
    });
    QDBusConnection::systemBus().connect("org.freedesktop.login1", "/org/freedesktop/login1",
        "org.freedesktop.login1.Manager", "PrepareForSleep", worker_, SLOT(setSleeping(bool)));
    menu_ = new QMenu();
    auto *open = menu_->addAction(QString::fromUtf8("打开 DevStats"));
    connect(open, &QAction::triggered, this, [this] { showModule(); });
    auto *diagnose = menu_->addAction(QStringLiteral("一键诊断"));
    diagnose->setObjectName("diagnoseAction");
    connect(diagnose, &QAction::triggered, this, [this] {
        showModule("diagnostics");
        QMetaObject::invokeMethod(worker_, "runDiagnostics", Qt::QueuedConnection);
    });
    auto *automation = menu_->addAction(QStringLiteral("自动查杀…"));
    automation->setObjectName("processAutomationAction");
    connect(automation, &QAction::triggered, this, [this] { showModule("automation"); });
    auto *relief = menu_->addAction(QStringLiteral("内存减压…"));
    connect(relief, &QAction::triggered, this, [this] { showModule("memory"); showMemoryRelief(); });
    menu_->addSeparator();
    auto *metrics = menu_->addMenu(QString::fromUtf8("托盘显示指标"));
    QStringList enabled = settings_.value("trayMetrics", QStringList{"cpu", "memory", "network", "battery"}).toStringList();
    for (int i = 0; i < metricKeys.size(); ++i) {
        const QString key = metricKeys[i];
        auto *choice = metrics->addAction(metricTitles[i]);
        choice->setCheckable(true); choice->setChecked(enabled.contains(key));
        connect(choice, &QAction::toggled, this, [this, key](bool checked) {
            QStringList selected = settings_.value("trayMetrics", QStringList{"cpu", "memory", "network", "battery"}).toStringList();
            if (checked && !selected.contains(key)) selected.append(key);
            if (!checked) selected.removeAll(key);
            storePreference("trayMetrics", selected);
            updateTray();
        });
        // Headless Qt plugins have no native tray integration. Old Qt 5 XEmbed
        // fallback code can dereference a missing native interface offscreen.
        // Real X11/Wayland sessions retain Qt's automatic late tray discovery.
        if (QApplication::platformName() == "offscreen" || QApplication::platformName() == "minimal") continue;
        auto *tray = new QSystemTrayIcon(this);
        tray->setObjectName("DevStats." + key);
        tray->setContextMenu(menu_);
        connect(tray, &QSystemTrayIcon::activated, this, [this, key](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showModule(key);
        });
        trays_.append(tray);
    }
    menu_->addSeparator();
    auto *terminal = menu_->addAction(QString::fromUtf8("打开终端"));
    connect(terminal, &QAction::triggered, this, [this] { launchApplication("terminal"); });
    auto *files = menu_->addAction(QString::fromUtf8("打开数据文件夹"));
    connect(files, &QAction::triggered, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(devStatsDataRoot())); });
    menu_->addSeparator();
    auto *quit = menu_->addAction(QString::fromUtf8("退出 DevStats"));
    connect(quit, &QAction::triggered, qApp, &QApplication::quit);
    connect(qApp, &QApplication::paletteChanged, this, [this](const QPalette &) { updateTray(); applyMenuTheme(); });
    applyMenuTheme();
}

void AppController::applyMenuTheme() {
    const bool dark = appearance_ == 1 || (appearance_ == 0 && DevStatsUI::isDark(qApp->palette()));
    menu_->setPalette(DevStatsUI::palette(dark));
    menu_->setStyleSheet(DevStatsUI::styleSheet(dark));
}

// An automatic kill happens out of sight, so it is announced. At most one message
// every 30 seconds: a respawning program must not turn into a notification storm.
void AppController::notifyAutomation() {
    const auto log = latest_.value("processAutomation").toObject().value("log").toArray();
    if (log.isEmpty()) return;
    const auto entry = log.first().toObject();
    const QString id = entry.value("id").toString();
    if (lastNotifiedAction_.isEmpty()) { lastNotifiedAction_ = id; return; } // Past actions are not news.
    if (id == lastNotifiedAction_ || id.isEmpty()) return;
    lastNotifiedAction_ = id;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!automationNotifications_ || now - lastNotificationAt_ < 30000) return;
    for (auto *tray : trays_) {
        if (!tray->isVisible() || !QSystemTrayIcon::supportsMessages()) continue;
        lastNotificationAt_ = now;
        tray->showMessage(QStringLiteral("自动查杀：%1").arg(entry.value("name").toString()),
                          entry.value("condition").toString() + QStringLiteral("\n") + entry.value("message").toString(),
                          QSystemTrayIcon::Information, 6000);
        break;
    }
}

AppController::~AppController() {
    stop();
    if(reliefDialog_) delete reliefDialog_.data();
    if (window_) delete window_.data();
    for (auto *tray : trays_) tray->setContextMenu(nullptr);
    delete menu_;
}
MonitorWindow *AppController::window() const { return window_.data(); }

bool AppController::eventFilter(QObject *object, QEvent *event) {
    // A close/hide from an already minimized window does not change its
    // presentation-visible flag. Still bound this genuinely hidden window.
    if (!stopped_ && object == window_.data() &&
        (event->type() == QEvent::Close || event->type() == QEvent::Hide)) windowReleaseTimer_->start();
    return QObject::eventFilter(object, event);
}

void AppController::start(bool show) {
    thread_.start();
    updateTray();
    if (show) showModule();
    else if (!trays_.isEmpty()) {
        // Give the desktop panel time to start at login. Without a supported
        // tray, keep the application reachable through its detail window.
        QTimer::singleShot(15000, this, [this] {
            if (!stopped_ && !window_ && !QSystemTrayIcon::isSystemTrayAvailable()) {
                qWarning("DevStats: desktop tray unavailable; opening detail window.");
                showModule();
            }
        });
    }
}

void AppController::stop() {
    if (stopped_) return;
    stopped_ = true;
    windowReleaseTimer_->stop();
    if (thread_.isRunning()) {
        QMetaObject::invokeMethod(worker_, "stop", Qt::BlockingQueuedConnection);
        thread_.quit();
        thread_.wait();
    }
    settings_.sync();
}

void AppController::ensureWindow() {
    if (window_) return;
    window_ = new MonitorWindow(appearance_);
    window_->setAttribute(Qt::WA_DeleteOnClose, false);
    window_->installEventFilter(this);
    window_->setPreferences(autoStartEnabled(), trayTheme_, projects_, customApps_, largeTrayText_, automationNotifications_);
    window_->setIncidentRecording(incidentRecording_);
    window_->rulesPanel()->setConfiguration(automationEnabled_, automationRules_);
    connect(window_->rulesPanel(), &ProcessRulesPanel::configurationRequested, worker_, &MonitorWorker::setProcessRules);
    connect(window_->rulesPanel(), &ProcessRulesPanel::pauseRequested, worker_, &MonitorWorker::pauseProcessRules);
    if (settings_.contains("windowGeometry")) window_->restoreGeometry(settings_.value("windowGeometry").toByteArray());
    window_->setSelectedModule(selected_);
    connect(window_, &MonitorWindow::selectionChanged, this, [this](const QString &module) { selected_ = module; });
    connect(window_, &MonitorWindow::visibilityChanged, worker_, &MonitorWorker::setPresentationVisible);
    connect(window_, &MonitorWindow::networkObservationRequested, worker_, &MonitorWorker::setNetworkObservation);
    connect(window_, &MonitorWindow::visibilityChanged, this, [this](bool visible) {
        if (!visible && window_) { settings_.setValue("windowGeometry", window_->saveGeometry()); windowReleaseTimer_->start(); }
        else windowReleaseTimer_->stop();
    });
    connect(worker_, &MonitorWorker::actionFinished, window_, &MonitorWindow::setDiagnosticFeedback);
    connect(window_, &MonitorWindow::diagnosticRunRequested, worker_, [this](bool start) {
        if (start) worker_->runDiagnostics(); else worker_->cancelDiagnostics();
    });
    connect(window_, &MonitorWindow::memoryReliefRequested, this, &AppController::showMemoryRelief);
    connect(window_, &MonitorWindow::incidentRequested, worker_, &MonitorWorker::requestIncident);
    connect(window_, &MonitorWindow::exportIncidentRequested, worker_, &MonitorWorker::exportIncident);
    connect(window_, &MonitorWindow::incidentRecordingRequested, this, [this](bool enabled) {
        incidentRecording_ = enabled;
        storePreference("incidentRecording", enabled);
        QMetaObject::invokeMethod(worker_, "setIncidentRecording", Qt::QueuedConnection, Q_ARG(bool, enabled));
    });
    connect(window_, &MonitorWindow::diagnosticExportRequested, this, [this] {
        saveReport(window_->diagnosticReport(), QStringLiteral("导出诊断报告（含应用名、PID 和资源指标，不含命令行与项目路径）"), "DevStats-diagnostics-");
    });
    connect(window_, &MonitorWindow::processActionRequested, this, [this](const QJsonObject &identity, const QString &action) {
        if (action != "terminate" && action != "kill") return;
        auto current = [this, identity] {
            if (latest_.value("processSampleAgeSeconds").toDouble(999) > 5 || std::abs(QDateTime::currentMSecsSinceEpoch() / 1000. - latest_.value("timestamp").toDouble()) > 6) return false;
            for (const auto &v : latest_.value("processes").toArray()) {
                const auto p = v.toObject();
                if (p.value("pid") == identity.value("pid") && p.value("startTicks") == identity.value("startTicks") && p.value("executablePath") == identity.value("executablePath")) return true;
            }
            return false;
        };
        const auto checked = ProcessActions().inspect(identity);
        if (!current()) { window_->setDiagnosticFeedback(QStringLiteral("进程已变化，请重新选择。")); return; }
        if (!checked.value("canTerminate").toBool()) { window_->setDiagnosticFeedback(checked.value("message").toString()); return; }
        const bool force = action == "kill";
        QMessageBox confirm(QMessageBox::Warning, force ? QStringLiteral("强制结束此进程") : QStringLiteral("结束此进程"), QString(), QMessageBox::Cancel, window_);
        confirm.setObjectName("confirmDiagnosticProcess");
        confirm.setTextFormat(Qt::PlainText);
        confirm.setText(QStringLiteral("%1（PID %2）\n%3\n\n%4").arg(identity.value("name").toString(), QString::number(identity.value("pid").toDouble(), 'f', 0), identity.value("executablePath").toString(),
            force ? QStringLiteral("进程会被立即终止，来不及保存任何内容。") : QStringLiteral("程序会收到结束请求，通常会正常退出。未保存的内容可能丢失。")));
        auto *proceed = confirm.addButton(force ? QStringLiteral("强制结束") : QStringLiteral("结束进程"), QMessageBox::AcceptRole);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() != proceed) { window_->setDiagnosticFeedback(QString()); return; }
        if (!current()) { window_->setDiagnosticFeedback(QStringLiteral("进程已变化，未执行。请重新选择。")); return; }
        const auto result = ProcessActions().execute(identity, action);
        window_->setDiagnosticFeedback(result.value("message").toString());
        QMetaObject::invokeMethod(worker_, "refreshDiagnostics", Qt::QueuedConnection);
    });
    connect(window_, &MonitorWindow::durationChanged, worker_, &MonitorWorker::setHistoryDuration);
    connect(window_, &MonitorWindow::addProjectRequested, this, &AppController::addProject);
    connect(window_, &MonitorWindow::removeProjectRequested, this, [this](const QString &path) {
        projects_.removeAll(path); storePreference("projects", projects_); updatePreferences();
        QMetaObject::invokeMethod(worker_, "setProjects", Qt::QueuedConnection, Q_ARG(QStringList, projects_));
    });
    connect(window_, &MonitorWindow::autoStartRequested, this, &AppController::setAutoStart);
    connect(window_, &MonitorWindow::largeTrayTextRequested, this, [this](bool enabled) {
        largeTrayText_ = enabled; storePreference("trayLargeText", enabled); updateTray();
    });
    connect(window_, &MonitorWindow::automationNotificationsRequested, this, [this](bool enabled) {
        automationNotifications_ = enabled; storePreference("automationNotifications", enabled);
    });
    connect(window_, &MonitorWindow::trayThemeRequested, this, [this](int mode) {
        trayTheme_ = qBound(0, mode, 2); storePreference("trayTheme", trayTheme_); updateTray();
    });
    connect(window_, &MonitorWindow::appearanceRequested, this, [this](int mode) {
        appearance_ = qBound(0, mode, 2); storePreference("appearance", appearance_);
        applyMenuTheme();
    });
    connect(window_, &MonitorWindow::quickLaunchRequested, this, &AppController::launchApplication);
    if (!latest_.isEmpty()) window_->setSnapshot(latest_);
    window_->setIncidents(incidents_);
    if (!history_.isEmpty()) window_->setHistories(history_, historyStart_, historyEnd_);
    QMetaObject::invokeMethod(worker_, "setHistoryDuration", Qt::QueuedConnection, Q_ARG(qint64, window_->historyDuration()));
    QMetaObject::invokeMethod(worker_, "requestIncidents", Qt::QueuedConnection);
}

void AppController::showMemoryRelief() {
    ensureWindow();
    if(!reliefDialog_) {
        reliefDialog_=new MemoryReliefDialog(window_);reliefDialog_->setAttribute(Qt::WA_DeleteOnClose);
        connect(reliefDialog_,&MemoryReliefDialog::refreshRequested,worker_,&MonitorWorker::refreshDiagnostics);
        connect(reliefDialog_,&MemoryReliefDialog::trimMemoryRequested,worker_,&MonitorWorker::reclaimMemoryForRelief);
        connect(reliefDialog_,&MemoryReliefDialog::closeApplicationsRequested,worker_,&MonitorWorker::inspectApplicationWindows);
        connect(reliefDialog_,&QDialog::finished,this,[this] {
            QMetaObject::invokeMethod(worker_,"cancelApplicationClose",Qt::QueuedConnection,Q_ARG(QString,QString()));
        });
    }
    reliefDialog_->setSnapshot(latest_);reliefDialog_->setBusy(reliefBusy_);
    if(!reliefOutcome_.isEmpty()) reliefDialog_->setOutcome(reliefOutcome_);
    reliefDialog_->show();reliefDialog_->raise();reliefDialog_->activateWindow();
    QMetaObject::invokeMethod(worker_,"refreshDiagnostics",Qt::QueuedConnection);
}

void AppController::saveReport(const QJsonObject &report, const QString &title, const QString &prefix) {
    if (!window_) return;
    const QString path = QFileDialog::getSaveFileName(window_, title,
        QDir::homePath() + "/" + prefix + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss") + ".json", QStringLiteral("JSON 报告 (*.json)"));
    if (path.isEmpty()) return;
    QSaveFile file(path); file.setDirectWriteFallback(false);
    const auto data = QJsonDocument(report).toJson(QJsonDocument::Indented);
    const bool opened = file.open(QIODevice::WriteOnly);
    const bool secure = opened && file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (!secure || file.write(data) != data.size() || !file.commit()) {
        file.cancelWriting(); window_->setDiagnosticFeedback(QStringLiteral("报告未保存：%1").arg(file.errorString()));
    } else window_->setDiagnosticFeedback(QStringLiteral("报告已保存至 %1").arg(path));
}

void AppController::showModule(const QString &module) {
    if (!module.isEmpty()) selected_ = module;
    ensureWindow();
    const bool reopening = !window_->isVisible() || window_->isMinimized();
    window_->setSelectedModule(selected_);
    if (!latest_.isEmpty()) {
        auto presentation = latest_;
        if (reopening && selected_ == "network") {
            presentation["networkApplicationsPrevious"] = ProcessNetworkSampler::retainedObservation(
                latest_.value("networkApplications").toObject(), latest_.value("networkApplicationsPrevious").toObject());
            presentation["networkApplications"] = QJsonObject{{"state", "starting"}, {"observing", true}, {"applications", QJsonArray{}}};
        }
        window_->setSnapshot(presentation);
    }
    if (!history_.isEmpty()) window_->setHistories(history_, historyStart_, historyEnd_);
    if (window_->isMinimized()) window_->showNormal();
    else window_->show();
    window_->raise();
    window_->activateWindow();
}

void AppController::updatePreferences() {
    if (window_) window_->setPreferences(autoStartEnabled(), trayTheme_, projects_, customApps_, largeTrayText_, automationNotifications_);
}

// A preference that cannot be written comes back as its default after a restart,
// which looks like the setting "not sticking". Say why, once per run.
void AppController::storePreference(const QString &key, const QVariant &value) {
    settings_.setValue(key, value);
    settings_.sync();
    if (settings_.status() == QSettings::NoError || settingsWarningShown_) return;
    settingsWarningShown_ = true;
    showError(QStringLiteral("设置无法保存到 %1，下次启动会恢复为默认值。请确认这个文件和所在目录属于当前用户并且可写。").arg(settings_.fileName()));
}

void AppController::showError(const QString &message) {
    ensureWindow();
    window_->show();
    QMessageBox::warning(window_, "DevStats", message);
}

void AppController::updateTray() {
    QColor ink = QApplication::palette().color(QPalette::WindowText);
    if (trayTheme_ == 1) ink = QColor(236, 239, 242);
    if (trayTheme_ == 2) ink = QColor(36, 40, 44);
    QStringList enabled;
    // A list saved by an earlier version may name a metric that no longer exists.
    for (const auto &metric : settings_.value("trayMetrics", QStringList{"cpu", "memory", "network", "battery"}).toStringList())
        if (metricKeys.contains(metric)) enabled.append(metric);
    if (enabled.isEmpty()) enabled.append("cpu");
    for (int index = 0; index < trays_.size(); ++index) {
        auto *tray = trays_[index]; const QString metric = metricKeys[index];
        if (!enabled.contains(metric)) { if (tray->isVisible()) tray->hide(); continue; }
        const QString key = DevStatsTray::cacheKey(metric, latest_, ink, largeTrayText_);
        if (!trayIconKeys_.contains(metric) || trayIconKeys_.value(metric) != key) {
            QIcon icon;
            for (int scale : {1, 2, 3}) icon.addPixmap(QPixmap::fromImage(DevStatsTray::render(metric, latest_, ink, scale, largeTrayText_)));
            tray->setIcon(icon); trayIconKeys_[metric] = key;
        }
        const QString tooltip = DevStatsTray::toolTip(metric, latest_);
        if (trayToolTips_.value(metric) != tooltip) { tray->setToolTip(tooltip); trayToolTips_[metric] = tooltip; }
        if (!tray->isVisible()) tray->show();
    }
}

QString AppController::autostartPath() {
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart/devstats.desktop";
}

bool AppController::autoStartEnabled() {
    QFile file(autostartPath());
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray content = file.readAll();
    return content.contains("Name=DevStats") && content.contains("Exec=")
        && !content.contains("Hidden=true") && !content.contains("X-GNOME-Autostart-enabled=false");
}

void AppController::setAutoStart(bool enabled) {
    const QString path = autostartPath();
    if (enabled) {
        if (!QDir().mkpath(QFileInfo(path).path())) { showError(QString::fromUtf8("无法创建登录启动目录。")); return; }
        QSaveFile file(path);
        if (!file.open(QIODevice::WriteOnly)) { showError(file.errorString()); return; }
        const QString executable = devStatsLauncherPath();
        if (executable.contains('\n') || executable.contains('\r')) { showError(QString::fromUtf8("应用路径含无效换行。")); return; }
        QString quoted;
        for (QChar character : executable) {
            if (character == '%') { quoted += "%%"; continue; }
            if (character == '\\' || character == '"' || character == '`' || character == '$') quoted += '\\';
            quoted += character;
        }
        // Desktop Entry decoding precedes Exec token decoding.
        quoted.replace("\\", "\\\\");
        const QByteArray content = "[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- \"" + quoted.toUtf8()
            + "\" --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
        file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
        if (file.write(content) != content.size() || !file.commit()) { showError(file.errorString()); return; }
    } else if (QFile::exists(path)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || !file.readAll().contains("Name=DevStats")) {
            showError(QString::fromUtf8("登录启动文件不属于 DevStats，未修改。")); return;
        }
        file.close();
        if (!QFile::remove(path)) { showError(QString::fromUtf8("无法关闭登录启动。")); return; }
    }
    updatePreferences();
}

void AppController::addProject() {
    ensureWindow();
    const QString path = QFileDialog::getExistingDirectory(window_, QString::fromUtf8("选择需要归组的项目文件夹"));
    if (path.isEmpty()) return;
    const QString canonical = QFileInfo(path).canonicalFilePath();
    if (!canonical.isEmpty() && !projects_.contains(canonical)) projects_.append(canonical);
    storePreference("projects", projects_); updatePreferences();
    QMetaObject::invokeMethod(worker_, "setProjects", Qt::QueuedConnection, Q_ARG(QStringList, projects_));
}

void AppController::launchApplication(const QString &id) {
    if (id == "files") { QDesktopServices::openUrl(QUrl::fromLocalFile(QDir::homePath())); return; }
    if (id == "add") {
        ensureWindow();
        if (customApps_.size() >= 8) { showError(QString::fromUtf8("最多添加 8 个快捷应用。")); return; }
        const QString path = QFileDialog::getOpenFileName(window_, QString::fromUtf8("添加快捷应用"),
            "/usr/share/applications", QString::fromUtf8("桌面应用 (*.desktop)"));
        if (!path.isEmpty() && !customApps_.contains(path)) customApps_.append(path);
        storePreference("customApps", customApps_); updatePreferences(); return;
    }
    if (id.startsWith("remove:")) {
        customApps_.removeAll(id.mid(7)); storePreference("customApps", customApps_); updatePreferences(); return;
    }
    if (id.startsWith("custom:")) {
        const QString path = id.mid(7), gio = QStandardPaths::findExecutable("gio");
        if (!customApps_.contains(path) || !QFileInfo(path).isFile() || gio.isEmpty()
            || !QProcess::startDetached(gio, {"launch", path}))
            showError(QString::fromUtf8("无法启动快捷应用，请检查 .desktop 文件和系统 gio 工具。"));
        return;
    }
    const QMap<QString, QStringList> commands{
        {"terminal", {"x-terminal-emulator", "mate-terminal", "ukui-terminal", "xterm"}},
        {"monitor", {"ukui-system-monitor", "mate-system-monitor", "gnome-system-monitor"}},
        {"settings", {"ukui-control-center", "mate-control-center", "gnome-control-center"}}};
    for (const auto &command : commands.value(id)) {
        const QString executable = QStandardPaths::findExecutable(command);
        if (!executable.isEmpty() && QProcess::startDetached(executable, {})) return;
    }
    showError(QString::fromUtf8("未找到可用的系统应用，请通过自定义快捷应用添加。"));
}
