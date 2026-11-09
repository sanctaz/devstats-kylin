#include "memory_optimizer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSysInfo>
#include <QTextStream>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <vector>
#include <unistd.h>
#if defined(__GLIBC__)
#include <gnu/libc-version.h>
#endif

namespace {
int checks = 0;
void check(bool condition, const char *message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

QJsonObject snapshot(double seconds, const QString &kind = QStringLiteral("memory")) {
    QJsonArray findings;
    if (!kind.isEmpty()) findings.append(QJsonObject{{"kind", kind}});
    return {{"monotonicSeconds", seconds}, {"timestamp", 1800000000.0 + seconds},
        {"diagnostics", QJsonObject{{"findings", findings}}}};
}

struct Fixture {
    qint64 clock = 0;
    int trimCalls = 0, rssCalls = 0, trimReturn = 1;
    qint64 before = 65536, after = 32768;
    MemoryOptimizer optimizer;
    Fixture() : optimizer(MemoryOptimizer::Dependencies{
        [this] { return clock; },
        [this] { ++trimCalls; return trimReturn; },
        [this] { return (++rssCalls % 2) ? before : after; }}) {}
    QJsonObject at(qint64 milliseconds, double seconds, const QString &kind = QStringLiteral("memory")) {
        clock = milliseconds;
        return optimizer.observe(snapshot(seconds, kind));
    }
};

void resultShape(const QJsonObject &result, bool automatic, bool attempted, bool released) {
    check(result.size() == 8, "result contains the eight documented fields");
    check(result["timestamp"].isDouble(), "result timestamp is a report field");
    check(result["automatic"].isBool() && result["automatic"].toBool() == automatic, "automatic flag");
    check(result["attempted"].isBool() && result["attempted"].toBool() == attempted, "attempted flag");
    check(result["trimReleased"].isBool() && result["trimReleased"].toBool() == released, "glibc release flag");
    check(!result["message"].toString().isEmpty(), "result has clear user feedback");
}

void testFreshEvidenceAndAutomaticCooldown() {
    Fixture f;
    check(f.at(0, 0).isEmpty(), "one memory finding is insufficient");
    check(f.at(50, 0, QStringLiteral("oom")).isEmpty(), "cached snapshot cannot trigger even for OOM");
    check(f.at(100, .1).isEmpty(), "rapid new samples do not count as independent memory evidence");
    auto result = f.at(1000, 1);
    resultShape(result, true, true, true);
    check(f.trimCalls == 1 && f.rssCalls == 2, "one automatic action reads only own RSS twice");
    check(result["rssBeforeBytes"].toDouble() == 65536 && result["rssAfterBytes"].toDouble() == 32768 &&
        result["observedReductionBytes"].toDouble() == 32768, "RSS observation recorded separately");
    check(result["message"].toString().contains(QStringLiteral("不能证明")), "RSS reduction does not promise system memory release");
    check(f.at(2000, 2, QStringLiteral("oom")).isEmpty(), "OOM still obeys automatic cooldown");
    check(f.at(300999, 300.999, QStringLiteral("oom")).isEmpty(), "automatic cooldown cannot expire early");
    resultShape(f.at(301000, 301, QStringLiteral("oom")), true, true, true);
    check(f.trimCalls == 2, "automatic cooldown expires at five minutes");
    check(f.at(999999, 301, QStringLiteral("oom")).isEmpty(), "elapsed time alone never makes a cached sample fresh");
    check(f.trimCalls == 2, "duplicate callbacks never perform trim");
}

void testSharedManualCooldown() {
    Fixture f;
    resultShape(f.optimizer.reclaim(), false, true, true);
    f.clock = 9999;
    auto blocked = f.optimizer.reclaim();
    resultShape(blocked, false, false, false);
    check(blocked["message"].toString().contains(QStringLiteral("10 秒冷却期")) &&
        blocked["message"].toString().contains(QStringLiteral("1 秒")), "manual cooldown describes interval and remaining wait");
    check(blocked["rssBeforeBytes"].isNull() && blocked["observedReductionBytes"].isNull(), "cooldown cannot invent RSS observations");
    check(f.trimCalls == 1 && f.rssCalls == 2, "cooldown does not call allocator or RSS reader");
    f.clock = 10000;
    resultShape(f.optimizer.reclaim(), false, true, true);
    check(f.at(10001, 0, QStringLiteral("oom")).isEmpty(), "manual action also starts automatic cooldown");
    check(f.at(309999, 299.998, QStringLiteral("oom")).isEmpty(), "manual-to-automatic cooldown is at least five minutes");
    resultShape(f.at(310000, 300, QStringLiteral("oom")), true, true, true);
    f.clock = 319999;
    resultShape(f.optimizer.reclaim(), false, false, false);
    f.clock = 320000;
    resultShape(f.optimizer.reclaim(), false, true, true);
    check(f.trimCalls == 4, "both entry points update shared cooldown");
}

void testUnknownHealthyGapAndReset() {
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "initial memory sample");
        f.clock = 2000;
        check(f.optimizer.observe(QJsonObject{{"monotonicSeconds", 2.0}}).isEmpty(), "unknown diagnostics break evidence");
        check(f.at(4000, 4).isEmpty(), "memory after unknown is first evidence");
        check(!f.at(6000, 6).isEmpty(), "two known memory samples after unknown trigger");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "initial healthy-reset fixture");
        check(f.at(2000, 2, QString()).isEmpty(), "healthy finding clears pressure streak");
        check(f.at(4000, 4).isEmpty(), "one finding after healthy does not trigger");
        check(!f.at(6000, 6).isEmpty(), "two findings after healthy trigger");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "sample gap fixture starts");
        check(f.at(16000, 16).isEmpty(), "sample gap beyond fifteen seconds resets streak");
        check(!f.at(18000, 18).isEmpty(), "two samples after sample gap trigger");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "local gap fixture starts");
        check(f.at(16000, 2).isEmpty(), "local elapsed gap also resets streak");
        check(!f.at(18000, 4).isEmpty(), "two samples after local gap trigger");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "boundary fixture first evidence");
        check(!f.at(15000, 15).isEmpty(), "fifteen second boundary is accepted");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "pre-sleep first finding");
        f.optimizer.resetObservation();
        check(f.at(2000, 0).isEmpty(), "reset admits recreated sampler epoch as first finding");
        check(!f.at(4000, 2).isEmpty(), "reset requires two fresh findings");
        f.optimizer.resetObservation();
        check(f.at(6000, 0, QStringLiteral("oom")).isEmpty(), "sleep reset retains action cooldown");
        resultShape(f.at(304000, 298, QStringLiteral("oom")), true, true, true);
        check(f.trimCalls == 2, "recreated sample epoch cannot corrupt local cooldown");
    }
    {
        Fixture f;
        resultShape(f.at(0, 0, QStringLiteral("oom")), true, true, true);
        check(f.trimCalls == 1, "first OOM sample acts immediately");
    }
    {
        Fixture f;
        check(f.at(0, 0, QStringLiteral("cpu")).isEmpty(), "CPU finding does not request memory reclamation");
        check(f.at(2000, 2, QStringLiteral("io")).isEmpty(), "IO finding does not request memory reclamation");
        check(f.trimCalls == 0 && f.rssCalls == 0, "observation alone avoids allocator/RSS overhead");
    }
}

