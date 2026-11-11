#include "monitor_window.h"
#include "devstats_version.h"
#include "i18n.h"
#include "charts.h"
#include "diagnostics_panel.h"
#include "history_feed.h"
#include "network_activity_panel.h"
#include "process_rules_panel.h"
#include "resource_breakdown.h"
#include "ui_components.h"
#include "ui_theme.h"

#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGuiApplication>
#include <QHeaderView>
#include <QHideEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <functional>

using DevStatsUI::card;
using DevStatsUI::label;

namespace {
constexpr int ModuleRole = Qt::UserRole;
constexpr int BadgeRole = Qt::UserRole + 1;
constexpr int BadgeToneRole = Qt::UserRole + 2;
constexpr int ProcessRole = Qt::UserRole + 1;

const QStringList Modules = {"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics", "automation", "projects"};
// Looked up when shown, so they follow the language in use; one entry per module, in order.
QStringList titles() {
    return {T("总览"), T("处理器"), T("内存"), T("磁盘"), T("网络"), T("电池"), T("卡顿诊断"), T("自动查杀"), T("开发项目")};
}
QStringList subtitles() {
    return {T("系统状态、最忙的进程和最近的卡顿"), T("使用率、核心负载和占用最高的进程"),
        T("物理内存、缓存和占用最高的进程"), T("读写速率、设备繁忙度和卷容量"), T("上传、下载和按应用排行"),
        T("电源状态和应用耗电影响"), T("找出卡顿的原因和相关进程"), T("进程运行或超过 CPU 阈值时自动结束"),
        T("按项目归组查看进程")};
}
// Pages without a history chart do not show the time range selector.
const QSet<QString> HistoryModules = {"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics"};

QString unavailable() { return QString::fromUtf8("—"); }
double numeric(const QJsonValue &value) { return value.isDouble() ? value.toDouble() : -1; }
QString bytes(const QJsonValue &value, bool rate = false) { return formatBytes(numeric(value), rate); }
QString percent(const QJsonValue &value) { return formatPercent(numeric(value)); }
QString cpuPercent(const QJsonValue &value) { return value.isDouble() ? QString::number(value.toDouble(), 'f', 1) + "%" : unavailable(); }
QString number(const QJsonValue &value, int decimals = 0) { return value.isDouble() ? QString::number(value.toDouble(), 'f', decimals) : unavailable(); }
QString duration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0) return unavailable();
    const qint64 value = qint64(seconds);
    if (value >= 86400) return T("%1 天 %2 小时").arg(value / 86400).arg(value / 3600 % 24);
    if (value >= 3600) return T("%1 小时 %2 分").arg(value / 3600).arg(value / 60 % 60);
    if (value >= 60) return T("%1 分 %2 秒").arg(value / 60).arg(value % 60);
    return T("%1 秒").arg(value);
}
QString batteryState(const QJsonObject &battery) {
    if (battery.value("isPresent").isBool() && !battery.value("isPresent").toBool()) return T("未检测到电池");
    const QString state = battery.value("state").toString();
    if (state == "charging") return T("正在充电");
    if (state == "discharging") return T("使用电池");
    if (state == "full") return T("电量已充满");
    if (state == "pluggedIn") return T("已接通电源");
    return T("状态暂不可用");
}
QString jsonStrings(const QJsonArray &array) {
    QStringList values;
    for (const auto &value : array) values.append(value.toString());
    return values.isEmpty() ? unavailable() : values.join("  ·  ");
}
QString stateTone(const QString &state) {
    return state == "critical" ? QStringLiteral("critical") : state == "attention" ? QStringLiteral("warn") : state == "normal" ? QStringLiteral("ok") : QString();
}

QIcon moduleIcon(const QString &module, const QPalette &palette) {
    const auto c = DevStatsUI::colors(palette);
    QIcon icon;
    for (const auto state : {QIcon::Normal, QIcon::Selected}) for (int scale : {1, 2, 3}) {
        QPixmap image(20 * scale, 20 * scale); image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
        QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
        const QColor color = state == QIcon::Selected ? c.accent : c.secondary;
        p.setPen(QPen(color, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (module == "overview") { for (int y : {3, 11}) for (int x : {3, 11}) p.drawRoundedRect(QRectF(x, y, 6, 6), 1, 1); }
        else if (module == "cpu") { p.drawRoundedRect(QRectF(5, 5, 10, 10), 2, 2); for (int i : {7, 10, 13}) { p.drawLine(i, 2, i, 4); p.drawLine(i, 16, i, 18); p.drawLine(2, i, 4, i); p.drawLine(16, i, 18, i); } }
        else if (module == "memory") { p.drawRoundedRect(QRectF(2, 5, 16, 10), 2, 2); for (int x : {5, 9, 13}) { p.drawRect(QRectF(x, 8, 2, 4)); p.drawLine(x, 16, x, 18); } }
        else if (module == "disk") { p.drawRoundedRect(QRectF(2, 4, 16, 13), 2, 2); p.drawLine(3, 12, 17, 12); p.drawPoint(14, 15); }
        else if (module == "network") { p.drawLine(6, 16, 6, 4); p.drawLine(3, 7, 6, 4); p.drawLine(9, 7, 6, 4); p.drawLine(14, 4, 14, 16); p.drawLine(11, 13, 14, 16); p.drawLine(17, 13, 14, 16); }
        else if (module == "battery") { p.drawRoundedRect(QRectF(2, 6, 14, 9), 2, 2); p.drawLine(18, 9, 18, 12); p.fillRect(QRectF(5, 9, 7, 3), color); }
        else if (module == "diagnostics") { p.drawEllipse(QRectF(2, 2, 12, 12)); p.drawLine(12, 12, 18, 18); p.drawLine(4, 8, 6, 8); p.drawLine(6, 8, 8, 5); p.drawLine(8, 5, 10, 10); p.drawLine(10, 10, 12, 8); }
        else if (module == "automation") { p.drawEllipse(QRectF(4, 4, 12, 12)); p.drawLine(10, 1, 10, 7); p.drawLine(10, 13, 10, 19); p.drawLine(1, 10, 7, 10); p.drawLine(13, 10, 19, 10); }
        else { p.drawLine(7, 5, 2, 10); p.drawLine(2, 10, 7, 15); p.drawLine(13, 5, 18, 10); p.drawLine(18, 10, 13, 15); p.drawLine(11, 4, 9, 16); }
        p.end(); icon.addPixmap(image, state);
    }
    return icon;
}
QIcon quickIcon(const QString &kind, const QPalette &palette, const QColor &override = QColor()) {
    QIcon icon;
    for (int scale : {1, 2, 3}) {
        QPixmap image(22 * scale, 22 * scale); image.setDevicePixelRatio(scale); image.fill(Qt::transparent);
        QPainter p(&image); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(override.isValid() ? override : DevStatsUI::colors(palette).secondary, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (kind == "terminal") {
            p.drawRoundedRect(QRectF(2, 3, 18, 16), 3, 3); p.drawLine(6, 7, 10, 10); p.drawLine(10, 10, 6, 13); p.drawLine(12, 14, 16, 14);
        } else if (kind == "files") {
            QPainterPath path; path.moveTo(2, 7); path.lineTo(2, 5); path.quadTo(2, 3, 4, 3); path.lineTo(9, 3); path.lineTo(11, 6); path.lineTo(18, 6); path.quadTo(20, 6, 20, 8); path.lineTo(20, 17); path.quadTo(20, 19, 18, 19); path.lineTo(4, 19); path.quadTo(2, 19, 2, 17); path.closeSubpath(); p.drawPath(path); p.drawLine(3, 9, 19, 9);
        } else if (kind == "settings") {
            p.drawEllipse(QRectF(5, 5, 12, 12)); p.drawEllipse(QRectF(9, 9, 4, 4));
            for (int angle = 0; angle < 360; angle += 45) { p.save(); p.translate(11, 11); p.rotate(angle); p.drawLine(0, -8, 0, -6); p.restore(); }
        } else if (kind == "add") {
            p.drawRoundedRect(QRectF(3, 3, 16, 16), 4, 4); p.drawLine(7, 11, 15, 11); p.drawLine(11, 7, 11, 15);
        } else if (kind == "apps") {
            for (int y : {4, 12}) for (int x : {4, 12}) p.drawRoundedRect(QRectF(x, y, 6, 6), 1.5, 1.5);
        } else {
            p.drawRoundedRect(QRectF(2, 3, 18, 16), 3, 3); QPainterPath path; path.moveTo(4, 12); path.lineTo(7, 12); path.lineTo(9, 7); path.lineTo(12, 15); path.lineTo(14, 10); path.lineTo(18, 10); p.drawPath(path);
        }
        p.end(); icon.addPixmap(image);
    }
    return icon;
}

class BatteryIndicator final : public QWidget {
public:
    BatteryIndicator() { setFixedSize(82, 44); setAccessibleName(T("电源状态")); }
    void setState(double level, const QString &state) { level_ = level; state_ = state; setAccessibleName(T("电池 %1").arg(formatPercent(level))); update(); }
protected:
    void paintEvent(QPaintEvent *) override {
        const auto c = DevStatsUI::colors(window()->palette()); QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c.secondary, 1.8)); p.drawRoundedRect(QRectF(2, 5, 68, 34), 6, 6); p.drawLine(75, 16, 75, 28);
        if (level_ >= 0) { QColor fill = level_ <= .15 ? c.critical : c.accent; fill.setAlphaF(.55); p.setPen(Qt::NoPen); p.setBrush(fill); p.drawRoundedRect(QRectF(6, 9, 60 * qBound(0., level_, 1.), 26), 3, 3); }
        if (state_ == "charging") { QPainterPath bolt; bolt.moveTo(40, 9); bolt.lineTo(28, 24); bolt.lineTo(36, 24); bolt.lineTo(32, 35); bolt.lineTo(46, 19); bolt.lineTo(38, 19); bolt.closeSubpath(); p.fillPath(bolt, c.text); }
    }
private:
    double level_ = -1; QString state_;
};

// Sidebar rows: icon, title and an optional status badge, with non-selectable
// section captions between groups.
class NavigationDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;
    QSize sizeHint(const QStyleOptionViewItem &, const QModelIndex &index) const override {
        return QSize(0, index.data(ModuleRole).toString().isEmpty() ? 30 : 34);
    }
    void paint(QPainter *p, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const auto c = DevStatsUI::colors(option.widget->window()->palette());
        p->save();
        p->setRenderHint(QPainter::Antialiasing);
        const QRectF row = QRectF(option.rect).adjusted(0, 1, 0, -1);
        const QString title = index.data(Qt::DisplayRole).toString();
        if (index.data(ModuleRole).toString().isEmpty()) {
            QFont caption = option.font; caption.setPointSizeF(9); caption.setWeight(QFont::DemiBold);
            p->setFont(caption); p->setPen(c.muted);
            p->drawText(row.adjusted(10, 8, -6, 0), Qt::AlignLeft | Qt::AlignVCenter, title);
            p->restore();
            return;
        }
        const bool selected = option.state & QStyle::State_Selected, hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            p->setPen(Qt::NoPen); p->setBrush(selected ? c.accentSoft : c.raised);
            p->drawRoundedRect(row, 6, 6);
        }
        const QIcon icon = index.data(Qt::DecorationRole).value<QIcon>();
        icon.paint(p, QRect(int(row.left()) + 10, int(row.center().y()) - 9, 18, 18), Qt::AlignCenter, selected ? QIcon::Selected : QIcon::Normal);
        QFont font = option.font; font.setPointSizeF(10.5);
        if (selected) font.setWeight(QFont::DemiBold);
        p->setFont(font);
        p->setPen(selected ? c.text : c.secondary);
        QRectF text = row.adjusted(36, 0, -8, 0);
        const QString badge = index.data(BadgeRole).toString();
        if (!badge.isEmpty()) {
            QFont small = font; small.setPointSizeF(8.5); small.setWeight(QFont::DemiBold);
            const QFontMetrics metrics(small);
            const qreal width = std::max<qreal>(18, metrics.horizontalAdvance(badge) + 10);
            const QRectF pill(row.right() - width - 7, row.center().y() - 9, width, 18);
            const QColor tone = DevStatsUI::toneColor(c, index.data(BadgeToneRole).toString());
            QColor soft = tone; soft.setAlphaF(.2);
            p->setPen(Qt::NoPen); p->setBrush(soft); p->drawRoundedRect(pill, 9, 9);
            p->setFont(small); p->setPen(tone); p->drawText(pill, Qt::AlignCenter, badge);
            p->setFont(font); p->setPen(selected ? c.text : c.secondary);
            text.setRight(pill.left() - 4);
        }
        p->drawText(text, Qt::AlignLeft | Qt::AlignVCenter, title);
        p->restore();
    }
};

