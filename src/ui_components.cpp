#include "ui_components.h"
#include "i18n.h"
#include "ui_theme.h"
#include "resource_breakdown.h"

#include <QAbstractItemView>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QHeaderView>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QStyle>
#include <QToolButton>
#include <QTextDocument>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace DevStatsUI {
QString literal(const QString &text) {
    return Qt::mightBeRichText(text) ? QStringLiteral("<p style=\"white-space:pre-wrap\">%1</p>").arg(text.toHtmlEscaped()) : text;
}

bool literalToolTip(QHelpEvent *event, QAbstractItemView *view, const QModelIndex &index) {
    const QString text = index.isValid() ? index.data(Qt::ToolTipRole).toString() : QString();
    QToolTip::showText(event->globalPos(), literal(text), view->viewport(), index.isValid() ? view->visualRect(index) : QRect());
    event->setAccepted(!text.isEmpty());
    return !text.isEmpty();
}

QLabel *label(const QString &text, const char *role, const QString &name) {
    auto *widget = new QLabel(text);
    widget->setTextFormat(Qt::PlainText);
    widget->setWordWrap(true);
    if (role) widget->setProperty("role", QString::fromLatin1(role));
    if (!name.isEmpty()) widget->setObjectName(name);
    return widget;
}

QLabel *pill(const QString &text, const QString &tone, const QString &name) {
    auto *widget = new QLabel(text);
    widget->setTextFormat(Qt::PlainText);
    widget->setProperty("pill", true);
    widget->setProperty("tone", tone);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    if (!name.isEmpty()) widget->setObjectName(name);
    return widget;
}

void setStyleProperty(QWidget *widget, const char *property, const QVariant &value) {
    if (widget->property(property) == value) return;
    widget->setProperty(property, value);
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
}

void setTone(QWidget *widget, const QString &tone) { setStyleProperty(widget, "tone", tone); }

QString severityTone(const QString &severity) {
    return severity == QLatin1String("critical") ? QStringLiteral("critical")
         : severity == QLatin1String("attention") ? QStringLiteral("warn") : QStringLiteral("info");
}

QString severityCaption(const QString &severity) {
    return severity == QLatin1String("critical") ? T("严重")
         : severity == QLatin1String("attention") ? T("注意") : T("提示");
}

QVBoxLayout *card(QVBoxLayout *parent, const QString &title, const QString &hint, QWidget *headerWidget) {
    auto *frame = new QFrame;
    frame->setProperty("card", true);
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(10);
    if (!title.isEmpty() || headerWidget) {
        auto *heading = new QHBoxLayout;
        heading->setSpacing(8);
        auto *caption = label(title, "heading");
        caption->setWordWrap(false);
        heading->addWidget(caption);
        heading->addStretch();
        if (headerWidget) heading->addWidget(headerWidget);
        layout->addLayout(heading);
    }
    if (!hint.isEmpty()) layout->addWidget(label(hint, "caption"));
    parent->addWidget(frame);
    return layout;
}

QFrame *separator() {
    auto *line = new QFrame;
    line->setObjectName("separator");
    line->setFrameShape(QFrame::NoFrame);
    return line;
}

QTreeWidget *table(const QString &name, const QStringList &headers) {
    auto *tree = new QTreeWidget;
    tree->setObjectName(name);
    tree->setHeaderLabels(headers);
    tree->setRootIsDecorated(false);
    tree->setUniformRowHeights(true);
    tree->setSelectionMode(QAbstractItemView::SingleSelection);
    tree->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    tree->setFrameShape(QFrame::NoFrame);
    tree->setItemDelegate(new MetricItemDelegate(tree));
    tree->setTextElideMode(Qt::ElideRight);
    tree->setEditTriggers(QAbstractItemView::NoEditTriggers);
    tree->header()->setStretchLastSection(false);
    tree->header()->setSectionResizeMode(QHeaderView::ResizeToContents);
    tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    tree->header()->setDefaultAlignment(Qt::AlignRight | Qt::AlignVCenter);
    tree->headerItem()->setTextAlignment(0, Qt::AlignLeft | Qt::AlignVCenter);
    return tree;
}

