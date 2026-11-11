#include "diagnostics.h"
#include "i18n.h"

#include <QHash>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <optional>

namespace {
using Number = std::optional<double>;
constexpr double MiB = 1024.0 * 1024.0;
constexpr double GiB = 1024.0 * MiB;

Number metric(const QJsonValue &value, double maximum = 1e300) {
    if (!value.isDouble()) return {};
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < 0 || number > maximum) return {};
    return number;
}
Number metric(const QJsonObject &object, const char *key, double maximum = 1e300) {
    return metric(object[key], maximum);
}
Number psi(const QJsonObject &pressure, const char *resource, const char *type) {
    return metric(pressure[resource].toObject()[type].toObject(), "avg10", 100);
}
bool atLeast(Number value, double threshold) { return value && *value >= threshold; }
QString percent(double fraction) { return QString::number(fraction * 100, 'f', 0) + '%'; }
QString milliseconds(double value) {
    return value >= 1000 ? T("%1 秒").arg(QString::number(value / 1000, 'f', 1))
                         : T("%1 毫秒").arg(QString::number(value, 'f', 0));
}
QString size(double bytes) {
    if (bytes >= GiB) return QString::number(bytes / GiB, 'f', 1) + QStringLiteral(" GB");
    if (bytes >= MiB) return QString::number(bytes / MiB, 'f', 0) + QStringLiteral(" MB");
    return QString::number(bytes / 1024, 'f', 0) + QStringLiteral(" KB");
}
QString cores(double oneCorePercent) { return T("%1 核").arg(QString::number(oneCorePercent / 100, 'f', 1)); }

bool isDisplayStack(const QString &name) {
    return name == "Xorg" || name == "Xwayland" || name.startsWith("ukui-kwin") || name.startsWith("kwin_") ||
           name == "ukwm" || name == "mutter" || name == "gnome-shell" || name == "marco" || name == "xfwm4";
}

// Only evaluated when a finding needs to name a process. Idle snapshots, which
// are the overwhelming majority, never walk the process list here.
// CPU a process used itself plus what its children used without ever being seen
// alive (short-lived processes, credited when the parent waited for them).
double cpuWithChildren(const QJsonObject &p) {
    const double own = p[QLatin1String("cpuPercent")].toDouble(-1), children = p[QLatin1String("childCpuPercent")].toDouble(0);
    return own < 0 && children <= 0 ? -1 : std::max(0.0, own) + children;
}

struct ProcessSurvey {
    QString cpuName, memoryName, ioName, displayName, ownName;
    // cpu includes short-lived children; own is the process alone (a pegged thread).
    double cpu = -1, memory = -1, io = -1, display = -1, cpuChildren = 0, own = -1;
    QStringList blocked;
    explicit ProcessSurvey(const QJsonObject &snapshot) {
        for (const auto *key : {"processes", "systemProcesses"}) {
            for (const auto &value : snapshot[key].toArray()) {
                const auto p = value.toObject();
                // DevStats never names itself: opening its window or running a diagnosis
                // briefly costs CPU, and that is not why the computer was slow.
                if (p[QLatin1String("self")].toBool()) continue;
                const QString name = p[QLatin1String("name")].toString();
                const double usage = cpuWithChildren(p), rss = p[QLatin1String("memoryBytes")].toDouble(-1);
                const double disk = p[QLatin1String("ioReadBytesPerSecond")].toDouble(0) + p[QLatin1String("ioWriteBytesPerSecond")].toDouble(0);
                if (usage > cpu) { cpu = usage; cpuName = name; cpuChildren = p[QLatin1String("childCpuPercent")].toDouble(0); }
                if (p[QLatin1String("cpuPercent")].toDouble(-1) > own) { own = p[QLatin1String("cpuPercent")].toDouble(-1); ownName = name; }
                if (rss > memory) { memory = rss; memoryName = name; }
                if (disk > io) { io = disk; ioName = name; }
                if (isDisplayStack(name) && usage > display) { display = usage; displayName = name; }
                if (p[QLatin1String("state")].toString() == "D" && blocked.size() < 4 && !blocked.contains(name)) blocked.append(name);
            }
        }
    }
};

