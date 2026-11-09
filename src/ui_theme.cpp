#include "ui_theme.h"

#include <QList>
#include <QPair>

namespace DevStatsUI {
QColor toneColor(const Colors &c, const QString &tone) {
    if (tone == QLatin1String("ok")) return c.ok;
    if (tone == QLatin1String("warn")) return c.warn;
    if (tone == QLatin1String("critical")) return c.critical;
    if (tone == QLatin1String("info")) return c.accent;
    return c.muted;
}

QString styleSheet(bool dark) {
    const auto c = colors(dark);
    QString style = QString::fromUtf8(R"CSS(
QWidget { font-size: 10.5pt; }
QWidget#shell, QDialog { background: @background@; color: @text@; }
QWidget#headerBar { background: @background@; }
QWidget#sidebar { background: @sidebar@; border-right: 1px solid @border@; }

/* Text roles */
QLabel { background: transparent; border: none; color: @text@; }
QLabel[role="caption"], QLabel[textRole="muted"] { color: @secondary@; font-size: 9.5pt; }
QLabel[role="muted"], QLabel[textRole="caption"] { color: @muted@; font-size: 9.5pt; }
QLabel[role="section"] { color: @muted@; font-size: 9pt; font-weight: 600; }
QLabel[role="heading"], QLabel[panelHeading="true"] { font-size: 11pt; font-weight: 600; }
QLabel[role="strong"] { font-weight: 600; }
QLabel[role="title"] { font-size: 14pt; font-weight: 600; }
QLabel[role="brand"] { font-size: 12.5pt; font-weight: 700; }
QLabel[role="headline"] { font-size: 13.5pt; font-weight: 600; }
QLabel[role="number"] { font-size: 19pt; font-weight: 600; }
QLabel[role="metric"] { font-size: 15pt; font-weight: 600; }
QLabel[tone="ok"] { color: @ok@; }
QLabel[tone="warn"] { color: @warn@; }
QLabel[tone="critical"] { color: @critical@; }
QLabel[tone="info"] { color: @accent@; }
QLabel[pill="true"] { font-size: 9pt; font-weight: 600; padding: 1px 7px; border-radius: 4px; background: @raised@; color: @secondary@; }
QLabel[pill="true"][tone="ok"] { background: @okSoft@; color: @ok@; }
QLabel[pill="true"][tone="warn"] { background: @warnSoft@; color: @warn@; }
QLabel[pill="true"][tone="critical"] { background: @criticalSoft@; color: @critical@; }
QLabel[pill="true"][tone="info"] { background: @accentSoft@; color: @accent@; }

/* Surfaces */
QFrame[card="true"] { background: @surface@; border: 1px solid @border@; border-radius: 10px; }
QFrame[card="true"] QLabel { border: none; }
QFrame[card="true"][tone="warn"] { background: @warnSoft@; border-color: @warn@; }
QFrame[card="true"][tone="critical"] { background: @criticalSoft@; border-color: @critical@; }
QFrame[tile="true"] { background: @raised@; border: 1px solid transparent; border-radius: 8px; }
QFrame[tile="true"][clickable="true"]:hover { border-color: @border@; }
QFrame[tile="true"][tone="warn"] { border-color: @warn@; }
QFrame[tile="true"][tone="critical"] { border-color: @critical@; }
QFrame[rowItem="true"] { background: transparent; border: none; border-top: 1px solid @border@; border-radius: 0; }
QFrame[rowItem="true"][first="true"] { border-top: none; }
QFrame#separator { background: @border@; border: none; max-height: 1px; min-height: 1px; }
QScrollArea, QStackedWidget#modulePages { background: transparent; border: none; }
QWidget[pageBody="true"] { background: transparent; }

/* Navigation */
QListWidget#navigation { background: transparent; border: none; outline: none; }

/* Tables */
QTreeWidget, QTreeView { background: transparent; color: @text@; border: none; outline: none; font-size: 10pt;
    selection-background-color: @selection@; selection-color: @text@; }
QTreeView::item { min-height: 25px; padding: 2px 4px; border: none; border-bottom: 1px solid @border@; }
QTreeView::item:hover { background: @raised@; }
QTreeView::item:selected { background: @selection@; color: @text@; }
QTreeView::item:disabled { color: @muted@; }
QHeaderView { background: transparent; }
QHeaderView::section { background: transparent; color: @muted@; border: none; border-bottom: 1px solid @border@; padding: 5px 6px; font-size: 9.5pt; font-weight: 500; }
QListWidget#customApps { background: @raised@; color: @text@; border: 1px solid @border@; border-radius: 6px; outline: none; }
QListWidget#customApps::item { padding: 5px 8px; }
QListWidget#customApps::item:selected { background: @selection@; color: @text@; }
QTextBrowser#incidentDetail { color: @text@; background: transparent; border: none; selection-background-color: @selection@; }

/* Controls */
QPushButton, QToolButton, QComboBox { color: @text@; background: @raised@; border: 1px solid @border@; border-radius: 6px; padding: 4px 11px; min-height: 19px; }
QPushButton:hover, QToolButton:hover, QComboBox:hover { border-color: @muted@; }
QPushButton:pressed, QToolButton:pressed { background: @selection@; }
QPushButton:focus, QToolButton:focus, QComboBox:focus { border-color: @accent@; }
QPushButton:disabled, QToolButton:disabled { color: @muted@; background: transparent; }
QPushButton[primary="true"] { background: @accent@; color: @background@; border-color: @accent@; font-weight: 600; }
QPushButton[primary="true"]:hover { border-color: @text@; }
QPushButton[primary="true"]:disabled { background: @raised@; color: @muted@; border-color: @border@; }
QPushButton[danger="true"] { color: @critical@; }
QPushButton[danger="true"]:hover { border-color: @critical@; }
QPushButton[danger="true"]:disabled { color: @muted@; }
QPushButton[tab="true"] { background: transparent; color: @secondary@; border: none; border-bottom: 2px solid transparent; border-radius: 0; padding: 5px 1px; }
QPushButton[tab="true"]:hover { color: @text@; }
QPushButton[tab="true"]:checked { color: @text@; border-bottom-color: @accent@; font-weight: 600; }
QPushButton[tab="true"]:focus { border-top: none; }
QToolButton[ghost="true"], QToolButton[quick="true"] { background: transparent; border: 1px solid transparent; padding: 3px; }
QToolButton[ghost="true"]:hover, QToolButton[quick="true"]:hover { background: @raised@; }
QToolButton[ghost="true"]:pressed, QToolButton[quick="true"]:pressed { background: @selection@; }
QTreeView QToolButton[ghost="true"] { padding: 1px 3px; }
QToolButton[disclosure="true"] { background: transparent; border: none; color: @secondary@; padding: 2px 0; font-size: 9.5pt; }
QToolButton[disclosure="true"]:hover { color: @text@; }
QToolButton::menu-indicator { image: none; width: 0; }
QComboBox { padding-right: 22px; }
QComboBox::drop-down { border: none; width: 20px; }
QComboBox::down-arrow { image: url(:/devstats/chevron-@scheme@.png); width: 10px; height: 6px; margin-right: 6px; }
QComboBox QAbstractItemView { background: @raised@; color: @text@; border: 1px solid @border@; padding: 3px; outline: none; selection-background-color: @selection@; selection-color: @text@; }
QLineEdit, QSpinBox { color: @text@; background: @raised@; border: 1px solid @border@; border-radius: 6px; padding: 4px 8px; min-height: 19px; selection-background-color: @selection@; selection-color: @text@; }
QLineEdit:focus, QSpinBox:focus { border-color: @accent@; }
QLineEdit:disabled, QSpinBox:disabled { color: @muted@; background: transparent; }
QSpinBox::up-button, QSpinBox::down-button { width: 0; border: none; }
QSpinBox::up-arrow, QSpinBox::down-arrow { image: none; width: 0; height: 0; }
QRadioButton { color: @text@; spacing: 7px; }
QRadioButton::indicator { width: 14px; height: 14px; border-radius: 8px; border: 1px solid @muted@; background: transparent; }
QRadioButton::indicator:checked { border: 4px solid @accent@; width: 8px; height: 8px; background: @background@; }
QProgressBar { background: @raised@; color: @text@; border: none; border-radius: 4px; text-align: center; font-size: 9pt; }
QProgressBar::chunk { background: @selection@; border-radius: 4px; }
QProgressBar[thin="true"] { max-height: 4px; min-height: 4px; border-radius: 2px; }
QProgressBar[thin="true"]::chunk { background: @accent@; border-radius: 2px; }

/* Scrollbars, menus, tooltips */
QScrollBar:vertical { background: transparent; width: 8px; margin: 2px 0; }
QScrollBar::handle:vertical { background: @border@; border-radius: 3px; min-height: 28px; }
QScrollBar::handle:vertical:hover { background: @muted@; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
QScrollBar:horizontal { background: transparent; height: 8px; margin: 0 2px; }
QScrollBar::handle:horizontal { background: @border@; border-radius: 3px; min-width: 28px; }
QScrollBar::handle:horizontal:hover { background: @muted@; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }
QScrollBar::add-page:horizontal, QScrollBar::sub-page:horizontal { background: transparent; }
QMenu { background: @raised@; color: @text@; border: 1px solid @border@; padding: 4px; }
QMenu::item { padding: 6px 16px; border-radius: 4px; }
QMenu::item:selected { background: @selection@; }
QMenu::item:disabled { color: @muted@; }
QMenu::separator { height: 1px; background: @border@; margin: 4px 6px; }
QToolTip { background: @raised@; color: @text@; border: 1px solid @border@; padding: 5px; }
)CSS");
    const QList<QPair<QString, QColor>> replacements{{"background", c.background}, {"surface", c.surface}, {"sidebar", c.sidebar},
        {"raised", c.raised}, {"border", c.border}, {"text", c.text}, {"secondary", c.secondary}, {"muted", c.muted},
        {"accentSoft", c.accentSoft}, {"accent", c.accent}, {"selection", c.selection},
        {"okSoft", c.okSoft}, {"ok", c.ok}, {"warnSoft", c.warnSoft}, {"warn", c.warn},
        {"criticalSoft", c.criticalSoft}, {"critical", c.critical}};
    for (const auto &entry : replacements) style.replace('@' + entry.first + '@', entry.second.name());
    style.replace(QStringLiteral("@scheme@"), dark ? QStringLiteral("dark") : QStringLiteral("light"));
    return style;
}
}
