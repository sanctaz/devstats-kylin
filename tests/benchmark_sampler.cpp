#include "../src/linux_sampler.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <cstdio>
#include <sys/resource.h>

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc != 4) return 2;
    const QString root = QString::fromLocal8Bit(argv[1]);
    const int iterations = QString::fromLatin1(argv[2]).toInt();
    LinuxSampler sampler(root + "/proc", root + "/sys");
    if (QString::fromLatin1(argv[3]) == "configured") sampler.setProjectRoots({root + "/project"});
    for (int i = 0; i < 4; ++i) sampler.sample();
    rusage before{}, after{};
    getrusage(RUSAGE_SELF, &before);
    QElapsedTimer timer; timer.start();
    QJsonObject last;
    for (int i = 0; i < iterations; ++i) last = sampler.sample();
    const double wall = timer.nsecsElapsed() / 1e6;
    getrusage(RUSAGE_SELF, &after);
    const auto seconds = [](const timeval &v) { return v.tv_sec + v.tv_usec / 1e6; };
    QJsonObject stable{{"processes", last["processes"]}, {"groups", last["groups"]}, {"processCount", last["processCount"]}};
    const QByteArray digest = QCryptographicHash::hash(QJsonDocument(stable).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex();
    if (last["processCount"].toInt() != 1600 || last["groups"].toArray().size() != 1) return 3;
    const double cpu = seconds(after.ru_utime) + seconds(after.ru_stime) - seconds(before.ru_utime) - seconds(before.ru_stime);
    std::printf("{\"iterations\":%d,\"processes\":1600,\"cpuMs\":%.3f,\"wallMs\":%.3f,\"maxRssKiB\":%ld,\"snapshotSha256\":\"%s\"}\n",
                iterations, cpu * 1000, wall, after.ru_maxrss, digest.constData());
}
