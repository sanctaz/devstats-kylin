#include "runtime_env.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool LaunchEnvironmentPrepared = false;
QString LauncherPath;
}

void restoreDevStatsLaunchEnvironment() {
    if (LaunchEnvironmentPrepared) return;
    LaunchEnvironmentPrepared = true;
    LauncherPath = QCoreApplication::applicationFilePath();
    const QString candidate = qEnvironmentVariable("DEVSTATS_LAUNCHER_PATH");
    const QFileInfo wrapper(candidate);
    const QString canonical = wrapper.canonicalFilePath();
    struct stat info {}, bundled {}, running {};
    const QByteArray bundledPath = QFile::encodeName(QFileInfo(canonical).dir().filePath(QStringLiteral("libexec/devstats")));
    const QByteArray runningPath = QFile::encodeName(QCoreApplication::applicationFilePath());
    const bool trusted = QDir::isAbsolutePath(candidate) && !canonical.isEmpty()
        && wrapper.fileName() == QStringLiteral("devstats") && wrapper.isExecutable()
        && ::stat(QFile::encodeName(canonical).constData(), &info) == 0
        && S_ISREG(info.st_mode) && info.st_uid == getuid() && !(info.st_mode & 0022)
        && ::stat(bundledPath.constData(), &bundled) == 0 && ::stat(runningPath.constData(), &running) == 0
        && S_ISREG(bundled.st_mode) && bundled.st_dev == running.st_dev && bundled.st_ino == running.st_ino;
    if (trusted) {
        LauncherPath = canonical;
        QCoreApplication::setLibraryPaths({QFileInfo(canonical).dir().filePath(QStringLiteral("plugins"))});
    }
    for (const char *name : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                             "QT_QPA_PLATFORMTHEME", "QML2_IMPORT_PATH"}) {
        const QByteArray backup = QByteArray("DEVSTATS_ORIGINAL_") + name;
        const QByteArray present = qgetenv((backup + "_SET").constData());
        if (trusted && present == "1") qputenv(name, qgetenv(backup.constData()));
        else if (trusted && present == "0") qunsetenv(name);
        qunsetenv(backup.constData());
        qunsetenv((backup + "_SET").constData());
    }
    qunsetenv("DEVSTATS_LAUNCHER_PATH");
}

QString devStatsLauncherPath() {
    restoreDevStatsLaunchEnvironment();
    return LauncherPath;
}

QString devStatsDataRoot() {
    const QString override = qEnvironmentVariable("DEVSTATS_DATA_DIR");
    if (!override.isEmpty()) return QDir::cleanPath(QDir::current().absoluteFilePath(override));
    QString xdg = qEnvironmentVariable("XDG_DATA_HOME");
    if (xdg.isEmpty() || !QDir::isAbsolutePath(xdg)) xdg = QDir::home().filePath(QStringLiteral(".local/share"));
    return QDir(xdg).filePath(QStringLiteral("devstats"));
}
