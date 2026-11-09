#include "../src/controller.h"
#include "../src/monitor_window.h"
#include "../src/process_rules_panel.h"
#include "../src/ui_components.h"
#include "../src/process_rules.h"
#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QSpinBox>
#include <QRadioButton>
#include <QComboBox>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
int checks = 0;
void check(bool value, const char *message) { ++checks; if (!value) { std::fprintf(stderr, "FAIL process rules controller: %s\n", message); std::exit(1); } }
void drain() { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); }
bool waitFor(const std::function<bool()> &test) {
    QElapsedTimer clock; clock.start();
    while (!test() && clock.elapsed() < 8000) { QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec(); }
    return test();
}
QAction *trayEntry() {
    for (auto *widget : QApplication::topLevelWidgets()) if (auto *menu = qobject_cast<QMenu *>(widget))
        for (auto *action : menu->actions()) if (action->objectName() == "processAutomationAction") return action;
    return nullptr;
}
void whenModal(const char *name, const std::function<void(QWidget *)> &act) {
    auto *timer = new QTimer(qApp);
    timer->setInterval(10);
    const QByteArray wanted(name);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, wanted, act] {
        auto *modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName().toUtf8() != wanted) return;
        timer->stop(); timer->deleteLater();
        act(modal);
    });
    timer->start();
}
void confirmNext() {
    QTimer::singleShot(0, [] {
        auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        check(box && box->objectName() == "confirmProcessAutomation", "controller enabling uses review confirmation");
        for (auto *button : box->buttons()) if (box->buttonRole(button) == QMessageBox::AcceptRole) { button->click(); return; }
        check(false, "confirmation has explicit accept");
    });
}
}

class MonitorWorkerTest {
public:
    static void lifecycle(const QString &root) {
        MonitorWorker worker(root, {}, false); worker.start();
        const auto timers = worker.findChildren<QTimer *>().size();
        check(worker.processRules_ && !worker.processRules_->state().value("enabled").toBool(), "worker default automation off");
        const QJsonArray rules{QJsonObject{{"id", "lifecycle"}, {"enabled", true}, {"match", "path"}, {"value", "/opt/devstats-owned-qa-not-installed/helper"}}};
        worker.setProcessRules(true, rules);
        check(worker.processRules_->state().value("enabled").toBool() && worker.timer_->interval() == 2000, "rules use existing two-second scheduler");
        check(worker.findChildren<QTimer *>().size() == timers, "enabling creates no periodic timers or polling helpers");
        worker.setSleeping(true); check(!worker.timer_->isActive(), "sleep pauses existing scheduler");
        worker.setSleeping(false); check(worker.timer_->isActive() && worker.processRules_->state().value("enabled").toBool(), "wake preserves rules and resumes fresh sampler");
        check(worker.latest_.value("processAutomation").toObject().value("log").toArray().isEmpty(), "nonmatching lifecycle test never signals any process");
        worker.stop(); check(!worker.processRules_ && !worker.timer_, "stop destroys rule engine and scheduler");
    }
};

