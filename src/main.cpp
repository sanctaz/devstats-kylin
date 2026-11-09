#include <QApplication>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTimer>
#include <QTemporaryDir>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QListWidget>
#include <QComboBox>
#include <QImage>
#include <QPainter>
#include <QTextStream>
#include <csignal>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <memory>
#include "controller.h"
#include "monitor_window.h"
#include "linux_sampler.h"
#include "runtime_env.h"
#include "history_store.h"
#include "diagnostics.h"
#include "devstats_version.h"

namespace {
int signalWriter = -1;
void terminateSignal(int) {
    const int saved = errno;
    const char value = 1;
    if (signalWriter >= 0) { const auto ignored = ::write(signalWriter, &value, 1); (void)ignored; }
    errno = saved;
}
class TerminationSignals {
public:
    explicit TerminationSignals(QCoreApplication &app) {
        if (::pipe2(pipes_, O_CLOEXEC | O_NONBLOCK)) return;
        signalWriter = pipes_[1];
        struct sigaction action {};
        action.sa_handler = terminateSignal;
        sigemptyset(&action.sa_mask);
        for (int number : {SIGTERM, SIGINT, SIGHUP}) ::sigaction(number, &action, nullptr);
        notifier_.reset(new QSocketNotifier(pipes_[0], QSocketNotifier::Read));
        QObject::connect(notifier_.get(), &QSocketNotifier::activated, &app, [&app, this](int) {
            char buffer[64];
            while (::read(pipes_[0], buffer, sizeof(buffer)) > 0) {}
            app.quit();
        });
    }
    ~TerminationSignals() {
        signalWriter = -1;
        for (int number : {SIGTERM, SIGINT, SIGHUP}) ::signal(number, SIG_DFL);
        notifier_.reset();
        if (pipes_[0] >= 0) ::close(pipes_[0]);
        if (pipes_[1] >= 0) ::close(pipes_[1]);
    }
private:
    int pipes_[2] = {-1, -1};
    std::unique_ptr<QSocketNotifier> notifier_;
};

int snapshotMain(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    restoreDevStatsLaunchEnvironment();
    app.setApplicationName("DevStats");
    app.setOrganizationName("DevStats");
    TerminationSignals termination(app);
    LinuxSampler sampler(qEnvironmentVariable("DEVSTATS_PROC_ROOT", "/proc"),
                         qEnvironmentVariable("DEVSTATS_SYS_ROOT", "/sys"));
    sampler.setProjectRoots(qEnvironmentVariable("DEVSTATS_PROJECTS").split(':', QString::SkipEmptyParts));
    RecentActivityTracker activity;
    HistoryStore history(devStatsDataRoot());
    history.open();
    QJsonObject latest;
    QTimer::singleShot(2100, &app, [&] {
        latest = sampler.sample();
        activity.apply(latest);
        history.record(latest);
        latest["version"] = DEVSTATS_VERSION;
        latest["platform"] = "linux";
        latest["processSampleAgeSeconds"] = 0;
        latest["diagnostics"] = diagnoseSystem(latest);
        history.flush();
        QTextStream(stdout) << QJsonDocument(latest).toJson(QJsonDocument::Compact) << '\n';
        app.quit();
    });
    auto latestFirst = sampler.sample();
    activity.apply(latestFirst);
    history.record(latestFirst);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &app, [&] { history.flush(); });
    return app.exec();
}
}

