#include "../src/controller.h"
#include "../src/monitor_window.h"
#include "../src/ui_theme.h"
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QSettings>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <cstdio>
#include <cstdlib>
#include <functional>

// The interface appearance is the user's choice: it must survive closing the
// window, the window being released while hidden, and a restart.
namespace {
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL appearance controller: %s\n", message); std::exit(1); }
}
void drain() {
    for (int pass = 0; pass < 6; ++pass) { QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete); QApplication::processEvents(); }
}
bool waitFor(const std::function<bool()> &test) {
    QElapsedTimer clock; clock.start();
    while (!test() && clock.elapsed() < 8000) { QEventLoop loop; QTimer::singleShot(20, &loop, &QEventLoop::quit); loop.exec(); }
    return test();
}
bool dark(QWidget *widget) { return DevStatsUI::isDark(widget->palette()); }
// What the user sees, not just the stored mode: the window background itself.
bool darkBackground(QWidget *widget) {
    // Rendered pixels, so a style sheet that disagrees with the palette is caught too.
    const QImage image = widget->grab().toImage();
    const QColor content = image.pixelColor(image.width() - 12, image.height() - 12);
    const QColor sidebar = image.pixelColor(12, image.height() / 2);
    const bool paletteDark = widget->palette().color(QPalette::Window).lightness() < 128;
    const bool pixelsDark = content.lightness() < 128 && sidebar.lightness() < 128;
    const bool pixelsLight = content.lightness() >= 128 && sidebar.lightness() >= 128;
    check(paletteDark ? pixelsDark : pixelsLight, "what is drawn matches the chosen appearance");
    return paletteDark;
}
// Every card on every page, sampled inside its padding where the user sees its background.
void checkCards(MonitorWindow *window, bool expectDark, const char *what) {
    int cards = 0;
    QStringList wrong;
    for (const QString module : {"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics", "automation", "projects"}) {
        window->setSelectedModule(module); drain();
        // The whole page, including what is scrolled below the window.
        auto *page = window->findChild<QWidget *>(module + "Page");
        const QImage image = page->grab().toImage();
        for (auto *frame : page->findChildren<QFrame *>()) {
            if (!frame->isVisible() || !frame->property("card").toBool() || frame->property("tone").isValid()) continue;
            const QPoint at = frame->mapTo(page, QPoint(8, frame->height() / 2));
            if (!image.rect().contains(at)) continue;
            ++cards;
            if ((image.pixelColor(at).lightness() < 128) != expectDark) {
                auto *title = frame->findChild<QLabel *>();
                wrong << module + ":" + (title ? title->text() : frame->objectName());
            }
        }
    }
    window->setSelectedModule("overview"); drain();
    if (!wrong.isEmpty()) std::fprintf(stderr, "cards with the wrong appearance: %s\n", qPrintable(wrong.join(", ")));
    if (cards < 12) std::fprintf(stderr, "only %d cards found\n", cards);
    check(cards >= 12 && wrong.isEmpty(), what);
}
QAction *openAction() {
    for (auto *widget : QApplication::topLevelWidgets()) if (auto *menu = qobject_cast<QMenu *>(widget))
        for (auto *action : menu->actions()) if (action->text() == QStringLiteral("打开 DevStats")) return action;
    return nullptr;
}
void choose(MonitorWindow *window, int mode) {
    window->findChild<QToolButton *>("settingsButton")->click(); drain();
    auto *combo = window->findChild<QComboBox *>("appearanceMode");
    check(combo != nullptr, "settings offer the appearance");
    combo->setCurrentIndex(mode); drain();
    if (auto *dialog = window->findChild<QDialog *>("preferencesDialog")) dialog->close();
    drain();
}
// The file on disk, not QSettings: within one process QSettings shares unsaved values.
int stored(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return -1;
    for (const auto &line : QString::fromUtf8(file.readAll()).split('\n'))
        if (line.startsWith(QStringLiteral("appearance="))) return line.mid(11).trimmed().toInt();
    return -1;
}
void release(AppController &controller) {
    QPointer<MonitorWindow> window = controller.window();
    window->hide(); drain();
    auto *ttl = controller.findChild<QTimer *>("windowReleaseTimer");
    ttl->stop(); QMetaObject::invokeMethod(ttl, "timeout", Qt::DirectConnection); drain();
    check(!window, "a hidden window is released");
}
}