void row(QTreeWidget *tree, const QStringList &values, const QString &key = QString()) {
    auto *item = new QTreeWidgetItem(tree, values);
    item->setData(0, Qt::UserRole, key);
    for (int i = 0; i < values.size(); ++i) { item->setToolTip(i, values[i]); if (i) item->setTextAlignment(i, Qt::AlignRight | Qt::AlignVCenter); }
}
}

struct MonitorWindow::Impl {
    MonitorWindow *q;
    QListWidget *navigation = nullptr;
    QStackedWidget *pages = nullptr;
    QLabel *title = nullptr, *subtitle = nullptr, *updated = nullptr, *host = nullptr;
    QComboBox *range = nullptr;
    QLineEdit *projectFilter = nullptr;
    QTreeWidget *projectTree = nullptr;
    QHash<QString, QLabel *> values;
    QHash<QString, QTreeWidget *> tables;
    QHash<QString, MeterTile *> tiles;
    QMultiMap<QString, HistoryChart *> charts;
    QGridLayout *coreGrid = nullptr, *tileGrid = nullptr, *overviewColumns = nullptr;
    QVector<MeterTile *> overviewTiles;
    QVector<QFrame *> overviewCards;
    int tileColumns = 0;
    bool overviewStacked = false;
    QVector<QProgressBar *> cores;
    int coreColumns = 2;
    BatteryIndicator *batteryGlyph = nullptr;
    ResourceBreakdown *cpuBreakdown = nullptr, *memoryBreakdown = nullptr;
    QFrame *healthBanner = nullptr;
    QLabel *healthPill = nullptr, *healthHeadline = nullptr, *healthSummary = nullptr;
    QPointer<QDialog> settings;
    QPointer<ToggleSwitch> autoStartCheck, largeTextCheck, notificationCheck;
    QPointer<QComboBox> themeCombo, appearanceCombo;
    QPointer<QListWidget> appsList;
    QHBoxLayout *customLaunches = nullptr;
    QJsonObject snapshot;
    HistoryFeed histories;
    QJsonArray incidents;
    QHash<QString, QJsonObject> pageInputs;
    DiagnosticsPanel *diagnostics = nullptr;
    ProcessRulesPanel *rules = nullptr;
    NetworkActivityPanel *networkActivity = nullptr;
    bool networkObserving = false;
    bool suspectRanking = false;
    bool autoStart = false, visible = false;
    int trayTheme = 0, appearance = 1, language = 0;
    bool largeTrayText = true, automationNotifications = true;
    QStringList manualProjects, customApps;

    explicit Impl(MonitorWindow *window) : q(window) {}

