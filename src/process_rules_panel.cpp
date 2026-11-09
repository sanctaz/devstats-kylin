#include "process_rules_panel.h"
#include "charts.h"
#include "process_rules.h"
#include "resource_breakdown.h"
#include "ui_components.h"
#include "ui_theme.h"

#include <QButtonGroup>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QToolButton>
#include <QTreeWidget>
#include <QUuid>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using DevStatsUI::card;
using DevStatsUI::label;

namespace {
constexpr int IdRole = Qt::UserRole;
const QString None = QString::fromUtf8("—");

QString displayName(const QJsonObject &rule) {
    const QString value = rule.value("value").toString();
    return rule.value("match").toString() == "path" ? QFileInfo(value).fileName() : value;
}
QString conditionText(const QJsonObject &rule) {
    if (!rule.value("cpuAbovePercent").isDouble()) return QStringLiteral("运行即结束");
    const QString limit = QStringLiteral("CPU > %1%").arg(rule.value("cpuAbovePercent").toDouble(), 0, 'f', 0);
    const int sustain = rule.value("sustainSeconds").toInt();
    return sustain ? limit + QStringLiteral(" · %1 秒").arg(sustain) : limit;
}
QString actionText(const QJsonObject &rule) {
    return rule.value("forceKill").toBool()
        ? QStringLiteral("请求结束，%1 秒后强制").arg(rule.value("graceSeconds").toInt(10))
        : QStringLiteral("请求结束");
}
QPair<QString, QString> statusText(const QJsonObject &rule, const QJsonObject &status, bool masterOn) {
    if (!masterOn || !rule.value("enabled").toBool()) return {QStringLiteral("已暂停"), QStringLiteral("neutral")};
    if (status.isEmpty()) return {QStringLiteral("等待采样"), QStringLiteral("neutral")};
    if (status.value("waitingExit").toInt()) return {QStringLiteral("等待进程退出"), QStringLiteral("warn")};
    const int matched = status.value("matched").toInt();
    if (status.value("unresponsive").toInt()) return {QStringLiteral("进程没有响应结束请求"), QStringLiteral("warn")};
    if (status.value("protected").toInt() && status.value("protected").toInt() == matched) return {QStringLiteral("受保护，无法结束"), QStringLiteral("warn")};
    if (status.value("armed").toInt()) {
        return {QStringLiteral("已超限 %1 / %2 秒 · 平均 %3%")
                    .arg(int(status.value("armedSeconds").toDouble())).arg(status.value("sustainSeconds").toInt())
                    .arg(status.value("averageCpuPercent").toDouble(), 0, 'f', 0), QStringLiteral("warn")};
    }
    if (!matched) {
        if (status.value("otherUser").toInt()) return {QStringLiteral("匹配到其他用户的进程，无法结束"), QStringLiteral("warn")};
        return {QStringLiteral("未运行"), QStringLiteral("neutral")};
    }
    const auto top = status.value("topCpuPercent");
    if (rule.value("cpuAbovePercent").isDouble()) {
        return {QStringLiteral("运行中 %1 个 · 最高 %2 · 未超限").arg(matched).arg(top.isDouble() ? QString::number(top.toDouble(), 'f', 0) + '%' : None), QStringLiteral("ok")};
    }
    return {QStringLiteral("运行中 %1 个").arg(matched), QStringLiteral("ok")};
}
QPair<QString, QString> phaseCaption(const QJsonObject &entry) {
    const QString phase = entry.value("phase").toString();
    if (phase == "exited") return {QStringLiteral("已结束"), QStringLiteral("ok")};
    if (phase == "killed") return {QStringLiteral("已强制结束"), QStringLiteral("ok")};
    if (phase == "requested" || phase == "forcing") return {QStringLiteral("等待退出"), QStringLiteral("warn")};
    if (phase == "survived") return {QStringLiteral("未退出"), QStringLiteral("warn")};
    if (phase == "refused") return {QStringLiteral("已拒绝"), QStringLiteral("critical")};
    return entry.value("ok").toBool() ? qMakePair(QStringLiteral("已发送"), QStringLiteral("info")) : qMakePair(QStringLiteral("失败"), QStringLiteral("critical"));
}

class RuleEditor final : public QDialog {
public:
    RuleEditor(QWidget *parent, const QJsonObject &rule, const QJsonArray &processes, int coreCount)
        : QDialog(parent), processes_(processes), coreCount_(qMax(1, coreCount)) {
        setObjectName("processRuleEditor");
        setWindowTitle(rule.isEmpty() ? QStringLiteral("添加规则") : QStringLiteral("编辑规则"));
        setPalette(parent->palette());
        resize(600, 610);
        setMinimumWidth(560);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(20, 18, 20, 16); layout->setSpacing(14);
        layout->addWidget(label(windowTitle(), "title"));

        layout->addWidget(label(QStringLiteral("目标程序"), "strong"));
        auto *targetRow = new QHBoxLayout; targetRow->setSpacing(8);
        mode_ = new QComboBox;
        mode_->setObjectName("automationMatchMode");
        mode_->addItem(QStringLiteral("程序路径"), "path");
        mode_->addItem(QStringLiteral("程序名"), "name");
        value_ = new QLineEdit;
        value_->setObjectName("automationTarget");
        auto *pick = new QPushButton(QStringLiteral("选择进程…")); pick->setObjectName("automationPickProcess");
        auto *browse = new QPushButton(QStringLiteral("浏览…")); browse->setObjectName("automationBrowse");
        targetRow->addWidget(mode_); targetRow->addWidget(value_, 1);
        layout->addLayout(targetRow);
        auto *pickRow = new QHBoxLayout; pickRow->setSpacing(8);
        pickRow->addWidget(pick); pickRow->addWidget(browse);
        matchHint_ = label({}, "muted", "automationMatchHint");
        pickRow->addWidget(matchHint_, 1);
        layout->addLayout(pickRow);

        layout->addWidget(label(QStringLiteral("什么时候结束"), "strong"));
        always_ = new QRadioButton(QStringLiteral("只要运行就结束")); always_->setObjectName("automationAlways");
        whenBusy_ = new QRadioButton(QStringLiteral("CPU 超过阈值时才结束")); whenBusy_->setObjectName("automationWhenBusy");
        auto *group = new QButtonGroup(this); group->addButton(whenBusy_); group->addButton(always_);
        layout->addWidget(whenBusy_);
        auto *limits = new QHBoxLayout; limits->setContentsMargins(22, 0, 0, 0); limits->setSpacing(8);
        cpu_ = new QSpinBox; cpu_->setObjectName("automationCpuLimit"); cpu_->setRange(1, 12800); cpu_->setSuffix("%"); cpu_->setValue(80); cpu_->setMinimumWidth(76); cpu_->setAlignment(Qt::AlignRight);
        sustain_ = new QSpinBox; sustain_->setObjectName("automationSustain"); sustain_->setRange(0, 3600); sustain_->setSuffix(QStringLiteral(" 秒")); sustain_->setValue(30); sustain_->setMinimumWidth(76); sustain_->setAlignment(Qt::AlignRight);
        limits->addWidget(label(QStringLiteral("超过"), "caption")); limits->addWidget(cpu_);
        limits->addWidget(label(QStringLiteral("持续"), "caption")); limits->addWidget(sustain_);
        limits->addStretch();
        layout->addLayout(limits);
        cpuHint_ = label({}, "muted", "automationCpuHint");
        cpuHint_->setContentsMargins(22, 0, 0, 0);
        layout->addWidget(cpuHint_);
        layout->addWidget(always_);

        layout->addWidget(label(QStringLiteral("怎样结束"), "strong"));
        auto *forceRow = new QHBoxLayout; forceRow->setSpacing(8);
        force_ = new ToggleSwitch; force_->setObjectName("automationForceKill"); force_->setChecked(true);
        grace_ = new QSpinBox; grace_->setObjectName("automationGrace"); grace_->setRange(1, 120); grace_->setSuffix(QStringLiteral(" 秒")); grace_->setValue(10); grace_->setMinimumWidth(66); grace_->setAlignment(Qt::AlignRight);
        forceRow->addWidget(force_);
        forceRow->addWidget(label(QStringLiteral("先请求结束，"), "caption"));
        forceRow->addWidget(grace_);
        forceRow->addWidget(label(QStringLiteral("后仍未退出则强制结束"), "caption"));
        forceRow->addStretch();
        layout->addLayout(forceRow);

        layout->addWidget(DevStatsUI::separator());
        sentence_ = label({}, nullptr, "automationSentence");
        layout->addWidget(sentence_);
        running_ = label({}, "caption", "automationRunning");
        layout->addWidget(running_);
        error_ = label({}, "caption", "automationEditorError");
        DevStatsUI::setTone(error_, QStringLiteral("critical"));
        error_->hide();
        layout->addWidget(error_);
        layout->addStretch();
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存规则"));
        buttons->button(QDialogButtonBox::Save)->setObjectName("automationSaveRule");
        buttons->button(QDialogButtonBox::Save)->setProperty("primary", true);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);

