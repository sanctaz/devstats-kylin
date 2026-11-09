#pragma once
#include <QColor>
#include <QPalette>
#include <QString>

// One source for every colour and text size in the application window and its
// dialogs. Status colours are reserved for status: blue means "you can act on
// this", green / amber / red mean healthy / needs attention / serious.
namespace DevStatsUI {
struct Colors {
    QColor background, sidebar, surface, raised, border;
    QColor text, secondary, muted, accent, accentSoft, selection;
    QColor ok, okSoft, warn, warnSoft, critical, criticalSoft;
};
inline bool isDark(const QPalette &palette) { return palette.color(QPalette::Window).lightness() < 128; }
inline Colors colors(bool dark) {
    if (dark) return {QColor("#15171b"), QColor("#101216"), QColor("#1c1f25"), QColor("#262a31"), QColor("#2e333b"),
        QColor("#eceef2"), QColor("#a9b1bd"), QColor("#7a828e"), QColor("#5aa2ff"), QColor("#1b2b40"), QColor("#253954"),
        QColor("#41c98b"), QColor("#163026"), QColor("#f0b545"), QColor("#352a14"), QColor("#ff6f6a"), QColor("#3a1f1e")};
    return {QColor("#f3f4f7"), QColor("#e9ebef"), QColor("#ffffff"), QColor("#f0f2f5"), QColor("#dfe2e8"),
        QColor("#1b2028"), QColor("#565f6c"), QColor("#858d99"), QColor("#0b6bdb"), QColor("#e4effc"), QColor("#d9e8fb"),
        QColor("#14935a"), QColor("#e2f5eb"), QColor("#a96f04"), QColor("#faf0d9"), QColor("#cf3a31"), QColor("#fbe8e6")};
}
inline Colors colors(const QPalette &palette) { return colors(isDark(palette)); }
inline QPalette palette(bool dark) {
    const auto c = colors(dark);
    QPalette p;
    p.setColor(QPalette::Window, c.background); p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.surface); p.setColor(QPalette::AlternateBase, c.raised);
    p.setColor(QPalette::Text, c.text); p.setColor(QPalette::Button, c.raised); p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::Highlight, c.selection); p.setColor(QPalette::HighlightedText, c.text);
    p.setColor(QPalette::ToolTipBase, c.raised); p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::Midlight, c.border); p.setColor(QPalette::Mid, c.border); p.setColor(QPalette::Dark, c.border);
    p.setColor(QPalette::PlaceholderText, c.muted);
    p.setColor(QPalette::Disabled, QPalette::Text, c.muted); p.setColor(QPalette::Disabled, QPalette::ButtonText, c.muted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.muted);
    return p;
}
// tone: "ok", "warn", "critical", "info" or anything else for neutral.
QColor toneColor(const Colors &c, const QString &tone);
QString styleSheet(bool dark);
}
