#include "diagnostic_run.h"
#include "diagnostics.h"
#include <QVector>
#include <algorithm>
#include <cmath>

namespace {
constexpr qint64 Duration = 10000;
constexpr int MaximumSuspects = 64;
// Kinds listed in a run's status strip. Others still appear as findings.
const QStringList CheckKinds = {"cpu", "memory", "io", "responsiveness", "display", "thermal"};
const QStringList CheckTitles = {QStringLiteral("处理器"), QStringLiteral("内存"), QStringLiteral("磁盘"), QStringLiteral("调度"), QStringLiteral("显示"), QStringLiteral("温度")};
bool measured(const QJsonValue &v, double maximum = 1e300) {
    return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 && v.toDouble() <= maximum;
}
bool checkKnown(const QJsonObject &s, const QString &kind) {
    const auto cpu = s.value("cpu").toObject(), memory = s.value("memory").toObject();
    const auto probe = s.value("responsiveness").toObject(), thermal = s.value("thermal").toObject();
    if (kind == "cpu") return measured(cpu.value("usage"), 1);
    if (kind == "memory") return measured(memory.value("totalBytes")) && measured(memory.value("availableBytes"));
    if (kind == "io") return measured(s.value("diskIO").toObject().value("busiestUtilization"), 1) || measured(cpu.value("iowait"), 1);
    if (kind == "responsiveness") return measured(probe.value("schedulerMaxMs"));
    if (kind == "display") return measured(probe.value("displayMaxMs"));
    return measured(thermal.value("hottestCelsius")) || measured(thermal.value("frequencyFraction"));
}
int severityRank(const QJsonObject &finding) {
    const auto severity = finding.value("severity").toString();
    return severity == "critical" ? 0 : severity == "attention" ? 1 : 2;
}
}

bool DiagnosticRun::start(qint64 monotonicMilliseconds, double wallSeconds) {
    if (running()) return false;
    *this = DiagnosticRun();
    state_ = QStringLiteral("running"); started_ = monotonicMilliseconds; startedAt_ = wallSeconds;
    return true;
}

bool DiagnosticRun::observe(const QJsonObject &snapshot, qint64 monotonicMilliseconds, double wallSeconds) {
    if (!running()) return false;
    elapsed_ = qMax<qint64>(elapsed_, qMax<qint64>(0, monotonicMilliseconds - started_));
    // A refresh/republish within a timer interval must not inflate persistence.
    // Count at most six samples (baseline plus five intervals) per observation.
    if (samples_ < 6 && (lastObservation_ < 0 || elapsed_ - lastObservation_ >= 1500)) {
        if (lastObservation_ >= 0 && elapsed_ - lastObservation_ > 3000) {
            for (auto it = findings_.begin(); it != findings_.end(); ++it) { it->consecutive = 0; it->consecutiveAt = -1; }
            const QString gap = QStringLiteral("采样线程自身被延迟超过 3 秒：这段时间系统很可能卡住了，但没有留下数据。");
            if (!coverage_.contains(gap)) coverage_.append(gap);
        }
        lastObservation_ = elapsed_; ++samples_;
        const auto diagnosis = snapshot.value("diagnostics").isObject() ? snapshot.value("diagnostics").toObject() : diagnoseSystem(snapshot);
        QHash<QString, QJsonObject> found;
        for (const auto &value : diagnosis.value("findings").toArray()) {
            const auto finding = value.toObject();
            found[finding.value("kind").toString()] = finding;
        }
        for (const auto &kind : CheckKinds) {
            auto &check = checks_[kind];
            if (found.contains(kind) || checkKnown(snapshot, kind)) ++check.known; else ++check.unknown;
            if (found.contains(kind)) ++check.pressure;
        }
        for (auto it = findings_.begin(); it != findings_.end(); ++it)
            if (!found.contains(it.key())) { it->consecutive = 0; it->consecutiveAt = -1; }
        for (auto it = found.cbegin(); it != found.cend(); ++it) {
            auto &f = findings_[it.key()];
            ++f.observed;
            if (!f.consecutive) f.consecutiveAt = elapsed_;
            ++f.consecutive;
            f.longest = qMax(f.longest, f.consecutive);
            f.longestMilliseconds = qMax(f.longestMilliseconds, elapsed_ - f.consecutiveAt);
            // Keep the most severe reading, and within one severity the latest.
            if (f.representative.isEmpty() || severityRank(it.value()) <= severityRank(f.representative)) f.representative = it.value();
        }
        for (const auto &v : diagnosis.value("coverage").toArray()) {
            const QString text = v.toString();
            if (!text.isEmpty() && !coverage_.contains(text) && coverage_.size() < 12) coverage_.append(text);
        }
        const auto probe = snapshot.value("responsiveness").toObject();
        if (measured(probe.value("schedulerMaxMs"))) schedulerMax_ = std::max(schedulerMax_, probe.value("schedulerMaxMs").toDouble());
        if (measured(probe.value("displayMaxMs"))) displayMax_ = std::max(displayMax_, probe.value("displayMaxMs").toDouble());
        observeSuspects(snapshot);
    }
    if (elapsed_ >= Duration) return finish(QStringLiteral("completed"), monotonicMilliseconds, wallSeconds);
    return false;
}