        id_ = rule.value("id").toString(QUuid::createUuid().toString(QUuid::WithoutBraces));
        enabled_ = rule.value("enabled").toBool(true);
        if (!rule.isEmpty()) {
            mode_->setCurrentIndex(rule.value("match").toString() == "name" ? 1 : 0);
            value_->setText(rule.value("value").toString());
            const bool busy = rule.value("cpuAbovePercent").isDouble();
            whenBusy_->setChecked(busy); always_->setChecked(!busy);
            if (busy) { cpu_->setValue(int(rule.value("cpuAbovePercent").toDouble())); sustain_->setValue(rule.value("sustainSeconds").toInt()); }
            force_->setChecked(rule.value("forceKill").toBool());
            grace_->setValue(rule.value("graceSeconds").toInt(10));
        } else whenBusy_->setChecked(true);

        connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { refresh(); });
        connect(value_, &QLineEdit::textChanged, this, [this] { refresh(); });
        connect(whenBusy_, &QRadioButton::toggled, this, [this] { refresh(); });
        connect(cpu_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { refresh(); });
        connect(sustain_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { refresh(); });
        connect(force_, &ToggleSwitch::toggled, this, [this] { refresh(); });
        connect(grace_, QOverload<int>::of(&QSpinBox::valueChanged), this, [this] { refresh(); });
        connect(pick, &QPushButton::clicked, this, [this] { pickProcess(); });
        connect(browse, &QPushButton::clicked, this, [this] {
            const auto path = QFileDialog::getOpenFileName(this, QStringLiteral("选择可执行程序"));
            if (!path.isEmpty()) { mode_->setCurrentIndex(0); value_->setText(QFileInfo(path).canonicalFilePath()); }
        });
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { submit(); });
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        refresh();
    }
    void setProcess(const QJsonObject &process) {
        mode_->setCurrentIndex(0);
        value_->setText(process.value("executablePath").toString());
        // Suggest a limit the process is really exceeding, rounded down to a clean figure.
        const double cpu = process.value("cpuPercent").toDouble(-1);
        if (cpu >= 40) cpu_->setValue(std::max(50, int(cpu * .7) / 10 * 10));
    }
    QJsonObject rule() const {
        QJsonObject result{{"id", id_}, {"enabled", enabled_}, {"match", mode_->currentData().toString()}, {"value", target()},
            {"forceKill", force_->isChecked()}, {"graceSeconds", grace_->value()}};
        if (whenBusy_->isChecked()) { result["cpuAbovePercent"] = double(cpu_->value()); result["sustainSeconds"] = sustain_->value(); }
        return result;
    }
    void setError(const QString &message) { error_->setText(message); error_->setVisible(!message.isEmpty()); }
    std::function<QString(const QJsonObject &)> validator;
