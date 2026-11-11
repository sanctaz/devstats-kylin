// The interface language: what the catalog returns, what "follow the system" means,
// and that a choice in the settings rebuilds what is on screen and survives a restart.
#include "../src/controller.h"
#include "../src/i18n.h"
#include "../src/monitor_window.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <functional>

namespace {
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL language: %s\n", message); std::exit(1); }
}
void drain() {
    for (int pass = 0; pass < 6; ++pass) { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); }
}
bool waitFor(const std::function<bool()> &test) {
    QElapsedTimer clock; clock.start();
    while (!test() && clock.elapsed() < 12000) { QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec(); }
    return test();
}
bool hasChinese(const QString &text) {
    static const QRegularExpression chinese(QStringLiteral("[\\x{3400}-\\x{9fff}\\x{3000}-\\x{303f}\\x{ff00}-\\x{ffef}]"));
    return text.contains(chinese);
}
QString title(MonitorWindow *window) {
    auto *label = window ? window->findChild<QLabel *>("moduleTitle") : nullptr;
    return label ? label->text() : QString();
}
QString actionText(const char *name) {
    for (auto *widget : QApplication::topLevelWidgets()) if (auto *menu = qobject_cast<QMenu *>(widget))
        if (auto *action = menu->findChild<QAction *>(name, Qt::FindDirectChildrenOnly)) return action->text();
    return QString();
}
// The file on disk, not QSettings: within one process QSettings shares unsaved values.
int stored(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n'))
        if (line.startsWith(QStringLiteral("language="))) return line.mid(9).trimmed().toInt();
    return -1;
}
// Chooses a language in the settings dialog and waits for the window built in it.
MonitorWindow *choose(AppController &controller, int mode, const QString &expectedTitle) {
    controller.window()->showSettings(); drain();
    auto *combo = controller.window()->findChild<QComboBox *>("languageMode");
    check(combo && combo->count() == 3, "the settings offer the language");
    check(combo->itemText(1) == QString::fromUtf8("中文") && combo->itemText(2) == QStringLiteral("English"), "each language is named in itself");
    const QPointer<MonitorWindow> before = controller.window();
    combo->setCurrentIndex(mode);
    check(waitFor([&] { return before.isNull() && controller.window() && controller.window()->isVisible() && title(controller.window()) == expectedTitle; }),
          "the window is built again in the chosen language");
    check(controller.window()->settingsVisible(), "the settings dialog comes back after the change");
    auto *again = controller.window()->findChild<QComboBox *>("languageMode");
    check(again && again->currentIndex() == mode, "the settings show the choice");
    return controller.window();
}
void system(const char *language, const char *all, const char *messages, const char *lang) {
    const auto set = [](const char *name, const char *value) { if (value) qputenv(name, value); else qunsetenv(name); };
    set("LANGUAGE", language); set("LC_ALL", all); set("LC_MESSAGES", messages); set("LANG", lang);
}
}