void fitRows(QTreeWidget *tree, int minimumRows, int maximumRows) {
    int rows = 0;
    for (int i = 0; i < tree->topLevelItemCount(); ++i) {
        const auto *item = tree->topLevelItem(i);
        if (item->isHidden()) continue;
        ++rows;
        if (item->isExpanded()) for (int j = 0; j < item->childCount(); ++j) rows += !item->child(j)->isHidden();
    }
    rows = qBound(minimumRows, rows, maximumRows);
    const int rowHeight = tree->topLevelItemCount() ? qMax(30, tree->visualItemRect(tree->topLevelItem(0)).height()) : 30;
    const int header = tree->isHeaderHidden() ? 0 : tree->header()->sizeHint().height();
    tree->setFixedHeight(header + rows * rowHeight + 2);
}

QWidget *disclosure(QLayout *parent, const QString &title, const QString &name, QWidget *body) {
    auto *toggle = new QToolButton;
    toggle->setObjectName(name);
    toggle->setText(title);
    toggle->setCheckable(true);
    toggle->setArrowType(Qt::RightArrow);
    toggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle->setProperty("disclosure", true);
    QObject::connect(toggle, &QToolButton::toggled, body, [toggle, body](bool open) {
        body->setVisible(open);
        toggle->setArrowType(open ? Qt::DownArrow : Qt::RightArrow);
    });
    body->hide();
    parent->addWidget(toggle);
    parent->addWidget(body);
    return toggle;
}
}

FlowLayout::FlowLayout(int horizontalSpacing, int verticalSpacing, QWidget *parent)
    : QLayout(parent), horizontal_(horizontalSpacing), vertical_(verticalSpacing) { setContentsMargins(0, 0, 0, 0); }
FlowLayout::~FlowLayout() { while (auto *item = takeAt(0)) delete item; }
void FlowLayout::addItem(QLayoutItem *item) { items_.append(item); }
int FlowLayout::count() const { return items_.size(); }
QLayoutItem *FlowLayout::itemAt(int index) const { return items_.value(index); }
QLayoutItem *FlowLayout::takeAt(int index) { return index >= 0 && index < items_.size() ? items_.takeAt(index) : nullptr; }
Qt::Orientations FlowLayout::expandingDirections() const { return {}; }
bool FlowLayout::hasHeightForWidth() const { return true; }
int FlowLayout::heightForWidth(int width) const { return arrange(QRect(0, 0, width, 0), false); }
void FlowLayout::setGeometry(const QRect &rect) { QLayout::setGeometry(rect); arrange(rect, true); }
QSize FlowLayout::sizeHint() const {
    // One row: what the layout takes when there is room.
    int width = 0, height = 0, shown = 0;
    for (const auto *item : items_) {
        if (item->isEmpty()) continue;
        width += item->sizeHint().width(); height = std::max(height, item->sizeHint().height()); ++shown;
    }
    const auto margins = contentsMargins();
    return QSize(width + horizontal_ * std::max(0, shown - 1) + margins.left() + margins.right(), height + margins.top() + margins.bottom());
}
QSize FlowLayout::minimumSize() const {
    // One item per row: the narrowest the layout can become.
    QSize size;
    for (const auto *item : items_) if (!item->isEmpty()) size = size.expandedTo(item->minimumSize());
    const auto margins = contentsMargins();
    return size + QSize(margins.left() + margins.right(), margins.top() + margins.bottom());
}
int FlowLayout::arrange(const QRect &rect, bool apply) const {
    const auto margins = contentsMargins();
    const QRect area = rect.adjusted(margins.left(), margins.top(), -margins.right(), -margins.bottom());
    int x = area.x(), y = area.y(), rowHeight = 0;
    for (auto *item : items_) {
        if (item->isEmpty()) continue;
        const QSize size = item->sizeHint();
        if (x > area.x() && x + size.width() > area.x() + area.width()) { x = area.x(); y += rowHeight + vertical_; rowHeight = 0; }
        if (apply) item->setGeometry(QRect(QPoint(x, y), size));
        x += size.width() + horizontal_;
        rowHeight = std::max(rowHeight, size.height());
    }
    return y + rowHeight - rect.y() + margins.bottom();
}

ToggleSwitch::ToggleSwitch(QWidget *parent) : QAbstractButton(parent) {
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setFixedSize(sizeHint());
    setFocusPolicy(Qt::StrongFocus);
}