struct Finding {
    int order;
    QJsonObject json;
};
const QStringList Priority = {"oom", "memory", "io", "cpu", "thermal", "interrupt", "forks", "compositor", "display", "responsiveness", "disk", "singleCore"};
}

QJsonObject diagnoseSystem(const QJsonObject &snapshot) {
    const auto cpu = snapshot["cpu"].toObject();
    const auto memory = snapshot["memory"].toObject();
    const auto pressure = snapshot["pressure"].toObject();
    const auto vm = snapshot["vm"].toObject();
    const auto disk = snapshot["diskIO"].toObject();
    const auto thermal = snapshot["thermal"].toObject();
    const auto probe = snapshot["responsiveness"].toObject();
    const auto states = snapshot["processStates"].toObject();

    const Number usage = metric(cpu, "usage", 1), iowait = metric(cpu, "iowait", 1), irq = metric(cpu, "irq", 1);
    const Number coreCount = metric(cpu, "coreCount"), running = metric(cpu, "runningProcesses");
    const Number blocked = metric(cpu, "blockedProcesses"), queueWait = metric(cpu, "runQueueWait"), forks = metric(cpu, "forksPerSecond");
    const auto load = cpu["load"].toArray();
    const Number load1 = metric(load.isEmpty() ? QJsonValue(QJsonValue::Null) : load[0]);
    Number loadPerCore;
    if (coreCount && *coreCount > 0 && load1) loadPerCore = *load1 / *coreCount;
    const Number total = metric(memory, "totalBytes"), available = metric(memory, "availableBytes");
    Number availableFraction;
    if (total && *total > 0 && available && *available <= *total) availableFraction = *available / *total;
    const Number swapIn = metric(vm, "swapInBytesPerSecond"), swapOut = metric(vm, "swapOutBytesPerSecond");
    const Number oom = metric(vm, "oomKillDelta");
    const Number allocStalls = metric(vm, "allocStallsPerSecond"), directScan = metric(vm, "directReclaimPagesPerSecond");
    const Number cpuSome = psi(pressure, "cpu", "some");
    const Number memorySome = psi(pressure, "memory", "some"), memoryFull = psi(pressure, "memory", "full");
    const Number ioSome = psi(pressure, "io", "some"), ioFull = psi(pressure, "io", "full");
    const Number diskBusy = metric(disk, "busiestUtilization", 1), diskWait = metric(disk, "busiestAwaitMilliseconds");
    const Number schedulerMax = metric(probe, "schedulerMaxMs"), displayMax = metric(probe, "displayMaxMs");
    const Number hottest = metric(thermal, "hottestCelsius"), frequency = metric(thermal, "frequencyFraction");
    const bool psiAvailable = bool(cpuSome) || bool(memorySome) || bool(ioSome);

    std::optional<ProcessSurvey> surveyed;
    auto survey = [&]() -> const ProcessSurvey & { if (!surveyed) surveyed.emplace(snapshot); return *surveyed; };

    QVector<Finding> findings;
    bool critical = false;
    auto add = [&](const char *kind, bool severe, const QString &title, const QStringList &evidence, const QString &suggestion,
                   const char *action = "none", bool immediate = false) {
        critical |= severe;
        findings.append({int(Priority.indexOf(QString::fromLatin1(kind))),
            QJsonObject{{"kind", QString::fromLatin1(kind)}, {"severity", severe ? "critical" : "attention"},
                {"title", title}, {"evidence", evidence.join(QStringLiteral(" · "))}, {"suggestion", suggestion},
                {"action", QString::fromLatin1(action)}, {"immediate", immediate}}});
    };

    if (atLeast(oom, 1)) {
        add("oom", true, T("系统内存耗尽，已有进程被内核结束"),
            {T("本次采样内 OOM 终止 %1 次").arg(*oom, 0, 'f', 0)},
            T("保存工作，关闭不用的大程序。被结束的进程可用 journalctl -k | grep -i oom 查看。"), "relief", true);
    }

    // Memory. Used swap or a large cache alone is not a problem; ongoing reclaim is.
    const bool lowMemory = availableFraction && *availableFraction <= .10;
    const bool activeSwap = atLeast(swapIn, MiB) || atLeast(swapOut, MiB);
    const bool directReclaim = atLeast(allocStalls, 1) || atLeast(directScan, 2000);
    const bool memoryStall = atLeast(memorySome, 5) || atLeast(memoryFull, 1);
    const bool swapStorm = atLeast(swapIn, 20 * MiB);
    if ((lowMemory && (activeSwap || directReclaim || memoryStall)) || memoryStall || swapStorm) {
        const bool severe = (availableFraction && *availableFraction <= .03 && (activeSwap || directReclaim)) ||
            atLeast(memoryFull, 10) || atLeast(memorySome, 30) || atLeast(allocStalls, 50);
        QStringList evidence;
        if (available && availableFraction) evidence << T("可用 %1（%2）").arg(size(*available), percent(*availableFraction));
        if (activeSwap) evidence << T("换入 %1/s，换出 %2/s").arg(size(swapIn.value_or(0)), size(swapOut.value_or(0)));
        if (directReclaim) evidence << T("直接回收 %1 次/s").arg(allocStalls.value_or(0), 0, 'f', 0);
        if (memorySome || memoryFull) evidence << T("内存停顿 %1%").arg(memorySome.value_or(memoryFull.value_or(0)), 0, 'f', 1);
        const auto &top = survey();
        add("memory", severe, lowMemory ? T("内存不足，系统正在回收内存") : T("内存回收造成停顿"), evidence,
            top.memory > 0 ? T("占用最多的是 %1（%2）。保存工作后关闭它或其他不用的程序。").arg(top.memoryName, size(top.memory))
                           : T("保存工作后关闭不用的程序。"), "relief");
    }

    // Disk. Device wait time is the direct symptom; PSI and iowait corroborate it.
    const bool slowDisk = atLeast(diskBusy, .85) && atLeast(diskWait, 50);
    const bool ioStall = atLeast(ioSome, 10) || atLeast(ioFull, 2);
    const bool ioWaiting = atLeast(iowait, .20) && atLeast(blocked, 1);
    if (slowDisk || ioStall || ioWaiting) {
        const bool severe = atLeast(diskWait, 300) || atLeast(ioFull, 20) || atLeast(ioSome, 50);
        QStringList evidence;
        if (diskBusy) evidence << T("%1 繁忙 %2").arg(disk["busiestDevice"].toString(), percent(*diskBusy));
        if (diskWait) evidence << T("平均等待 %1").arg(milliseconds(*diskWait));
        if (iowait) evidence << QStringLiteral("iowait %1").arg(percent(*iowait));
        if (ioSome) evidence << T("I/O 停顿 %1%").arg(*ioSome, 0, 'f', 1);
        const auto &top = survey();
        if (!top.blocked.isEmpty()) evidence << T("等待磁盘的进程：%1").arg(top.blocked.join(T("、")));
        add("io", severe, T("磁盘繁忙，读写响应变慢"), evidence,
            top.io > 0 ? T("读写最多的是 %1（%2/s）。大文件复制、更新或扫描可错开进行。").arg(top.ioName, size(top.io))
                       : T("大文件复制、系统更新或安全扫描可错开进行。"), "suspect");
    }

    // CPU.
    const bool queueFull = coreCount && *coreCount > 0 && running && *running >= *coreCount;
    const bool cpuBusy = atLeast(usage, .85) && (atLeast(loadPerCore, .9) || queueFull);
    // PSI is a ten-second average, so it needs a current reading beside it: otherwise
    // the finding would outlive the load by half a minute.
    const bool cpuContention = atLeast(usage, .6) && (atLeast(cpuSome, 15) || atLeast(queueWait, .5));
    if (cpuBusy || cpuContention) {
        const bool severe = atLeast(usage, .95) && (atLeast(loadPerCore, 1.5) || atLeast(cpuSome, 30) || atLeast(queueWait, 1.5));
        QStringList evidence;
        if (usage) evidence << QStringLiteral("CPU %1").arg(percent(*usage));
        if (load1 && coreCount) evidence << T("负载 %1 / %2 核").arg(*load1, 0, 'f', 1).arg(*coreCount, 0, 'f', 0);
        if (queueWait) evidence << T("排队 %1").arg(*queueWait, 0, 'f', 2);
        if (cpuSome) evidence << T("CPU 停顿 %1%").arg(*cpuSome, 0, 'f', 1);
        const auto &top = survey();
        const bool named = top.cpu >= 50;
        const QString scope = top.cpuChildren >= top.cpu / 2 ? T("，含它启动的短暂子进程") : QString();
        add("cpu", severe, named ? T("CPU 繁忙，占用最高的是 %1（%2%3）").arg(top.cpuName, cores(top.cpu), scope) : T("CPU 繁忙，任务正在排队"),
            evidence, T("确认不需要后可结束下方的进程；经常出现的话可以加入自动查杀。"), "suspect");
    }

    // A single pegged thread leaves a many-core machine responsive overall, so it
    // is only worth a hint unless it also feeds one of the findings above.
    if (!cpuBusy && !cpuContention && snapshot["processesFresh"].toBool()) {
        const auto &top = survey();
        if (top.own >= 95) {
            QJsonObject hint{{"kind", "singleCore"}, {"severity", "info"},
                {"title", T("%1 占满了约 %2").arg(top.ownName, cores(top.own))},
                {"evidence", T("整机 CPU %1").arg(usage ? percent(*usage) : QStringLiteral("—"))},
                {"suggestion", T("该程序本身可能无响应，其他程序一般不受影响。")}, {"action", "suspect"}, {"immediate", false}};
            findings.append({int(Priority.indexOf("singleCore")), hint});
        }
    }

    // Thermal limits and frequency caps slow everything without raising any load figure.
    const bool hot = atLeast(hottest, 90);
    const bool slowedUnderLoad = atLeast(usage, .7) && frequency && *frequency <= .6;
    if (hot || slowedUnderLoad) {
        QStringList evidence;
        if (hottest) evidence << T("最高 %1 °C（%2）").arg(*hottest, 0, 'f', 0).arg(thermal["hottestZone"].toString());
        if (frequency) evidence << T("CPU 频率为最高的 %1").arg(percent(*frequency));
        if (usage) evidence << QStringLiteral("CPU %1").arg(percent(*usage));
        add("thermal", atLeast(hottest, 100), hot ? T("CPU 温度过高，可能降频") : T("高负载下 CPU 频率偏低"), evidence,
            thermal["limited"].toBool() ? T("当前频率上限被限制。检查电源模式是否为节能，并清理散热口。")
                                        : T("检查散热口和电源模式；接通电源通常能恢复频率。"));
    }

    if (atLeast(irq, .25)) {
        add("interrupt", atLeast(irq, .5), T("中断处理占用过高"), {T("硬中断和软中断占 %1").arg(percent(*irq))},
            T("常见于大流量网络或异常设备。可暂时断开外设或网络后对比。"));
    }

    // CPU that no surviving process accounts for points at many short-lived processes.
    const Number attributed = metric(snapshot["attributedCpuPercent"]);
    // The parent that waited for them carries their CPU, which names the source.
    QJsonObject source;
    for (const auto &value : snapshot["processCreation"].toObject()["parents"].toArray())
        if (!value.toObject()["self"].toBool()) { source = value.toObject(); break; }
    const double sourceCpu = source["childCpuPercent"].toDouble(0);
    const int sourceChildren = source["newChildren"].toInt(0);
    const double unattributed = usage && coreCount && attributed ? *usage * *coreCount * 100 - *attributed : 0;
    if ((atLeast(forks, 100) && unattributed >= 100) || (atLeast(forks, 50) && sourceCpu >= 50)) {
        QStringList evidence{T("每秒新建 %1 个进程").arg(*forks, 0, 'f', 0)};
        if (unattributed >= 50) evidence << T("约 %1的 CPU 没有归属到存活进程").arg(cores(unattributed));
        QString advice = T("通常来自循环调用命令的脚本。检查定时任务或构建脚本。");
        if (sourceCpu >= 10 || sourceChildren >= 5) {
            const QString who = T("%1（PID %2%3）").arg(source["name"].toString(), QString::number(source["pid"].toDouble(), 'f', 0),
                source["own"].toBool() ? QString() : T("，") + source["owner"].toString());
            QStringList facts;
            if (sourceCpu >= 10) facts << T("它启动的短暂子进程用了约 %1").arg(cores(sourceCpu));
            if (sourceChildren >= 5) facts << T("两次扫描间新出现 %1 个子进程").arg(sourceChildren);
            evidence << T("主要来自 %1：%2").arg(who, facts.join(T("，")));
            advice = source["own"].toBool()
                ? T("%1 在反复启动命令。确认它在做什么；不需要的话可以结束它，或加入自动查杀。").arg(source["name"].toString())
                : T("%1 属于 %2，在反复启动命令；只能由管理员处理，常见于安全软件或系统服务的定时检查。").arg(source["name"].toString(), source["owner"].toString());
        }
        add("forks", false, T("短命进程过多"), evidence, advice, "suspect");
    }

    if (snapshot["processesFresh"].toBool()) {
        const auto &top = survey();
        if (top.display >= 70) {
            add("compositor", top.display >= 150, T("桌面显示服务占用偏高：%1").arg(top.displayName), {T("占用 %1").arg(cores(top.display))},
                T("关闭窗口特效或减少高刷新内容（视频、动画）后观察。"));
        }
    }

    // Direct measurements of a stall.
    if (atLeast(displayMax, 500)) {
        add("display", atLeast(displayMax, 2000), T("桌面显示响应变慢"), {T("显示服务往返 %1（正常低于 20 毫秒）").arg(milliseconds(*displayMax))},
            T("CPU 不高时多与显卡驱动或窗口合成有关，可关闭窗口特效后对比。"), "none", atLeast(displayMax, 3000));
    }
    if (atLeast(schedulerMax, 300)) {
        const bool frozen = *schedulerMax >= 1000;
        add("responsiveness", frozen, frozen ? T("系统出现短暂冻结") : T("程序调度明显延迟"),
            {T("线程被延迟 %1（正常低于 10 毫秒）").arg(milliseconds(*schedulerMax))},
            T("此时的进程排行最值得查看。"), "suspect", frozen);
    }

    // Capacity is not a stall by itself, but a nearly full disk causes its own failures.
    bool diskKnown = false;
    for (const auto &value : snapshot["disks"].toArray()) {
        const auto volume = value.toObject();
        const Number fraction = metric(volume, "usageFraction", 1), free = metric(volume, "availableBytes");
        if (!fraction || !free) continue;
        diskKnown = true;
        if (*fraction < .95 || *free > 2 * GiB) continue;
        add("disk", *fraction >= .99 && *free <= .5 * GiB, T("磁盘空间不足：%1").arg(volume["mountPoint"].toString()),
            {T("已用 %1，剩余 %2").arg(percent(*fraction), size(*free))},
            T("清理大文件或构建产物。"));
    }

    std::stable_sort(findings.begin(), findings.end(), [](const Finding &a, const Finding &b) {
        const auto rank = [](const Finding &f) { const auto s = f.json["severity"].toString(); return s == "critical" ? 0 : s == "attention" ? 1 : 2; };
        return rank(a) != rank(b) ? rank(a) < rank(b) : a.order < b.order;
    });
    QJsonArray findingList;
    int problems = 0;
    for (const auto &finding : findings) {
        findingList.append(finding.json);
        if (finding.json["severity"].toString() != "info") ++problems;
    }

    QJsonArray coverage;
    if (!psiAvailable) coverage.append(T("此内核没有 PSI（/proc/pressure），改用调度排队、内存回收和磁盘等待时间判断。"));
    if (!queueWait) coverage.append(T("调度排队时间不可用（/proc/schedstat）。"));
    if (!schedulerMax) coverage.append(T("线程调度延迟尚无数据。"));
    if (!probe["displayAvailable"].toBool()) coverage.append(T("未连接到 X 显示服务，无法测量显示响应。"));
    if (!hottest && !frequency) coverage.append(T("此设备未提供温度或频率读数。"));
    if (!diskBusy) coverage.append(T("磁盘繁忙度和等待时间尚无数据。"));
    if (!diskKnown) coverage.append(T("未读取到文件系统容量。"));
    coverage.append(T("其他用户和系统进程只能看到 CPU 与内存，不能结束；当前用户的进程才能操作。"));
    // Read from the sampler's own figure: healthy snapshots must not walk the process list.
    const Number selfCpu = metric(snapshot["selfCpuPercent"]);
    if (atLeast(selfCpu, 30)) coverage.append(T("DevStats 自身此刻占用 %1（打开窗口或一键诊断时会短暂升高），已排除在可疑程序之外。").arg(cores(*selfCpu)));
    coverage.append(T("GPU 负载、应用内部的锁等待和网络延迟无法直接测量。"));

    const bool known = usage && availableFraction;
    const QString state = critical ? QStringLiteral("critical") : problems ? QStringLiteral("attention")
        : known ? QStringLiteral("normal") : QStringLiteral("unknown");
    QString headline, summary;
    if (problems) {
        const auto first = findings.first().json;
        headline = first["title"].toString();
        summary = first["evidence"].toString();
        if (problems > 1) summary += T("。另有 %1 项相关线索。").arg(problems - 1);
    } else if (known) {
        headline = T("系统运行流畅");
        summary = T("CPU %1，可用内存 %2").arg(percent(*usage), size(*available));
        if (schedulerMax) summary += T("，调度延迟 %1").arg(milliseconds(*schedulerMax));
    } else {
        headline = T("正在收集数据");
        summary = T("首次采样后约 2 秒给出结果。");
    }

    auto severityOf = [&](const QStringList &kinds) {
        QString tone = QStringLiteral("ok");
        for (const auto &finding : findings) {
            if (!kinds.contains(finding.json["kind"].toString())) continue;
            const auto severity = finding.json["severity"].toString();
            if (severity == "critical") return QStringLiteral("critical");
            if (severity == "attention") tone = QStringLiteral("warn");
        }
        return tone;
    };
    auto meter = [](const char *key, const QString &label, const QString &value, const QString &detail, const QString &tone, Number fraction) {
        return QJsonObject{{"key", QString::fromLatin1(key)}, {"label", label}, {"value", value}, {"detail", detail}, {"tone", tone},
            {"fraction", fraction ? QJsonValue(std::min(1.0, *fraction)) : QJsonValue(QJsonValue::Null)}};
    };
    const QString none = QString::fromUtf8("—");
    const QString cpuDetail = load1 && coreCount
        ? T("负载 %1 / %2 核").arg(*load1, 0, 'f', 1).arg(*coreCount, 0, 'f', 0) + (queueWait ? T(" · 排队 %1").arg(*queueWait, 0, 'f', 1) : QString())
        : T("等待采样");
    const QString memoryDetail = available
        ? T("可用 %1 · 换页 %2/s").arg(size(*available), size(swapIn.value_or(0) + swapOut.value_or(0))) : T("等待采样");
    const QString diskDetail = diskBusy
        ? T("%1 · 等待 %2").arg(disk["busiestDevice"].toString(), diskWait ? milliseconds(*diskWait) : none) + (iowait ? QStringLiteral(" · iowait %1").arg(percent(*iowait)) : QString())
        : T("等待采样");
    const QString responseDetail = displayMax ? T("显示服务 %1").arg(milliseconds(*displayMax)) : T("显示服务不可测");
    const QString thermalDetail = frequency ? T("频率为最高的 %1").arg(percent(*frequency)) : T("频率不可读");
    const auto runningNow = states["running"], zombies = states["zombie"];
    const QJsonArray meters{
        meter("cpu", T("处理器"), usage ? percent(*usage) : none, cpuDetail, !usage ? QStringLiteral("neutral") : severityOf({"cpu", "interrupt", "forks"}), usage),
        meter("memory", T("内存"), metric(memory, "fraction", 1) ? percent(*metric(memory, "fraction", 1)) : none, memoryDetail, !available ? QStringLiteral("neutral") : severityOf({"memory", "oom"}), metric(memory, "fraction", 1)),
        meter("disk", T("磁盘繁忙"), diskBusy ? percent(*diskBusy) : none, diskDetail, !diskBusy ? QStringLiteral("neutral") : severityOf({"io"}), diskBusy),
        meter("response", T("响应延迟"), schedulerMax ? milliseconds(*schedulerMax) : none, responseDetail, !schedulerMax ? QStringLiteral("neutral") : severityOf({"responsiveness", "display", "compositor"}), schedulerMax ? Number(*schedulerMax / 1000) : Number()),
        meter("thermal", T("温度"), hottest ? QString::number(*hottest, 'f', 0) + QStringLiteral(" °C") : none, thermalDetail, !hottest && !frequency ? QStringLiteral("neutral") : severityOf({"thermal"}), hottest ? Number(*hottest / 100) : Number()),
        meter("blocked", T("等待磁盘的进程"), blocked ? QString::number(*blocked, 'f', 0) : none,
              T("运行 %1 · 僵尸 %2").arg(runningNow.isDouble() ? QString::number(runningNow.toInt()) : none, zombies.isDouble() ? QString::number(zombies.toInt()) : none),
              !blocked ? QStringLiteral("neutral") : atLeast(blocked, 4) ? QStringLiteral("warn") : QStringLiteral("ok"), blocked ? Number(*blocked / 16) : Number())};
    return {{"state", state}, {"headline", headline}, {"summary", summary}, {"findings", findingList}, {"meters", meters}, {"coverage", coverage}};
}