    int rowFor(const QString &module) const {
        for (int i = 0; i < navigation->count(); ++i)
            if (navigation->item(i)->data(ModuleRole).toString() == module) return i;
        return -1;
    }
    QLabel *stat(QLayout *layout, const QString &key, const QString &caption, const char *role = nullptr) {
        auto *container = new QWidget;
        auto *column = new QVBoxLayout(container);
        column->setContentsMargins(0, 0, 0, 0); column->setSpacing(2);
        column->addWidget(label(caption, "caption"));
        auto *value = label(unavailable(), role, key);
        column->addWidget(value);
        values.insert(key, value);
        layout->addWidget(container);
        return value;
    }
    void assign(const QString &key, const QString &value) { if (values.contains(key) && values[key]->text() != value) values[key]->setText(value); }
    HistoryChart *chart(QLayout *layout, const QString &metric, HistoryChart::Unit unit, bool compact = false) {
        auto *widget = new HistoryChart(unit);
        widget->setObjectName(metric + "History");
        widget->setCompact(compact);
        charts.insert(metric, widget);
        layout->addWidget(widget);
        return widget;
    }
    QTreeWidget *processTable(const QString &key, int height,
                              const QStringList &columns = {T("进程"), T("用户"), "PID", "CPU", T("内存")}) {
        auto *tree = DevStatsUI::table(key, columns);
        tree->setMinimumHeight(height);
        tree->headerItem()->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        tree->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(tree, &QTreeWidget::customContextMenuRequested, q, [this, tree](const QPoint &position) { processMenu(tree, position); });
        tables[key] = tree;
        return tree;
    }
    void processMenu(QTreeWidget *tree, const QPoint &position) {
        auto *item = tree->itemAt(position);
        if (!item) return;
        const auto process = item->data(0, ProcessRole).toJsonObject();
        if (process.isEmpty()) return;
        QMenu menu(tree);
        const bool own = !process.value("executablePath").toString().isEmpty();
        auto *terminate = menu.addAction(T("结束进程…"));
        auto *kill = menu.addAction(T("强制结束…"));
        menu.addSeparator();
        auto *rule = menu.addAction(T("加入自动查杀…"));
        terminate->setEnabled(own); kill->setEnabled(own); rule->setEnabled(own);
        if (!own) { menu.addSeparator(); menu.addAction(T("其他用户的进程，无法操作"))->setEnabled(false); }
        const auto *chosen = menu.exec(tree->viewport()->mapToGlobal(position));
        if (chosen == terminate) emit q->processActionRequested(process, "terminate");
        else if (chosen == kill) emit q->processActionRequested(process, "kill");
        else if (chosen == rule) { navigation->setCurrentRow(rowFor("automation")); rules->addRuleFor(process); }
    }
    // Merges the current user's processes with the bounded system list, so a root
    // daemon or kernel thread can top the ranking.
    void processRows(const QString &key, bool memory, int limit) {
        QVector<QJsonObject> processes;
        for (const auto *source : {"processes", "systemProcesses"})
            for (const auto &entry : snapshot.value(source).toArray()) processes.append(entry.toObject());
        const int count = qMin(limit, processes.size());
        std::partial_sort(processes.begin(), processes.begin() + count, processes.end(), [memory](const QJsonObject &a, const QJsonObject &b) {
            return a.value(memory ? "memoryBytes" : "cpuPercent").toDouble(-1) > b.value(memory ? "memoryBytes" : "cpuPercent").toDouble(-1);
        });
        processes.resize(count);
        fillProcesses(key, processes, [](const QJsonObject &process) {
            return QStringList{cpuPercent(process.value("cpuPercent")), bytes(process.value("memoryBytes"))};
        }, T("暂无可读取的进程"));
    }
    static double diskBytes(const QJsonObject &process) {
        const auto read = process.value("ioReadBytesPerSecond"), write = process.value("ioWriteBytesPerSecond");
        return read.isDouble() || write.isDouble() ? read.toDouble(0) + write.toDouble(0) : -1;
    }
    // The current user's processes by bytes read and written. Other users' volume
    // needs root, so they follow by time spent waiting for disk, when the kernel counts it.
    void diskProcessRows() {
        QVector<QJsonObject> own, system;
        for (const auto &entry : snapshot.value("processes").toArray()) {
            const auto process = entry.toObject();
            if (diskBytes(process) > 0 || process.value("ioWaitPercent").toDouble(0) >= 1) own.append(process);
        }
        for (const auto &entry : snapshot.value("systemProcesses").toArray()) {
            const auto process = entry.toObject();
            if (process.value("ioWaitPercent").toDouble(0) >= 1) system.append(process);
        }
        std::sort(own.begin(), own.end(), [](const QJsonObject &a, const QJsonObject &b) {
            if (diskBytes(a) != diskBytes(b)) return diskBytes(a) > diskBytes(b);
            return a.value("ioWaitPercent").toDouble(-1) > b.value("ioWaitPercent").toDouble(-1);
        });
        std::sort(system.begin(), system.end(), [](const QJsonObject &a, const QJsonObject &b) {
            return a.value("ioWaitPercent").toDouble(-1) > b.value("ioWaitPercent").toDouble(-1);
        });
        own.resize(qMin(8, own.size())); system.resize(qMin(4, system.size()));
        fillProcesses("diskProcesses", own + system, [](const QJsonObject &process) {
            const auto wait = process.value("ioWaitPercent");
            return QStringList{bytes(process.value("ioReadBytesPerSecond"), true), bytes(process.value("ioWriteBytesPerSecond"), true),
                               wait.isDouble() ? QString::number(wait.toDouble(), 'f', 0) + '%' : unavailable()};
        }, T("最近没有进程读写磁盘"));
        DevStatsUI::fitRows(tables.value("diskProcesses"), 3, 12);
    }
    // Row objects are reused by process identity, keeping selection and scroll position.
    void fillProcesses(const QString &key, const QVector<QJsonObject> &processes,
                       const std::function<QStringList(const QJsonObject &)> &measures, const QString &empty) {
        auto *tree = tables.value(key);
        if (!tree) return;
        const int count = processes.size();
        const QString selected = tree->currentItem() ? tree->currentItem()->data(0, Qt::UserRole).toString() : QString();
        const int scroll = tree->verticalScrollBar()->value();
        QSignalBlocker blocker(tree);
        tree->setUpdatesEnabled(false);
        QHash<QString, QTreeWidgetItem *> existing;
        while (tree->topLevelItemCount()) { auto *item = tree->takeTopLevelItem(0); existing.insert(item->data(0, Qt::UserRole).toString(), item); }
        for (int i = 0; i < count; ++i) {
            const auto &process = processes[i];
            const QString identity = number(process.value("pid")) + ':' + number(process.value("startTicks"));
            auto *item = existing.take(identity);
            if (!item) item = new QTreeWidgetItem;
            const QString owner = process.value("owner").toString();
            // DevStats' own row says so: it is excluded from findings, not hidden.
            const QString name = process.value("name").toString() + (process.value("self").toBool() ? T("（本工具）") : QString());
            const QStringList cells = QStringList{name, owner.isEmpty() ? T("我") : owner, number(process.value("pid"))}
                + measures(process);
            for (int column = 0; column < cells.size(); ++column) {
                if (item->text(column) != cells[column]) item->setText(column, cells[column]);
                if (column > 1) item->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            }
            item->setData(0, Qt::UserRole, identity);
            item->setData(0, ProcessRole, process);
            item->setToolTip(0, process.value("executablePath").toString().isEmpty() ? process.value("name").toString() : process.value("executablePath").toString());
            tree->addTopLevelItem(item);
            if (identity == selected) tree->setCurrentItem(item);
        }
        qDeleteAll(existing);
        if (!count) {
            QStringList blank{empty};
            while (blank.size() < tree->columnCount()) blank << unavailable();
            row(tree, blank);
        }
        tree->verticalScrollBar()->setValue(scroll);
        tree->setUpdatesEnabled(true);
    }
    void renderProjects() {
        const int scrollPosition = projectTree->verticalScrollBar()->value();
        QSet<QString> expanded;
        QString selected;
        for (int i = 0; i < projectTree->topLevelItemCount(); ++i) {
            auto *item = projectTree->topLevelItem(i);
            if (item->isExpanded()) expanded.insert(item->data(0, Qt::UserRole).toString());
            if (item->isSelected()) selected = item->data(0, Qt::UserRole).toString();
        }
        projectTree->clear();
        QMap<QString, QJsonObject> groups;
        for (const auto &entry : snapshot.value("groups").toArray()) {
            const auto group = entry.toObject(); groups.insert(group.value("path").toString(), group);
        }
        for (const auto &path : manualProjects) if (!groups.contains(path)) groups.insert(path, {{"path", path}, {"name", QFileInfo(path).fileName()}, {"processes", QJsonArray()}});
        const QString filter = projectFilter->text().trimmed();
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            const auto group = it.value(); const auto processes = group.value("processes").toArray();
            QString name = group.value("name").toString(); if (name.isEmpty()) name = it.key();
            bool matches = filter.isEmpty() || name.contains(filter, Qt::CaseInsensitive) || it.key().contains(filter, Qt::CaseInsensitive);
            for (const auto &entry : processes) matches |= entry.toObject().value("name").toString().contains(filter, Qt::CaseInsensitive);
            if (!matches) continue;
            auto *parent = new QTreeWidgetItem(projectTree, {name, cpuPercent(group.value("cpuPercent")), bytes(group.value("memoryBytes")), QString::number(processes.size())});
            parent->setData(0, Qt::UserRole, it.key()); parent->setToolTip(0, it.key());
            QFont font = parent->font(0); font.setBold(true); parent->setFont(0, font);
            for (int column = 1; column < 4; ++column) parent->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            for (const auto &entry : processes) {
                const auto process = entry.toObject();
                auto *child = new QTreeWidgetItem(parent, {process.value("name").toString(), cpuPercent(process.value("cpuPercent")), bytes(process.value("memoryBytes")), "PID " + number(process.value("pid"))});
                child->setData(0, Qt::UserRole, it.key());
                for (int column = 1; column < 4; ++column) child->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            }
            if (processes.isEmpty()) new QTreeWidgetItem(parent, {T("当前没有关联的运行进程"), unavailable(), unavailable(), unavailable()});
            parent->setExpanded(expanded.contains(it.key()) || !filter.isEmpty());
            if (selected == it.key()) parent->setSelected(true);
        }
        if (!projectTree->topLevelItemCount()) row(projectTree, {T("添加项目目录，或启动开发进程"), unavailable(), unavailable(), unavailable()});
        projectTree->verticalScrollBar()->setValue(scrollPosition);
    }
    void renderBatteryImpact() {
        auto *tree = tables.value("batteryImpact");
        const int scroll = tree->verticalScrollBar()->value();
        QSet<QString> expanded;
        QString selected;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            const auto *item = tree->topLevelItem(i);
            if (item->isExpanded()) expanded.insert(item->data(0, Qt::UserRole).toString());
            if (item->isSelected()) selected = item->data(0, Qt::UserRole).toString();
            for (int j = 0; j < item->childCount(); ++j)
                if (item->child(j)->isSelected()) selected = item->child(j)->data(0, Qt::UserRole).toString();
        }
        const QSignalBlocker blocker(tree);
        tree->setUpdatesEnabled(false); tree->clear();
        int count = 0, partial = 0;
        for (const auto &entry : snapshot.value("activityApplications").toArray()) {
            const auto app = entry.toObject();
            const QString identity = app.value("identity").toString();
            const bool complete = app.value("cpuComplete").toBool();
            auto activity = [](const QJsonValue &value, bool complete) {
                return cpuPercent(value) + (value.isDouble() && !complete ? QStringLiteral(" *") : QString());
            };
            auto io = [](const QJsonObject &object, const QString &field) {
                return bytes(object.value(field), true) + (object.value(field).isDouble() && !object.value(field + "Complete").toBool() ? QStringLiteral(" *") : QString());
            };
            const QString processCount = app.value("noReadableProcesses").toBool() ? T("0（最近结束）") : number(app.value("processCount"));
            auto *parent = new QTreeWidgetItem(tree, {app.value("name").toString(), processCount,
                activity(app.value("cpuAveragePercent"), complete), io(app, "ioReadBytesPerSecond"), io(app, "ioWriteBytesPerSecond")});
            parent->setData(0, Qt::UserRole, identity);
            parent->setToolTip(0, app.value("desktopFile").toString() + '\n' + app.value("executablePath").toString());
            parent->setToolTip(2, T("统计窗口 %1；完整采样覆盖 %2%。* 表示只测到部分活动。")
                .arg(duration(app.value("cpuObservationSeconds").toDouble())).arg(app.value("cpuCoverageFraction").toDouble() * 100, 0, 'f', 0));
            for (int column = 1; column < 5; ++column) parent->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
            if (!complete) ++partial;
            for (const auto &value : app.value("processes").toArray()) {
                const auto process = value.toObject();
                const QString key = identity + ':' + number(process.value("pid")) + ':' + number(process.value("startTicks"));
                auto *child = new QTreeWidgetItem(parent, {process.value("name").toString(), "PID " + number(process.value("pid")),
                    cpuPercent(process.value("cpuAveragePercent")), bytes(process.value("ioReadBytesPerSecond"), true), bytes(process.value("ioWriteBytesPerSecond"), true)});
                child->setData(0, Qt::UserRole, key);
                child->setToolTip(0, process.value("executablePath").toString());
                for (int column = 1; column < 5; ++column) child->setTextAlignment(column, Qt::AlignRight | Qt::AlignVCenter);
                if (selected == key) child->setSelected(true);
            }
            parent->setExpanded(expanded.contains(identity));
            if (selected == identity) parent->setSelected(true);
            if (++count == 10) break;
        }
        assign("batteryImpactNote", count ? T("统计窗口 %1 · CPU 100% = 1 个核心%2")
            .arg(duration(snapshot.value("activityObservationSeconds").toDouble()))
            .arg(partial ? T(" · * 只测到部分活动") : QString())
            : T("正在积累观测数据…"));
        if (!count) row(tree, {T("等待有效应用采样"), unavailable(), unavailable(), unavailable(), unavailable()});
        tree->verticalScrollBar()->setValue(scroll); tree->setUpdatesEnabled(true);
    }
    void renderOverview() {
        const auto cpu = snapshot.value("cpu").toObject(), memory = snapshot.value("memory").toObject();
        const auto network = snapshot.value("network").toObject(), io = snapshot.value("diskIO").toObject();
        const auto diagnosis = snapshot.value("diagnostics").toObject();
        const QString tone = stateTone(diagnosis.value("state").toString());
        DevStatsUI::setTone(healthBanner, tone == "ok" ? QString() : tone);
        DevStatsUI::setTone(healthPill, tone);
        healthPill->setText(tone == "critical" ? T("严重") : tone == "warn" ? T("注意") : tone == "ok" ? T("流畅") : T("采样中"));
        healthHeadline->setText(diagnosis.value("headline").toString(T("正在收集数据")));
        healthSummary->setText(diagnosis.value("summary").toString());
        const auto loads = cpu.value("load").toArray();
        tiles["cpu"]->setContent(T("处理器"), percent(cpu.value("usage")),
            loads.isEmpty() ? QString() : T("负载 %1 · %2 核").arg(number(loads[0], 1), number(cpu.value("coreCount"))), "ok", -1);
        tiles["memory"]->setContent(T("内存"), percent(memory.value("fraction")),
            T("可用 %1 / %2").arg(bytes(memory.value("availableBytes")), bytes(memory.value("totalBytes"))), "ok", -1);
        tiles["disk"]->setContent(T("磁盘"), percent(io.value("busiestUtilization")),
            T("读 %1 · 写 %2").arg(bytes(io.value("readBytesPerSecond"), true), bytes(io.value("writeBytesPerSecond"), true)), "ok", -1);
        tiles["network"]->setContent(T("网络"), QStringLiteral("↑ ") + bytes(network.value("uploadBytesPerSecond"), true),
            QStringLiteral("↓ %1 · %2").arg(bytes(network.value("downloadBytesPerSecond"), true), network.value("interface").toString(T("未连接"))), "ok", -1);
        processRows("overviewProcesses", false, 6);
        renderRecentIncidents();
        const auto automation = snapshot.value("processAutomation").toObject();
        int active = 0;
        for (const auto &value : automation.value("rules").toArray()) if (value.toObject().value("enabled").toBool()) ++active;
        const bool enabled = automation.value("enabled").toBool();
        assign("overviewAutomation", enabled && active ? T("运行中 · %1 条规则").arg(active) : automation.value("rules").toArray().isEmpty() ? T("尚未设置规则") : T("已暂停"));
        auto *log = tables.value("overviewAutomationLog");
        const auto entries = automation.value("log").toArray();
        QStringList rendered;
        for (int i = 0; i < qMin(3, entries.size()); ++i) {
            const auto entry = entries[i].toObject();
            rendered << QDateTime::fromMSecsSinceEpoch(qint64(entry.value("timestamp").toDouble() * 1000)).toString("MM-dd HH:mm") + "  " +
                        entry.value("name").toString(QFileInfo(entry.value("executablePath").toString()).fileName()) + "  " + entry.value("message").toString();
        }
        if (log) {
            log->clear();
            for (const auto &line : rendered) new QTreeWidgetItem(log, {line});
            log->setVisible(!rendered.isEmpty());
        }
    }
    void renderRecentIncidents() {
        auto *tree = tables.value("overviewIncidents");
        if (!tree) return;
        tree->clear();
        int count = 0;
        for (const auto &value : incidents) {
            const auto event = value.toObject();
            const qint64 at = qint64(event.value("startTimestamp").toDouble());
            const auto app = event.value("strongestApplication").toObject();
            auto *item = new QTreeWidgetItem(tree, {QDateTime::fromSecsSinceEpoch(at).toString("MM-dd HH:mm"),
                event.value("summary").toString(), duration(event.value("durationSeconds").toDouble())});
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->setToolTip(1, event.value("summary").toString() + (app.isEmpty() ? QString() : QLatin1Char('\n') + T("相关：%1").arg(app.value("name").toString())));
            if (++count == 4) break;
        }
        if (!count) new QTreeWidgetItem(tree, {T("暂无记录"), T("发生卡顿时会自动记录"), unavailable()});
    }
    void layoutTiles(int contentWidth) {
        const int columns = contentWidth >= 720 ? 4 : 2;
        if (columns != tileColumns && tileGrid) {
            tileColumns = columns;
            for (int i = 0; i < overviewTiles.size(); ++i) {
                tileGrid->removeWidget(overviewTiles[i]);
                tileGrid->addWidget(overviewTiles[i], i / columns, i % columns);
            }
            for (int i = 0; i < 4; ++i) tileGrid->setColumnStretch(i, i < columns ? 1 : 0);
        }
        const bool stacked = contentWidth < 760;
        if (overviewColumns && overviewCards.size() == 3 && stacked != overviewStacked) {
            overviewStacked = stacked;
            for (auto *frame : overviewCards) overviewColumns->removeWidget(frame);
            overviewColumns->addWidget(overviewCards[0], 0, 0, stacked ? 1 : 2, 1);
            overviewColumns->addWidget(overviewCards[1], stacked ? 1 : 0, stacked ? 0 : 1);
            overviewColumns->addWidget(overviewCards[2], stacked ? 2 : 1, stacked ? 0 : 1);
            overviewColumns->setColumnStretch(1, stacked ? 0 : 1);
        }
    }
    void applyCustomPalette(const QPalette &theme) {
        for (auto *chart : charts) { chart->setPalette(theme); chart->update(); }
        cpuBreakdown->setPalette(theme); memoryBreakdown->setPalette(theme);
        batteryGlyph->setPalette(theme); batteryGlyph->update();
        diagnostics->setPalette(theme);
        for (auto *chart : diagnostics->findChildren<HistoryChart *>()) { chart->setPalette(theme); chart->update(); }
    }
    QLabel *transferRow(QVBoxLayout *card, const QString &key, const QString &caption);
    void buildOverview(QVBoxLayout *page);
    void buildCpu(QVBoxLayout *page);
    void buildMemory(QVBoxLayout *page);
    void buildDisk(QVBoxLayout *page);
    void buildNetwork(QVBoxLayout *page);
    void buildBattery(QVBoxLayout *page);
    void buildProjects(QVBoxLayout *page);
    QWidget *buildSidebar(const QPalette &theme);
    QWidget *buildHeader(const QPalette &theme);
};

