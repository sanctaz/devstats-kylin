#include "memory_relief_dialog.h"
#include "charts.h"
#include "ui_theme.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFrame>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSet>
#include <QSignalBlocker>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace {
constexpr int KeyRole = Qt::UserRole;
constexpr int ValueRole = Qt::UserRole + 1;
constexpr int ReasonRole = Qt::UserRole + 2;
// Keep the standard item view interaction and layout. Some Linux Qt styles
// render the item check indicator as a solid square when a stylesheet is active.
class CheckDelegate final : public QStyledItemDelegate {
public:
    explicit CheckDelegate(QObject *parent) : QStyledItemDelegate(parent) {}
    void paint(QPainter *painter, const QStyleOptionViewItem &source, const QModelIndex &index) const override {
        QStyledItemDelegate::paint(painter, source, index);
        QStyleOptionViewItem option(source); initStyleOption(&option, index);
        if (!(option.features & QStyleOptionViewItem::HasCheckIndicator) || !option.widget) return;
        const auto c = DevStatsUI::colors(option.widget->window()->palette());
        const QRect indicator = option.widget->style()->subElementRect(QStyle::SE_ItemViewItemCheckIndicator, &option, option.widget);
        painter->save();
        painter->fillRect(indicator, option.state & QStyle::State_Selected ? c.selection : c.surface);
        painter->setRenderHint(QPainter::Antialiasing);
        if (!(option.state & QStyle::State_Enabled)) painter->setOpacity(.55);
        const QRectF box(indicator.center().x() - 6.5, indicator.center().y() - 6.5, 13, 13);
        const bool checked = option.checkState == Qt::Checked;
        painter->setPen(QPen(checked ? c.accent : c.secondary, 1)); painter->setBrush(checked ? c.accent : c.surface);
        painter->drawRoundedRect(box, 2, 2);
        if (checked) {
            painter->setPen(QPen(c.background, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF points[]{{box.left() + 3, box.top() + 6.5}, {box.left() + 5.4, box.top() + 9}, {box.left() + 10, box.top() + 3.8}};
            painter->drawPolyline(points, 3);
        }
        painter->restore();
    }
};
double number(const QJsonValue &value) {
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= 0 ? value.toDouble() : -1;
}
QString processKey(const QJsonObject &p) {
    return QStringLiteral("p:%1:%2").arg(p.value("pid").toDouble(), 0, 'f', 0).arg(p.value("startTicks").toDouble(), 0, 'f', 0);
}
QString applicationKey(const QJsonObject &p) {
    const QString id = p.value("applicationIdentity").toString();
    return id.isEmpty() ? processKey(p) : QStringLiteral("a:") + id;
}
QLabel *label(const QString &text, const QString &name = {}, bool muted = false) {
    auto *widget = new QLabel(text); widget->setObjectName(name); widget->setWordWrap(true);
    widget->setTextFormat(Qt::PlainText); widget->setTextInteractionFlags(Qt::TextSelectableByMouse);
    widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    QFont font = widget->font(); font.setPointSizeF(muted ? 11.5 : 13); widget->setFont(font);
    if (muted) widget->setProperty("textRole", "muted");
    return widget;
}
QVBoxLayout *card(QVBoxLayout *parent, const QString &title) {
    auto *frame = new QFrame; frame->setProperty("card", true);
    auto *layout = new QVBoxLayout(frame); layout->setContentsMargins(16, 14, 16, 14); layout->setSpacing(9);
    auto *heading = label(title); QFont font = heading->font(); font.setPointSizeF(13); font.setWeight(QFont::DemiBold); heading->setFont(font);
    layout->addWidget(heading); parent->addWidget(frame); return layout;
}
QTreeWidget *table(const QString &name, const QStringList &headers, int height) {
    auto *tree = new QTreeWidget; tree->setObjectName(name); tree->setHeaderLabels(headers);
    tree->setItemDelegate(new CheckDelegate(tree));
    tree->setRootIsDecorated(false); tree->setUniformRowHeights(true); tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setFrameShape(QFrame::NoFrame); tree->setMinimumHeight(height);
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents); tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->setTextElideMode(Qt::ElideRight); return tree;
}
QSet<QString> checkedKeys(QTreeWidget *tree) {
    QSet<QString> keys;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const auto *item = tree->topLevelItem(i);
        if (item->checkState(0) == Qt::Checked) keys.insert(item->data(0, KeyRole).toString());
    }
    return keys;
}
struct Position {
    QString current;
    int vertical = 0, horizontal = 0;
    explicit Position(QTreeWidget *tree) : current(tree->currentItem() ? tree->currentItem()->data(0, KeyRole).toString() : QString()),
        vertical(tree->verticalScrollBar()->value()), horizontal(tree->horizontalScrollBar()->value()) {}
    void restore(QTreeWidget *tree) const {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->data(0, KeyRole).toString() == current) tree->setCurrentItem(tree->topLevelItem(i));
        tree->doItemsLayout(); tree->verticalScrollBar()->setValue(vertical); tree->horizontalScrollBar()->setValue(horizontal);
    }
};
}

