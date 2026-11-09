#include "activity_tracker.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/resource.h>

namespace {
void require(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL activity benchmark: %s\n", message); std::exit(1); }
}
void writeFile(const QString &path, const QByteArray &data) {
    require(QDir().mkpath(QFileInfo(path).absolutePath()), "create fixture directory");
    QFile file(path);
    require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "write fixture");
}
void prepare(const QString &root) {
    const QString shell = root + "/shells/bash";
    writeFile(shell, "fixture shell\n");
    QByteArray jsonLines;
    for (int app = 0; app < 32; ++app) {
        const QString directory = root + QString("/apps/pkg-%1").arg(app, 2, 10, QLatin1Char('0'));
        const QString executable = directory + "/editor";
        const QString alias = directory + "/editor-alias";
        const QString command = root + QString("/commands/command-%1").arg(app);
        writeFile(executable, "fixture executable\n"); writeFile(command, "fixture command\n");
        if (!QFileInfo::exists(alias)) require(QFile::link(executable, alias), "create fixture alias");
        for (int helper = 0; helper < 4; ++helper) writeFile(directory + QString("/lib/helper-%1").arg(helper), "fixture helper\n");
        writeFile(root + QString("/applications/editor-%1.desktop").arg(app),
            "[Desktop Entry]\nType=Application\nName=Fixture Editor " + QByteArray::number(app) + "\nExec=" + executable.toUtf8() + "\n");
        const int basePid = 1000 + app * 50;
        for (int member = 0; member < 50; ++member) {
            QString path = member % 2 ? alias : executable;
            int parent = member ? basePid : 0;
            if (member >= 32 && member < 48) {
                path = directory + QString("/lib/helper-%1").arg(member % 4);
                parent = member % 4 ? basePid + member - 1 : basePid;
            } else if (member == 48) path = shell;
            else if (member == 49) { path = command; parent = basePid + 48; }
            QJsonObject row {{"pid", basePid + member}, {"parentPID", parent}, {"startTicks", 50000 + basePid + member},
                {"executablePath", path}, {"name", QFileInfo(path).fileName()},
                {"cpuPercent", member % 17 ? QJsonValue((member % 7) * .125) : QJsonValue(QJsonValue::Null)},
                {"ioReadBytesPerSecond", member * 100.0}, {"ioWriteBytesPerSecond", member % 13 ? QJsonValue(member * 40.0) : QJsonValue(QJsonValue::Null)}};
            jsonLines += QJsonDocument(row).toJson(QJsonDocument::Compact) + '\n';
        }
    }
    writeFile(root + "/processes.jsonl", jsonLines);
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const auto arguments = app.arguments();
    QString root;
    int warmup = 32, scans = 200;
    bool makeFixture = false, trace = false;
    for (int i = 1; i < arguments.size(); ++i) {
        const auto argument = arguments.at(i);
        if (argument.startsWith("--fixture=")) root = argument.mid(10);
        else if (argument.startsWith("--warmup=")) warmup = argument.mid(9).toInt();
        else if (argument.startsWith("--scans=")) scans = argument.mid(8).toInt();
        else if (argument == "--prepare") makeFixture = true;
        else if (argument == "--trace") trace = true;
        else require(false, "unknown option");
    }
    require(QDir::isAbsolutePath(root) && warmup >= 0 && scans > 0, "use --fixture=/absolute/path [--prepare] [--scans=N] [--warmup=N] [--trace]");
    if (makeFixture) prepare(root);
    QFile input(root + "/processes.jsonl"); require(input.open(QIODevice::ReadOnly), "open fixture");
    QJsonArray rows;
    QSet<QString> distinctPaths;
    QCryptographicHash fixtureHash(QCryptographicHash::Sha256);
    while (!input.atEnd()) {
        const auto line = input.readLine(); fixtureHash.addData(line);
        const auto document = QJsonDocument::fromJson(line);
        require(document.isObject(), "parse fixture row");
        const auto row = document.object(); rows.append(row); distinctPaths.insert(row.value("executablePath").toString());
    }
    require(rows.size() == 1600, "fixture must contain exactly 1600 processes");
    RecentActivityTracker tracker({root + "/applications"});
    QJsonObject snapshot;
    qint64 nanoseconds = 0;
    std::clock_t cpu = 0;
    QCryptographicHash traceHash(QCryptographicHash::Sha256);
    for (int scan = 0; scan < warmup + scans; ++scan) {
        snapshot = {{"processMonotonicSeconds", scan * 2.0}, {"processes", rows}};
        QElapsedTimer timer;
        const auto cpuStart = std::clock(); timer.start();
        tracker.apply(snapshot);
        const auto elapsed = timer.nsecsElapsed();
        const auto consumed = std::clock() - cpuStart;
        if (scan >= warmup) { nanoseconds += elapsed; cpu += consumed; }
        if (trace) traceHash.addData(QJsonDocument(snapshot).toJson(QJsonDocument::Compact));
    }
    struct rusage usage {};
    require(getrusage(RUSAGE_SELF, &usage) == 0, "read resource usage");
    // Linux ru_maxrss is KiB; macOS reports bytes. This benchmark also builds on
    // macOS for local correctness checks, but performance comparisons use Linux.
    qint64 maxRssKiB = usage.ru_maxrss;
#ifdef Q_OS_MACOS
    maxRssKiB /= 1024;
#endif
    QJsonObject result {{"fixtureRows", rows.size()}, {"fixtureSha256", QString::fromLatin1(fixtureHash.result().toHex())},
        {"warmupScans", warmup}, {"measuredScans", scans}, {"scanIntervalSeconds", 2},
        {"applyCpuSeconds", double(cpu) / CLOCKS_PER_SEC}, {"applyWallSeconds", nanoseconds / 1e9},
        {"maxRssKiB", maxRssKiB}, {"distinctInputPaths", distinctPaths.size()},
        {"finalProcessCount", snapshot.value("processes").toArray().size()},
        {"finalApplicationCount", snapshot.value("activityApplications").toArray().size()},
        {"finalOutputSha256", QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(snapshot).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())}};
    if (trace) result["allScanOutputsSha256"] = QString::fromLatin1(traceHash.result().toHex());
    std::puts(QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
    return 0;
}