int main(int argc, char **argv) {
    QTemporaryDir root; check(root.isValid(), "isolated root");
    qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8()); qputenv("XDG_DATA_HOME", (root.path() + "/share").toUtf8());
    qputenv("DEVSTATS_DATA_DIR", (root.path() + "/data").toUtf8());
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    const QString settingsPath = root.path() + "/config/devstats/settings.ini";
    { QSettings settings(settingsPath, QSettings::IniFormat); settings.setValue("unrelatedPreference", "retain-this"); settings.setValue("incidentRecording", true); settings.sync(); }
    QDir().mkpath(root.path() + "/data");
    const QByteArray retainedLog = QJsonDocument(QJsonArray{QJsonObject{{"timestamp", 1770000000.}, {"ok", false}, {"code", "protectedProcess"}, {"message", "retained-result"}, {"ruleValue", "previous-rule"}, {"executablePath", "/opt/previous-helper"}}}).toJson(QJsonDocument::Compact);
    QFile log(root.path() + "/data/process-rules-log.json"); check(log.open(QIODevice::WriteOnly), "seed private retained log"); log.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner); check(log.write(retainedLog) == retainedLog.size(), "write retained log"); log.close();
    {
        AppController controller; controller.start(false);
        check(waitFor([&] { return controller.latestSnapshot().contains("processAutomation"); }), "worker publishes automation state");
        check(!controller.latestSnapshot().value("processAutomation").toObject().value("enabled").toBool(), "existing user upgrade defaults automation off");
        auto *entry = trayEntry(); check(entry, "tray entry present"); entry->trigger(); drain();
        check(controller.window() && controller.window()->isVisible() && controller.window()->selectedModule() == "automation", "tray opens the automation page");
        auto *panel = controller.window()->rulesPanel();
        check(waitFor([&] { auto *log = panel->findChild<QTreeWidget *>("automationLog"); return log->topLevelItemCount() == 1 && log->topLevelItem(0)->text(5) == "retained-result"; }), "restart log reaches the page");
        whenModal("processRuleEditor", [](QWidget *editor) {
            editor->findChild<QLineEdit *>("automationTarget")->setText("/opt/devstats-owned-qa-not-installed/helper");
            editor->findChild<QSpinBox *>("automationCpuLimit")->setValue(120);
            editor->findChild<QPushButton *>("automationSaveRule")->click();
        });
        panel->findChild<QPushButton *>("automationAddRule")->click(); drain();
        check(waitFor([&] { return controller.latestSnapshot().value("processAutomation").toObject().value("rules").toArray().size() == 1; }), "new rule reaches the worker while paused");
        const auto stored = controller.latestSnapshot().value("processAutomation").toObject().value("rules").toArray().first().toObject();
        check(stored.value("cpuAbovePercent").toDouble() == 120 && stored.value("sustainSeconds").toInt() == 30 && stored.value("forceKill").toBool(), "the worker holds the CPU limit");
        whenModal("confirmProcessAutomation", [](QWidget *box) {
            auto *message = qobject_cast<QMessageBox *>(box);
            for (auto *button : message->buttons()) if (message->buttonRole(button) == QMessageBox::AcceptRole) { button->click(); return; }
        });
        panel->findChild<ToggleSwitch *>("automationEnabled")->click(); drain();
        check(waitFor([&] { return controller.latestSnapshot().value("processAutomation").toObject().value("enabled").toBool(); }), "confirmed switch reaches the worker");
        check(waitFor([&] { QSettings settings(settingsPath, QSettings::IniFormat); return settings.value("automationEnabled").toBool(); }), "accepted configuration persists to the user's settings");
        { QSettings settings(settingsPath, QSettings::IniFormat); const auto saved = QJsonDocument::fromJson(settings.value("automationRules").toByteArray()).array(); check(saved.first().toObject().value("cpuAbovePercent").toDouble() == 120, "the limit persists"); }
        check(waitFor([&] { return !panel->findChild<QLabel *>("automationStatus")->text().isEmpty(); }), "live status");
        check(log.open(QIODevice::ReadOnly) && log.readAll() == retainedLog, "nonmatching rule leaves the retained log byte-identical"); log.close();
        // The page lives in the main window, so an open editor protects it from release.
        QPointer<MonitorWindow> window = controller.window(); window->hide(); drain();
        auto *ttl = controller.findChild<QTimer *>("windowReleaseTimer"); ttl->stop();
        bool held = false;
        whenModal("processRuleEditor", [&](QWidget *editor) {
            QMetaObject::invokeMethod(ttl, "timeout", Qt::DirectConnection);
            held = window && editor->isVisible();
            qobject_cast<QDialog *>(editor)->reject();
        });
        panel->findChild<QPushButton *>("automationAddRule")->click(); drain();
        check(held, "an open rule editor keeps the hidden window alive");
        ttl->stop(); QMetaObject::invokeMethod(ttl, "timeout", Qt::DirectConnection); drain();
        check(!window, "a hidden main window with no open editor is released");
        check(trayEntry(), "tray remains available after releasing the window"); trayEntry()->trigger(); drain();
        check(controller.window()->rulesPanel()->findChild<ToggleSwitch *>("automationEnabled")->isChecked(), "recreated page shows the running configuration");
        controller.stop();
    }
    drain();
    {
        AppController controller; controller.start(false);
        check(waitFor([&] { return controller.latestSnapshot().value("processAutomation").toObject().value("enabled").toBool(); }), "restart restores the enabled rules");
        controller.showModule("automation"); drain();
        auto *panel = controller.window()->rulesPanel();
        check(panel->findChild<ToggleSwitch *>("automationEnabled")->isChecked() && panel->findChild<QTreeWidget *>("automationRules")->topLevelItemCount() == 1, "restart shows the saved rule");
        panel->findChild<ToggleSwitch *>("automationEnabled")->click(); drain();
        check(waitFor([&] { return !controller.latestSnapshot().value("processAutomation").toObject().value("enabled").toBool(); }), "pause propagates without confirmation");
        check(waitFor([&] { QSettings settings(settingsPath, QSettings::IniFormat); return !settings.value("automationEnabled").toBool(); }), "pause survives future restart");
        controller.stop();
    }
    drain();
    { QSettings settings(settingsPath, QSettings::IniFormat); check(settings.value("unrelatedPreference") == "retain-this" && settings.value("incidentRecording").toBool(), "existing unrelated settings and stall recording retained"); }
    MonitorWorkerTest::lifecycle(root.path() + "/lifecycle");
    check(log.open(QIODevice::ReadOnly) && log.readAll() == retainedLog, "tests never modify pre-existing action history"); log.close();
    std::printf("PASS process rules controller: %d checks; real settings/restart, tray entry, CPU limit persistence, window release, pause/sleep/stop and unchanged polling\n", checks);
    return 0;
}