void MonitorWindow::Impl::buildOverview(QVBoxLayout *page) {
    healthBanner = new QFrame;
    healthBanner->setObjectName("healthBanner");
    healthBanner->setProperty("card", true);
    auto *banner = new QHBoxLayout(healthBanner);
    banner->setContentsMargins(16, 12, 14, 12); banner->setSpacing(12);
    healthPill = DevStatsUI::pill(T("采样中"), {}, "healthState");
    banner->addWidget(healthPill, 0, Qt::AlignTop);
    auto *words = new QVBoxLayout; words->setSpacing(2);
    healthHeadline = label(T("正在收集数据"), "headline", "healthHeadline");
    healthSummary = label({}, "caption", "healthSummary");
    words->addWidget(healthHeadline); words->addWidget(healthSummary);
    banner->addLayout(words, 1);
    auto *diagnose = new QPushButton(T("一键诊断"));
    diagnose->setObjectName("overviewDiagnose");
    diagnose->setProperty("primary", true);
    banner->addWidget(diagnose, 0, Qt::AlignVCenter);
    connect(diagnose, &QPushButton::clicked, q, [this] { navigation->setCurrentRow(rowFor("diagnostics")); emit q->diagnosticRunRequested(true); });
    page->addWidget(healthBanner);

    auto *tileHost = new QWidget;
    tileGrid = new QGridLayout(tileHost);
    tileGrid->setContentsMargins(0, 0, 0, 0); tileGrid->setSpacing(10);
    const QList<QPair<QString, HistoryChart::Unit>> kinds{{"cpu", HistoryChart::Unit::Percent}, {"memory", HistoryChart::Unit::Percent},
        {"disk", HistoryChart::Unit::Percent}, {"network", HistoryChart::Unit::BytesPerSecond}};
    for (const auto &kind : kinds) {
        auto *tile = new MeterTile("overview_" + kind.first);
        tile->setClickable(true);
        const QString metric = kind.first == "disk" ? QStringLiteral("diskBusy") : kind.first == "network" ? QStringLiteral("networkDownload") : kind.first;
        auto *spark = new HistoryChart(kind.second);
        spark->setObjectName("overview_" + kind.first + "Spark");
        spark->setCompact(true);
        charts.insert(metric, spark);
        tile->addContent(spark);
        tiles[kind.first] = tile;
        overviewTiles.append(tile);
        const QString module = kind.first;
        connect(tile, &MeterTile::clicked, q, [this, module] { navigation->setCurrentRow(rowFor(module)); });
    }
    layoutTiles(900);
    page->addWidget(tileHost);

    auto *columnsHost = new QWidget;
    overviewColumns = new QGridLayout(columnsHost);
    overviewColumns->setContentsMargins(0, 0, 0, 0); overviewColumns->setSpacing(10);
    // The cards are placed in a grid below, so this layout only hands them over.
    QVBoxLayout hidden;
    auto *processes = card(&hidden, T("最忙的进程"), T("含系统进程。右键可结束或加入自动查杀。"));
    processes->addWidget(processTable("overviewProcesses", 188));
    auto *incidentsCard = new QPushButton(T("全部记录"));
    incidentsCard->setObjectName("overviewAllIncidents");
    connect(incidentsCard, &QPushButton::clicked, q, [this] { q->showIncidents(); });
    auto *recent = card(&hidden, T("最近的卡顿"), {}, incidentsCard);
    auto *recentTable = DevStatsUI::table("overviewIncidents", {T("时间"), T("原因"), T("持续")});
    recentTable->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    recentTable->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    recentTable->headerItem()->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
    recentTable->setMinimumHeight(118);
    tables["overviewIncidents"] = recentTable;
    recent->addWidget(recentTable);
    auto *rulesOpen = new QPushButton(T("管理规则"));
    rulesOpen->setObjectName("overviewAutomationOpen");
    connect(rulesOpen, &QPushButton::clicked, q, [this] { navigation->setCurrentRow(rowFor("automation")); });
    auto *automation = card(&hidden, T("自动查杀"), {}, rulesOpen);
    auto *automationState = label(T("尚未设置规则"), "caption", "overviewAutomation");
    values.insert("overviewAutomation", automationState);
    automation->addWidget(automationState);
    auto *automationLog = new QTreeWidget;
    automationLog->setObjectName("overviewAutomationLog");
    automationLog->setHeaderHidden(true); automationLog->setRootIsDecorated(false); automationLog->setFrameShape(QFrame::NoFrame);
    automationLog->setSelectionMode(QAbstractItemView::NoSelection); automationLog->setFocusPolicy(Qt::NoFocus);
    automationLog->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    automationLog->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    automationLog->setMaximumHeight(84); automationLog->hide();
    tables["overviewAutomationLog"] = automationLog;
    automation->addWidget(automationLog);
    for (auto *layout : {processes, recent, automation}) overviewCards.append(qobject_cast<QFrame *>(layout->parentWidget()));
    for (auto *frame : overviewCards) hidden.removeWidget(frame);
    overviewColumns->addWidget(overviewCards[0], 0, 0, 2, 1);
    overviewColumns->addWidget(overviewCards[1], 0, 1);
    overviewColumns->addWidget(overviewCards[2], 1, 1);
    overviewColumns->setColumnStretch(0, 1); overviewColumns->setColumnStretch(1, 1);
    page->addWidget(columnsHost);
}

void MonitorWindow::Impl::buildCpu(QVBoxLayout *page) {
    auto *summary = card(page, T("处理器使用率"));
    auto *figures = new FlowLayout(24, 10);
    stat(figures, "cpuUsage", T("当前"), "number");
    stat(figures, "cpuUser", T("用户"));
    stat(figures, "cpuSystem", T("系统"));
    stat(figures, "cpuIowait", T("I/O 等待"));
    stat(figures, "cpuLoad", T("负载 1 / 5 / 15 分"));
    stat(figures, "cpuFrequency", T("频率"));
    stat(figures, "cpuTemperature", T("温度"));
    stat(figures, "cpuUptime", T("开机"));
    summary->addLayout(figures);
    chart(summary, "cpu", HistoryChart::Unit::Percent);
    cpuBreakdown = new ResourceBreakdown;
    cpuBreakdown->setObjectName("cpuBreakdown");
    summary->addWidget(cpuBreakdown);
    auto *coreCard = card(page, T("逻辑核心"));
    coreGrid = new QGridLayout; coreGrid->setHorizontalSpacing(10); coreGrid->setVerticalSpacing(6);
    coreCard->addLayout(coreGrid);
    auto *processes = card(page, T("CPU 占用最高的进程"), T("含系统进程 · 100% 为占满一个核心 · 右键可结束或加入自动查杀"));
    processes->addWidget(processTable("cpuProcesses", 280));
}

