#include "../src/report_html.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void check(bool value, const char *message) {
    ++checks;
    if (!value) { std::fprintf(stderr, "FAIL report html: %s\n", message); std::exit(1); }
}
constexpr double GiB = 1024.0 * 1024 * 1024;
const QString Hostile = QStringLiteral("<script>alert(1)</script>");

// Nothing from the data may become markup, run, or fetch anything.
void checkInert(const QString &html, const char *what) {
    check(html.startsWith("<!doctype html>"), what);
    check(html.contains("Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:\""), what);
    check(!html.contains("<script", Qt::CaseInsensitive) && !html.contains("<img", Qt::CaseInsensitive) && !html.contains("<svg onload", Qt::CaseInsensitive), what);
    // An event handler inside a real tag; escaped text never contains a raw "<".
    check(!html.contains(QRegularExpression("<[a-z][^>]*\\son[a-z]+\\s*=", QRegularExpression::CaseInsensitiveOption)), what);
    check(!html.contains("http:") && !html.contains("https:") && !html.contains("url("), what);
}
QJsonArray series(double start, const QList<double> &minutes, double base) {
    QJsonArray points;
    for (double minute : minutes)
        points.append(QJsonObject{{"timestamp", start + minute * 60}, {"value", base + minute / 1000}, {"minimum", base}, {"maximum", base + .1}});
    return points;
}
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const double now = QDateTime::currentSecsSinceEpoch(), start = now - 3600;

    // A diagnosis report whose every free text field tries to inject markup.
    const QJsonObject report{
        {"version", "0.8.4"}, {"capturedAt", now},
        {"diagnostics", QJsonObject{{"state", "critical"}, {"headline", QStringLiteral("内存不足 <b>")}, {"summary", "a & b \"q\""},
            {"findings", QJsonArray{QJsonObject{{"severity", "critical"}, {"title", Hostile}, {"evidence", "<img src=x onerror=alert(1)>"}, {"suggestion", QStringLiteral("关闭不用的程序")}}}},
            {"meters", QJsonArray{QJsonObject{{"label", QStringLiteral("内存")}, {"value", "98%"}, {"detail", "<i>"}, {"tone", "critical\" onclick=\"x"}}}},
            {"coverage", QJsonArray{QStringLiteral("此内核没有 PSI。")}}}},
        {"suspects", QJsonArray{QJsonObject{{"name", "<svg onload=alert(1)>"}, {"owner", ""}, {"own", true}, {"processCount", 2}, {"cpuPercent", 150.0},
            {"memoryBytes", 2 * GiB}, {"ioBytesPerSecond", QJsonValue::Null}, {"reason", QStringLiteral("占内存 30%")}},
            QJsonObject{{"name", "kswapd0"}, {"owner", "root"}, {"kernelThread", true}, {"processCount", 1}, {"cpuPercent", 40.0}, {"memoryBytes", 0}}}},
        {"oneClickDiagnosis", QJsonObject{{"state", "completed"}, {"startedAt", now - 12}, {"elapsedSeconds", 10}, {"sampleCount", 6},
            {"headline", QStringLiteral("内存不足")}, {"summary", QStringLiteral("持续")},
            {"checks", QJsonArray{QJsonObject{{"title", QStringLiteral("内存")}, {"status", "pressure"}}, QJsonObject{{"title", QStringLiteral("温度")}, {"status", "unknown"}}}},
            {"findings", QJsonArray{QJsonObject{{"severity", "attention"}, {"title", QStringLiteral("磁盘繁忙")}, {"persistence", "sustained"}, {"observedSamples", 5}, {"totalSamples", 6}}}},
            {"suspects", QJsonArray{QJsonObject{{"name", "firefox"}, {"processCount", 3}, {"averageCpuPercent", 120.0}, {"peakCpuPercent", 300.0}, {"peakMemoryBytes", GiB}}}},
            {"coverage", QJsonArray{QStringLiteral("这是约 10 秒的观察。"), QStringLiteral("此内核没有 PSI。")}}}},
        {"cpu", QJsonObject{{"usage", .97}, {"coreCount", 8}, {"load", QJsonArray{13.4, 10.0, 8.2}}}},
        {"memory", QJsonObject{{"totalBytes", 16 * GiB}, {"availableBytes", .3 * GiB}}},
        {"diskIO", QJsonObject{{"busiestDevice", "nvme0n1<"}, {"busiestUtilization", .97}, {"busiestAwaitMilliseconds", 340}}},
        {"processStates", QJsonObject{{"total", 300}, {"running", 9}, {"blocked", 3}, {"zombie", 0}}},
        // Two runs of minutes with an hour-long gap between them: the line must break.
        {"history", QJsonObject{{"cpu", series(start, {0, 1, 2, 3, 50, 51, 52}, .5)}, {"diskBusy", series(start, {10}, .3)}, {"swapOut", QJsonArray{}}}},
        {"historyStart", start}, {"historyEnd", now}, {"note", QStringLiteral("排行表示相关。")}};
    const QString html = DevStatsReport::diagnosisHtml(report);
    checkInert(html, "diagnosis report is inert");
    check(html.contains("&lt;script&gt;alert(1)&lt;/script&gt;") && html.contains("a &amp; b &quot;q&quot;") && html.contains("&lt;svg onload=alert(1)&gt;"),
          "text that looks like markup is shown literally");
    check(html.contains("class=\"meter neutral\"") && !html.contains("onclick=\"x\""), "a tone outside the known set cannot add attributes");
    for (const auto *title : {"关键指标", "发现的问题", "一键诊断", "相关进程", "详细数据", "历史曲线", "数据来源与限制", "原始数据（JSON）"})
        check(html.contains(QString::fromUtf8(title)), "every section is present");
    check(html.contains(QStringLiteral("持续，5 / 6 次采样")) && html.contains(QStringLiteral("内存 有压力")) && html.contains(QStringLiteral("温度 不可测")), "the one-click result reads plainly");
    check(html.contains(QStringLiteral("<td>我</td>")) && html.contains(QStringLiteral("<td>root</td>")), "owners are named");
    check(html.count("<polyline") == 2, "a gap in the history breaks the line");
    check(html.count("<circle") == 1, "a single minute is still drawn");
    check(html.contains(QStringLiteral("这段时间没有数据")), "an empty series says so");
    check(html.contains("&quot;version&quot;: &quot;0.8.4&quot;"), "the data is appended, escaped");
    check(html.contains(QStringLiteral("13.40 / 10.00 / 8.20")) && html.contains("nvme0n1&lt;"), "details are listed");
    check(html.count(QStringLiteral("<li>此内核没有 PSI。</li>")) == 1 && html.contains(QStringLiteral("<li>这是约 10 秒的观察。</li>")), "a note shared by the live and one-click results is listed once");

    const QString empty = DevStatsReport::diagnosisHtml({});
    checkInert(empty, "an empty report is still a page");
    check(empty.contains(QStringLiteral("还没有结论")) && empty.contains(QStringLiteral("没有进程数据")) && !empty.contains(QStringLiteral("一键诊断")), "an empty report explains itself");

    // A recorded stall.
    const QJsonObject witness{{"timestamp", now - 300}, {"labels", QJsonArray{"onset"}}, {"cpu", QJsonObject{{"usage", .5}}},
        {"findings", QJsonArray{QJsonObject{{"severity", "attention"}, {"title", QStringLiteral("CPU 繁忙")}}}}};
    const QJsonObject peak{{"timestamp", now - 200}, {"labels", QJsonArray{"peakCPU", "peakMemoryPressure"}}, {"cpu", QJsonObject{{"usage", .99}}},
        {"memory", QJsonObject{{"availableBytes", .2 * GiB}}}, {"vm", QJsonObject{{"swapInBytesPerSecond", 1024.0 * 1024}, {"swapOutBytesPerSecond", 0}}},
        {"responsiveness", QJsonObject{{"schedulerMaxMs", 1400}, {"displayMaxMs", 640}}}, {"thermal", QJsonObject{{"hottestCelsius", 93}}},
        {"findings", QJsonArray{QJsonObject{{"severity", "attention"}, {"title", QStringLiteral("CPU 繁忙")}}, QJsonObject{{"severity", "critical"}, {"title", Hostile}}}},
        {"topProcesses", QJsonArray{QJsonObject{{"name", "<b>evil</b>"}, {"pid", 42}, {"cpuPercent", 300.0}, {"memoryBytes", GiB}, {"ioReadBytesPerSecond", 2048.0}},
                                    QJsonObject{{"name", "Xorg"}, {"owner", "root"}, {"pid", 812}, {"cpuPercent", 74.0}}}},
        {"processCoverage", QJsonObject{{"notes", QJsonArray{QStringLiteral("这次采样的进程列表是缓存的。")}}}}};
    const QJsonObject incident{{"id", "e1"}, {"summary", QStringLiteral("CPU 繁忙 & <u>")}, {"headlineEvidence", QStringLiteral("CPU 99%")}, {"status", "recovered"},
        {"severity", "critical"}, {"startTimestamp", now - 300}, {"durationSeconds", 125}, {"episodeCount", 2}, {"kinds", QJsonArray{"cpu", "memory"}},
        {"interruptionReason", "<i>bye</i>"}, {"strongestApplication", QJsonObject{{"name", "firefox"}, {"cpuPercent", 300.0}}}, {"strongestApplicationBasis", "cpu"},
        {"witnesses", QJsonArray{witness, peak}}, {"prelude", QJsonArray{QJsonObject{{"timestamp", now - 400}, {"cpu", QJsonObject{{"usage", .2}}}}}}};
    const QString stall = DevStatsReport::incidentHtml(incident);
    checkInert(stall, "incident report is inert");
    check(stall.contains("CPU 繁忙 &amp; &lt;u&gt;") && stall.contains("&lt;b&gt;evil&lt;/b&gt;") && stall.contains("&lt;i&gt;bye&lt;/i&gt;"), "incident text is escaped");
    check(stall.contains(QStringLiteral("已恢复")) && stall.contains(QStringLiteral("持续 2 分 5 秒")) && stall.contains(QStringLiteral("合并 2 段"))
          && stall.contains(QStringLiteral("类型：CPU、内存")), "the header says what happened");
    check(stall.contains(QStringLiteral("开始时")) && stall.contains(QStringLiteral("CPU 峰值/内存峰值")) && stall.contains(QStringLiteral("1.4 秒")), "key moments are tabled");
    check(stall.count(QStringLiteral("<strong>CPU 繁忙</strong>")) == 1, "a finding seen twice is listed once");
    check(stall.contains(QStringLiteral("最忙的程序：<strong>firefox</strong>（300.0%）")), "the busiest program is named");
    check(stall.contains("<td class=\"num\">42</td>") && stall.contains("<td>root</td>") && stall.contains("2.00 KB/s"), "the busiest processes at the peak");
    check(stall.contains(QStringLiteral("发生前的状态")) && stall.contains(QStringLiteral("这次采样的进程列表是缓存的。")), "prelude and coverage notes");
    const QString emptyStall = DevStatsReport::incidentHtml({});
    checkInert(emptyStall, "an empty incident is still a page");
    check(emptyStall.contains(QStringLiteral("这条记录没有保存关键时刻")) && emptyStall.contains(QStringLiteral("资源压力事件")), "an empty incident explains itself");

    // File naming and owner-only, atomic writing.
    check(DevStatsReport::htmlPath("/tmp/report") == "/tmp/report.html" && DevStatsReport::htmlPath("/tmp/r.html") == "/tmp/r.html"
          && DevStatsReport::htmlPath("/tmp/r.htm") == "/tmp/r.htm", "a missing suffix becomes .html");
    QTemporaryDir directory;
    check(directory.isValid(), "temporary directory");
    const QString path = directory.filePath("report.html");
    check(DevStatsReport::writePrivate(path, html.toUtf8()).isEmpty(), "report written");
    QFile written(path);
    check(written.open(QIODevice::ReadOnly) && written.readAll() == html.toUtf8(), "written content matches");
    const auto permissions = written.permissions();
    check((permissions & (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther | QFileDevice::WriteOther)) == 0
          && (permissions & QFileDevice::ReadOwner) && (permissions & QFileDevice::WriteOwner), "only the owner can read the report");
    written.close();
    check(DevStatsReport::writePrivate(path, "second").isEmpty(), "an existing report is replaced");
    check(written.open(QIODevice::ReadOnly) && written.readAll() == "second", "replacement is complete");
    written.close();
    const QString locked = directory.filePath("locked");
    check(QDir().mkpath(locked) && QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner), "read-only folder");
    check(!DevStatsReport::writePrivate(locked + "/report.html", "x").isEmpty() && !QFile::exists(locked + "/report.html"), "a failed write says so and leaves nothing");
    QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    std::printf("PASS report html: %d checks; escaped, script-free, offline pages with every section, gaps, empty data and owner-only files\n", checks);
    return 0;
}