private:
    QString target() const {
        const QString value = value_->text().trimmed();
        if (mode_->currentData().toString() == "path") { const QString canonical = QFileInfo(value).canonicalFilePath(); if (!canonical.isEmpty()) return canonical; }
        return value;
    }
    QVector<QJsonObject> runningMatches() const {
        QVector<QJsonObject> result;
        const QString wanted = target();
        if (wanted.isEmpty()) return result;
        const bool byPath = mode_->currentData().toString() == "path";
        for (const auto &value : processes_) {
            const auto p = value.toObject();
            const QString path = p.value("executablePath").toString();
            if (byPath ? path == wanted : QFileInfo(path).fileName() == wanted) result.append(p);
        }
        return result;
    }
    void refresh() {
        const bool busy = whenBusy_->isChecked();
        cpu_->setEnabled(busy); sustain_->setEnabled(busy);
        grace_->setEnabled(force_->isChecked());
        const bool byPath = mode_->currentData().toString() == "path";
        value_->setPlaceholderText(byPath ? QStringLiteral("例如 /opt/example/bin/helper") : QStringLiteral("例如 example-helper"));
        matchHint_->setText(byPath ? QStringLiteral("只匹配这一个路径的程序，推荐。")
                                   : QStringLiteral("匹配所有同名程序，区分大小写，不匹配命令行参数。"));
        const double total = cpu_->value() / double(coreCount_);
        cpuHint_->setText(QStringLiteral("100% = 占满 1 个核心。%1% 约等于全机 %2%（本机 %3 个核心）。按单个进程判断。持续时间内的平均值超过阈值才会触发。")
            .arg(cpu_->value()).arg(total, 0, 'f', total < 10 ? 1 : 0).arg(coreCount_));
        cpuHint_->setVisible(busy);
        const QString shown = target().isEmpty() ? QStringLiteral("该程序") : (byPath ? QFileInfo(target()).fileName() : target());
        const QString when = busy ? (sustain_->value() ? QStringLiteral("当 %1 的 CPU 超过 %2%，持续 %3 秒时").arg(shown).arg(cpu_->value()).arg(sustain_->value())
                                                         : QStringLiteral("当 %1 的 CPU 超过 %2% 时").arg(shown).arg(cpu_->value()))
                                  : QStringLiteral("只要 %1 运行").arg(shown);
        const QString how = force_->isChecked() ? QStringLiteral("，先请求结束，%1 秒后仍未退出则强制结束。").arg(grace_->value()) : QStringLiteral("，请求结束（不强制）。");
        sentence_->setText(when + how);
        const auto matches = runningMatches();
        if (matches.isEmpty()) running_->setText(target().isEmpty() ? QString() : QStringLiteral("现在没有匹配的进程在运行。"));
        else {
            double top = -1;
            for (const auto &p : matches) top = std::max(top, p.value("cpuPercent").toDouble(-1));
            running_->setText(QStringLiteral("现在有 %1 个匹配的进程在运行，最高 CPU %2。")
                .arg(matches.size()).arg(top >= 0 ? QString::number(top, 'f', 0) + '%' : None));
        }
        setError({});
    }
    void pickProcess() {
        QDialog picker(this);
        picker.setObjectName("automationProcessPicker");
        picker.setWindowTitle(QStringLiteral("选择进程"));
        picker.setPalette(palette());
        picker.resize(680, 440);
        auto *layout = new QVBoxLayout(&picker);
        auto *filter = new QLineEdit;
        filter->setObjectName("automationProcessFilter");
        filter->setPlaceholderText(QStringLiteral("搜索程序名或路径"));
        layout->addWidget(filter);
        auto *tree = DevStatsUI::table("automationProcessChoices", {QStringLiteral("程序"), "PID", "CPU", QStringLiteral("内存"), QStringLiteral("路径")});
        tree->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
        tree->header()->setSectionResizeMode(4, QHeaderView::Stretch);
        layout->addWidget(tree, 1);
        QVector<QJsonObject> rows;
        for (const auto &value : processes_) {
            const auto p = value.toObject();
            const QString path = p.value("executablePath").toString();
            if (!path.isEmpty() && QDir::isAbsolutePath(path) && !path.endsWith(" (deleted)")) rows.append(p);
        }
        std::sort(rows.begin(), rows.end(), [](const QJsonObject &a, const QJsonObject &b) { return a.value("cpuPercent").toDouble(-1) > b.value("cpuPercent").toDouble(-1); });
        for (const auto &p : rows) {
            const QString path = p.value("executablePath").toString();
            const auto cpu = p.value("cpuPercent");
            auto *item = new QTreeWidgetItem(tree, {QFileInfo(path).fileName(), QString::number(p.value("pid").toInt()),
                cpu.isDouble() ? QString::number(cpu.toDouble(), 'f', 1) + '%' : None, formatBytes(p.value("memoryBytes").toDouble(-1)), path});
            for (int column = 1; column < 4; ++column) item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(4, Qt::AlignLeft | Qt::AlignVCenter);
            item->setToolTip(4, path);
            item->setData(0, IdRole, p);
        }
        connect(filter, &QLineEdit::textChanged, &picker, [tree](const QString &text) {
            for (int i = 0; i < tree->topLevelItemCount(); ++i) {
                auto *item = tree->topLevelItem(i);
                item->setHidden(!item->text(0).contains(text, Qt::CaseInsensitive) && !item->text(4).contains(text, Qt::CaseInsensitive));
            }
        });
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("选择"));
        buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
        layout->addWidget(buttons);
        connect(tree, &QTreeWidget::itemSelectionChanged, &picker, [tree, buttons] { buttons->button(QDialogButtonBox::Ok)->setEnabled(tree->currentItem() && !tree->currentItem()->isHidden()); });
        connect(tree, &QTreeWidget::itemDoubleClicked, &picker, [&picker] { picker.accept(); });
        connect(buttons, &QDialogButtonBox::accepted, &picker, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &picker, &QDialog::reject);
        if (picker.exec() != QDialog::Accepted || !tree->currentItem()) return;
        setProcess(tree->currentItem()->data(0, IdRole).toJsonObject());
    }
    void submit() {
        if (validator) {
            const QString problem = validator(rule());
            if (!problem.isEmpty()) { setError(problem); return; }
        }
        // An unconditional rule acts as soon as it is saved: say so with the real targets.
        if (!whenBusy_->isChecked()) {
            const auto matches = runningMatches();
            if (!matches.isEmpty()) {
                QStringList names;
                for (const auto &p : matches.mid(0, 6)) names << QStringLiteral("%1（PID %2）").arg(p.value("name").toString()).arg(p.value("pid").toInt());
                QMessageBox confirm(QMessageBox::Warning, QStringLiteral("将立即结束正在运行的进程"),
                    QStringLiteral("保存后，以下 %1 个进程会立即被结束，未保存的内容可能丢失：\n\n%2").arg(matches.size()).arg(names.join('\n')), QMessageBox::Cancel, this);
                confirm.setObjectName("confirmRuleImmediate");
                confirm.setTextFormat(Qt::PlainText);
                auto *proceed = confirm.addButton(QStringLiteral("确认保存"), QMessageBox::AcceptRole);
                confirm.setDefaultButton(QMessageBox::Cancel);
                confirm.exec();
                if (confirm.clickedButton() != proceed) return;
            }
        }
        accept();
    }
    QJsonArray processes_;
    int coreCount_;
    QString id_;
    bool enabled_ = true;
    QComboBox *mode_;
    QLineEdit *value_;
    QRadioButton *always_, *whenBusy_;
    QSpinBox *cpu_, *sustain_, *grace_;
    ToggleSwitch *force_;
    QLabel *matchHint_, *cpuHint_, *sentence_, *running_, *error_;
};
}