void testClockIntegrityAndWallTime() {
    {
        Fixture f;
        check(f.at(1000, 1).isEmpty(), "clock-integrity first pressure sample");
        check(f.at(2000, .5).isEmpty(), "backward sample cannot trigger");
        check(f.at(3000, 1).isEmpty(), "old high-water sample remains stale");
        check(f.at(4000, 2).isEmpty(), "first fresh sample after regression starts new evidence");
        check(!f.at(6000, 4).isEmpty(), "two fresh samples after regression trigger");
    }
    for (const auto &invalid : {QJsonValue(QJsonValue::Null), QJsonValue(QStringLiteral("2")),
            QJsonValue(std::numeric_limits<double>::infinity()), QJsonValue(-1.0)}) {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "invalid-time fixture first evidence");
        auto bad = snapshot(2);
        bad["monotonicSeconds"] = invalid;
        f.clock = 2000;
        check(f.optimizer.observe(bad).isEmpty(), "invalid sample time never causes action");
        check(f.at(4000, 4).isEmpty(), "unknown sample time breaks continuity");
        check(!f.at(6000, 6).isEmpty(), "continuity can recover after invalid time");
    }
    {
        Fixture f;
        f.clock = 1000;
        resultShape(f.optimizer.reclaim(), false, true, true);
        f.clock = 500;
        resultShape(f.optimizer.reclaim(), false, false, false);
        check(f.at(600, 1, QStringLiteral("oom")).isEmpty(), "local backward clock suppresses automatic action");
        f.clock = 10999;
        resultShape(f.optimizer.reclaim(), false, false, false);
        f.clock = 11000;
        resultShape(f.optimizer.reclaim(), false, true, true);
        check(f.trimCalls == 2, "local backward clock cannot shorten cooldown");
    }
    {
        Fixture f;
        f.clock = -1;
        resultShape(f.optimizer.reclaim(), false, false, false);
        check(f.at(-1, 0, QStringLiteral("oom")).isEmpty(), "unknown local clock prevents automatic action");
        check(f.trimCalls == 0, "invalid local clocks avoid allocator");
    }
    {
        Fixture f;
        auto first = snapshot(0); first["timestamp"] = 900000000000.0;
        check(f.optimizer.observe(first).isEmpty(), "wall-time fixture first sample");
        auto second = snapshot(2); second["timestamp"] = -1000.0;
        f.clock = 2000;
        check(!f.optimizer.observe(second).isEmpty(), "wall-clock reversal does not affect evidence");
        auto jumped = snapshot(4, QStringLiteral("oom")); jumped["timestamp"] = 990000000000.0;
        f.clock = 4000;
        check(f.optimizer.observe(jumped).isEmpty(), "wall-clock jump cannot expire cooldown");
    }
    {
        Fixture f;
        check(f.at(0, 0).isEmpty(), "minimum-interval fixture first finding");
        check(f.at(100, 2).isEmpty(), "fast local callbacks cannot create evidence by sample-clock jump");
        check(!f.at(1000, 3).isEmpty(), "minimum interval requires local elapsed time too");
    }
}

