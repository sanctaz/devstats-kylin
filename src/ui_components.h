#pragma once
#include <QAbstractButton>
#include <QFrame>
#include <QJsonObject>
#include <QVector>

class QAbstractItemView;
class QHBoxLayout;
class QHelpEvent;
class QModelIndex;
class QLabel;
class QTreeWidget;
class QVBoxLayout;

namespace DevStatsUI {
QLabel *label(const QString &text = {}, const char *role = nullptr, const QString &name = {});
QLabel *pill(const QString &text = {}, const QString &tone = {}, const QString &name = {});
// Sets a stylesheet property and re-applies styles only when the value changed.
void setStyleProperty(QWidget *widget, const char *property, const QVariant &value);
void setTone(QWidget *widget, const QString &tone);
QString severityTone(const QString &severity);
QString severityCaption(const QString &severity);
// A titled card appended to parent; returns the layout for its content. An
// optional widget sits at the right end of the title row.
QVBoxLayout *card(QVBoxLayout *parent, const QString &title, const QString &hint = {}, QWidget *headerWidget = nullptr);
QFrame *separator();
QTreeWidget *table(const QString &name, const QStringList &headers);
// Sizes a list to its rows, between the bounds, so a short list leaves no blank
// panel and a long one scrolls.
void fitRows(QTreeWidget *tree, int minimumRows, int maximumRows);
// A collapsed section: button text toggles body visibility.
QWidget *disclosure(QLayout *parent, const QString &title, const QString &name, QWidget *body);
// Names and paths come from other programs. Shown in a tooltip they must stay text:
// anything that looks like markup is escaped instead of being rendered.
QString literal(const QString &text);
// For item delegates: shows the item's tooltip literally. Returns whether one was shown.
bool literalToolTip(QHelpEvent *event, QAbstractItemView *view, const QModelIndex &index);
}

// An on/off control that looks the same under every Linux widget style.
class ToggleSwitch final : public QAbstractButton {
    Q_OBJECT
public:
    explicit ToggleSwitch(QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(38, 22); }
protected:
    void paintEvent(QPaintEvent *) override;
};

// Page-level tabs built from plain buttons. Pages are ordinary widgets the owner
// shows and hides, so wrapped text sizes the page and no hidden page takes room.
class SectionTabs final : public QWidget {
    Q_OBJECT
public:
    explicit SectionTabs(QWidget *parent = nullptr);
    void addTab(const QString &text);
    int count() const { return buttons_.size(); }
    int currentIndex() const { return current_; }
    void setCurrentIndex(int index);
signals:
    void currentChanged(int index);
private:
    QVector<QAbstractButton *> buttons_;
    QHBoxLayout *row_;
    int current_ = -1;
};

// A metric tile with a thin usage bar: label, big value, one line of detail.
class MeterTile final : public QFrame {
    Q_OBJECT
public:
    explicit MeterTile(const QString &name = {}, QWidget *parent = nullptr);
    void setContent(const QString &label, const QString &value, const QString &detail, const QString &tone, double fraction);
    void setMeter(const QJsonObject &meter);
    void setClickable(bool clickable);
    // Appends a widget, such as a sparkline, below the detail line.
    void addContent(QWidget *widget);
    QLabel *valueLabel() const { return value_; }
signals:
    void clicked();
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;
private:
    QLabel *label_, *value_, *detail_;
    QVBoxLayout *column_;
    QString tone_;
    double fraction_ = -1;
};