int findingPriority(const QString &kind) {
    const int index = Priority.indexOf(kind);
    return index < 0 ? Priority.size() : index;
}

QString suspectBasis(const QJsonObject &diagnosis) {
    for (const auto &value : diagnosis["findings"].toArray()) {
        const auto kind = value.toObject()["kind"].toString();
        if (kind == "memory" || kind == "oom") return QStringLiteral("memory");
        if (kind == "io") return QStringLiteral("io");
        if (kind == "cpu" || kind == "responsiveness" || kind == "singleCore") return QStringLiteral("cpu");
    }
    return QStringLiteral("cpu");
}

QJsonArray rankSuspects(const QJsonObject &snapshot, const QString &basis, int limit) {
    struct Group {
        QString key, name, owner;
        bool own = false, kernel = false, cpuKnown = false, ioKnown = false;
        int count = 0, blocked = 0, born = 0;
        double cpu = 0, memory = 0, io = 0, growth = 0, children = 0;
        QVector<QJsonObject> members;
    };
    auto basisValue = [&basis](double cpu, double memory, double io) { return basis == "memory" ? memory : basis == "io" ? io : cpu; };
    QHash<QString, QString> applicationNames;
    for (const auto &value : snapshot["activityApplications"].toArray()) {
        const auto app = value.toObject();
        applicationNames.insert(app[QLatin1String("identity")].toString(), app[QLatin1String("name")].toString());
    }
    QHash<QString, Group> groups;
    auto include = [&](const QJsonObject &p, const QString &key, const QString &name, const QString &owner, bool own) {
        auto &group = groups[key];
        if (group.key.isEmpty()) { group.key = key; group.name = name; group.owner = owner; group.own = own; group.kernel = p[QLatin1String("kernelThread")].toBool(); }
        ++group.count;
        if (p[QLatin1String("cpuPercent")].isDouble()) { group.cpu += p[QLatin1String("cpuPercent")].toDouble(); group.cpuKnown = true; }
        // Short-lived children are credited to the parent that started them.
        if (p[QLatin1String("childCpuPercent")].toDouble(0) > 0) { group.cpu += p[QLatin1String("childCpuPercent")].toDouble(); group.children += p[QLatin1String("childCpuPercent")].toDouble(); group.cpuKnown = true; }
        group.born += p[QLatin1String("newChildren")].toInt(0);
        group.memory += p[QLatin1String("memoryBytes")].toDouble(0);
        if (p[QLatin1String("ioReadBytesPerSecond")].isDouble() || p[QLatin1String("ioWriteBytesPerSecond")].isDouble()) {
            group.io += p[QLatin1String("ioReadBytesPerSecond")].toDouble(0) + p[QLatin1String("ioWriteBytesPerSecond")].toDouble(0); group.ioKnown = true;
        }
        group.growth += p[QLatin1String("memoryGrowthBytesPerSecond")].toDouble(0);
        if (p[QLatin1String("state")].toString() == "D") ++group.blocked;
        group.members.append(p);
    };
    for (const auto &value : snapshot["processes"].toArray()) {
        const auto p = value.toObject();
        if (p[QLatin1String("self")].toBool()) continue;
        const QString identity = p[QLatin1String("applicationIdentity")].toString();
        const QString key = identity.isEmpty() ? QStringLiteral("pid:%1:%2").arg(p[QLatin1String("pid")].toDouble(), 0, 'f', 0).arg(p[QLatin1String("startTicks")].toDouble(), 0, 'f', 0) : QStringLiteral("app:") + identity;
        include(p, key, applicationNames.value(identity, p[QLatin1String("name")].toString()), QString(), true);
    }
    for (const auto &value : snapshot["systemProcesses"].toArray()) {
        const auto p = value.toObject();
        // Kernel workers differ only in their queue suffix: kworker/u8:2 and kworker/3:1.
        const QString name = p[QLatin1String("name")].toString().section('/', 0, 0);
        include(p, QStringLiteral("sys:") + name + ':' + p[QLatin1String("owner")].toString(), name, p[QLatin1String("owner")].toString(), false);
    }
    QVector<Group> ordered;
    ordered.reserve(groups.size());
    for (const auto &group : groups) ordered.append(group);
    std::sort(ordered.begin(), ordered.end(), [&](const Group &a, const Group &b) {
        // Anyone stuck on the disk leads an I/O diagnosis, whatever its throughput.
        if (basis == "io" && (a.blocked > 0) != (b.blocked > 0)) return a.blocked > 0;
        const double left = basisValue(a.cpu, a.memory, a.io), right = basisValue(b.cpu, b.memory, b.io);
        if (left != right) return left > right;
        if (a.cpu != b.cpu) return a.cpu > b.cpu;
        if (a.memory != b.memory) return a.memory > b.memory;
        return a.key < b.key;
    });
    const double coreCount = std::max(1.0, snapshot["cpu"].toObject()["coreCount"].toDouble(1));
    const double totalMemory = snapshot["memory"].toObject()["totalBytes"].toDouble(0);
    QJsonArray result;
    for (auto &group : ordered) {
        if (result.size() >= limit) break;
        // Each member's value is read once and only the eight shown are ordered: a
        // group of a hundred processes would otherwise repeat thousands of lookups.
        QVector<QPair<double, int>> keyed;
        keyed.reserve(group.members.size());
        for (int i = 0; i < group.members.size(); ++i) {
            const auto &p = group.members[i];
            keyed.append({basisValue(cpuWithChildren(p), p[QLatin1String("memoryBytes")].toDouble(-1),
                                     p[QLatin1String("ioReadBytesPerSecond")].toDouble(0) + p[QLatin1String("ioWriteBytesPerSecond")].toDouble(0)), i});
        }
        const int shown = std::min(8, int(keyed.size()));
        std::partial_sort(keyed.begin(), keyed.begin() + shown, keyed.end(), [](const QPair<double, int> &a, const QPair<double, int> &b) {
            return a.first != b.first ? a.first > b.first : a.second < b.second;
        });
        QJsonArray members;
        for (int i = 0; i < shown; ++i) members.append(group.members[keyed[i].second]);
        QStringList reason;
        if (group.blocked) reason << T("%1 个进程在等待磁盘").arg(group.blocked);
        if (group.cpu >= 25) reason << T("约占全机 CPU 的 %1").arg(percent(std::min(1.0, group.cpu / (coreCount * 100))));
        if (group.children >= 25) reason << T("其中短暂子进程约 %1").arg(cores(group.children));
        if (group.born >= 5) reason << T("新启动 %1 个子进程").arg(group.born);
        if (totalMemory > 0 && group.memory >= totalMemory * .1) reason << T("占内存 %1").arg(percent(group.memory / totalMemory));
        if (group.growth >= 5 * MiB) reason << T("内存增长 %1/s").arg(size(group.growth));
        if (group.io >= 5 * MiB) reason << T("读写 %1/s").arg(size(group.io));
        result.append(QJsonObject{{"key", group.key}, {"name", group.name}, {"owner", group.owner}, {"own", group.own}, {"kernelThread", group.kernel},
            {"processCount", group.count}, {"cpuPercent", group.cpuKnown ? QJsonValue(group.cpu) : QJsonValue(QJsonValue::Null)},
            {"memoryBytes", group.memory}, {"ioBytesPerSecond", group.ioKnown ? QJsonValue(group.io) : QJsonValue(QJsonValue::Null)},
            {"blockedCount", group.blocked}, {"reason", reason.join(QStringLiteral(" · "))}, {"members", members}});
    }
    return result;
}