void testAllocatorReturnAndUnknownRss() {
    for (int trimReturn : {0, 1, -1}) {
        Fixture f;
        f.trimReturn = trimReturn;
        auto result = f.optimizer.reclaim();
        resultShape(result, false, trimReturn >= 0, trimReturn == 1);
        check(f.trimCalls == 1, "allocator is called exactly once");
        check(result["message"].toString().contains(trimReturn < 0 ? QStringLiteral("不支持") :
            trimReturn == 0 ? QStringLiteral("未报告") : QStringLiteral("报告已归还")), "allocator return is described accurately");
    }
    for (int unknown : {0, 1, 2}) {
        Fixture f;
        if (unknown != 1) f.before = -1;
        if (unknown != 0) f.after = -1;
        const auto result = f.optimizer.reclaim();
        resultShape(result, false, true, true);
        check(result["rssBeforeBytes"].isNull() == (f.before < 0) &&
            result["rssAfterBytes"].isNull() == (f.after < 0), "unmeasurable RSS stays null");
        check(result["observedReductionBytes"].isNull(), "partial RSS cannot imply reduction");
        check(result["message"].toString().contains(QStringLiteral("无法完整测量")), "missing RSS is disclosed");
    }
    Fixture growth;
    growth.after = growth.before * 2;
    const auto result = growth.optimizer.reclaim();
    check(result["observedReductionBytes"].toDouble() == 0, "RSS growth is not reported as negative freed bytes");
    check(result["trimReleased"].toBool(), "glibc flag and RSS observation remain independent");
}

QJsonObject testRealAllocatorIsolated() {
    // Own small heap chunks in this dedicated test process only. No required RSS
    // decrease: glibc is allowed to retain fragmented arenas or return zero.
    std::vector<void *> blocks;
    for (int i = 0; i < 256; ++i) {
        void *block = std::malloc(64 * 1024);
        check(block != nullptr, "isolated allocation succeeds");
        std::memset(block, 0x7b, 64 * 1024);
        blocks.push_back(block);
    }
    for (void *block : blocks) std::free(block);
    MemoryOptimizer optimizer;
    QElapsedTimer elapsed;
    elapsed.start();
    const auto result = optimizer.reclaim();
    const qint64 nanoseconds = elapsed.nsecsElapsed();
#if defined(__GLIBC__)
    check(result["attempted"].toBool() && result["trimReleased"].isBool(), "real glibc malloc_trim path executed");
    check(result["rssBeforeBytes"].isDouble() && result["rssAfterBytes"].isDouble(), "real Linux current-process RSS measured");
    const QString libc = QString::fromLatin1(gnu_get_libc_version());
#else
    const QString libc = QStringLiteral("non-glibc");
    check(!result["attempted"].toBool(), "non-glibc correctly reports unavailable");
#endif
    resultShape(optimizer.reclaim(), false, false, false);
    return {{"libc", libc}, {"architecture", QSysInfo::buildCpuArchitecture()},
        {"qtVersion", QString::fromLatin1(qVersion())}, {"euid", double(::geteuid())},
        {"reclaimNanoseconds", double(nanoseconds)}, {"result", result},
        {"allocationBytes", 256 * 64 * 1024}, {"rssDecreaseRequired", false}};
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    try {
        testFreshEvidenceAndAutomaticCooldown();
        testSharedManualCooldown();
        testUnknownHealthyGapAndReset();
        testClockIntegrityAndWallTime();
        testAllocatorReturnAndUnknownRss();
        const auto real = testRealAllocatorIsolated();
        QTextStream(stdout) << QJsonDocument(QJsonObject{{"status", "passed"}, {"checks", checks},
            {"realAllocator", real}}).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    } catch (const std::exception &error) {
        QTextStream(stderr) << "memory optimizer test failed: " << error.what() << '\n';
        return 1;
    }
}
