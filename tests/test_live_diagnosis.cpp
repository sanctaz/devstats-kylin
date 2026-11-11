// The real worker on this machine's real /proc, with real CPU load. Everything else
// about diagnosis is tested with fixtures; this proves the pieces work together.
#include "../src/controller.h"

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>
#include <QVector>
#include <QtTest/QTest>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>

namespace {
void check(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL live diagnosis: %s\n", what); std::exit(1); } }
QJsonObject find(const QJsonArray &rows, const char *field, const QString &value) {
    for (const auto &row : rows) if (row.toObject().value(field).toString() == value) return row.toObject();
    return {};
}
}

int main(int argc, char **argv) {
    QTemporaryDir root;
    check(root.isValid(), "private data directory");
    qputenv("DEVSTATS_DATA_DIR", (root.path() + "/data").toUtf8());
    qunsetenv("DEVSTATS_DISABLE_PROBES");
    QApplication app(argc, argv);
    qRegisterMetaType<QJsonObject>(); qRegisterMetaType<QJsonArray>();
    MonitorWorker worker(root.path() + "/data", {}, false);
    QJsonObject latest;
    QObject::connect(&worker, &MonitorWorker::snapshotReady, [&](const QJsonObject &snapshot) { latest = snapshot; });
    // The history as the window receives it: a complete range, then single points.
    HistoryFeed history;
    QJsonObject completeRange;
    int completeRanges = 0, additions = 0;
    bool additionsAreSinglePoints = true;
    QObject::connect(&worker, &MonitorWorker::historyReady, [&](const QJsonObject &range, qint64 start, qint64 end) {
        ++completeRanges; completeRange = range; history.reset(range, start, end);
    });
    QObject::connect(&worker, &MonitorWorker::historyAppended, [&](const QJsonObject &points, qint64 start, qint64 end) {
        ++additions; history.append(points, start, end);
        const double newest = std::floor(latest.value("timestamp").toDouble());
        additionsAreSinglePoints &= double(end) == newest && points.value("cpu").toObject().value("timestamp").toDouble() == newest
            && points.value("memory").toObject().value("value").isDouble();
    });
    worker.setPresentationVisible(true);
    worker.start();
    QTest::qWait(4500);

    // Opening the window sends the whole range once. After that each sample sends
    // only its own points, and together they are what a complete range would hold.
    check(completeRanges == 1 && additions >= 1 && additionsAreSinglePoints, "after the first complete range each sample adds one point per metric");
    {
        const auto cpu = history.series("cpu"), memory = history.series("memory"), latency = history.series("schedulerLatency");
        const qint64 start = history.start(), end = history.end();
        worker.setHistoryDuration(120);
        check(completeRanges == 2 && history.start() == start && history.end() == end, "changing the range sends a complete range at once");
        check(cpu.size() >= 2 && completeRange.value("cpu").toArray() == cpu && completeRange.value("memory").toArray() == memory
              && completeRange.value("schedulerLatency").toArray() == latency, "the points added one by one equal a freshly built complete range");
    }

    // What a sample now contains, measured on a real kernel.
    check(latest.value("responsiveness").toObject().value("schedulerMaxMs").isDouble(), "scheduler latency is measured");
    check(latest.value("cpu").toObject().value("forksPerSecond").isDouble() && latest.value("cpu").toObject().value("irq").isDouble(), "fork rate and interrupt share");
    check(latest.value("diskIO").toObject().contains("busiestUtilization"), "disk latency fields exist");
    check(latest.value("processStates").toObject().value("total").toInt() >= latest.value("processCount").toInt(), "the scan covers every user");
    check(!latest.value("processes").toArray().isEmpty(), "own processes");
    bool kernel = false;
    for (const auto &value : latest.value("systemProcesses").toArray()) kernel |= value.toObject().value("kernelThread").toBool();
    check(kernel, "kernel threads are visible as suspects");
    for (const auto &value : latest.value("systemProcesses").toArray())
        check(!value.toObject().contains("owner") || value.toObject().value("executablePath").toString().isEmpty(), "other users' paths are never read");
    check(latest.value("diagnostics").toObject().value("meters").toArray().size() == 6, "diagnosis has its meters");
    check(latest.value("suspects").toArray().isEmpty(), "pages that do not show suspects do not pay for ranking them");
    worker.setSuspectRanking(true);
    check(!latest.value("suspects").toArray().isEmpty(), "opening the diagnosis page ranks the scan already in hand");
    QTest::qWait(2500);
    check(!latest.value("suspects").toArray().isEmpty(), "the diagnosis page keeps getting ranked suspects");
    check(latest.value("selfCpuPercent").isDouble() && latest.value("selfPid").toDouble() == double(QCoreApplication::applicationPid()),
          "DevStats' own CPU is reported at the top level");

    // Oversubscribe every core at the lowest priority, so nothing else on the machine suffers.
    QVector<std::shared_ptr<QProcess>> burners;
    const int count = QThread::idealThreadCount() * 2;
    for (int i = 0; i < count; ++i) {
        auto burner = std::make_shared<QProcess>();
        burner->setStandardOutputFile(QProcess::nullDevice());
        burner->start("nice", {"-n", "19", "yes"});
        check(burner->waitForStarted(5000), "load generator started");
        burners.append(burner);
    }
    QTest::qWait(4000);
    worker.runDiagnostics();
    check(latest.value("diagnosticRun").toObject().value("state") == "running", "run started");
    QTest::qWait(10500);
    const auto run = latest.value("diagnosticRun").toObject();
    check(run.value("state") == "completed" && run.value("sampleCount").toInt() == 6, "run completed");
    check(find(run.value("findings").toArray(), "kind", "cpu").value("persistence") == "sustained", "sustained CPU contention is found");
    const auto burner = find(run.value("suspects").toArray(), "name", "yes");
    check(burner.value("own").toBool() && burner.value("processCount").toInt() == count && burner.value("averageCpuPercent").toDouble() >= 50, "the load generators are the top suspects, merged, with real averages");
    check(run.value("schedulerMaxMs").isDouble(), "latency kept for the run");
    check(latest.value("diagnostics").toObject().value("state") != "normal", "the live banner agrees");
    for (auto &process : burners) { process->kill(); process->waitForFinished(5000); }

    // After the load the finding goes away by itself.
    QTest::qWait(6500);
    check(find(latest.value("diagnostics").toObject().value("findings").toArray(), "kind", "cpu").isEmpty(), "contention clears when the load ends");
    worker.stop();
    std::printf("PASS live diagnosis: %s\n", QJsonDocument(QJsonObject{{"cores", QThread::idealThreadCount()}, {"schedulerMaxMs", run.value("schedulerMaxMs")}, {"yesAverageCpu", burner.value("averageCpuPercent")},
        {"processes", latest.value("processStates").toObject().value("total")}, {"systemProcesses", latest.value("systemProcesses").toArray().size()}, {"psi", !latest.value("pressure").toObject().value("cpu").isNull()},
        {"diskBusy", latest.value("diskIO").toObject().value("busiestUtilization")}, {"temperature", latest.value("thermal").toObject().value("hottestCelsius")}}).toJson(QJsonDocument::Compact).constData());
    return 0;
}