QString DiagnosticRun::rankingBasis() const {
    QString basis = QStringLiteral("cpu");
    int strongest = 0;
    for (const auto &kind : {QStringLiteral("cpu"), QStringLiteral("memory"), QStringLiteral("io")}) {
        const auto f = findings_.value(kind);
        if (!f.observed) continue;
        const bool sustained = f.longest >= 3 && f.longestMilliseconds >= 3000;
        const int priority = (f.representative.value("severity") == "critical" ? 1000 : 0) + (sustained ? 100 : 0) + f.observed;
        if (priority > strongest) { strongest = priority; basis = kind; }
    }
    return basis;
}

void DiagnosticRun::observeSuspects(const QJsonObject &snapshot) {
    // CPU and I/O rates need two scans, and cached process lists repeat old rates.
    const auto stamp = snapshot.value("processMonotonicSeconds");
    if (!snapshot.value("processesFresh").toBool() || !measured(stamp) || stamp.toDouble() <= lastProcessStamp_) return;
    lastProcessStamp_ = stamp.toDouble();
    auto rows = snapshot.value("suspects").toArray();
    if (rows.isEmpty()) rows = rankSuspects(snapshot, QStringLiteral("cpu"), MaximumSuspects);
    for (const auto &value : rows) {
        const auto row = value.toObject();
        const QString key = row.value("key").toString();
        if (!suspects_.contains(key) && suspects_.size() >= MaximumSuspects) continue;
        auto &suspect = suspects_[key];
        suspect.latest = row;
        if (measured(row.value("cpuPercent"))) {
            const double cpu = row.value("cpuPercent").toDouble();
            suspect.cpuSum += cpu; ++suspect.cpuSamples;
            suspect.cpuPeak = std::max(suspect.cpuPeak, cpu);
        }
        suspect.memoryPeak = std::max(suspect.memoryPeak, row.value("memoryBytes").toDouble(-1));
        suspect.ioPeak = std::max(suspect.ioPeak, row.value("ioBytesPerSecond").toDouble(-1));
        if (row.value("blockedCount").toInt() > 0) ++suspect.blockedSamples;
    }
}

bool DiagnosticRun::finish(const QString &state, qint64 monotonicMilliseconds, double wallSeconds) {
    if (!running() || (state != "completed" && state != "cancelled" && state != "interrupted")) return false;
    elapsed_ = qMax<qint64>(elapsed_, qMax<qint64>(0, monotonicMilliseconds - started_));
    state_ = state; finishedAt_ = wallSeconds;
    return true;
}

