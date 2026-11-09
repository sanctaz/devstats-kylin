#include "runtime_env.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    restoreDevStatsLaunchEnvironment();
    QJsonObject result;
    bool leaked = qEnvironmentVariableIsSet("DEVSTATS_LAUNCHER_PATH");
    for (const char *name : {"LD_LIBRARY_PATH", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
                             "QT_QPA_PLATFORMTHEME", "QML2_IMPORT_PATH"}) {
        result[name] = qEnvironmentVariableIsSet(name) ? QJsonValue(qEnvironmentVariable(name)) : QJsonValue(QJsonValue::Null);
        const QByteArray backup = QByteArray("DEVSTATS_ORIGINAL_") + name;
        leaked = leaked || qEnvironmentVariableIsSet(backup.constData())
            || qEnvironmentVariableIsSet((backup + "_SET").constData());
    }
    result["launcher"] = devStatsLauncherPath();
    result["backupLeaked"] = leaked;
    // The second call must preserve the already restored environment/path.
    restoreDevStatsLaunchEnvironment();
    result["launcherStable"] = devStatsLauncherPath() == result["launcher"].toString();
    QTextStream(stdout) << QJsonDocument(result).toJson(QJsonDocument::Compact) << '\n';
}