struct ProcessRulesPanel::Impl {
    ProcessRulesPanel *q;
    QJsonArray rules, processes, logEntries;
    QJsonObject status;
    bool enabled = false, loading = false;
    int coreCount = 1, pendingChanges = 0;
    ToggleSwitch *master = nullptr;
    QLabel *masterState = nullptr, *feedback = nullptr, *empty = nullptr, *logEmpty = nullptr;
    QFrame *masterCard = nullptr;
    QTreeWidget *table = nullptr, *log = nullptr;
    QPushButton *addButton = nullptr;
    explicit Impl(ProcessRulesPanel *panel) : q(panel) {}

    QString validate(const QJsonArray &candidate) const {
        ProcessRules validator;
        const auto result = validator.configure(false, candidate);
        return result.value("ok").toBool() ? QString() : result.value("message").toString();
    }
    void apply() {
        say(QStringLiteral("正在应用…"));
        ++pendingChanges;
        emit q->configurationRequested(enabled, rules);
    }
    void renderMaster() {
        int active = 0;
        for (const auto &value : rules) if (value.toObject().value("enabled").toBool()) ++active;
        QSignalBlocker block(master);
        master->setChecked(enabled);
        masterState->setText(rules.isEmpty() ? QStringLiteral("还没有规则。添加规则后打开开关开始生效。")
            : enabled ? QStringLiteral("正在运行 · %1 条规则生效 · 平时每 10 秒检查一次，超限后每 2 秒").arg(active)
                      : QStringLiteral("已暂停 · 不会结束任何进程"));
        DevStatsUI::setTone(masterState, enabled && active ? QStringLiteral("ok") : QString());
    }
    // Hidden while empty, so a blank line does not sit at the bottom of the card.
    void say(const QString &message) { feedback->setText(message); feedback->setVisible(!message.isEmpty()); }
    void renderRules() {
        const QString selected = table->currentItem() ? table->currentItem()->data(0, IdRole).toString() : QString();
        QSignalBlocker block(table);
        table->clear();
        for (int i = 0; i < rules.size(); ++i) {
            const auto rule = rules[i].toObject();
            auto *item = new QTreeWidgetItem(table);
            item->setData(0, IdRole, rule.value("id").toString());
            item->setText(1, displayName(rule));
            item->setToolTip(1, rule.value("value").toString());
            item->setText(2, conditionText(rule));
            item->setToolTip(2, actionText(rule));
            auto *toggle = new ToggleSwitch;
            toggle->setObjectName("automationRuleToggle");
            toggle->setChecked(rule.value("enabled").toBool());
            auto *holder = new QWidget;
            auto *row = new QHBoxLayout(holder); row->setContentsMargins(2, 0, 0, 0); row->addWidget(toggle);
            table->setItemWidget(item, 0, holder);
            const QString id = rule.value("id").toString();
            connect(toggle, &ToggleSwitch::toggled, q, [this, id](bool on) { setRuleEnabled(id, on); });
            auto *editButton = new QToolButton; editButton->setText(QStringLiteral("编辑")); editButton->setObjectName("automationEditRule"); editButton->setProperty("ghost", true);
            auto *removeButton = new QToolButton; removeButton->setText(QStringLiteral("删除")); removeButton->setObjectName("automationRemoveRule"); removeButton->setProperty("ghost", true);
            auto *actions = new QWidget;
            auto *actionRow = new QHBoxLayout(actions); actionRow->setContentsMargins(0, 0, 0, 0); actionRow->setSpacing(2);
            actionRow->addWidget(editButton); actionRow->addWidget(removeButton);
            table->setItemWidget(item, 4, actions);
            connect(editButton, &QToolButton::clicked, q, [this, id] { edit(id, {}); });
            connect(removeButton, &QToolButton::clicked, q, [this, id] { remove(id); });
            if (id == selected) table->setCurrentItem(item);
        }
        table->setVisible(!rules.isEmpty());
        empty->setVisible(rules.isEmpty());
        renderStatus();
        renderMaster();
        DevStatsUI::fitRows(table, 1, 12);
    }
    void renderStatus() {
        for (int i = 0; i < table->topLevelItemCount() && i < rules.size(); ++i) {
            auto *item = table->topLevelItem(i);
            const auto rule = rules[i].toObject();
            const auto state = statusText(rule, status.value(rule.value("id").toString()).toObject(), enabled);
            if (item->text(3) != state.first) item->setText(3, state.first);
            item->setToolTip(3, status.value(rule.value("id").toString()).toObject().value("protectedReason").toString(state.first));
            item->setData(3, MetricItemDelegate::ToneRole, state.second == "neutral" ? QString() : state.second);
        }
    }
    void renderLog() {
        const int scroll = log->verticalScrollBar()->value();
        log->clear();
        for (const auto &value : logEntries) {
            const auto entry = value.toObject();
            const double seconds = entry.value("timestamp").toDouble();
            const auto phase = phaseCaption(entry);
            const QString name = entry.value("name").toString(QFileInfo(entry.value("executablePath").toString()).fileName());
            const auto trigger = entry.value("triggerCpuPercent");
            QString why = entry.value("condition").toString();
            if (trigger.isDouble()) why += QStringLiteral("（实际 %1%）").arg(trigger.toDouble(), 0, 'f', 0);
            auto *item = new QTreeWidgetItem(log, {seconds > 0 ? QDateTime::fromMSecsSinceEpoch(qint64(seconds * 1000)).toString("MM-dd HH:mm:ss") : None,
                name, QString::number(entry.value("pid").toInt()), why, phase.first, entry.value("message").toString()});
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(3, Qt::AlignLeft | Qt::AlignVCenter);
            item->setTextAlignment(4, Qt::AlignLeft | Qt::AlignVCenter);
            item->setTextAlignment(5, Qt::AlignLeft | Qt::AlignVCenter);
            item->setData(4, MetricItemDelegate::ToneRole, phase.second);
            item->setToolTip(1, entry.value("executablePath").toString());
            item->setToolTip(5, entry.value("message").toString());
        }
        log->verticalScrollBar()->setValue(scroll);
        log->setVisible(!logEntries.isEmpty());
        logEmpty->setVisible(logEntries.isEmpty());
        DevStatsUI::fitRows(log, 1, 12);
    }
    void setRuleEnabled(const QString &id, bool on) {
        for (int i = 0; i < rules.size(); ++i) {
            auto rule = rules[i].toObject();
            if (rule.value("id").toString() != id) continue;
            rule["enabled"] = on;
            rules[i] = rule;
        }
        renderStatus();
        renderMaster();
        apply();
    }
    void remove(const QString &id) {
        for (int i = rules.size() - 1; i >= 0; --i) if (rules[i].toObject().value("id").toString() == id) rules.removeAt(i);
        renderRules();
        apply();
    }
    void edit(const QString &id, const QJsonObject &process) {
        QJsonObject current;
        for (const auto &value : rules) if (value.toObject().value("id").toString() == id) current = value.toObject();
        RuleEditor editor(q, current, processes, coreCount);
        if (!process.isEmpty()) editor.setProcess(process);
        auto replaced = [this, id](const QJsonObject &rule) {
            QJsonArray candidate = rules;
            bool found = false;
            for (int i = 0; i < candidate.size(); ++i) if (candidate[i].toObject().value("id").toString() == rule.value("id").toString()) { candidate[i] = rule; found = true; }
            if (!found) candidate.append(rule);
            return candidate;
        };
        editor.validator = [this, replaced](const QJsonObject &rule) { return validate(replaced(rule)); };
        if (editor.exec() != QDialog::Accepted) return;
        rules = replaced(editor.rule());
        renderRules();
        apply();
    }
};

