#include "i18n.h"
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QByteArray>
#include <atomic>

namespace {
std::atomic<bool> english{false};

// Read once, on first use from any thread, and never changed afterwards.
const QHash<QString, QString> &catalog() {
    static const QHash<QString, QString> entries = [] {
        QHash<QString, QString> loaded;
        QFile file(QStringLiteral(":/devstats/i18n-en.json"));
        if (file.open(QIODevice::ReadOnly)) {
            const auto object = QJsonDocument::fromJson(file.readAll()).object();
            loaded.reserve(object.size());
            for (auto it = object.constBegin(); it != object.constEnd(); ++it) loaded.insert(it.key(), it.value().toString());
        }
        return loaded;
    }();
    return entries;
}
}

QString T(const char *chinese) {
    const QString source = QString::fromUtf8(chinese);
    if (!english.load(std::memory_order_relaxed)) return source;
    const auto found = catalog().constFind(source);
    // A text without an entry stays readable rather than empty; the catalog check fails the build's tests.
    return found == catalog().constEnd() ? source : found.value();
}

QString Tc(const char *context, const char *chinese) {
    const QString source = QString::fromUtf8(chinese);
    if (!english.load(std::memory_order_relaxed)) return source;
    const auto found = catalog().constFind(QLatin1Char('@') + QString::fromUtf8(context) + QLatin1Char('@') + source);
    return found == catalog().constEnd() ? source : found.value();
}

QString Tn(const char *chinese, qint64 count) {
    const QString text = T(chinese);
    const int bar = text.indexOf(QLatin1Char('|'));
    if (bar < 0) return text;
    return count == 1 ? text.left(bar) : text.mid(bar + 1);
}

void DevStatsLanguage::apply(int choice) {
    english.store(choice == English || (choice != Chinese && !systemIsChinese()), std::memory_order_relaxed);
}

bool DevStatsLanguage::isEnglish() { return english.load(std::memory_order_relaxed); }

bool DevStatsLanguage::systemIsChinese() {
    // What the desktop session set, read the way gettext programs read it: the locale is
    // LC_ALL, else LC_MESSAGES, else LANG; when it is a real locale, the first language
    // listed in LANGUAGE comes before it.
    QByteArray locale;
    for (const char *name : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        locale = qgetenv(name);
        if (!locale.isEmpty()) break;
    }
    if (locale.isEmpty() || locale == "C" || locale == "POSIX" || locale.startsWith("C.")) return false;
    const QByteArray preferred = qgetenv("LANGUAGE").split(':').value(0);
    return (preferred.isEmpty() ? locale : preferred).startsWith("zh");
}

QString DevStatsLanguage::tag() { return isEnglish() ? QStringLiteral("en") : QStringLiteral("zh-CN"); }