void ToggleSwitch::paintEvent(QPaintEvent *) {
    const auto c = DevStatsUI::colors(window()->palette());
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF track(1, 2, width() - 2, height() - 4);
    p.setPen(hasFocus() ? QPen(c.accent, 1) : Qt::NoPen);
    p.setBrush(!isEnabled() ? c.raised : isChecked() ? c.accent : c.border);
    p.drawRoundedRect(track, track.height() / 2, track.height() / 2);
    const qreal knob = track.height() - 4;
    const qreal x = isChecked() ? track.right() - knob - 2 : track.left() + 2;
    p.setPen(Qt::NoPen);
    p.setBrush(isEnabled() ? QColor(Qt::white) : c.muted);
    p.drawEllipse(QRectF(x, track.top() + 2, knob, knob));
}

SectionTabs::SectionTabs(QWidget *parent) : QWidget(parent) {
    row_ = new QHBoxLayout(this);
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(20);
    row_->addStretch();
}

void SectionTabs::addTab(const QString &text) {
    auto *button = new QPushButton(text);
    button->setProperty("tab", true);
    button->setCheckable(true);
    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    const int index = buttons_.size();
    connect(button, &QPushButton::clicked, this, [this, index] { setCurrentIndex(index); });
    row_->insertWidget(index, button);
    buttons_.append(button);
    if (current_ < 0) setCurrentIndex(0);
}

void SectionTabs::setCurrentIndex(int index) {
    if (index < 0 || index >= buttons_.size()) return;
    for (int i = 0; i < buttons_.size(); ++i) buttons_[i]->setChecked(i == index);
    if (index == current_) return;
    current_ = index;
    emit currentChanged(index);
}

MeterTile::MeterTile(const QString &name, QWidget *parent) : QFrame(parent) {
    setProperty("tile", true);
    if (!name.isEmpty()) setObjectName(name);
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(12, 10, 12, 12);
    column_->setSpacing(2);
    label_ = DevStatsUI::label({}, "caption");
    label_->setWordWrap(false);
    value_ = DevStatsUI::label(QString::fromUtf8("—"), "metric", name.isEmpty() ? QString() : name + "Value");
    value_->setWordWrap(false);
    detail_ = DevStatsUI::label({}, "muted", name.isEmpty() ? QString() : name + "Detail");
    column_->addWidget(label_);
    column_->addWidget(value_);
    column_->addWidget(detail_);
}

void MeterTile::setContent(const QString &label, const QString &value, const QString &detail, const QString &tone, double fraction) {
    if (label_->text() != label) label_->setText(label);
    if (value_->text() != value) value_->setText(value);
    if (detail_->text() != detail) detail_->setText(detail);
    const QString shown = tone == QLatin1String("ok") ? QString() : tone;
    DevStatsUI::setTone(this, shown);
    DevStatsUI::setTone(value_, shown);
    if (tone_ != tone || fraction_ != fraction) { tone_ = tone; fraction_ = fraction; update(); }
}

void MeterTile::setMeter(const QJsonObject &meter) {
    setContent(meter.value("label").toString(), meter.value("value").toString(), meter.value("detail").toString(),
               meter.value("tone").toString(), meter.value("fraction").toDouble(-1));
}

void MeterTile::setClickable(bool clickable) {
    setProperty("clickable", clickable);
    setCursor(clickable ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void MeterTile::addContent(QWidget *widget) {
    column_->addSpacing(4);
    column_->addWidget(widget);
}

void MeterTile::mouseReleaseEvent(QMouseEvent *event) {
    QFrame::mouseReleaseEvent(event);
    if (property("clickable").toBool() && rect().contains(event->pos())) emit clicked();
}

void MeterTile::paintEvent(QPaintEvent *event) {
    QFrame::paintEvent(event);
    if (fraction_ < 0 || !std::isfinite(fraction_)) return;
    const auto c = DevStatsUI::colors(window()->palette());
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    const QRectF track(12, height() - 7, std::max(0, width() - 24), 3);
    QColor base = c.secondary; base.setAlphaF(.16);
    p.setBrush(base);
    p.drawRoundedRect(track, 1.5, 1.5);
    p.setBrush(tone_ == QLatin1String("ok") ? c.accent : DevStatsUI::toneColor(c, tone_));
    p.drawRoundedRect(QRectF(track.left(), track.top(), track.width() * std::clamp(fraction_, 0., 1.), track.height()), 1.5, 1.5);
}
