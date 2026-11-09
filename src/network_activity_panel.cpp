#include "network_activity_panel.h"
#include "charts.h"
#include "ui_components.h"
#include <QComboBox>
#include <QDateTime>
#include <QHeaderView>
#include <QFontMetrics>
#include <QJsonArray>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QScrollBar>
#include <QSet>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVector>
#include <algorithm>
#include <cmath>

namespace {
double measured(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 ? value.toDouble() : -1;
}
QLabel *text(const QString &value, const QString &name, bool muted = false) {
    auto *label = new QLabel(value); label->setObjectName(name); label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    if (muted) label->setProperty("textRole", "muted");
    return label;
}
QString rate(const QJsonObject &row, const QString &key) {
    const double value = measured(row.value(key));
    return (value >= 0 && row.value("partial").toBool() ? QStringLiteral("≥ ") : QString()) + formatBytes(value, true);
}
QString processKey(const QJsonObject &row) {
    return QStringLiteral("pid:%1:%2").arg(row.value("pid").toDouble(), 0, 'f', 0).arg(row.value("startTicks").toDouble(), 0, 'f', 0);
}
}

struct NetworkActivityPanel::Impl {
    QJsonObject observation, previous;
    bool historical = false;
    QString displayState = "inactive";
    QLabel *status, *leader, *coverage, *count;
    QTreeWidget *tree;
    QComboBox *sort;
    QLineEdit *search;
    bool dirty = true;
    void filter() {
        const QString query = search->text().trimmed(); int visible = 0; QTreeWidgetItem *first = nullptr;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) {
            auto *app = tree->topLevelItem(i);
            const bool parentMatch = query.isEmpty() || app->text(0).contains(query, Qt::CaseInsensitive);
            bool childMatch = false;
            for (int j = 0; j < app->childCount(); ++j) {
                auto *child = app->child(j);
                const auto process = child->data(0, Qt::UserRole + 1).toJsonObject();
                const bool match = parentMatch || child->text(0).contains(query, Qt::CaseInsensitive) || QString::number(qint64(process.value("pid").toDouble())).contains(query);
                child->setHidden(!match); childMatch |= match;
            }
            const bool show = (parentMatch || childMatch) && visible < 30;
            app->setHidden(!show);
            if (show) { ++visible; if (!first) first = app; if (!parentMatch && childMatch) app->setExpanded(true); }
        }
        count->setText(QStringLiteral("显示 %1 / %2 个候选应用 · 展开查看 PID；共享连接不会重复计入应用").arg(visible).arg(tree->topLevelItemCount()));
        const QString state = displayState;
        const QString metric = sort->currentData().toString();
        const bool download = metric == "downloadBytesPerSecond";
        const auto best = first ? first->data(0, Qt::UserRole + 1).toJsonObject() : QJsonObject();
        if (!best.isEmpty() && measured(best.value(metric)) > 0) {
            QString value = rate(best, metric); value.replace(' ', QChar(0x00a0));
            leader->setText(QStringLiteral("%1%2最多：%3 · %4").arg(query.isEmpty() ? QString() : QStringLiteral("匹配应用中"), download ? "下载" : "上传", best.value("name").toString(), value));
        } else if (!query.isEmpty() && !first) leader->setText(QStringLiteral("没有匹配的应用或 PID"));
        else if (state == "starting") leader->setText(QStringLiteral("正在等待第一段完整采样…"));
        else if (state == "unavailable") leader->setText(QStringLiteral("应用流量暂不可用"));
        else if (!first) leader->setText(QStringLiteral("暂无可归属的 TCP 连接"));
        else if (measured(best.value(metric)) < 0) leader->setText(QStringLiteral("暂未取得可比较的 TCP %1速率").arg(download ? "下载" : "上传"));
        else leader->setText(QStringLiteral("本轮已测连接暂未见活跃%1").arg(download ? "下载" : "上传"));
        if (historical) leader->setText(QStringLiteral("上次采样 · ") + leader->text());
    }
    void render() {
        dirty = false;
        const QString state = observation.value("state").toString("inactive");
        historical = (state == "inactive" || state == "starting" || state == "unavailable") && previous.value("sampledAt").toDouble() > 0 &&
            (previous.value("state").toString() == "ready" || previous.value("state").toString() == "partial");
        const auto &display = historical ? previous : observation;
        displayState = display.value("state").toString("inactive");
        const QMap<QString, QString> states{{"inactive", "打开网络页后开始应用流量观察"}, {"starting", "正在建立速率基线，约 2 秒后显示排行…"},
            {"ready", "正在观察可归属的 TCP 流量"}, {"partial", "部分流量可统计，未覆盖的部分保持未知"}, {"unavailable", "当前环境无法取得应用 TCP 统计"}};
        QString description = states.value(state, states.value("unavailable"));
        const QString reason = observation.value("reason").toString();
        if (!reason.isEmpty() && reason != description) description += QStringLiteral(" · ") + reason;
        const double sampled = measured(display.value("sampledAt"));
        if (historical) description = QStringLiteral("上次采样 %1 · %2 · 历史值，非实时").arg(
            QDateTime::fromMSecsSinceEpoch(qint64(sampled * 1000)).toLocalTime().toString("yyyy-MM-dd HH:mm:ss"),
            state == "unavailable" ? QStringLiteral("当前不可用") : state == "inactive" ? QStringLiteral("观察已暂停") : QStringLiteral("正在更新"));
        else if (sampled > 0) description += QStringLiteral(" · %1 更新").arg(QDateTime::fromMSecsSinceEpoch(qint64(sampled * 1000)).toLocalTime().toString("HH:mm:ss"));
        status->setText(description);
        QStringList limits{QStringLiteral("仅包含当前用户、当前网络命名空间内可归属的 TCP 连接，TCP 上传为对端确认的字节。"),
            QStringLiteral("UDP/QUIC、未捕获到的短连接及退出连接不完整；代理/VPN 可能显示为代理程序，不能将排行之和当作网卡总量。")};
        for (const auto &value : observation.value("coverage").toArray()) {
            const QString line = value.toString().left(360);
            if (!line.isEmpty() && !limits.contains(line)) limits << line;
            if (limits.size() >= 12) break;
        }
        coverage->setText(limits.join('\n'));
        const QString metric = sort->currentData().toString();
        QVector<QJsonObject> applications;
        if (displayState != "inactive" && displayState != "unavailable") for (const auto &value : display.value("applications").toArray()) {
            applications.append(value.toObject()); if (applications.size() == 60) break;
        }
        std::sort(applications.begin(), applications.end(), [&](const QJsonObject &a, const QJsonObject &b) {
            const double x = measured(a.value(metric)), y = measured(b.value(metric));
            return x != y ? x > y : a.value("identity").toString() < b.value("identity").toString();
        });
        QSet<QString> expanded; QString selected;
        for (int i = 0; i < tree->topLevelItemCount(); ++i) if (tree->topLevelItem(i)->isExpanded()) expanded.insert(tree->topLevelItem(i)->data(0, Qt::UserRole).toString());
        if (tree->currentItem()) selected = tree->currentItem()->data(0, Qt::UserRole).toString();
        const int scroll = tree->verticalScrollBar()->value(), horizontal = tree->horizontalScrollBar()->value();
        {
            QSignalBlocker block(tree); tree->setUpdatesEnabled(false);
            QHash<QString, QTreeWidgetItem *> remaining;
            for (int i = 0; i < tree->topLevelItemCount(); ++i) remaining.insert(tree->topLevelItem(i)->data(0, Qt::UserRole).toString(), tree->topLevelItem(i));
            auto cells = [](QTreeWidgetItem *item, const QStringList &values) { for (int i = 0; i < values.size(); ++i) if (item->text(i) != values[i]) item->setText(i, values[i]); };
            int position = 0;
            for (const auto &app : applications) {
                const QString id = app.value("identity").toString();
                auto *item = remaining.take(id);
                if (!item) { item = new QTreeWidgetItem; tree->insertTopLevelItem(position, item); }
                else if (tree->indexOfTopLevelItem(item) != position) { tree->takeTopLevelItem(tree->indexOfTopLevelItem(item)); tree->insertTopLevelItem(position, item); }
                ++position;
                cells(item, {app.value("name").toString(), QStringLiteral("%1 / %2").arg(app.value("processCount").toInt()).arg(app.value("connectionCount").toInt()), rate(app, "uploadBytesPerSecond"), rate(app, "downloadBytesPerSecond")});
                item->setData(0, Qt::UserRole, id); item->setData(0, Qt::UserRole + 1, app);
                const QString hint = app.value("name").toString() + QStringLiteral("\n%1 个进程 · %2 个连接").arg(app.value("processCount").toInt()).arg(app.value("connectionCount").toInt())
                    + (app.value("partial").toBool() ? QStringLiteral("\n部分观测；已知值是可见连接的下界。") : QString());
                for (int column = 0; column < 4; ++column) {
                    const QString tip = DevStatsUI::literal(hint + (column == 2 ? "\n↑ " + rate(app, "uploadBytesPerSecond") : column == 3 ? "\n↓ " + rate(app, "downloadBytesPerSecond") : QString()));
                    if (item->toolTip(column) != tip) item->setToolTip(column, tip);
                }
                QHash<QString, QTreeWidgetItem *> children;
                for (int i = 0; i < item->childCount(); ++i) children.insert(item->child(i)->data(0, Qt::UserRole).toString(), item->child(i));
                int index = 0;
                for (const auto &value : app.value("processes").toArray()) {
                    const auto process = value.toObject(); const QString key = processKey(process);
                    auto *child = children.take(key);
                    if (!child) { child = new QTreeWidgetItem; item->insertChild(index, child); }
                    else if (item->indexOfChild(child) != index) { item->takeChild(item->indexOfChild(child)); item->insertChild(index, child); }
                    ++index;
                    auto displayProcess = process;
                    if (process.value("shared").toBool()) { displayProcess["uploadBytesPerSecond"] = QJsonValue(QJsonValue::Null); displayProcess["downloadBytesPerSecond"] = QJsonValue(QJsonValue::Null); }
                    cells(child, {process.value("name").toString(), QStringLiteral("%1 / %2").arg(qint64(process.value("pid").toDouble())).arg(process.value("connectionCount").toInt()), rate(displayProcess, "uploadBytesPerSecond"), rate(displayProcess, "downloadBytesPerSecond")});
                    child->setData(0, Qt::UserRole, key); child->setData(0, Qt::UserRole + 1, process);
                    const QString tip = process.value("shared").toBool() ? QStringLiteral("多个进程持有共享连接，无法区分各进程流量；应用只计算一次。") : QString();
                    for (int column = 0; column < 4; ++column) if (child->toolTip(column) != tip) child->setToolTip(column, tip);
                    if (selected == key) tree->setCurrentItem(child);
                    if (index == 100) break;
                }
                qDeleteAll(children); item->setExpanded(expanded.contains(id)); if (selected == id) tree->setCurrentItem(item);
            }
            qDeleteAll(remaining);
            tree->doItemsLayout(); filter(); tree->verticalScrollBar()->setValue(scroll); tree->horizontalScrollBar()->setValue(horizontal); tree->setUpdatesEnabled(true);
        }
        tree->setVisible(!applications.isEmpty());
    }
};