int main(int argc, char **argv) {
    QTemporaryDir root; check(root.isValid(), "isolated root");
    qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8()); qputenv("XDG_DATA_HOME", (root.path() + "/share").toUtf8());
    qputenv("DEVSTATS_DATA_DIR", (root.path() + "/data").toUtf8());
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    QDir().mkpath(root.path() + "/data");
    const QString settingsPath = root.path() + "/config/devstats/settings.ini";

    // What the catalog returns.
    DevStatsLanguage::apply(DevStatsLanguage::Chinese);
    check(!DevStatsLanguage::isEnglish() && T("总览") == QString::fromUtf8("总览") && DevStatsLanguage::tag() == "zh-CN", "the source is in Chinese");
    check(Tn("%1 个进程", 1).arg(1) == QString::fromUtf8("1 个进程"), "Chinese has one form for any count");
    check(Tc("问题", "持续") == QString::fromUtf8("持续") && Tc("规则", "持续") == QString::fromUtf8("持续"), "in Chinese a use does not change the words");
    DevStatsLanguage::apply(DevStatsLanguage::English);
    check(DevStatsLanguage::isEnglish() && T("总览") == "Overview" && DevStatsLanguage::tag() == "en", "English comes from the catalog");
    check(T("%1 核 · %2").arg("8", "16 GB") == "8 cores · 16 GB", "placeholders are kept");
    check(Tn("%1 个进程", 1).arg(1) == "1 process" && Tn("%1 个进程", 3).arg(3) == "3 processes" && Tn("%1 个连接", 0).arg(0) == "0 connections",
          "a count chooses between one and many");
    check(T("这句话不在词表里") == QString::fromUtf8("这句话不在词表里"), "a text without an entry stays readable");
    check(T("持续") == "Duration" && Tc("问题", "持续") == "Sustained" && Tc("规则", "持续") == "for", "one Chinese word, a wording per use");
    check(Tc("没有这个语境", "持续") == QString::fromUtf8("持续"), "an unknown use stays readable");

    // What "follow the system" means: the first language of LANGUAGE, else the locale
    // of LC_ALL, LC_MESSAGES or LANG; the C locale and no locale at all are English.
    system(nullptr, nullptr, nullptr, "zh_CN.UTF-8"); check(DevStatsLanguage::systemIsChinese(), "LANG=zh_CN.UTF-8 is Chinese");
    system("zh_CN:zh", nullptr, nullptr, "zh_CN.UTF-8"); check(DevStatsLanguage::systemIsChinese(), "the usual Kylin session is Chinese");
    system(nullptr, nullptr, nullptr, "zh_TW.UTF-8"); check(DevStatsLanguage::systemIsChinese(), "traditional Chinese gets Chinese");
    system(nullptr, nullptr, nullptr, "en_US.UTF-8"); check(!DevStatsLanguage::systemIsChinese(), "an English system is not Chinese");
    system(nullptr, nullptr, nullptr, "ja_JP.UTF-8"); check(!DevStatsLanguage::systemIsChinese(), "any other language gets English");
    system("zh_CN:en", nullptr, nullptr, "en_US.UTF-8"); check(DevStatsLanguage::systemIsChinese(), "LANGUAGE comes before LANG");
    system("en_US:zh_CN", nullptr, nullptr, "zh_CN.UTF-8"); check(!DevStatsLanguage::systemIsChinese(), "the first language of LANGUAGE counts");
    system(nullptr, "en_US.UTF-8", nullptr, "zh_CN.UTF-8"); check(!DevStatsLanguage::systemIsChinese(), "LC_ALL comes before LANG");
    system(nullptr, nullptr, "zh_CN.UTF-8", "en_US.UTF-8"); check(DevStatsLanguage::systemIsChinese(), "LC_MESSAGES comes before LANG");
    system("zh_CN", nullptr, nullptr, "C"); check(!DevStatsLanguage::systemIsChinese(), "the C locale is English whatever LANGUAGE says");
    system(nullptr, nullptr, nullptr, nullptr); check(!DevStatsLanguage::systemIsChinese(), "no locale at all is English");
    DevStatsLanguage::apply(DevStatsLanguage::FollowSystem);
    check(DevStatsLanguage::isEnglish(), "following a system without a locale gives English");
    system("zh_CN:zh", nullptr, nullptr, "zh_CN.UTF-8");
    DevStatsLanguage::apply(DevStatsLanguage::FollowSystem);
    check(!DevStatsLanguage::isEnglish(), "following a Chinese system gives Chinese");

    // A choice in the settings, on a Chinese system.
    {
        AppController controller; controller.start(true); drain();
        check(controller.window() && title(controller.window()) == QString::fromUtf8("总览") && actionText("openAction") == QString::fromUtf8("打开 DevStats"),
              "a first start follows the system");
        auto *window = choose(controller, DevStatsLanguage::English, QStringLiteral("Overview"));
        check(actionText("openAction") == "Open DevStats" && actionText("quitAction") == "Quit DevStats" && actionText("diagnoseAction") == "One-click diagnosis",
              "the tray menu follows");
        auto *navigation = window->findChild<QListWidget *>();
        bool english = navigation && navigation->count() > 0;
        for (int row = 0; navigation && row < navigation->count(); ++row) english &= !hasChinese(navigation->item(row)->text());
        check(english, "the navigation is in English");
        check(waitFor([&] { return stored(settingsPath) == DevStatsLanguage::English; }), "the choice is saved");
        check(waitFor([&] {
            const auto diagnosis = controller.latestSnapshot().value("diagnostics").toObject();
            return !diagnosis.value("headline").toString().isEmpty() && !hasChinese(diagnosis.value("headline").toString())
                && !hasChinese(QString::fromUtf8(QJsonDocument(diagnosis.value("coverage").toArray()).toJson()));
        }), "the next diagnosis is worded in English");
        controller.stop();
    }
    drain();
    {
        AppController controller; controller.start(true); drain();
        check(DevStatsLanguage::isEnglish() && title(controller.window()) == "Overview" && actionText("openAction") == "Open DevStats", "a restart keeps English");
        choose(controller, DevStatsLanguage::FollowSystem, QString::fromUtf8("总览"));
        check(actionText("openAction") == QString::fromUtf8("打开 DevStats"), "back to the system's Chinese");
        check(waitFor([&] { return stored(settingsPath) == DevStatsLanguage::FollowSystem; }), "following the system is saved");
        // Choosing the language the system already has changes the setting, not the screen.
        const QPointer<MonitorWindow> same = controller.window();
        controller.window()->showSettings(); drain();
        controller.window()->findChild<QComboBox *>("languageMode")->setCurrentIndex(DevStatsLanguage::Chinese);
        check(waitFor([&] { return stored(settingsPath) == DevStatsLanguage::Chinese; }), "an explicit Chinese is saved");
        drain();
        check(!same.isNull() && same == controller.window() && title(controller.window()) == QString::fromUtf8("总览"), "the window is not rebuilt for the same language");
        controller.stop();
    }
    drain();
    DevStatsLanguage::apply(DevStatsLanguage::Chinese);
    std::printf("PASS language: %d checks; catalog, system language, change in settings, restart\n", checks);
    return 0;
}
