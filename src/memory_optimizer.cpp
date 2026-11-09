#include "memory_optimizer.h"

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <cmath>
#include <limits>
#include <utility>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace {
constexpr qint64 AutomaticCooldown = 5 * 60 * 1000;
constexpr qint64 ManualCooldown = 10 * 1000;
constexpr qint64 MaximumGap = 15 * 1000;
constexpr double MinimumEvidenceInterval = 1.0;

int trimOwnHeap() {
#if defined(__GLIBC__)
    return ::malloc_trim(0);
#else
    return -1;
#endif
}

qint64 ownResidentBytes() {
    // Only read this process, twice per actual action. No process enumeration.
    QFile statm(QStringLiteral("/proc/self/statm"));
    if (!statm.open(QIODevice::ReadOnly)) return -1;
    const auto fields = statm.read(256).simplified().split(' ');
    if (fields.size() < 2) return -1;
    bool valid = false;
    const qulonglong pages = fields[1].toULongLong(&valid);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    if (!valid || pageSize <= 0 || pages > qulonglong(std::numeric_limits<qint64>::max()) / qulonglong(pageSize)) return -1;
    return qint64(pages * qulonglong(pageSize));
}

QJsonValue bytesOrUnknown(qint64 bytes) {
    return bytes < 0 ? QJsonValue(QJsonValue::Null) : QJsonValue(double(bytes));
}

QJsonObject outcome(bool automatic) {
    return {{"timestamp", double(QDateTime::currentMSecsSinceEpoch()) / 1000.0},
        {"automatic", automatic}, {"attempted", false}, {"trimReleased", false},
        {"rssBeforeBytes", QJsonValue::Null}, {"rssAfterBytes", QJsonValue::Null},
        {"observedReductionBytes", QJsonValue::Null}};
}
}

MemoryOptimizer::MemoryOptimizer() : MemoryOptimizer(Dependencies{}) {}
MemoryOptimizer::MemoryOptimizer(Dependencies dependencies) : dependencies_(std::move(dependencies)) {
    elapsed_.start();
    if (!dependencies_.trim) dependencies_.trim = trimOwnHeap;
    if (!dependencies_.residentBytes) dependencies_.residentBytes = ownResidentBytes;
}

qint64 MemoryOptimizer::now() {
    const qint64 value = dependencies_.monotonicMilliseconds ? dependencies_.monotonicMilliseconds() : elapsed_.elapsed();
    // A faulty/replaced injected clock must not shorten an existing cooldown.
    if (value < 0 || value < clockHighWater_) return -1;
    clockHighWater_ = value;
    return value;
}

void MemoryOptimizer::resetObservation() {
    lastSample_ = pressureSample_ = -1;
    lastSampleClock_ = pressureClock_ = -1;
    pressureSamples_ = 0;
}

QJsonObject MemoryOptimizer::observe(const QJsonObject &snapshot) {
    const auto sampleValue = snapshot.value("monotonicSeconds");
    const double sample = sampleValue.toDouble(-1);
    const qint64 milliseconds = now();
    if (!sampleValue.isDouble() || !std::isfinite(sample) || sample < 0 || milliseconds < 0) {
        pressureSamples_ = 0;
        pressureSample_ = -1;
        pressureClock_ = -1;
        return {};
    }
    if (lastSample_ >= 0 && sample <= lastSample_) {
        if (sample < lastSample_) { pressureSamples_ = 0; pressureSample_ = -1; pressureClock_ = -1; }
        return {}; // A UI/network republish is never additional evidence.
    }
    if (lastSample_ >= 0 && (sample - lastSample_ > double(MaximumGap) / 1000.0 ||
            milliseconds - lastSampleClock_ > MaximumGap)) {
        pressureSamples_ = 0;
        pressureSample_ = -1;
        pressureClock_ = -1;
    }
    lastSample_ = sample;
    lastSampleClock_ = milliseconds;

    bool memory = false, oom = false;
    const auto findings = snapshot.value("diagnostics").toObject().value("findings");
    for (const auto &entry : findings.toArray()) {
        const QString kind = entry.toObject().value("kind").toString();
        memory |= kind == QStringLiteral("memory");
        oom |= kind == QStringLiteral("oom");
    }
    if (!memory) {
        pressureSamples_ = 0;
        pressureSample_ = -1;
        pressureClock_ = -1;
    } else if (!pressureSamples_) {
        pressureSamples_ = 1;
        pressureSample_ = sample;
        pressureClock_ = milliseconds;
    } else if (sample - pressureSample_ >= MinimumEvidenceInterval && milliseconds - pressureClock_ >= 1000) {
        pressureSamples_ = 2;
    }
    if (!oom && pressureSamples_ < 2) return {};
    if (lastAttempt_ >= 0 && milliseconds - lastAttempt_ < AutomaticCooldown) return {};
    pressureSamples_ = 0;
    pressureSample_ = -1;
    pressureClock_ = -1;
    return perform(true, milliseconds);
}

QJsonObject MemoryOptimizer::reclaim() {
    const qint64 milliseconds = now();
    auto result = outcome(false);
    if (milliseconds < 0) {
        result["message"] = QStringLiteral("单调计时暂不可用，未尝试回收；请稍后重试。");
        return result;
    }
    if (lastAttempt_ >= 0 && milliseconds - lastAttempt_ < ManualCooldown) {
        const qint64 remaining = (ManualCooldown - (milliseconds - lastAttempt_) + 999) / 1000;
        result["message"] = QStringLiteral("本应用内存回收处于 10 秒冷却期，还需等待 %1 秒；本次未尝试回收。").arg(remaining);
        return result;
    }
    return perform(false, milliseconds);
}

QJsonObject MemoryOptimizer::perform(bool automatic, qint64 milliseconds) {
    // Both entry points share one attempt time: a manual action also delays auto.
    lastAttempt_ = milliseconds;
    const qint64 before = dependencies_.residentBytes();
    const int trimmed = dependencies_.trim();
    const qint64 after = dependencies_.residentBytes();
    auto result = outcome(automatic);
    result["attempted"] = trimmed == 0 || trimmed == 1;
    result["trimReleased"] = trimmed == 1;
    result["rssBeforeBytes"] = bytesOrUnknown(before);
    result["rssAfterBytes"] = bytesOrUnknown(after);
    if (before >= 0 && after >= 0) result["observedReductionBytes"] = double(qMax<qint64>(0, before - after));
    if (trimmed != 0 && trimmed != 1) {
        result["message"] = QStringLiteral("此平台不支持 glibc 空闲堆页回收，本次未尝试回收。");
    } else {
        QString message = trimmed == 1
            ? QStringLiteral("已尝试回收本应用空闲堆页；glibc 报告已归还部分空闲堆页。")
            : QStringLiteral("已尝试回收本应用空闲堆页；glibc 未报告归还空闲堆页。");
        if (before < 0 || after < 0) message += QStringLiteral("本次无法完整测量本应用 RSS。");
        else message += QStringLiteral("RSS 前后差仅为即时观测，不能证明系统可用内存增加。");
        result["message"] = message;
    }
    return result;
}