struct MemoryReliefDialog::Impl {
    MemoryReliefDialog *q;
    QJsonObject snapshot;
    bool busy = false;
    bool styling = false;
    QTreeWidget *apps = nullptr;
    QPushButton *closeApps = nullptr, *trim = nullptr, *refresh = nullptr;
    QLabel *applicationHint = nullptr, *feedback = nullptr, *outcome = nullptr;
    explicit Impl(MemoryReliefDialog *dialog) : q(dialog) {}
    void appearance() {
        if (styling) return;
        styling = true;
        const bool dark = DevStatsUI::isDark(q->parentWidget() ? q->parentWidget()->palette() : q->palette());
        q->setPalette(DevStatsUI::palette(dark));
        // A parent window's stylesheet already cascades to this dialog.
        if (!q->parentWidget()) q->setStyleSheet(DevStatsUI::styleSheet(dark));
        styling = false;
    }
    bool freshProcesses() const {
        const double age = number(snapshot.value("processSampleAgeSeconds")), at = number(snapshot.value("timestamp"));
        return age >= 0 && age <= 5 && at > 0 && std::abs(QDateTime::currentMSecsSinceEpoch() / 1000. - at) <= 6;
    }
    void actions() {
        int appCount = 0, processCount = 0; QString reason;
        for (int i = 0; i < apps->topLevelItemCount(); ++i) {
            auto *item = apps->topLevelItem(i);
            if (item->checkState(0) != Qt::Checked) continue;
            ++appCount; processCount += item->data(0, ValueRole).toJsonArray().size();
            if (!item->data(0, ReasonRole).toString().isEmpty()) reason = item->data(0, ReasonRole).toString();
        }
        QString hint = QStringLiteral("先勾选闲置应用；随后核对窗口并确认。后台进程可能保留，不保证释放全部 RSS。");
        if (!freshProcesses()) hint = QStringLiteral("进程观测已过期或不可用，请刷新后再操作。");
        else if (!reason.isEmpty()) hint = reason;
        else if (processCount > 64) hint = QStringLiteral("所选应用共 %1 个进程，超过 64 个核对上限。请减少所选应用。").arg(processCount);
        else if (appCount) hint = QStringLiteral("已选 %1 个应用、%2 个进程。下一步核对并确认；请先保存工作。").arg(appCount).arg(processCount);
        applicationHint->setText(hint); closeApps->setToolTip(hint);
        closeApps->setEnabled(!busy && freshProcesses() && appCount > 0 && processCount <= 64 && reason.isEmpty());
        trim->setEnabled(!busy); refresh->setEnabled(!busy); apps->setEnabled(!busy);
    }
    void renderApplications() {
        const auto checked = checkedKeys(apps); const Position position(apps); QSignalBlocker blocker(apps);
        QMap<QString, QJsonArray> groups; QMap<QString, QJsonObject> applicationInfo;
        for (const auto &v : snapshot.value("activityApplications").toArray()) {
            const auto app = v.toObject(); applicationInfo.insert(QStringLiteral("a:") + app.value("identity").toString(), app);
        }
        for (const auto &v : snapshot.value("processes").toArray()) {
            const auto process = v.toObject(); groups[applicationKey(process)].append(process);
        }
        struct App { QString key, name, reason; QJsonArray identities; double rss = 0; bool known = false, partial = false; };
        QList<App> rows;
        for (auto it = groups.cbegin(); it != groups.cend(); ++it) {
            App row; row.key = it.key(); const auto info = applicationInfo.value(row.key);
            row.name = info.value("name").toString(); row.partial = info.value("partial").toBool() || number(info.value("processCount")) > it.value().size();
            for (const auto &v : it.value()) {
                const auto p = v.toObject();
                if (row.name.isEmpty()) row.name = p.value("name").toString();
                const double rss = number(p.value("memoryBytes"));
                row.partial |= rss < 0 || p.value("partial").toBool(); if (rss >= 0) { row.known = true; row.rss += rss; }
                row.identities.append(QJsonObject{{"pid", p.value("pid")}, {"startTicks", p.value("startTicks")},
                    {"executablePath", p.value("executablePath")}, {"name", p.value("name").toString()},
                    {"applicationIdentity", p.value("applicationIdentity").toString()}, {"memoryBytes", rss >= 0 ? QJsonValue(rss) : QJsonValue(QJsonValue::Null)}});
                if (p.value("pid").toDouble() == double(QCoreApplication::applicationPid())) row.reason = QStringLiteral("DevStats 自身请使用下方的空闲内存回收。");
                else if (number(p.value("pid")) <= 1 || number(p.value("startTicks")) <= 0 || p.value("executablePath").toString().isEmpty())
                    row.reason = QStringLiteral("进程身份不完整或属于关键进程，不能请求关闭。");
            }
            if (row.name.isEmpty()) row.name = QStringLiteral("未知应用");
            if (row.identities.size() > 64) row.reason = QStringLiteral("此应用含 %1 个进程，超过 64 个核对上限，不能请求关闭。").arg(row.identities.size());
            rows.append(row);
        }
        std::sort(rows.begin(), rows.end(), [](const App &a, const App &b) { if (a.known != b.known) return a.known; if (a.rss != b.rss) return a.rss > b.rss; return a.key < b.key; });
        apps->clear();
        for (int i = 0; i < std::min(50, rows.size()); ++i) {
            const auto &row = rows[i];
            auto *item = new QTreeWidgetItem(apps, {row.name, QString::number(row.identities.size()),
                (row.partial && row.known ? QStringLiteral("≥ ") : QString()) + formatBytes(row.known ? row.rss : -1)});
            item->setData(0, KeyRole, row.key); item->setData(0, ValueRole, row.identities); item->setData(0, ReasonRole, row.reason);
            item->setFlags(item->flags() | Qt::ItemIsUserCheckable); item->setCheckState(0, checked.contains(row.key) && row.reason.isEmpty() ? Qt::Checked : Qt::Unchecked);
            QString detail = row.name + QStringLiteral("\n应用身份：") + row.key;
            for (int member = 0; member < std::min(8, row.identities.size()); ++member) { const auto p = row.identities[member].toObject(); detail += QStringLiteral("\nPID %1 · %2").arg(p.value("pid").toDouble(), 0, 'f', 0).arg(p.value("name").toString()); }
            if (row.identities.size() > 8) detail += QStringLiteral("\n另有 %1 个进程；核对请求包含全部成员。").arg(row.identities.size() - 8);
            if (!row.reason.isEmpty()) { item->setFlags(item->flags() & ~Qt::ItemIsEnabled); detail += '\n' + row.reason; }
            if (row.partial) detail += QStringLiteral("\n部分 RSS 观测，≥ 为已知部分之和；未知值不按零计算。");
            for (int column = 0; column < 3; ++column) item->setToolTip(column, detail);
        }
        position.restore(apps);
    }
};