int main(int argc, char **argv) {
    ::umask(0077);
    QStringList arguments;
    for (int i = 1; i < argc; ++i) arguments.append(QString::fromLocal8Bit(argv[i]));
    if (arguments.contains("--version") || arguments.contains("-v")) {
        QTextStream(stdout) << "DevStats " << DEVSTATS_VERSION << " (Linux ARM64)\n"; return 0;
    }
    if (arguments.contains("--help") || arguments.contains("-h")) {
        QTextStream(stdout)
            << "DevStats " << DEVSTATS_VERSION << " — Linux system monitor\n"
            << "  devstats                     Start in the system tray; reopen an existing instance\n"
            << "  devstats --show              Open the complete detail window\n"
            << "  devstats --background        Tray only, including login startup\n"
            << "  devstats --snapshot          Read two real system samples as JSON, no display required\n"
            << "  devstats --smoke-test [--render-preview DIRECTORY]\n";
        return 0;
    }
    if (arguments.contains("--snapshot")) return snapshotMain(argc, argv);
    const bool smoke = arguments.contains("--smoke-test") || arguments.contains("--render-preview");
    std::unique_ptr<QTemporaryDir> testData;
    if (smoke) {
        testData.reset(new QTemporaryDir());
        if (!testData->isValid()) return 74;
        qputenv("DEVSTATS_DATA_DIR", QFile::encodeName(testData->path() + "/data"));
        qputenv("XDG_CONFIG_HOME", QFile::encodeName(testData->path() + "/config"));
    }
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication app(argc, argv);
    restoreDevStatsLaunchEnvironment();
    app.setApplicationName("DevStats");
    app.setOrganizationName("DevStats");
    app.setApplicationVersion(DEVSTATS_VERSION);
    app.setQuitOnLastWindowClosed(false);
    TerminationSignals termination(app);
    const QString runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    const QString socketName = runtime + (smoke ? "/devstats-test-" + QString::number(::getpid()) : "/devstats") + ".socket";
    const QString lockName = socketName + ".lock";
    QLocalSocket existing;
    existing.connectToServer(socketName);
    if (existing.waitForConnected(250)) {
        const QByteArray message = arguments.contains("--background") ? QByteArray("ping\n") : QByteArray("show\n");
        existing.write(message); existing.waitForBytesWritten(500); return 0;
    }
    QLockFile lock(lockName);
    if (!lock.tryLock(0)) {
        QTextStream(stderr) << "DevStats is already starting/running. Try devstats --show again.\n"; return 1;
    }
    QLocalServer server;
    server.setSocketOptions(QLocalServer::UserAccessOption);
    QLocalServer::removeServer(socketName); // Only after acquiring this user's live-instance lock.
    if (!server.listen(socketName)) { QTextStream(stderr) << server.errorString() << '\n'; return 1; }
    AppController controller;
    QObject::connect(&app, &QApplication::aboutToQuit, &controller, &AppController::stop);
    QObject::connect(&server, &QLocalServer::newConnection, &app, [&] {
        while (server.hasPendingConnections()) {
            auto *socket = server.nextPendingConnection();
            QObject::connect(socket, &QLocalSocket::readyRead, &controller, [&, socket] {
                if (!socket->canReadLine()) return;
                const auto message = socket->readLine(128).trimmed();
                if (message == "show") controller.showModule();
                socket->disconnectFromServer();
            });
            QObject::connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
        }
    });
    controller.start(smoke || arguments.contains("--show"));
    int result = 0;
    if (smoke) {
        auto finished = std::make_shared<bool>(false);
        const int renderIndex = arguments.indexOf("--render-preview");
        const QString renderDirectory = renderIndex >= 0 ? arguments.value(renderIndex + 1) : QString();
        QObject::connect(&controller, &AppController::snapshotUpdated, &app, [&, finished, renderDirectory] {
            if (*finished) return;
            const auto snapshot = controller.latestSnapshot();
            if (!snapshot.value("cpu").toObject().value("usage").isDouble()) return;
            auto *window = controller.window();
            if (!window) return;
            *finished = true;
            const QStringList modules = {"overview", "cpu", "memory", "disk", "network", "battery", "diagnostics", "automation", "projects"};
            if (!renderDirectory.isEmpty() && !QDir().mkpath(renderDirectory)) { result = 74; app.quit(); return; }
            bool valid = window->findChild<QListWidget *>("navigation") != nullptr
                && window->findChild<QComboBox *>("historyRange") != nullptr;
            QMap<QString, QImage> previews;
            for (const auto &module : modules) {
                window->setSelectedModule(module);
                QApplication::processEvents();
                valid = valid && window->selectedModule() == module;
                if (!renderDirectory.isEmpty()) {
                    QImage image(window->size(), QImage::Format_ARGB32_Premultiplied);
                    image.fill(Qt::transparent); window->render(&image);
                    valid = image.save(renderDirectory + "/" + module + ".png") && valid;
                    if (module == "overview" || module == "battery") previews[module] = image;
                }
            }
            if (previews.size() == 2) {
                QImage contact(1380, 555, QImage::Format_ARGB32_Premultiplied); contact.fill(Qt::white);
                QPainter painter(&contact);
                painter.drawImage(QRect(0, 0, 690, 555), previews.value("overview"));
                painter.drawImage(QRect(690, 0, 690, 555), previews.value("battery"));
                painter.end(); contact.save(renderDirectory + "/contact-sheet.png");
            }
            window->close();
            QTimer::singleShot(20, &app, [&, valid] {
                result = valid ? 0 : 3;
                QTextStream(stdout) << (valid ? "PASS" : "FAIL") << ": final app native samples, nine modules, history controls, render and close-to-tray lifecycle\n";
                app.quit();
            });
        });
        QTimer::singleShot(18000, &app, [&, finished] {
            if (!*finished) { result = 4; QTextStream(stderr) << "Smoke test timed out\n"; app.quit(); }
        });
    }
    const int eventResult = app.exec();
    controller.stop();
    server.close();
    return result ? result : eventResult;
}
