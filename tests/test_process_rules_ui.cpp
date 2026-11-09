#include "../src/charts.h"
#include "../src/process_rules_panel.h"
#include "../src/ui_components.h"
#include "../src/ui_theme.h"

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
int checks = 0;
void check(bool ok, const char *what) { ++checks; if (!ok) { std::fprintf(stderr, "FAIL rules UI: %s\n", what); std::exit(1); } }
void settle() { QApplication::sendPostedEvents(); QApplication::processEvents(); }
// The next modal window opens inside exec(), so act on it from the event loop.
void whenModal(const char *name, const std::function<void(QWidget *)> &act) {
    auto *timer = new QTimer(qApp);
    timer->setInterval(5);
    const QByteArray wanted(name);
    QObject::connect(timer, &QTimer::timeout, qApp, [timer, wanted, act] {
        auto *modal = QApplication::activeModalWidget();
        if (!modal || modal->objectName().toUtf8() != wanted) return;
        timer->stop(); timer->deleteLater();
        act(modal);
    });
    timer->start();
}
void accept(QWidget *box) {
    auto *message = qobject_cast<QMessageBox *>(box);
    check(message && message->defaultButton() == message->button(QMessageBox::Cancel), "confirmation defaults to Cancel");
    check(message->textFormat() == Qt::PlainText, "confirmation is plain text");
    for (auto *button : message->buttons()) if (message->buttonRole(button) == QMessageBox::AcceptRole) { button->click(); return; }
    check(false, "confirmation offers an explicit accept");
}
QJsonObject snapshotWith(const QJsonObject &automation = {}, double cpu = 130) {
    return {{"cpu", QJsonObject{{"coreCount", 8}}}, {"processAutomation", automation},
        {"processes", QJsonArray{QJsonObject{{"pid", 77}, {"startTicks", 5}, {"name", "worker"}, {"executablePath", "/opt/fixture/worker"}, {"cpuPercent", cpu}, {"memoryBytes", 1048576}}}}};
}
QJsonObject cpuRule(const QString &id, const QString &value, double limit, int sustain) {
    return {{"id", id}, {"enabled", true}, {"match", "path"}, {"value", value}, {"cpuAbovePercent", limit}, {"sustainSeconds", sustain}, {"forceKill", true}, {"graceSeconds", 10}};
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setStyle("Fusion");
    qRegisterMetaType<QJsonArray>();
    ProcessRulesPanel panel;
    panel.setStyleSheet(DevStatsUI::styleSheet(true));
    panel.setPalette(DevStatsUI::palette(true));
    panel.resize(860, 700);
    panel.show();
    panel.setSnapshot(snapshotWith());
    settle();
    QSignalSpy configured(&panel, &ProcessRulesPanel::configurationRequested);
    QSignalSpy paused(&panel, &ProcessRulesPanel::pauseRequested);
    auto *master = panel.findChild<ToggleSwitch *>("automationEnabled");
    auto *status = panel.findChild<QLabel *>("automationStatus");
    auto *table = panel.findChild<QTreeWidget *>("automationRules");
    auto *feedback = panel.findChild<QLabel *>("automationFeedback");
    auto *add = panel.findChild<QPushButton *>("automationAddRule");

    // First run: nothing to do yet, and the switch cannot be turned on empty.
    check(!master->isChecked() && status->text().contains(QStringLiteral("还没有规则")) && table->isHidden() && !panel.findChild<QLabel *>("automationEmpty")->isHidden(), "empty state explains itself");
    master->click(); settle();
    check(!master->isChecked() && configured.isEmpty() && feedback->text().contains(QStringLiteral("至少一条")), "master refuses to start with no rules");

    // Editor: the CPU condition is the default, with a live explanation.
    whenModal("processRuleEditor", [&](QWidget *editor) {
        auto *target = editor->findChild<QLineEdit *>("automationTarget");
        auto *cpu = editor->findChild<QSpinBox *>("automationCpuLimit");
        auto *sustain = editor->findChild<QSpinBox *>("automationSustain");
        auto *busy = editor->findChild<QRadioButton *>("automationWhenBusy");
        auto *force = editor->findChild<ToggleSwitch *>("automationForceKill");
        auto *grace = editor->findChild<QSpinBox *>("automationGrace");
        auto *save = editor->findChild<QPushButton *>("automationSaveRule");
        check(busy->isChecked() && cpu->value() == 80 && sustain->value() == 30 && force->isChecked() && grace->value() == 10, "safe defaults: a CPU condition with a duration");
        auto *hint = editor->findChild<QLabel *>("automationCpuHint");
        check(hint->text().contains(QStringLiteral("本机 8 个核心")) && hint->text().contains("10"), "the limit is explained in whole-machine terms");
        cpu->setValue(160);
        check(hint->text().contains("20"), "explanation follows the value");
        target->setText("/opt/fixture/worker");
        auto *sentence = editor->findChild<QLabel *>("automationSentence");
        check(sentence->text().contains("worker") && sentence->text().contains("160%") && sentence->text().contains(QStringLiteral("30 秒")) && sentence->text().contains(QStringLiteral("强制结束")), "the rule reads as one sentence");
        check(editor->findChild<QLabel *>("automationRunning")->text().contains(QStringLiteral("1 个")) && editor->findChild<QLabel *>("automationRunning")->text().contains("130%"), "shows what matches right now");
        force->click();
        check(sentence->text().contains(QStringLiteral("不强制")) && !grace->isEnabled(), "force option changes the sentence and disables the wait");
        force->click();
        // Invalid input stays in the editor with the reason.
        auto *mode = editor->findChild<QComboBox *>("automationMatchMode");
        mode->setCurrentIndex(1);
        target->setText("bad name*");
        save->click(); settle();
        auto *error = editor->findChild<QLabel *>("automationEditorError");
        check(editor->isVisible() && !error->isHidden() && !error->text().isEmpty(), "invalid rule shows why and stays open");
        target->setText("/not-absolute/../x");
        mode->setCurrentIndex(0);
        target->setText("/opt/fixture/worker");
        check(error->isHidden(), "the error clears on edit");
        save->click();
    });
    add->click(); settle();
    check(configured.count() == 1, "saving applies at once");
    check(!configured.first()[0].toBool(), "adding a rule does not switch automation on");
    auto rule = configured.first()[1].toJsonArray().first().toObject();
    check(rule.value("value") == "/opt/fixture/worker" && rule.value("match") == "path" && rule.value("cpuAbovePercent").toDouble() == 160 &&
          rule.value("sustainSeconds").toInt() == 30 && rule.value("forceKill").toBool() && rule.value("graceSeconds").toInt() == 10 && rule.value("enabled").toBool(), "rule carries its limit, duration and escalation");
    const QString id = rule.value("id").toString();
    check(!id.isEmpty() && !table->isHidden() && table->topLevelItemCount() == 1, "rule is listed");
    check(table->topLevelItem(0)->text(1) == "worker" && table->topLevelItem(0)->toolTip(1) == "/opt/fixture/worker" && table->topLevelItem(0)->text(2).contains("160%") && table->topLevelItem(0)->text(2).contains(QStringLiteral("30 秒")), "row shows program and condition");
    check(table->topLevelItem(0)->text(3) == QStringLiteral("已暂停"), "paused while the master is off");

    // Worker confirms; the page takes the normalized form.
    panel.configurationFinished(QJsonObject{{"ok", true}, {"enabled", false}, {"rules", configured.first()[1].toJsonArray()}, {"message", QStringLiteral("已保存。")}});
    check(feedback->text() == QStringLiteral("已保存。"), "worker result shows");
    // Automation switched off elsewhere reaches an idle page.
    panel.setSnapshot(snapshotWith(QJsonObject{{"enabled", false}, {"rules", configured.first()[1].toJsonArray()}, {"status", QJsonObject{}}, {"log", QJsonArray{}}}));
    settle();

    // Turning on needs one explicit confirmation listing the rules. Cancel keeps it off.
    whenModal("confirmProcessAutomation", [](QWidget *box) { auto *m = qobject_cast<QMessageBox *>(box); check(m->text().contains("worker") && m->text().contains("160%"), "confirmation lists the rule"); m->button(QMessageBox::Cancel)->click(); });
    master->click(); settle();
    check(!master->isChecked() && configured.count() == 1, "cancel keeps automation off");
    whenModal("confirmProcessAutomation", accept);
    master->click(); settle();
    check(master->isChecked() && configured.count() == 2 && configured.last()[0].toBool(), "accept turns automation on");
    check(status->text().contains(QStringLiteral("1 条规则")), "status counts active rules");

    // A sample taken before the worker saw the change must not flip the switch back.
    panel.setSnapshot(snapshotWith(QJsonObject{{"enabled", false}, {"rules", QJsonArray{}}, {"status", QJsonObject{}}, {"log", QJsonArray{}}}));
    settle();
    check(master->isChecked(), "stale sample does not flip the switch while a change is in flight");
    panel.configurationFinished(QJsonObject{{"ok", true}, {"enabled", true}, {"rules", configured.last()[1].toJsonArray()}, {"message", QStringLiteral("已保存，规则已生效。")}});
    check(master->isChecked() && table->topLevelItemCount() == 1, "the worker's answer settles it");

    // Live status.
    const auto rules = configured.last()[1].toJsonArray();
    auto live = [&](const QJsonObject &state) {
        panel.setSnapshot(snapshotWith(QJsonObject{{"enabled", true}, {"rules", rules}, {"status", QJsonObject{{id, state}}}, {"log", QJsonArray{}}}));
        settle();
        return table->topLevelItem(0)->text(3);
    };
    check(live({{"matched", 0}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}) == QStringLiteral("未运行"), "not running");
    check(live({{"matched", 2}, {"armed", 0}, {"topCpuPercent", 40}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}}).contains(QStringLiteral("未超限")), "running below the limit");
    check(table->topLevelItem(0)->data(3, Qt::UserRole + 40).toString() == "ok", "healthy tone");
    const QString armed = live({{"matched", 1}, {"armed", 1}, {"armedSeconds", 18}, {"sustainSeconds", 30}, {"averageCpuPercent", 172}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 0}});
    check(armed.contains("18 / 30") && armed.contains("172%"), "countdown is visible");
    check(table->topLevelItem(0)->data(3, Qt::UserRole + 40).toString() == "warn", "countdown tone");
    check(live({{"matched", 1}, {"armed", 0}, {"protected", 0}, {"waitingExit", 1}, {"otherUser", 0}}).contains(QStringLiteral("等待")), "waiting for exit");
    check(live({{"matched", 1}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"unresponsive", 1}, {"otherUser", 0}}).contains(QStringLiteral("没有响应")), "a process that ignored the request is explained");
    check(live({{"matched", 1}, {"armed", 0}, {"protected", 1}, {"protectedReason", "x"}, {"waitingExit", 0}, {"otherUser", 0}}).contains(QStringLiteral("受保护")), "protected process is explained");
    check(table->topLevelItem(0)->toolTip(3) == "x", "protection reason in tooltip");
    check(live({{"matched", 0}, {"armed", 0}, {"protected", 0}, {"waitingExit", 0}, {"otherUser", 2}}).contains(QStringLiteral("其他用户")), "other users' processes are explained");

    // Execution log.
    auto *log = panel.findChild<QTreeWidget *>("automationLog");
    check(log->isHidden() && !panel.findChild<QLabel *>("automationLogEmpty")->isHidden(), "log empty state");
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.;
    auto row = [&](const char *phase, const QString &message, bool ok) {
        return QJsonObject{{"id", phase}, {"timestamp", now}, {"name", "worker"}, {"pid", 77}, {"executablePath", "/opt/fixture/worker"},
                           {"condition", QStringLiteral("CPU 超过 160%，持续 30 秒")}, {"triggerCpuPercent", 175.}, {"phase", phase}, {"message", message}, {"ok", ok}, {"code", phase}};
    };
    panel.setSnapshot(snapshotWith(QJsonObject{{"enabled", true}, {"rules", rules}, {"status", QJsonObject{}}, {"log", QJsonArray{row("exited", "a", true), row("killed", "b", true), row("survived", "c", false), row("refused", "d", false), row("requested", "e", true)}}}));
    settle();
    check(!log->isHidden() && log->topLevelItemCount() == 5, "log shows");
    check(log->topLevelItem(0)->text(1) == "worker" && log->topLevelItem(0)->text(2) == "77" && log->topLevelItem(0)->text(3).contains("175%"), "log says what triggered it");
    check(log->topLevelItem(0)->text(4) == QStringLiteral("已结束") && log->topLevelItem(0)->data(4, Qt::UserRole + 40).toString() == "ok", "exited");
    check(log->topLevelItem(1)->text(4) == QStringLiteral("已强制结束"), "killed");
    check(log->topLevelItem(2)->text(4) == QStringLiteral("未退出") && log->topLevelItem(2)->data(4, Qt::UserRole + 40).toString() == "warn", "survived");
    check(log->topLevelItem(3)->text(4) == QStringLiteral("已拒绝") && log->topLevelItem(3)->data(4, Qt::UserRole + 40).toString() == "critical", "refused");
    check(log->topLevelItem(4)->text(4) == QStringLiteral("等待退出"), "pending");
    // Entries from 0.7.x have no phase and no condition.
    panel.setSnapshot(snapshotWith(QJsonObject{{"enabled", true}, {"rules", rules}, {"status", QJsonObject{}}, {"log", QJsonArray{QJsonObject{{"timestamp", now}, {"ok", false}, {"code", "protectedProcess"}, {"message", "old"}, {"ruleValue", "x"}, {"executablePath", "/usr/bin/x"}, {"pid", 1}}}}}));
    settle();
    check(log->topLevelItem(0)->text(1) == "x" && log->topLevelItem(0)->text(4) == QStringLiteral("失败") && log->topLevelItem(0)->text(5) == "old", "older log rows still read");

    // Edit keeps the rule's identity and on/off state; the toggle applies at once.
    const int before = configured.count();
    whenModal("processRuleEditor", [&](QWidget *editor) {
        check(editor->findChild<QLineEdit *>("automationTarget")->text() == "/opt/fixture/worker" && editor->findChild<QSpinBox *>("automationCpuLimit")->value() == 160, "editor loads the rule");
        editor->findChild<QSpinBox *>("automationSustain")->setValue(60);
        editor->findChild<QPushButton *>("automationSaveRule")->click();
    });
    table->itemWidget(table->topLevelItem(0), 4)->findChild<QToolButton *>("automationEditRule")->click(); settle();
    check(configured.count() == before + 1, "edit applies");
    rule = configured.last()[1].toJsonArray().first().toObject();
    check(rule.value("id") == id && rule.value("sustainSeconds").toInt() == 60 && configured.last()[1].toJsonArray().size() == 1, "edit replaces rather than adds");
    table->itemWidget(table->topLevelItem(0), 0)->findChild<ToggleSwitch *>("automationRuleToggle")->click(); settle();
    check(configured.count() == before + 2 && !configured.last()[1].toJsonArray().first().toObject().value("enabled").toBool(), "per-rule toggle applies");
    check(table->topLevelItem(0)->text(3) == QStringLiteral("已暂停"), "a disabled rule reads as paused");

    // Unconditional rule that would act right now asks first, naming the victims.
    whenModal("processRuleEditor", [&](QWidget *editor) {
        auto *always = editor->findChild<QRadioButton *>("automationAlways");
        always->click();
        check(!editor->findChild<QSpinBox *>("automationCpuLimit")->isEnabled(), "limit is off for an unconditional rule");
        check(editor->findChild<QLabel *>("automationSentence")->text().contains(QStringLiteral("只要")), "sentence follows the condition");
        editor->findChild<QPushButton *>("automationSaveRule")->click();
    });
    bool immediate = false;
    whenModal("confirmRuleImmediate", [&](QWidget *box) {
        immediate = true;
        check(qobject_cast<QMessageBox *>(box)->text().contains("PID 77"), "names the running process");
        qobject_cast<QMessageBox *>(box)->button(QMessageBox::Cancel)->click();
        // Back in the editor with nothing saved; leave it.
        QTimer::singleShot(0, qApp, [] { if (auto *modal = QApplication::activeModalWidget()) if (modal->objectName() == "processRuleEditor") qobject_cast<QDialog *>(modal)->reject(); });
    });
    const int beforeImmediate = configured.count();
    table->itemWidget(table->topLevelItem(0), 4)->findChild<QToolButton *>("automationEditRule")->click(); settle();
    check(immediate && configured.count() == beforeImmediate, "cancelling the warning saves nothing");

    // A rule from a process picks up its path and a limit it is really exceeding.
    whenModal("processRuleEditor", [&](QWidget *editor) {
        check(editor->findChild<QLineEdit *>("automationTarget")->text() == "/opt/fixture/other", "path filled in");
        check(editor->findChild<QSpinBox *>("automationCpuLimit")->value() == 210, "limit suggested below the current use");
        qobject_cast<QDialog *>(editor)->reject();
    });
    panel.addRuleFor(QJsonObject{{"executablePath", "/opt/fixture/other"}, {"cpuPercent", 305.}, {"pid", 5}});
    settle();
    check(configured.count() == beforeImmediate, "cancel adds nothing");

    // Delete applies at once; the master can be paused immediately.
    table->itemWidget(table->topLevelItem(0), 4)->findChild<QToolButton *>("automationRemoveRule")->click(); settle();
    check(configured.last()[1].toJsonArray().isEmpty() && table->isHidden(), "delete applies");
    panel.setConfiguration(true, rules); settle();
    check(master->isChecked() && table->topLevelItemCount() == 1, "configuration reloads");
    master->click(); settle();
    check(!master->isChecked() && paused.count() == 1, "switching off pauses immediately, without validation");
    panel.configurationFinished(QJsonObject{{"ok", true}, {"pauseOnly", true}, {"message", QStringLiteral("已暂停")}});
    check(status->text().contains(QStringLiteral("已暂停")), "paused state");
    panel.configurationFinished(QJsonObject{{"ok", false}, {"message", QStringLiteral("无效")}});
    check(feedback->text() == QStringLiteral("无效") && table->topLevelItemCount() == 1, "a refused configuration keeps the page");

    std::printf("PASS rules UI: %d checks; empty state, editor defaults and explanation, CPU limit and duration, escalation, validation, confirmation, live status, log phases, editing, immediate-action warning, pause\n", checks);
    return 0;
}