MemoryReliefDialog::MemoryReliefDialog(QWidget *parent) : QDialog(parent), d(std::make_unique<Impl>(this)) {
    setObjectName("memoryReliefDialog"); setWindowTitle(QStringLiteral("内存减压")); resize(680, 560); setMinimumSize(560, 420);
    d->appearance();
    auto *layout = new QVBoxLayout(this); layout->setContentsMargins(18, 16, 18, 14); layout->setSpacing(10);
    auto *heading = label(QStringLiteral("内存减压")); heading->setProperty("role", "title"); layout->addWidget(heading);
    layout->addWidget(label(QStringLiteral("勾选不用的应用，请求它们正常关闭窗口。应用可以弹出保存询问，也可能保留后台进程。"), {}, true));
    auto *scroll = new QScrollArea; scroll->setObjectName("memoryReliefScroll"); scroll->setWidgetResizable(true); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setFrameShape(QFrame::NoFrame); scroll->viewport()->setAutoFillBackground(false);
    auto *body = new QWidget; body->setObjectName("memoryReliefBody"); body->setProperty("pageBody", true); auto *content = new QVBoxLayout(body); content->setContentsMargins(0, 0, 8, 0); content->setSpacing(12);
    auto *applicationCard = card(content, QStringLiteral("应用"));
    applicationCard->addWidget(label(QStringLiteral("当前用户 · 按内存排序前 50 个应用 · RSS 含共享页，≥ 为部分合计"), {}, true));
    d->apps = table("memoryReliefApplications", {"应用", "进程数", "内存"}, 220); applicationCard->addWidget(d->apps, 1);
    d->closeApps = new QPushButton(QStringLiteral("关闭所选应用的窗口…")); d->closeApps->setObjectName("closeMemoryReliefApplications"); d->closeApps->setProperty("primary", true); applicationCard->addWidget(d->closeApps);
    d->applicationHint = label({}, "memoryReliefApplicationHint", true); applicationCard->addWidget(d->applicationHint);
    d->outcome = label({}, "memoryReliefOutcome"); d->outcome->hide(); content->addWidget(d->outcome);
    auto *selfCard = card(content, QStringLiteral("DevStats 自身"));
    selfCard->addWidget(label(QStringLiteral("尝试归还本应用已经空闲的内存，保留监测；回收量以实际观测为准。"), {}, true));
    d->trim = new QPushButton(QStringLiteral("回收 DevStats 空闲内存")); d->trim->setObjectName("trimMemoryReliefSelf"); selfCard->addWidget(d->trim);
    content->addStretch(); scroll->setWidget(body); layout->addWidget(scroll, 1);
    auto *feedbackRow = new QHBoxLayout;
    d->feedback = label(QStringLiteral("默认不选任何应用。操作前会再次确认。"), "memoryReliefFeedback", true); feedbackRow->addWidget(d->feedback, 1); layout->addLayout(feedbackRow);
    auto *footer = new QHBoxLayout; d->refresh = new QPushButton(QStringLiteral("刷新观测")); d->refresh->setObjectName("refreshMemoryRelief"); footer->addWidget(d->refresh); footer->addStretch();
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); buttons->button(QDialogButtonBox::Close)->setText(QStringLiteral("关闭")); connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject); footer->addWidget(buttons); layout->addLayout(footer);
    connect(d->apps, &QTreeWidget::itemChanged, this, [this] { d->actions(); });
    connect(d->refresh, &QPushButton::clicked, this, &MemoryReliefDialog::refreshRequested);
    connect(d->trim, &QPushButton::clicked, this, [this] { if (d->busy) return; setBusy(true); emit trimMemoryRequested(); });
    connect(d->closeApps, &QPushButton::clicked, this, [this] {
        d->actions(); if (!d->closeApps->isEnabled()) { if (!d->freshProcesses()) setFeedback(QStringLiteral("进程观测已过期，请刷新后再操作。")); return; }
        QJsonArray identities; QStringList names;
        for (int i = 0; i < d->apps->topLevelItemCount(); ++i) {
            const auto *item = d->apps->topLevelItem(i); if (item->checkState(0) != Qt::Checked) continue;
            names.append(item->text(0)); for (const auto &identity : item->data(0, ValueRole).toJsonArray()) identities.append(identity);
        }
        setBusy(true); emit closeApplicationsRequested(identities, names);
    });
    d->actions();
}
MemoryReliefDialog::~MemoryReliefDialog() = default;
void MemoryReliefDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange) d->appearance();
}
void MemoryReliefDialog::setSnapshot(const QJsonObject &snapshot) { d->snapshot = snapshot; d->renderApplications(); d->actions(); }
void MemoryReliefDialog::setBusy(bool busy) { d->busy = busy; d->actions(); }
void MemoryReliefDialog::setFeedback(const QString &message) { d->feedback->setText(message); }
void MemoryReliefDialog::setOutcome(const QJsonObject &result) {
    if (result.isEmpty()) { d->outcome->clear(); d->outcome->hide(); return; }
    QStringList lines;
    const double before = number(result.value("beforeAvailableBytes")), after = number(result.value("afterAvailableBytes"));
    const QJsonValue delta = result.value("availableDeltaBytes");
    QString change = QString::fromUtf8("—");
    if (delta.isDouble() && std::isfinite(delta.toDouble())) change = (delta.toDouble() < 0 ? QString::fromUtf8("−") : QStringLiteral("+")) + formatBytes(std::abs(delta.toDouble()));
    lines << QStringLiteral("系统可用内存：%1 → %2（变化 %3）").arg(formatBytes(before), formatBytes(after), change);
    const double selected = number(result.value("selectedBeforeRssBytes")), remaining = number(result.value("remainingKnownRssBytes"));
    const double missing = number(result.value("missingProcessCount"));
    const double unknown = number(result.value("unknownProcessCount"));
    if (selected >= 0 || remaining >= 0 || missing >= 0)
        lines << QStringLiteral("所选进程 RSS：操作前已知 %1；之后仍可观测 %2；未观测到 %3 个进程，RSS 未知 %4 个。").arg(formatBytes(selected), formatBytes(remaining), missing >= 0 ? QString::number(missing, 'f', 0) : QString::fromUtf8("—"), unknown >= 0 ? QString::number(unknown, 'f', 0) : QString::fromUtf8("—"));
    if (!result.value("message").toString().isEmpty()) lines << result.value("message").toString();
    if (!result.value("measurementNote").toString().isEmpty()) lines << result.value("measurementNote").toString();
    lines << QStringLiteral("这是同期观测，其他程序也可能影响变化。");
    d->outcome->setText(lines.join('\n')); d->outcome->show();
}