void MonitorWindow::Impl::buildMemory(QVBoxLayout *page) {
    auto *relief = new QPushButton(T("内存减压…"));
    relief->setObjectName("memoryReliefFromMemory");
    connect(relief, &QPushButton::clicked, q, &MonitorWindow::memoryReliefRequested);
    auto *summary = card(page, T("物理内存"), {}, relief);
    auto *figures = new FlowLayout(24, 10);
    stat(figures, "memoryUsed", T("已使用"), "number");
    stat(figures, "memoryAvailable", T("可用"), "number");
    stat(figures, "memoryTotal", T("总计"));
    stat(figures, "memoryCached", T("页缓存"));
    stat(figures, "memoryBuffers", T("缓冲"));
    stat(figures, "memorySwap", T("交换分区"));
    summary->addLayout(figures);
    chart(summary, "memory", HistoryChart::Unit::Percent);
    memoryBreakdown = new ResourceBreakdown;
    memoryBreakdown->setObjectName("memoryBreakdown");
    summary->addWidget(memoryBreakdown);
    auto *processes = card(page, T("内存占用最高的进程"), T("含系统进程 · 内存为 RSS，可能包含共享页 · 右键可结束或加入自动查杀"));
    processes->addWidget(processTable("memoryProcesses", 280));
}

QLabel *MonitorWindow::Impl::transferRow(QVBoxLayout *card, const QString &key, const QString &caption) {
    auto *row = new QHBoxLayout; row->setSpacing(16);
    auto *summary = new QWidget; summary->setFixedWidth(150);
    auto *column = new QVBoxLayout(summary); column->setContentsMargins(0, 0, 0, 0);
    auto *value = stat(column, key, caption, "metric");
    column->addStretch();
    row->addWidget(summary);
    auto *plot = new QVBoxLayout; plot->setContentsMargins(0, 0, 0, 0);
    chart(plot, key, HistoryChart::Unit::BytesPerSecond);
    row->addLayout(plot, 1);
    card->addLayout(row);
    return value;
}

void MonitorWindow::Impl::buildDisk(QVBoxLayout *page) {
    auto *summary = card(page, T("磁盘读写"));
    transferRow(summary, "diskRead", T("读取"));
    transferRow(summary, "diskWrite", T("写入"));
    auto *busiest = card(page, T("读写最多的进程"),
        T("按读写量排列当前用户的进程。系统进程的读写量需要 root 权限才能读取，能统计时按等待磁盘的时间列在后面。右键可结束或加入自动查杀。"));
    auto *diskProcesses = processTable("diskProcesses", 0, {T("进程"), T("用户"), "PID", T("读取"),
                                                              T("写入"), T("等待磁盘")});
    busiest->addWidget(diskProcesses);
    auto *devices = card(page, T("磁盘设备"), T("繁忙度和平均等待时间比吞吐量更能反映卡顿。"));
    auto *deviceTable = DevStatsUI::table("diskDevices", {T("设备"), T("读取"), T("写入"), T("繁忙"), T("平均等待")});
    tables["devices"] = deviceTable;
    devices->addWidget(deviceTable);
    auto *volumes = card(page, T("本地卷"));
    auto *volumeTable = DevStatsUI::table("diskVolumes", {T("挂载点"), T("文件系统"), T("已用"), T("可用"), T("总计")});
    volumeTable->headerItem()->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
    tables["volumes"] = volumeTable;
    volumes->addWidget(volumeTable);
}

void MonitorWindow::Impl::buildNetwork(QVBoxLayout *page) {
    // Upload is deliberately above download, everywhere in this application.
    auto *transfers = card(page, T("网络传输"));
    transferRow(transfers, "networkUpload", T("↑ 上传"));
    transferRow(transfers, "networkDownload", T("↓ 下载"));
    networkActivity = new NetworkActivityPanel;
    page->addWidget(networkActivity);
    auto *interfaceCard = card(page, T("当前网络接口"));
    auto *first = new QHBoxLayout; first->setSpacing(24);
    stat(first, "networkInterface", T("默认路由接口"));
    stat(first, "networkTx", T("累计上传"));
    stat(first, "networkRx", T("累计下载"));
    first->addStretch();
    interfaceCard->addLayout(first);
    auto *second = new QHBoxLayout; second->setSpacing(24);
    stat(second, "networkIPv4", "IPv4");
    stat(second, "networkIPv6", "IPv6");
    second->addStretch();
    interfaceCard->addLayout(second);
}

void MonitorWindow::Impl::buildBattery(QVBoxLayout *page) {
    auto *summary = card(page, T("电池与电源"));
    auto *figures = new FlowLayout(24, 10);
    batteryGlyph = new BatteryIndicator;
    figures->addWidget(batteryGlyph);
    stat(figures, "batteryLevel", T("当前电量"), "number");
    stat(figures, "batteryState", T("电源状态"));
    stat(figures, "batteryHealth", T("电池健康"));
    stat(figures, "batteryCycles", T("循环次数"));
    summary->addLayout(figures);
    auto *history = chart(summary, "battery", HistoryChart::Unit::Percent);
    history->setMaximumHeight(150);
    auto *impact = card(page, T("应用耗电影响"), T("按应用合并多个进程。CPU 和磁盘活动是估算线索，不代表瓦数；GPU、屏幕和外设没有归因。"));
    auto *note = label(T("正在积累观测数据…"), "muted", "batteryImpactNote");
    values["batteryImpactNote"] = note;
    impact->addWidget(note);
    auto *tree = DevStatsUI::table("batteryImpactTable", {T("应用 / 进程"), T("进程数 / PID"), T("平均 CPU"), T("读取"), T("写入")});
    tree->setRootIsDecorated(true);
    tree->setMinimumHeight(280);
    tables["batteryImpact"] = tree;
    impact->addWidget(tree);
}

void MonitorWindow::Impl::buildProjects(QVBoxLayout *page) {
    auto *projects = card(page, T("项目与关联进程"), T("按工作目录和父子进程关系自动归组。移除目录只是取消手动归组，不会删除文件。"));
    auto *tools = new QHBoxLayout;
    projectFilter = new QLineEdit;
    projectFilter->setObjectName("projectFilter");
    projectFilter->setPlaceholderText(T("筛选项目、路径或进程 · Ctrl+F"));
    tools->addWidget(projectFilter, 1);
    auto *add = new QPushButton(T("添加目录")); add->setObjectName("addProject");
    auto *remove = new QPushButton(T("移除目录")); remove->setObjectName("removeProject");
    tools->addWidget(add); tools->addWidget(remove);
    projects->addLayout(tools);
    projectTree = DevStatsUI::table("projectTree", {T("项目 / 进程"), "CPU", T("内存"), T("进程数 / PID")});
    projectTree->setRootIsDecorated(true);
    projectTree->setMinimumHeight(420);
    projects->addWidget(projectTree);
    connect(projectFilter, &QLineEdit::textChanged, q, [this] { renderProjects(); });
    connect(add, &QPushButton::clicked, q, &MonitorWindow::addProjectRequested);
    connect(remove, &QPushButton::clicked, q, [this] {
        if (auto *item = projectTree->currentItem()) {
            const QString path = item->data(0, Qt::UserRole).toString();
            if (manualProjects.contains(path)) emit q->removeProjectRequested(path);
        }
    });
}

QWidget *MonitorWindow::Impl::buildSidebar(const QPalette &theme) {
    auto *sidebar = new QWidget;
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(186);
    auto *layout = new QVBoxLayout(sidebar);
    layout->setContentsMargins(10, 12, 10, 10); layout->setSpacing(6);
    auto *brand = new QHBoxLayout; brand->setContentsMargins(6, 2, 0, 6); brand->setSpacing(8);
    auto *mark = new QLabel; mark->setObjectName("brandMark");
    mark->setPixmap(quickIcon("brand", theme, DevStatsUI::colors(theme).accent).pixmap(24, 24));
    brand->addWidget(mark);
    auto *name = label("DevStats", "brand"); name->setWordWrap(false);
    brand->addWidget(name); brand->addStretch();
    layout->addLayout(brand);

    navigation = new QListWidget;
    navigation->setObjectName("navigation");
    navigation->setIconSize(QSize(18, 18));
    navigation->setItemDelegate(new NavigationDelegate(navigation));
    navigation->setMouseTracking(true);
    navigation->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navigation->setFrameShape(QFrame::NoFrame);
    const QList<QPair<QString, QStringList>> groups{
        {T("监控"), {"overview", "cpu", "memory", "disk", "network", "battery"}},
        {T("排查"), {"diagnostics", "automation"}},
        {T("开发"), {"projects"}}};
    for (const auto &group : groups) {
        auto *section = new QListWidgetItem(group.first, navigation);
        section->setFlags(Qt::NoItemFlags);
        for (const auto &module : group.second) {
            const int index = Modules.indexOf(module);
            auto *item = new QListWidgetItem(moduleIcon(module, theme), titles()[index], navigation);
            item->setData(ModuleRole, module);
            item->setToolTip(subtitles()[index] + QStringLiteral(" · Ctrl+%1").arg(index + 1));
            auto *shortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(index + 1)), q);
            shortcut->setObjectName(QStringLiteral("moduleShortcut%1").arg(index + 1));
            connect(shortcut, &QShortcut::activated, q, [this, module] { navigation->setCurrentRow(rowFor(module)); });
        }
    }
    layout->addWidget(navigation, 1);
    host = label(T("本机监控"), "muted", "hostSummary");
    layout->addWidget(host);
    layout->addWidget(DevStatsUI::separator());
    auto *launchers = new QHBoxLayout;
    launchers->setObjectName("quickLaunch"); launchers->setSpacing(2); launchers->setContentsMargins(0, 2, 0, 0);
    for (const auto &entry : QList<QPair<QString, QString>>{{"terminal", T("终端")}, {"monitor", T("系统监视器")}, {"files", T("文件")}, {"settings", T("系统设置")}}) {
        auto *button = new QToolButton;
        button->setObjectName("launch_" + entry.first);
        button->setProperty("quick", true);
        button->setIcon(quickIcon(entry.first, theme)); button->setIconSize(QSize(18, 18)); button->setFixedSize(28, 28);
        button->setToolTip(T("打开%1").arg(entry.second)); button->setAccessibleName(T("打开%1").arg(entry.second));
        launchers->addWidget(button);
        const QString id = entry.first;
        connect(button, &QToolButton::clicked, q, [this, id] { emit q->quickLaunchRequested(id); });
    }
    auto *addQuick = new QToolButton;
    addQuick->setObjectName("addQuickApplication"); addQuick->setProperty("quick", true);
    addQuick->setIcon(quickIcon("add", theme)); addQuick->setIconSize(QSize(18, 18)); addQuick->setFixedSize(28, 28);
    addQuick->setToolTip(T("添加快捷应用")); addQuick->setAccessibleName(T("添加快捷应用"));
    launchers->addWidget(addQuick);
    connect(addQuick, &QToolButton::clicked, q, [this] { emit q->quickLaunchRequested("add"); });
    customLaunches = new QHBoxLayout; customLaunches->setSpacing(2);
    launchers->addLayout(customLaunches);
    launchers->addStretch();
    layout->addLayout(launchers);
    return sidebar;
}