int main(int argc, char **argv) {
    QTemporaryDir root; check(root.isValid(), "isolated root");
    qputenv("XDG_CONFIG_HOME", (root.path() + "/config").toUtf8()); qputenv("XDG_DATA_HOME", (root.path() + "/share").toUtf8());
    qputenv("DEVSTATS_DATA_DIR", (root.path() + "/data").toUtf8());
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); qRegisterMetaType<QJsonArray>(); qRegisterMetaType<QJsonObject>();
    QDir().mkpath(root.path() + "/data");
    const QString settingsPath = root.path() + "/config/devstats/settings.ini";
    check(!DevStatsUI::isDark(app.palette()), "the test system palette is light, so following it gives light");
    {
        AppController controller; controller.start(true); drain();
        auto *window = controller.window();
        check(window && window->isVisible() && dark(window) && darkBackground(window), "a first start is dark");
        choose(window, 2);
        check(!dark(window) && !darkBackground(window), "light applies at once");
        checkCards(window, false, "every card on every page turns light at once");
        check(waitFor([&] { return stored(settingsPath) == 2; }), "light is saved");
        window->close(); drain();
        check(openAction() != nullptr, "the tray menu can reopen the window");
        openAction()->trigger(); drain();
        check(controller.window()->isVisible() && !darkBackground(controller.window()), "reopening keeps light");
        checkCards(controller.window(), false, "every card is light after reopening");
        release(controller);
        openAction()->trigger(); drain();
        check(controller.window() && !dark(controller.window()) && !darkBackground(controller.window()), "a released window comes back light");
        checkCards(controller.window(), false, "every card is light in a recreated window");
        controller.stop();
    }
    drain();
    check(stored(settingsPath) == 2, "light is still saved after quitting");
    {
        AppController controller; controller.start(true); drain();
        check(controller.window() && !dark(controller.window()) && !darkBackground(controller.window()), "a restart keeps light");
        checkCards(controller.window(), false, "every card is light after a restart");
        // Kept as evidence: what a restart in light looks like.
        const QString evidence = qEnvironmentVariable("DEVSTATS_TEST_EVIDENCE");
        if (!evidence.isEmpty()) controller.window()->findChild<QWidget *>("overviewPage")->grab().save(evidence + "/restart-light-overview.png");
        choose(controller.window(), 0);
        check(!darkBackground(controller.window()), "following a light system gives light");
        controller.stop();
    }
    drain();
    {
        AppController controller; controller.start(true); drain();
        check(stored(settingsPath) == 0 && !darkBackground(controller.window()), "a restart keeps following the system");
        choose(controller.window(), 1);
        controller.stop();
    }
    drain();
    {
        AppController controller; controller.start(true); drain();
        check(stored(settingsPath) == 1 && darkBackground(controller.window()), "a restart keeps dark");
        checkCards(controller.window(), true, "every card is dark after a restart in dark");
        controller.stop();
    }
    drain();
    // A settings file that cannot be written is reported, once, instead of the choice
    // silently coming back as dark after the next start.
    const QString folder = QFileInfo(settingsPath).path();
    {
        QFile::setPermissions(settingsPath, QFileDevice::ReadOwner);
        QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        AppController controller; controller.start(true); drain();
        QStringList warnings;
        QTimer watcher;
        QObject::connect(&watcher, &QTimer::timeout, [&] {
            auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
            if (!box) return;
            warnings << box->text();
            box->accept();
        });
        watcher.start(10);
        choose(controller.window(), 2);
        check(warnings.size() == 1 && warnings.first().contains(QStringLiteral("设置无法保存")) && warnings.first().contains(settingsPath), "a failed save is reported with the file");
        check(!darkBackground(controller.window()), "the choice still applies for this run");
        choose(controller.window(), 0);
        check(warnings.size() == 1, "the warning is not repeated");
        watcher.stop();
        controller.stop();
    }
    drain();
    // Restored only after the controller is gone, which retries its pending write.
    QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QFile::setPermissions(settingsPath, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    check(stored(settingsPath) == 1, "nothing was written while the file was read-only");
    std::printf("PASS appearance controller: %d checks; light, follow-system and dark survive reopening, window release and restart on every card of every page; an unwritable settings file is reported once\n", checks);
    return 0;
}
