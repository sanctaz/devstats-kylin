#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <functional>

// Owned and called only by MonitorWorker's thread. No timer or background scan.
// malloc_trim operates on this process's already free heap pages; it does not
// terminate processes, discard application data, or change system caches/swap.
class MemoryOptimizer final {
public:
    struct Dependencies {
        std::function<qint64()> monotonicMilliseconds;
        // The glibc return value is 0/1; -1 means unavailable on this platform.
        std::function<int()> trim;
        // Current process RSS, or -1 when it cannot be measured.
        std::function<qint64()> residentBytes;
    };

    MemoryOptimizer();
    explicit MemoryOptimizer(Dependencies dependencies);
    QJsonObject observe(const QJsonObject &snapshot);
    QJsonObject reclaim();
    // Break evidence continuity after sleep/recreated sampler, retaining cooldown.
    void resetObservation();

private:
    qint64 now();
    QJsonObject perform(bool automatic, qint64 milliseconds);
    QElapsedTimer elapsed_;
    Dependencies dependencies_;
    qint64 clockHighWater_ = -1, lastAttempt_ = -1;
    qint64 lastSampleClock_ = -1, pressureClock_ = -1;
    double lastSample_ = -1, pressureSample_ = -1;
    int pressureSamples_ = 0;
};