QWidget *MonitorWindow::Impl::buildHeader(const QPalette &theme) {
    auto *headerBar = new QWidget;
    headerBar->setObjectName("headerBar");
    auto *header = new QHBoxLayout(headerBar);
    header->setContentsMargins(20, 12, 16, 4); header->setSpacing(10);
    auto *headings = new QVBoxLayout; headings->setSpacing(0);
    title = label(titles().first(), "title", "moduleTitle"); title->setWordWrap(false);
    subtitle = label(subtitles().first(), "caption", "moduleSubtitle"); subtitle->setWordWrap(false);
    headings->addWidget(title); headings->addWidget(subtitle);
    header->addLayout(headings, 1);
    updated = label(T("等待首次采样"), "muted", "sampleStatus"); updated->setWordWrap(false);
    header->addWidget(updated);
    range = new QComboBox;
    range->setObjectName("historyRange");
    const QList<qint64> durations = {120, 600, 3600, 21600, 86400, 604800, 2592000};
    const QStringList durationLabels = {T("2 分钟"), T("10 分钟"), T("1 小时"), T("6 小时"), T("24 小时"), T("7 天"), T("30 天")};
    for (int i = 0; i < durations.size(); ++i) range->addItem(durationLabels[i], durations[i]);
    range->setCurrentIndex(1);
    range->setToolTip(T("历史曲线的时间范围")); range->setAccessibleName(T("历史范围"));
    header->addWidget(range);
    auto *settingsButton = new QToolButton;
    settingsButton->setObjectName("settingsButton");
    settingsButton->setProperty("ghost", true);
    settingsButton->setIcon(quickIcon("settings", theme)); settingsButton->setIconSize(QSize(20, 20)); settingsButton->setFixedSize(30, 30);
    settingsButton->setToolTip(T("设置 · Ctrl+,")); settingsButton->setAccessibleName(T("设置"));
    header->addWidget(settingsButton);
    connect(settingsButton, &QToolButton::clicked, q, &MonitorWindow::showSettings);
    connect(range, QOverload<int>::of(&QComboBox::currentIndexChanged), q, [this] { emit q->durationChanged(q->historyDuration()); });
    return headerBar;
}

MonitorWindow::MonitorWindow(int appearance, QWidget *parent) : QMainWindow(parent), d(new Impl(this)) {
    setObjectName("monitorWindow");
    setWindowTitle(QStringLiteral("DevStats"));
    setAttribute(Qt::WA_DeleteOnClose);
    setMinimumSize(780, 560);
    const QRect available = QGuiApplication::primaryScreen() ? QGuiApplication::primaryScreen()->availableGeometry() : QRect(0, 0, 1024, 800);
    resize(qMin(1100, qMax(780, available.width() - 40)), qMin(780, qMax(560, available.height() - 60)));
    // Only for the icons drawn while building; the real theme is applied at the end.
    const QPalette theme = DevStatsUI::palette(true);

    auto *central = new QWidget; central->setObjectName("shell"); setCentralWidget(central);
    auto *shell = new QHBoxLayout(central); shell->setContentsMargins(0, 0, 0, 0); shell->setSpacing(0);
    shell->addWidget(d->buildSidebar(theme));
    auto *content = new QVBoxLayout; content->setContentsMargins(0, 0, 0, 0); content->setSpacing(0);
    content->addWidget(d->buildHeader(theme));
    d->pages = new QStackedWidget; d->pages->setObjectName("modulePages");
    content->addWidget(d->pages, 1);
    shell->addLayout(content, 1);

    QVector<QVBoxLayout *> pages;
    for (const auto &module : Modules) {
        auto *area = new QScrollArea;
        area->setWidgetResizable(true);
        area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        area->setFrameShape(QFrame::NoFrame);
        area->viewport()->setAutoFillBackground(false);
        auto *body = new QWidget; body->setObjectName(module + "Page"); body->setProperty("pageBody", true);
        auto *layout = new QVBoxLayout(body); layout->setContentsMargins(20, 8, 20, 16); layout->setSpacing(10);
        area->setWidget(body); d->pages->addWidget(area); pages.append(layout);
    }
    d->buildOverview(pages[0]);
    d->buildCpu(pages[1]);
    d->buildMemory(pages[2]);
    d->buildDisk(pages[3]);
    d->buildNetwork(pages[4]);
    d->buildBattery(pages[5]);
    d->diagnostics = new DiagnosticsPanel; pages[6]->addWidget(d->diagnostics);
    connect(d->diagnostics, &DiagnosticsPanel::diagnosticRunRequested, this, &MonitorWindow::diagnosticRunRequested);
    connect(d->diagnostics, &DiagnosticsPanel::processActionRequested, this, &MonitorWindow::processActionRequested);
    connect(d->diagnostics, &DiagnosticsPanel::exportReportRequested, this, &MonitorWindow::diagnosticExportRequested);
    connect(d->diagnostics, &DiagnosticsPanel::incidentRequested, this, &MonitorWindow::incidentRequested);
    connect(d->diagnostics, &DiagnosticsPanel::incidentRecordingRequested, this, &MonitorWindow::incidentRecordingRequested);
    connect(d->diagnostics, &DiagnosticsPanel::exportIncidentRequested, this, &MonitorWindow::exportIncidentRequested);
    connect(d->diagnostics, &DiagnosticsPanel::memoryReliefRequested, this, &MonitorWindow::memoryReliefRequested);
    connect(d->diagnostics, &DiagnosticsPanel::addRuleRequested, this, [this](const QJsonObject &process) {
        d->navigation->setCurrentRow(d->rowFor("automation"));
        d->rules->addRuleFor(process);
    });
    d->rules = new ProcessRulesPanel; pages[7]->addWidget(d->rules);
    d->buildProjects(pages[8]);
    for (auto *page : pages) page->addStretch();

    connect(d->navigation, &QListWidget::currentRowChanged, this, [this](int row) {
        const QString module = row >= 0 ? d->navigation->item(row)->data(ModuleRole).toString() : QString();
        const int index = Modules.indexOf(module);
        if (index < 0) return;
        d->pages->setCurrentIndex(index);
        d->title->setText(titles()[index]);
        d->subtitle->setText(subtitles()[index]);
        d->range->setVisible(HistoryModules.contains(module));
        renderCurrentSnapshot();
        renderCurrentHistories();
        emit selectionChanged(module);
        publishNetworkObservation();
    });
    d->navigation->setCurrentRow(d->rowFor("overview"));
    auto *preferencesShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+,")), this);
    connect(preferencesShortcut, &QShortcut::activated, this, &MonitorWindow::showSettings);
    auto *searchShortcut = new QShortcut(QKeySequence::Find, this);
    searchShortcut->setObjectName("projectSearchShortcut");
    connect(searchShortcut, &QShortcut::activated, this, [this] {
        d->navigation->setCurrentRow(d->rowFor("projects"));
        d->projectFilter->setFocus();
        d->projectFilter->selectAll();
    });
    // The theme goes on last, once. A panel built on its own and then attached under a
    // style sheet keeps the palette it was first styled with, so switching from dark
    // to light before the first show left those cards dark.
    setAppearance(appearance);
}

MonitorWindow::~MonitorWindow() = default;

ProcessRulesPanel *MonitorWindow::rulesPanel() const { return d->rules; }

void MonitorWindow::setSnapshot(const QJsonObject &snapshot) {
    if (d->snapshot == snapshot) return;
    d->snapshot = snapshot;
    const qint64 timestamp = qint64(snapshot.value("timestamp").toDouble());
    d->updated->setText(timestamp > 0 ? T("%1 更新").arg(QDateTime::fromSecsSinceEpoch(timestamp).toString("hh:mm:ss")) : T("等待首次采样"));
    // Badges are visible from every page, so they follow every sample.
    const QString state = snapshot.value("diagnostics").toObject().value("state").toString();
    auto *diagnosticsItem = d->navigation->item(d->rowFor("diagnostics"));
    const QString diagnosticBadge = state == "critical" || state == "attention" ? QStringLiteral("!") : QString();
    if (diagnosticsItem->data(BadgeRole).toString() != diagnosticBadge || diagnosticsItem->data(BadgeToneRole).toString() != stateTone(state)) {
        diagnosticsItem->setData(BadgeRole, diagnosticBadge);
        diagnosticsItem->setData(BadgeToneRole, stateTone(state));
    }
    const auto automation = snapshot.value("processAutomation").toObject();
    int active = 0;
    for (const auto &value : automation.value("rules").toArray()) if (value.toObject().value("enabled").toBool()) ++active;
    auto *automationItem = d->navigation->item(d->rowFor("automation"));
    const QString rulesBadge = automation.value("enabled").toBool() && active ? QString::number(active) : QString();
    if (automationItem->data(BadgeRole).toString() != rulesBadge) {
        automationItem->setData(BadgeRole, rulesBadge);
        automationItem->setData(BadgeToneRole, QStringLiteral("info"));
    }
    renderCurrentSnapshot();
}