NetworkActivityPanel::NetworkActivityPanel(QWidget *parent) : QFrame(parent), d(new Impl) {
    setObjectName("networkActivityPanel"); setProperty("card", true);
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(20, 18, 20, 16); layout->setSpacing(10);
    auto *heading = text("应用网络活动 · TCP", "networkApplicationHeading"); QFont font = heading->font(); font.setPointSizeF(14); font.setWeight(QFont::DemiBold); heading->setFont(font); layout->addWidget(heading);
    d->leader = text("打开网络页查看下载最多的应用", "networkApplicationLeader"); QFont large = d->leader->font(); large.setPointSizeF(16); large.setWeight(QFont::DemiBold); d->leader->setFont(large); layout->addWidget(d->leader);
    layout->addWidget(text("普通用户可见的 TCP 流量；UDP/QUIC 暂不覆盖，代理流量可能归到代理程序。", "networkApplicationScope", true));
    auto *tools = new QHBoxLayout;
    d->search = new QLineEdit; d->search->setObjectName("networkApplicationSearch"); d->search->setPlaceholderText("搜索应用、进程或 PID"); d->search->setClearButtonEnabled(true);
    d->sort = new QComboBox; d->sort->setObjectName("networkApplicationSort"); d->sort->addItem("下载最多", "downloadBytesPerSecond"); d->sort->addItem("上传最多", "uploadBytesPerSecond");
    tools->addWidget(d->search, 1); tools->addWidget(d->sort); layout->addLayout(tools);
    d->status = text("等待网络页开始观察", "networkApplicationStatus", true); layout->addWidget(d->status);
    d->tree = new QTreeWidget; d->tree->setObjectName("networkApplications"); d->tree->setHeaderLabels({"应用 / 进程", "PID / 连接", "↑ 上传", "↓ 下载"});
    d->tree->setUniformRowHeights(true); d->tree->setFrameShape(QFrame::NoFrame); d->tree->setSelectionMode(QAbstractItemView::SingleSelection); d->tree->setEditTriggers(QAbstractItemView::NoEditTriggers); d->tree->setTextElideMode(Qt::ElideRight);
    d->tree->header()->setSectionResizeMode(QHeaderView::Interactive); d->tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    d->tree->header()->setStretchLastSection(false); d->tree->setColumnWidth(1, 105); d->tree->setColumnWidth(2, 100); d->tree->setColumnWidth(3, 100);
    QFont tableFont = font; tableFont.setPointSizeF(12); tableFont.setWeight(QFont::Normal); d->tree->setFont(tableFont);
    const int rateWidth = QFontMetrics(d->tree->font()).horizontalAdvance(QStringLiteral("≥ 1023.99 MB/s")) + 20;
    d->tree->setColumnWidth(1, 100); d->tree->setColumnWidth(2, rateWidth); d->tree->setColumnWidth(3, rateWidth);
    d->tree->setMinimumHeight(230); d->tree->setMaximumHeight(400); layout->addWidget(d->tree);
    d->count = text("", "networkApplicationCount", true); layout->addWidget(d->count);
    auto *toggle = new QToolButton; toggle->setObjectName("networkApplicationCoverageToggle"); toggle->setText("统计范围与权限说明"); toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); toggle->setArrowType(Qt::RightArrow); toggle->setCheckable(true); layout->addWidget(toggle);
    d->coverage = text("", "networkApplicationCoverage", true); d->coverage->hide(); layout->addWidget(d->coverage);
    connect(toggle, &QToolButton::toggled, this, [this, toggle](bool open) { d->coverage->setVisible(open); toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow); });
    connect(d->search, &QLineEdit::textChanged, this, [this] { d->filter(); });
    connect(d->sort, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this] { d->render(); });
}
NetworkActivityPanel::~NetworkActivityPanel() = default;
void NetworkActivityPanel::setObservation(const QJsonObject &observation, const QJsonObject &previous) { if (d->observation == observation && d->previous == previous) return; d->observation = observation; d->previous = previous; d->dirty = true; if (isVisible()) d->render(); }
void NetworkActivityPanel::showEvent(QShowEvent *event) { QFrame::showEvent(event); if (d->dirty) d->render(); }