QJsonObject DiagnosticRun::result() const {
    QVector<QJsonObject> ordered;
    int sustained = 0, transient = 0;
    for (auto it = findings_.cbegin(); it != findings_.cend(); ++it) {
        const auto &f = it.value();
        if (!f.observed) continue;
        const bool persistent = f.longest >= 3 && f.longestMilliseconds >= 3000;
        auto finding = f.representative;
        const bool info = finding.value("severity").toString() == "info";
        if (!info) persistent ? ++sustained : ++transient;
        finding["observedSamples"] = f.observed;
        finding["totalSamples"] = samples_;
        finding["persistence"] = persistent ? "sustained" : "transient";
        ordered.append(finding);
    }
    std::sort(ordered.begin(), ordered.end(), [](const QJsonObject &a, const QJsonObject &b) {
        const int ra = severityRank(a), rb = severityRank(b);
        if (ra != rb) return ra < rb;
        const bool sa = a.value("persistence").toString() == "sustained", sb = b.value("persistence").toString() == "sustained";
        if (sa != sb) return sa;
        return findingPriority(a.value("kind").toString()) < findingPriority(b.value("kind").toString());
    });
    QJsonArray findings, checks, coverage, suspects;
    for (const auto &finding : ordered) findings.append(finding);
    for (int i = 0; i < CheckKinds.size(); ++i) {
        const auto c = checks_.value(CheckKinds[i]);
        checks.append(QJsonObject{{"kind", CheckKinds[i]}, {"title", CheckTitles[i]},
            {"status", c.pressure ? "pressure" : c.known ? "ok" : "unknown"},
            {"observedSamples", c.pressure}, {"knownSamples", c.known}, {"unknownSamples", c.unknown}});
    }
    for (const auto &text : coverage_) coverage.append(text);
    coverage.append(QStringLiteral("这是约 10 秒的观察，间歇性卡顿请在卡顿发生时重新诊断，或查看卡顿记录。"));

    const QString basis = rankingBasis();
    QVector<QString> keys = suspects_.keys().toVector();
    auto value = [&](const Suspect &s) {
        if (basis == "memory") return s.memoryPeak;
        if (basis == "io") return s.ioPeak;
        return s.cpuSamples ? s.cpuSum / s.cpuSamples : -1.0;
    };
    std::sort(keys.begin(), keys.end(), [&](const QString &a, const QString &b) {
        const auto &l = suspects_[a], &r = suspects_[b];
        if (basis == "io" && (l.blockedSamples > 0) != (r.blockedSamples > 0)) return l.blockedSamples > 0;
        const double lv = value(l), rv = value(r);
        return lv != rv ? lv > rv : a < b;
    });
    for (int i = 0; i < std::min(8, keys.size()); ++i) {
        const auto &s = suspects_[keys[i]];
        auto row = s.latest;
        row["averageCpuPercent"] = s.cpuSamples ? QJsonValue(s.cpuSum / s.cpuSamples) : QJsonValue(QJsonValue::Null);
        row["peakCpuPercent"] = s.cpuPeak >= 0 ? QJsonValue(s.cpuPeak) : QJsonValue(QJsonValue::Null);
        row["peakMemoryBytes"] = s.memoryPeak >= 0 ? QJsonValue(s.memoryPeak) : QJsonValue(QJsonValue::Null);
        row["peakIOBytesPerSecond"] = s.ioPeak >= 0 ? QJsonValue(s.ioPeak) : QJsonValue(QJsonValue::Null);
        row["blockedSamples"] = s.blockedSamples;
        suspects.append(row);
    }

    QString headline, summary;
    if (state_ == "idle") {
        headline = QStringLiteral("一键诊断");
        summary = QStringLiteral("观察约 10 秒，找出卡顿原因和相关进程。");
    } else if (state_ == "running") {
        headline = QStringLiteral("正在观察系统…");
        summary = QStringLiteral("不会关闭任何程序。卡顿正在发生时诊断最准确。");
    } else if (ordered.isEmpty() || severityRank(ordered.first()) == 2) {
        headline = QStringLiteral("本轮未发现明显压力");
        summary = QStringLiteral("如果刚才感到卡顿，可以在卡顿发生时再诊断一次，或查看卡顿记录。");
    } else {
        headline = sustained ? ordered.first().value("title").toString() : QStringLiteral("只有短暂压力：") + ordered.first().value("title").toString();
        summary = sustained ? QStringLiteral("发现 %1 项持续问题%2。").arg(sustained).arg(transient ? QStringLiteral("、%1 项短暂线索").arg(transient) : QString())
                            : QStringLiteral("压力没有持续，可能已经过去。");
    }
    if (state_ == "cancelled") headline = QStringLiteral("已取消诊断");
    if (state_ == "interrupted") headline = QStringLiteral("诊断被中断");
    return {{"state", state_}, {"progress", state_ == "completed" ? 100 : qBound(0, int(qMin(elapsed_, Duration) * 100 / Duration), 100)},
        {"elapsedSeconds", elapsed_ / 1000.}, {"durationSeconds", 10}, {"sampleCount", samples_},
        {"startedAt", started_ >= 0 ? QJsonValue(startedAt_) : QJsonValue(QJsonValue::Null)},
        {"finishedAt", finishedAt_ > 0 ? QJsonValue(finishedAt_) : QJsonValue(QJsonValue::Null)},
        {"headline", headline}, {"summary", summary}, {"findings", findings}, {"checks", checks}, {"coverage", coverage},
        {"suspects", suspects}, {"rankingBasis", basis},
        {"schedulerMaxMs", schedulerMax_ >= 0 ? QJsonValue(schedulerMax_) : QJsonValue(QJsonValue::Null)},
        {"displayMaxMs", displayMax_ >= 0 ? QJsonValue(displayMax_) : QJsonValue(QJsonValue::Null)}};
}