void MonitorWindow::renderCurrentSnapshot() {
    if (d->snapshot.isEmpty()) return;
    const auto &snapshot = d->snapshot;
    const QString module = selectedModule();
    const QString host = T("%1 核 · %2").arg(number(snapshot.value("cpu").toObject().value("coreCount")), bytes(snapshot.value("memory").toObject().value("totalBytes")));
    if (d->host->text() != host) d->host->setText(host);
    static const QHash<QString, QStringList> fields{
        {"overview", {"cpu", "memory", "network", "diskIO", "diagnostics", "processes", "systemProcesses", "processAutomation"}},
        {"cpu", {"cpu", "processes", "systemProcesses", "thermal"}}, {"memory", {"memory", "processes", "systemProcesses"}},
        {"disk", {"diskIO", "disks"}}, {"network", {"network", "networkApplications", "networkApplicationsPrevious"}},
        {"battery", {"battery", "activityApplications", "activityObservationSeconds"}},
        {"automation", {"processAutomation", "processes"}}, {"projects", {"groups"}}};
    QJsonObject input;
    if (module == "diagnostics") input = snapshot;
    else for (const auto &key : fields.value(module)) input.insert(key, snapshot.value(key));
    if (module == "projects") input["manualProjects"] = QJsonArray::fromStringList(d->manualProjects);
    if (d->pageInputs.contains(module) && d->pageInputs.value(module) == input) return;
    d->pageInputs[module] = input;
    const auto cpu = snapshot.value("cpu").toObject(), memory = snapshot.value("memory").toObject();
    const auto network = snapshot.value("network").toObject(), battery = snapshot.value("battery").toObject();
    const auto io = snapshot.value("diskIO").toObject();
    if (module == "overview") d->renderOverview();
    if (module == "cpu") {
        const double usage = numeric(cpu.value("usage")), user = numeric(cpu.value("user")), system = numeric(cpu.value("system"));
        if (usage >= 0 && usage <= 1) {
            QVector<ResourceBreakdown::Segment> parts;
            if (user >= 0 && system >= 0 && user + system <= usage + .002) {
                const double u = std::min(user, usage), s = std::min(system, usage - u);
                parts = {{T("用户"), u, {}}, {T("系统"), s, {}}};
                if (usage - u - s > .00001) parts.append({T("其他"), usage - u - s, {}});
            } else parts.append({T("已使用"), usage, {}});
            parts.append({T("空闲"), 1 - usage, {}});
            d->cpuBreakdown->setSegments(parts);
        } else d->cpuBreakdown->setSegments({});
        d->assign("cpuUsage", percent(cpu.value("usage"))); d->assign("cpuUser", percent(cpu.value("user"))); d->assign("cpuSystem", percent(cpu.value("system")));
        d->assign("cpuIowait", percent(cpu.value("iowait")));
        QStringList loads; for (const auto &load : cpu.value("load").toArray()) loads.append(number(load, 2));
        d->assign("cpuLoad", loads.isEmpty() ? unavailable() : loads.join(" / "));
        d->assign("cpuUptime", duration(numeric(cpu.value("uptimeSeconds"))));
        const auto thermal = snapshot.value("thermal").toObject();
        d->assign("cpuTemperature", thermal.value("hottestCelsius").isDouble() ? QString::number(thermal.value("hottestCelsius").toDouble(), 'f', 0) + QStringLiteral(" °C") : unavailable());
        QStringList frequencies;
        for (const auto &value : thermal.value("clusters").toArray()) frequencies.append(QString::number(value.toObject().value("currentMHz").toDouble() / 1000, 'f', 1));
        d->assign("cpuFrequency", frequencies.isEmpty() ? unavailable() : frequencies.join(" / ") + " GHz");
        const auto coreValues = cpu.value("perCore").toArray();
        while (d->cores.size() < coreValues.size()) {
            const int index = d->cores.size();
            auto *bar = new QProgressBar;
            bar->setRange(0, 100); bar->setTextVisible(true); bar->setMinimumWidth(120); bar->setFixedHeight(22);
            bar->setObjectName("cpuCore" + QString::number(index));
            d->coreGrid->addWidget(bar, index / d->coreColumns, index % d->coreColumns);
            d->cores.append(bar);
        }
        for (int i = 0; i < d->cores.size(); ++i) {
            const bool exists = i < coreValues.size();
            d->cores[i]->setVisible(exists);
            if (exists) {
                d->cores[i]->setValue(int(qMax(0.0, numeric(coreValues[i])) * 100));
                d->cores[i]->setFormat(T("核心 %1   %2").arg(i + 1).arg(percent(coreValues[i])));
            }
        }
        d->processRows("cpuProcesses", false, 10);
    }
    if (module == "memory") {
        const double total = numeric(memory.value("totalBytes")), used = numeric(memory.value("usedBytes")), availableBytes = numeric(memory.value("availableBytes"));
        if (total > 0 && used >= 0 && availableBytes >= 0 && std::abs(used + availableBytes - total) <= std::max(1., total * .002))
            d->memoryBreakdown->setSegments({{T("已用"), used / total, formatBytes(used) + " · " + formatPercent(used / total)},
                {T("可用"), availableBytes / total, formatBytes(availableBytes) + " · " + formatPercent(availableBytes / total)}});
        else d->memoryBreakdown->setSegments({});
        for (const auto &pair : QList<QPair<QString, QString>>{{"memoryUsed", "usedBytes"}, {"memoryAvailable", "availableBytes"}, {"memoryTotal", "totalBytes"}, {"memoryCached", "cachedBytes"}, {"memoryBuffers", "buffersBytes"}})
            d->assign(pair.first, bytes(memory.value(pair.second)));
        d->assign("memorySwap", bytes(memory.value("swapUsedBytes")) + " / " + bytes(memory.value("swapTotalBytes")));
        d->processRows("memoryProcesses", true, 10);
    }
    if (module == "disk") {
        d->assign("diskRead", bytes(io.value("readBytesPerSecond"), true)); d->assign("diskWrite", bytes(io.value("writeBytesPerSecond"), true));
        d->diskProcessRows();
        d->tables["volumes"]->clear();
        for (const auto &entry : snapshot.value("disks").toArray()) {
            const auto volume = entry.toObject();
            row(d->tables["volumes"], {volume.value("mountPoint").toString(), volume.value("filesystem").toString(), percent(volume.value("usageFraction")), bytes(volume.value("availableBytes")), bytes(volume.value("totalBytes"))});
            d->tables["volumes"]->topLevelItem(d->tables["volumes"]->topLevelItemCount() - 1)->setTextAlignment(1, Qt::AlignLeft | Qt::AlignVCenter);
        }
        if (!d->tables["volumes"]->topLevelItemCount()) row(d->tables["volumes"], {T("暂无可读取的本地卷"), unavailable(), unavailable(), unavailable(), unavailable()});
        DevStatsUI::fitRows(d->tables["volumes"], 1, 12);
        d->tables["devices"]->clear();
        for (const auto &entry : io.value("devices").toArray()) {
            const auto device = entry.toObject();
            const auto wait = device.value("awaitMilliseconds");
            row(d->tables["devices"], {device.value("device").toString(), bytes(device.value("readBytesPerSecond"), true), bytes(device.value("writeBytesPerSecond"), true),
                percent(device.value("utilization")), wait.isDouble() ? T("%1 毫秒").arg(QString::number(wait.toDouble(), 'f', 1)) : unavailable()});
        }
        if (!d->tables["devices"]->topLevelItemCount()) row(d->tables["devices"], {T("暂无可读取的磁盘设备"), unavailable(), unavailable(), unavailable(), unavailable()});
        DevStatsUI::fitRows(d->tables["devices"], 1, 8);
    }
    if (module == "network") {
        d->assign("networkUpload", bytes(network.value("uploadBytesPerSecond"), true));
        d->assign("networkDownload", bytes(network.value("downloadBytesPerSecond"), true));
        d->networkActivity->setObservation(snapshot.value("networkApplications").toObject(), snapshot.value("networkApplicationsPrevious").toObject());
        QString interface = network.value("interface").toString();
        if (interface.isEmpty()) interface = unavailable();
        if (network.value("isUp").isBool()) interface += network.value("isUp").toBool() ? T(" · 已连接") : T(" · 未连接");
        d->assign("networkInterface", interface);
        d->assign("networkIPv4", jsonStrings(network.value("ipv4").toArray())); d->assign("networkIPv6", jsonStrings(network.value("ipv6").toArray()));
        d->assign("networkTx", bytes(network.value("txBytes"))); d->assign("networkRx", bytes(network.value("rxBytes")));
    }
    if (module == "battery") {
        d->assign("batteryLevel", percent(battery.value("level"))); d->assign("batteryState", batteryState(battery));
        d->batteryGlyph->setState(numeric(battery.value("level")), battery.value("state").toString());
        d->assign("batteryHealth", battery.value("healthPercent").isDouble() ? number(battery.value("healthPercent"), 1) + "%" : unavailable());
        d->assign("batteryCycles", number(battery.value("cycleCount")));
        d->renderBatteryImpact();
    }
    if (module == "projects") d->renderProjects();
    if (module == "diagnostics") d->diagnostics->setSnapshot(snapshot);
    if (module == "automation") d->rules->setSnapshot(snapshot);
}

void MonitorWindow::setHistories(const HistoryFeed &feed) {
    d->histories = feed;
    renderCurrentHistories();
}

void MonitorWindow::renderCurrentHistories() {
    if (selectedModule() == "diagnostics") d->diagnostics->setHistories(d->histories);
    auto *page = d->pages->currentWidget();
    for (auto it = d->charts.cbegin(); it != d->charts.cend(); ++it)
        if (page->isAncestorOf(it.value())) it.value()->setHistory(d->histories, it.key());
}

QJsonObject MonitorWindow::diagnosticReport() const { return d->diagnostics->report(); }
void MonitorWindow::setDiagnosticFeedback(const QString &message) { d->diagnostics->setFeedback(message); }
void MonitorWindow::setIncidents(const QJsonArray &incidents) {
    d->incidents = incidents;
    d->diagnostics->setIncidents(incidents);
    if (selectedModule() == "overview") d->renderRecentIncidents();
}
void MonitorWindow::setIncidentDetail(const QJsonObject &incident) { d->diagnostics->setIncidentDetail(incident); }
void MonitorWindow::setIncidentRecording(bool enabled) { d->diagnostics->setIncidentRecording(enabled); }
void MonitorWindow::showIncidents() {
    d->navigation->setCurrentRow(d->rowFor("diagnostics"));
    d->diagnostics->showIncidents();
}
void MonitorWindow::setSelectedModule(const QString &module) {
    const int index = Modules.indexOf(module);
    if (index < 0) return;
    {
        QSignalBlocker blocker(d->navigation);
        d->navigation->setCurrentRow(d->rowFor(module));
    }
    d->pages->setCurrentIndex(index);
    d->title->setText(titles()[index]);
    d->subtitle->setText(subtitles()[index]);
    d->range->setVisible(HistoryModules.contains(module));
    renderCurrentSnapshot();
    renderCurrentHistories();
    publishNetworkObservation();
}
QString MonitorWindow::selectedModule() const {
    const auto *item = d->navigation->currentItem();
    const QString module = item ? item->data(ModuleRole).toString() : QString();
    return module.isEmpty() ? QStringLiteral("overview") : module;
}
qint64 MonitorWindow::historyDuration() const { return d->range->currentData().toLongLong(); }