ProcessRulesPanel::ProcessRulesPanel(QWidget *parent) : QWidget(parent), d(new Impl(this)) {
    setObjectName("processRulesPanel");
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0); layout->setSpacing(10);

    d->masterCard = new QFrame;
    d->masterCard->setProperty("card", true);
    auto *masterRow = new QHBoxLayout(d->masterCard);
    masterRow->setContentsMargins(16, 12, 16, 12); masterRow->setSpacing(12);
    d->master = new ToggleSwitch;
    d->master->setObjectName("automationEnabled");
    masterRow->addWidget(d->master, 0, Qt::AlignVCenter);
    auto *words = new QVBoxLayout; words->setSpacing(2);
    words->addWidget(label(QStringLiteral("自动查杀"), "heading"));
    d->masterState = label({}, "caption", "automationStatus");
    words->addWidget(d->masterState);
    masterRow->addLayout(words, 1);
    layout->addWidget(d->masterCard);

    d->addButton = new QPushButton(QStringLiteral("添加规则…"));
    d->addButton->setObjectName("automationAddRule");
    d->addButton->setProperty("primary", true);
    auto *rulesColumn = card(layout, QStringLiteral("规则"), {}, d->addButton);
    d->table = DevStatsUI::table("automationRules", {QStringLiteral("开关"), QStringLiteral("程序"), QStringLiteral("条件"), QStringLiteral("当前状态"), QString()});
    d->table->setSelectionMode(QAbstractItemView::NoSelection);
    d->table->setFocusPolicy(Qt::NoFocus);
    d->table->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    d->table->header()->setSectionResizeMode(0, QHeaderView::Fixed);
    d->table->setColumnWidth(0, 56);
    d->table->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    d->table->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    d->table->header()->setSectionResizeMode(3, QHeaderView::Stretch);
    d->table->header()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    d->table->hide();
    rulesColumn->addWidget(d->table);
    d->empty = label(QStringLiteral("例如：让某个程序在 CPU 持续超过 80% 时自动结束。\n也可以在“卡顿诊断”或进程列表里右键一个进程，直接加入。"), "caption", "automationEmpty");
    rulesColumn->addWidget(d->empty);
    rulesColumn->addWidget(label(QStringLiteral("只能结束当前用户的进程。桌面、会话、终端和 DevStats 自身受保护。DevStats 退出后规则停止生效；如需开机生效，请在设置里打开登录后启动。"), "muted"));
    d->feedback = label({}, "caption", "automationFeedback");
    d->feedback->hide();
    rulesColumn->addWidget(d->feedback);

    auto *logColumn = card(layout, QStringLiteral("执行记录"), QStringLiteral("最近 100 条，退出后仍保留。"));
    d->log = DevStatsUI::table("automationLog", {QStringLiteral("时间"), QStringLiteral("程序"), "PID", QStringLiteral("触发条件"), QStringLiteral("结果"), QStringLiteral("详情")});
    d->log->header()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    d->log->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    d->log->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    d->log->header()->setSectionResizeMode(5, QHeaderView::Stretch);
    d->log->hide();
    logColumn->addWidget(d->log);
    d->logEmpty = label(QStringLiteral("还没有执行过。"), "caption", "automationLogEmpty");
    logColumn->addWidget(d->logEmpty);

    connect(d->addButton, &QPushButton::clicked, this, [this] { d->edit({}, {}); });
    connect(d->master, &ToggleSwitch::toggled, this, [this](bool on) {
        if (d->loading) return;
        if (!on) {
            d->enabled = false;
            d->renderRules();
            d->say(QStringLiteral("正在暂停…"));
            ++d->pendingChanges;
            emit pauseRequested();
            return;
        }
        QStringList targets;
        for (const auto &value : d->rules) {
            const auto rule = value.toObject();
            if (rule.value("enabled").toBool()) targets << displayName(rule) + QStringLiteral("：") + conditionText(rule);
        }
        if (targets.isEmpty()) {
            QSignalBlocker block(d->master);
            d->master->setChecked(false);
            d->say(QStringLiteral("请先添加至少一条启用的规则。"));
            return;
        }
        QMessageBox confirm(QMessageBox::Warning, QStringLiteral("开启自动查杀"),
            QStringLiteral("DevStats 将按以下 %1 条规则自动结束进程，不再逐次询问。未保存的内容可能丢失。\n\n%2").arg(targets.size()).arg(targets.mid(0, 8).join('\n')), QMessageBox::Cancel, this);
        confirm.setObjectName("confirmProcessAutomation");
        confirm.setTextFormat(Qt::PlainText);
        auto *enable = confirm.addButton(QStringLiteral("确认开启"), QMessageBox::AcceptRole);
        confirm.setDefaultButton(QMessageBox::Cancel);
        confirm.exec();
        if (confirm.clickedButton() != enable) {
            QSignalBlocker block(d->master);
            d->master->setChecked(false);
            d->say(QStringLiteral("已取消，自动查杀仍处于暂停。"));
            return;
        }
        d->enabled = true;
        d->renderRules();
        d->apply();
    });
    d->renderRules();
    d->renderLog();
}