void MonitorWindow::setPreferences(bool autoStart, int trayTheme, const QStringList &manualProjects, const QStringList &customApps, bool largeTrayText, bool automationNotifications, int language) {
    d->language = qBound(0, language, 2);
    d->largeTrayText = largeTrayText;
    d->automationNotifications = automationNotifications;
    if (d->largeTextCheck) { QSignalBlocker block(d->largeTextCheck); d->largeTextCheck->setChecked(largeTrayText); }
    if (d->notificationCheck) { QSignalBlocker block(d->notificationCheck); d->notificationCheck->setChecked(automationNotifications); }
    d->autoStart = autoStart; d->trayTheme = qBound(0, trayTheme, 2); d->manualProjects = manualProjects; d->customApps = customApps;
    if (d->autoStartCheck) { QSignalBlocker block(d->autoStartCheck); d->autoStartCheck->setChecked(autoStart); }
    if (d->themeCombo) { QSignalBlocker block(d->themeCombo); d->themeCombo->setCurrentIndex(d->trayTheme); }
    if (d->appsList) {
        d->appsList->clear();
        for (const auto &path : customApps) { auto *item = new QListWidgetItem(QFileInfo(path).completeBaseName(), d->appsList); item->setData(Qt::UserRole, path); item->setToolTip(DevStatsUI::literal(path)); }
    }
    while (auto *item = d->customLaunches->takeAt(0)) { delete item->widget(); delete item; }
    if (!customApps.isEmpty()) {
        auto *button = new QToolButton;
        button->setObjectName("customAppLauncher"); button->setProperty("quick", true);
        button->setIcon(quickIcon("apps", palette())); button->setIconSize(QSize(18, 18)); button->setFixedSize(28, 28);
        button->setToolTip(T("自定义快捷应用")); button->setAccessibleName(T("自定义快捷应用"));
        auto *menu = new QMenu(button);
        for (const auto &path : customApps) {
            auto *action = menu->addAction(QFileInfo(path).completeBaseName());
            action->setToolTip(DevStatsUI::literal(path));
            connect(action, &QAction::triggered, this, [this, path] { emit quickLaunchRequested("custom:" + path); });
        }
        button->setMenu(menu); button->setPopupMode(QToolButton::InstantPopup);
        d->customLaunches->addWidget(button);
    }
    if (selectedModule() == "projects") d->renderProjects();
}

void MonitorWindow::setAppearance(int mode) {
    if (property("appearanceMode").isValid() && property("appearanceMode").toInt() == mode && mode != 0) return;
    d->appearance = qBound(0, mode, 2);
    const bool dark = d->appearance == 1 || (d->appearance == 0 && DevStatsUI::isDark(qApp->palette()));
    const auto theme = DevStatsUI::palette(dark);
    setProperty("appearanceMode", d->appearance);
    setStyleSheet(QString());
    setPalette(theme);
    setStyleSheet(DevStatsUI::styleSheet(dark));
    if (d->settings) d->settings->setPalette(theme);
    if (d->appearanceCombo) { QSignalBlocker block(d->appearanceCombo); d->appearanceCombo->setCurrentIndex(d->appearance); }
    for (int i = 0; i < d->navigation->count(); ++i) {
        auto *item = d->navigation->item(i);
        const QString module = item->data(ModuleRole).toString();
        if (!module.isEmpty()) item->setIcon(moduleIcon(module, theme));
    }
    for (auto *button : findChildren<QToolButton *>()) {
        const auto name = button->objectName();
        if (name.startsWith("launch_")) button->setIcon(quickIcon(name.mid(7), theme));
        else if (name == "settingsButton") button->setIcon(quickIcon("settings", theme));
        else if (name == "addQuickApplication") button->setIcon(quickIcon("add", theme));
        else if (name == "customAppLauncher") button->setIcon(quickIcon("apps", theme));
    }
    if (auto *brand = findChild<QLabel *>("brandMark")) brand->setPixmap(quickIcon("brand", theme, DevStatsUI::colors(dark).accent).pixmap(24, 24));
    d->applyCustomPalette(theme);
    d->navigation->viewport()->update();
}

bool MonitorWindow::settingsVisible() const { return d->settings && d->settings->isVisible(); }

void MonitorWindow::showSettings() {
    if (d->settings) { d->settings->show(); d->settings->raise(); d->settings->activateWindow(); return; }
    auto *dialog = new QDialog(this);
    dialog->setObjectName("preferencesDialog");
    dialog->setWindowTitle(T("DevStats 设置"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->resize(480, 500);
    dialog->setPalette(palette());
    d->settings = dialog;
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(20, 18, 20, 16); layout->setSpacing(12);
    layout->addWidget(label(T("设置"), "title"));
    auto *form = new QFormLayout;
    form->setHorizontalSpacing(20); form->setVerticalSpacing(10);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    auto *appearance = new QComboBox;
    appearance->setObjectName("appearanceMode");
    appearance->addItems({T("跟随系统"), T("深色"), T("浅色")});
    appearance->setCurrentIndex(d->appearance);
    d->appearanceCombo = appearance;
    form->addRow(T("界面外观"), appearance);
    auto *language = new QComboBox;
    language->setObjectName("languageMode");
    // Each language is named in itself, so it can be found whichever one is showing.
    language->addItems({T("跟随系统"), QString::fromUtf8("中文"), QStringLiteral("English")}); // i18n: keep
    language->setCurrentIndex(d->language);
    form->addRow(T("语言"), language);
    auto *themes = new QComboBox;
    themes->setObjectName("trayTheme");
    themes->addItems({T("跟随系统"), T("浅色图标（深色面板）"), T("深色图标（浅色面板）")});
    themes->setCurrentIndex(d->trayTheme);
    d->themeCombo = themes;
    form->addRow(T("托盘图标"), themes);
    auto *largeText = new ToggleSwitch; largeText->setObjectName("trayLargeText");
    largeText->setChecked(d->largeTrayText);
    d->largeTextCheck = largeText;
    form->addRow(T("托盘使用大字（2K 屏幕）"), largeText);
    auto *autostart = new ToggleSwitch; autostart->setObjectName("autoStart");
    autostart->setChecked(d->autoStart);
    d->autoStartCheck = autostart;
    form->addRow(T("登录后启动 DevStats"), autostart);
    auto *notify = new ToggleSwitch; notify->setObjectName("automationNotifications");
    notify->setChecked(d->automationNotifications);
    d->notificationCheck = notify;
    form->addRow(T("自动查杀时显示通知"), notify);
    layout->addLayout(form);
    layout->addWidget(DevStatsUI::separator());
    layout->addWidget(label(T("快捷应用"), "strong"));
    auto *apps = new QListWidget; apps->setObjectName("customApps"); apps->setMinimumHeight(72);
    d->appsList = apps;
    layout->addWidget(apps, 1);
    for (const auto &path : d->customApps) { auto *item = new QListWidgetItem(QFileInfo(path).completeBaseName(), apps); item->setData(Qt::UserRole, path); item->setToolTip(DevStatsUI::literal(path)); }
    auto *tools = new QHBoxLayout;
    auto *add = new QPushButton(T("添加应用…"));
    auto *remove = new QPushButton(T("移除选中项"));
    tools->addWidget(add); tools->addWidget(remove); tools->addStretch();
    layout->addLayout(tools);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    buttons->button(QDialogButtonBox::Close)->setText(T("关闭"));
    // Which version is in use, beside the button that closes the dialog.
    auto *footer = new QHBoxLayout;
    auto *version = label(T("版本 %1").arg(QStringLiteral(DEVSTATS_VERSION)), "muted", "versionLabel");
    version->setWordWrap(false); version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    footer->addWidget(version); footer->addStretch(); footer->addWidget(buttons);
    layout->addLayout(footer);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    connect(largeText, &ToggleSwitch::toggled, this, [this](bool enabled) { d->largeTrayText = enabled; emit largeTrayTextRequested(enabled); });
    connect(notify, &ToggleSwitch::toggled, this, [this](bool enabled) { d->automationNotifications = enabled; emit automationNotificationsRequested(enabled); });
    connect(autostart, &ToggleSwitch::toggled, this, &MonitorWindow::autoStartRequested);
    connect(themes, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &MonitorWindow::trayThemeRequested);
    connect(appearance, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int mode) { setAppearance(mode); emit appearanceRequested(mode); });
    connect(language, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int mode) { d->language = mode; emit languageRequested(mode); });
    connect(add, &QPushButton::clicked, this, [this] { emit quickLaunchRequested("add"); });
    connect(remove, &QPushButton::clicked, this, [this, apps] { if (auto *item = apps->currentItem()) emit quickLaunchRequested("remove:" + item->data(Qt::UserRole).toString()); });
    dialog->show();
}

void MonitorWindow::publishNetworkObservation() {
    const bool active = d->visible && selectedModule() == "network";
    if (d->networkObserving != active) { d->networkObserving = active; emit networkObservationRequested(active); }
    // Suspects are shown on the diagnosis page only; other pages do not pay for the ranking.
    const bool ranking = d->visible && selectedModule() == "diagnostics";
    if (d->suspectRanking != ranking) { d->suspectRanking = ranking; emit suspectRankingRequested(ranking); }
}
void MonitorWindow::publishVisibility(bool visible) { if (d->visible != visible) { d->visible = visible; emit visibilityChanged(visible); } publishNetworkObservation(); }
void MonitorWindow::closeEvent(QCloseEvent *event) { publishVisibility(false); QMainWindow::closeEvent(event); }
void MonitorWindow::hideEvent(QHideEvent *event) { publishVisibility(false); QMainWindow::hideEvent(event); }
void MonitorWindow::showEvent(QShowEvent *event) { QMainWindow::showEvent(event); publishVisibility(!isMinimized()); }
void MonitorWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) publishVisibility(isVisible() && !isMinimized());
    if (event->type() == QEvent::ApplicationPaletteChange && d->appearance == 0) setAppearance(0);
}
void MonitorWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    d->layoutTiles(width() - 186 - 40);
    const int columns = width() >= 924 ? 4 : 2;
    if (d->coreGrid && columns != d->coreColumns) {
        d->coreColumns = columns;
        for (int i = 0; i < d->cores.size(); ++i) {
            d->coreGrid->removeWidget(d->cores[i]);
            d->coreGrid->addWidget(d->cores[i], i / columns, i % columns);
        }
    }
}