ProcessRulesPanel::~ProcessRulesPanel() = default;

void ProcessRulesPanel::setConfiguration(bool enabled, const QJsonArray &rules) {
    d->loading = true;
    d->enabled = enabled;
    d->rules = rules;
    d->renderRules();
    d->loading = false;
}

void ProcessRulesPanel::setSnapshot(const QJsonObject &snapshot) {
    d->processes = snapshot.value("processes").toArray();
    d->coreCount = snapshot.value("cpu").toObject().value("coreCount").toInt(d->coreCount);
    const auto automation = snapshot.value("processAutomation").toObject();
    const auto status = automation.value("status").toObject();
    if (status != d->status) { d->status = status; d->renderStatus(); }
    if (automation.contains("enabled") && automation.value("enabled").toBool() != d->enabled && !d->loading && !d->pendingChanges) {
        d->enabled = automation.value("enabled").toBool();
        d->renderMaster();
        d->renderStatus();
    }
    const auto entries = automation.value("log").toArray();
    if (entries != d->logEntries) { d->logEntries = entries; d->renderLog(); }
    const QString storage = automation.value("storageError").toString();
    if (!storage.isEmpty()) d->say(storage);
}

void ProcessRulesPanel::configurationFinished(const QJsonObject &result) {
    d->pendingChanges = qMax(0, d->pendingChanges - 1);
    if (result.value("ok").toBool()) {
        if (result.value("pauseOnly").toBool()) { d->enabled = false; d->renderMaster(); d->renderStatus(); }
        else setConfiguration(result.value("enabled").toBool(), result.value("rules").toArray());
    }
    d->say(result.value("message").toString());
}

void ProcessRulesPanel::addRuleFor(const QJsonObject &process) { d->edit({}, process); }
