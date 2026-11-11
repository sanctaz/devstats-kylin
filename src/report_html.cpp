#include "report_html.h"
#include "i18n.h"
#include "charts.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QPair>
#include <QSaveFile>
#include <QSet>
#include <QStringList>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <tuple>

namespace {
const QString None = QString::fromUtf8("—");

QString esc(const QString &text) { return text.toHtmlEscaped(); }
double n(const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()) && v.toDouble() >= 0 ? v.toDouble() : -1; }
QString fraction(const QJsonValue &v) { return formatPercent(n(v)); }
QString cpu(const QJsonValue &v) { return n(v) < 0 ? None : QString::number(n(v), 'f', 1) + '%'; }
QString size(const QJsonValue &v, bool rate = false) { return formatBytes(n(v), rate); }
QString decimal(const QJsonValue &v, int places = 1) { return n(v) < 0 ? None : QString::number(n(v), 'f', places); }
QString milliseconds(double ms) {
    if (ms < 0 || !std::isfinite(ms)) return None;
    return ms >= 1000 ? T("%1 秒").arg(QString::number(ms / 1000, 'f', 1)) : T("%1 毫秒").arg(QString::number(ms, 'f', 0));
}
QString milliseconds(const QJsonValue &v) { return milliseconds(n(v)); }
QString when(const QJsonValue &v, const QString &format = QStringLiteral("yyyy-MM-dd HH:mm:ss")) {
    if (n(v) <= 0) return None;
    const auto date = QDateTime::fromMSecsSinceEpoch(qint64(v.toDouble() * 1000)).toLocalTime();
    return date.isValid() ? date.toString(format) : None;
}
QString duration(const QJsonValue &v) {
    if (n(v) < 0) return None;
    const qint64 seconds = qint64(v.toDouble());
    if (seconds < 60) return T("%1 秒").arg(seconds);
    if (seconds < 3600) return T("%1 分 %2 秒").arg(seconds / 60).arg(seconds % 60);
    return T("%1 小时 %2 分").arg(seconds / 3600).arg(seconds / 60 % 60);
}
// Class names come only from this fixed set, never from the data.
QString tone(const QString &value) {
    static const QSet<QString> known{"ok", "warn", "critical", "neutral"};
    return known.contains(value) ? value : QStringLiteral("neutral");
}
QString pill(const QString &text, const QString &toneName) {
    return QStringLiteral("<span class=\"pill %1\">%2</span>").arg(tone(toneName), esc(text));
}
QPair<QString, QString> severityOf(const QString &value) {
    if (value == "critical") return {T("严重"), QStringLiteral("critical")};
    if (value == "attention") return {T("注意"), QStringLiteral("warn")};
    return {T("提示"), QStringLiteral("neutral")};
}
QPair<QString, QString> stateOf(const QString &value) {
    if (value == "critical") return {T("严重"), QStringLiteral("critical")};
    if (value == "attention") return {T("注意"), QStringLiteral("warn")};
    if (value == "normal") return {T("流畅"), QStringLiteral("ok")};
    return {T("未知"), QStringLiteral("neutral")};
}
QPair<QString, QString> statusOf(const QString &value) {
    if (value == "active") return {T("进行中"), QStringLiteral("warn")};
    if (value == "recovered") return {T("已恢复"), QStringLiteral("ok")};
    if (value == "interrupted") return {T("已中断"), QStringLiteral("neutral")};
    return {T("未知"), QStringLiteral("neutral")};
}
QString owner(const QJsonObject &process) {
    const QString name = process.value("owner").toString();
    if (!name.isEmpty()) return name;
    return process.value("kernelThread").toBool() ? T("内核") : T("我");
}

// Every cell is plain text and escaped here; numeric columns are right-aligned.
QString table(const QStringList &headers, const QVector<bool> &numeric, const QList<QStringList> &rows) {
    QString html = QStringLiteral("<div class=\"scroll\"><table><thead><tr>");
    for (int i = 0; i < headers.size(); ++i)
        html += QStringLiteral("<th%1>%2</th>").arg(numeric.value(i) ? QStringLiteral(" class=\"num\"") : QString(), esc(headers[i]));
    html += QStringLiteral("</tr></thead><tbody>");
    for (const auto &row : rows) {
        html += QStringLiteral("<tr>");
        for (int i = 0; i < headers.size(); ++i)
            html += QStringLiteral("<td%1>%2</td>").arg(numeric.value(i) ? QStringLiteral(" class=\"num\"") : QString(), esc(row.value(i, None)));
        html += QStringLiteral("</tr>");
    }
    return html + QStringLiteral("</tbody></table></div>");
}
QString section(const QString &title, const QString &content, const QString &extraClass = {}) {
    return QStringLiteral("<section%1><h2>%2</h2>%3</section>\n")
        .arg(extraClass.isEmpty() ? QString() : QStringLiteral(" class=\"%1\"").arg(extraClass), esc(title), content);
}
QString notes(const QStringList &lines) {
    if (lines.isEmpty()) return {};
    QString html = QStringLiteral("<ul class=\"notes\">");
    for (const auto &line : lines) html += QStringLiteral("<li>%1</li>").arg(esc(line));
    return html + QStringLiteral("</ul>");
}
QStringList strings(const QJsonArray &values) {
    QStringList result;
    for (const auto &value : values) if (!value.toString().isEmpty() && !result.contains(value.toString())) result << value.toString();
    return result;
}

// One finding per block: severity, title, evidence and what to do about it.
QString findings(const QJsonArray &list, const QString &empty) {
    if (list.isEmpty()) return QStringLiteral("<p class=\"empty\">%1</p>").arg(esc(empty));
    QString html;
    for (const auto &value : list) {
        const auto finding = value.toObject();
        const auto level = severityOf(finding.value("severity").toString());
        QString persistence;
        const QString kind = finding.value("persistence").toString();
        if (!kind.isEmpty()) {
            persistence = (kind == "sustained" ? Tc("问题", "持续") : T("偶发"));
            if (finding.value("observedSamples").isDouble() && finding.value("totalSamples").isDouble())
                persistence += T("，%1 / %2 次采样").arg(finding.value("observedSamples").toInt()).arg(finding.value("totalSamples").toInt());
        }
        html += QStringLiteral("<div class=\"finding\"><div class=\"finding-head\">%1<strong>%2</strong>%3</div>")
            .arg(pill(level.first, level.second), esc(finding.value("title").toString(T("未命名的问题"))),
                 persistence.isEmpty() ? QString() : QStringLiteral("<span class=\"muted\">%1</span>").arg(esc(persistence)));
        if (!finding.value("evidence").toString().isEmpty())
            html += QStringLiteral("<div class=\"evidence\">%1</div>").arg(esc(finding.value("evidence").toString()));
        if (!finding.value("suggestion").toString().isEmpty())
            html += T("<div class=\"suggestion\">建议：%1</div>").arg(esc(finding.value("suggestion").toString()));
        html += QStringLiteral("</div>");
    }
    return html;
}

enum class Unit { Fraction, Rate, Milliseconds };
QString valueText(double value, Unit unit) {
    if (unit == Unit::Fraction) return formatPercent(value);
    if (unit == Unit::Rate) return formatBytes(value, true);
    return milliseconds(value);
}
// A minute-aggregated series as an inline SVG: the average line over its min-max band,
// broken where the computer was off or asleep.
QString chart(const QString &title, const QJsonArray &points, double start, double end, Unit unit) {
    struct Point { double time, value, low, high; };
    QVector<Point> data;
    for (const auto &entry : points) {
        const auto point = entry.toObject();
        const double time = n(point.value("timestamp")), value = n(point.value("value"));
        if (time <= 0 || value < 0) continue;
        const double low = n(point.value("minimum")), high = n(point.value("maximum"));
        data.append({time, value, low >= 0 ? std::min(low, value) : value, high >= 0 ? std::max(high, value) : value});
    }
    std::sort(data.begin(), data.end(), [](const Point &a, const Point &b) { return a.time < b.time; });
    QString html = QStringLiteral("<div class=\"chart\"><div class=\"chart-head\"><span>%1</span>").arg(esc(title));
    if (data.isEmpty()) return html + QStringLiteral("</div><p class=\"empty\">%1</p></div>").arg(esc(T("这段时间没有数据")));
    double peak = 0, sum = 0;
    for (const auto &point : data) { peak = std::max(peak, point.high); sum += point.value; }
    html += T("<span class=\"muted\">平均 %1 · 峰值 %2</span></div>").arg(esc(valueText(sum / data.size(), unit)), esc(valueText(peak, unit)));
    if (!(end > start)) { start = data.first().time; end = data.last().time; }
    if (!(end > start)) end = start + 60;
    double top = peak > 0 ? peak * 1.1 : 1;
    if (unit == Unit::Fraction) top = std::min(1.0, std::max(top, 0.05));
    const auto x = [&](double time) { return std::clamp((time - start) / (end - start), 0.0, 1.0) * 600; };
    const auto y = [&](double value) { return 98 - std::clamp(value / top, 0.0, 1.0) * 94; };
    const auto pair = [](double a, double b) { return QString::number(a, 'f', 1) + ',' + QString::number(b, 'f', 1); };
    QVector<double> gaps;
    for (int i = 1; i < data.size(); ++i) gaps.append(data[i].time - data[i - 1].time);
    std::sort(gaps.begin(), gaps.end());
    const double limit = gaps.isEmpty() ? 0 : std::max(180.0, gaps[gaps.size() / 2] * 2.5);
    QString svg = QStringLiteral("<svg viewBox=\"0 0 600 100\" preserveAspectRatio=\"none\" role=\"img\" aria-label=\"%1\">"
                                 "<line class=\"grid\" x1=\"0\" y1=\"51\" x2=\"600\" y2=\"51\"/><line class=\"grid\" x1=\"0\" y1=\"98\" x2=\"600\" y2=\"98\"/>").arg(esc(title));
    int first = 0;
    for (int i = 1; i <= data.size(); ++i) {
        if (i < data.size() && data[i].time - data[i - 1].time <= limit) continue;
        if (i - first == 1) {
            svg += QStringLiteral("<circle class=\"dot\" cx=\"%1\" cy=\"%2\" r=\"2\"/>")
                .arg(QString::number(x(data[first].time), 'f', 1), QString::number(y(data[first].value), 'f', 1));
        } else {
            QStringList band, line;
            for (int k = first; k < i; ++k) { band << pair(x(data[k].time), y(data[k].high)); line << pair(x(data[k].time), y(data[k].value)); }
            for (int k = i - 1; k >= first; --k) band << pair(x(data[k].time), y(data[k].low));
            svg += QStringLiteral("<polygon class=\"band\" points=\"%1\"/><polyline class=\"line\" points=\"%2\"/>").arg(band.join(' '), line.join(' '));
        }
        first = i;
    }
    svg += QStringLiteral("</svg>");
    const QString format = end - start > 86400 ? QStringLiteral("MM-dd HH:mm") : QStringLiteral("HH:mm");
    return html + svg + QStringLiteral("<div class=\"axis\"><span>%1</span><span>%2</span></div></div>")
        .arg(esc(when(start, format)), esc(when(end, format)));
}

const char *Style = R"CSS(
:root { --bg:#f4f5f7; --surface:#ffffff; --text:#1d2129; --muted:#646a73; --border:#e1e4e8; --accent:#2563eb;
  --ok:#137333; --okSoft:#e6f4ea; --warn:#a15c00; --warnSoft:#fef3e0; --critical:#c5221f; --criticalSoft:#fce8e6; --neutralSoft:#eceef1; }
@media (prefers-color-scheme: dark) {
  :root { --bg:#14171c; --surface:#1c2027; --text:#e6e8eb; --muted:#9aa1ab; --border:#2c323b; --accent:#5b9cff;
    --ok:#5bd38a; --okSoft:#17301f; --warn:#f2b44c; --warnSoft:#36290f; --critical:#ff7b72; --criticalSoft:#3a1b1a; --neutralSoft:#2a3038; }
}
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--text);
  font: 14px/1.6 "Noto Sans CJK SC", "Source Han Sans SC", "WenQuanYi Micro Hei", "PingFang SC", "Microsoft YaHei", sans-serif; }
main { max-width: 1040px; margin: 0 auto; padding: 28px 16px 40px; }
header h1 { font-size: 22px; margin: 0 0 4px; }
.meta, .muted { color: var(--muted); }
.meta { font-size: 13px; margin: 0; }
section { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 16px 18px; margin-top: 14px; }
h2 { font-size: 16px; margin: 0 0 10px; }
.verdict h2 { font-size: 18px; margin: 0; }
h3 { font-size: 14px; margin: 14px 0 6px; }
.verdict-head { display: flex; align-items: center; gap: 10px; margin-bottom: 4px; }
.verdict p { margin: 4px 0 0; }
.verdict.critical { background: var(--criticalSoft); border-color: var(--critical); }
.verdict.warn { background: var(--warnSoft); border-color: var(--warn); }
.verdict.ok { background: var(--okSoft); border-color: var(--ok); }
.pill { display: inline-block; padding: 1px 9px; border-radius: 999px; font-size: 12px; font-weight: 600; white-space: nowrap; background: var(--neutralSoft); color: var(--muted); }
.pill.ok { background: var(--okSoft); color: var(--ok); }
.pill.warn { background: var(--warnSoft); color: var(--warn); }
.pill.critical { background: var(--criticalSoft); color: var(--critical); }
.grid { display: grid; grid-template-columns: repeat(3, 1fr); gap: 10px; }
@media (max-width: 640px) { .grid { grid-template-columns: repeat(2, 1fr); } }
.meter { border: 1px solid var(--border); border-radius: 8px; padding: 10px 12px; }
.meter .label { color: var(--muted); font-size: 13px; }
.meter .value { font-size: 22px; font-weight: 700; line-height: 1.3; }
.meter .detail { color: var(--muted); font-size: 12.5px; }
.meter.warn { border-color: var(--warn); } .meter.warn .value { color: var(--warn); }
.meter.critical { border-color: var(--critical); } .meter.critical .value { color: var(--critical); }
.checks { display: flex; flex-wrap: wrap; gap: 6px; margin: 4px 0 10px; }
.finding { padding: 10px 0; border-top: 1px solid var(--border); }
.finding:first-child { border-top: none; padding-top: 0; }
.finding-head { display: flex; align-items: center; flex-wrap: wrap; gap: 8px; }
.evidence { color: var(--muted); margin-top: 2px; }
.suggestion { margin-top: 2px; }
.scroll { overflow-x: auto; }
table { width: 100%; border-collapse: collapse; font-size: 13px; }
th, td { text-align: left; padding: 6px 8px; border-bottom: 1px solid var(--border); vertical-align: top; }
th { color: var(--muted); font-weight: 500; }
.num { text-align: right; font-variant-numeric: tabular-nums; white-space: nowrap; }
.charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(300px, 1fr)); gap: 16px; }
.chart-head { display: flex; justify-content: space-between; gap: 8px; font-size: 13px; }
.chart svg { display: block; width: 100%; height: 92px; margin-top: 4px; }
.chart .grid { stroke: var(--border); stroke-width: 1; vector-effect: non-scaling-stroke; }
.chart .band { fill: var(--accent); opacity: .16; }
.chart .line { fill: none; stroke: var(--accent); stroke-width: 1.6; vector-effect: non-scaling-stroke; }
.chart .dot { fill: var(--accent); }
.axis { display: flex; justify-content: space-between; color: var(--muted); font-size: 11.5px; }
.empty { color: var(--muted); margin: 0; }
.notes { margin: 0; padding-left: 20px; color: var(--muted); }
details.raw { margin-top: 14px; }
details.raw summary { cursor: pointer; color: var(--muted); }
details.raw pre { overflow: auto; max-height: 520px; background: var(--surface); border: 1px solid var(--border); border-radius: 8px; padding: 12px; font-size: 12px; }
footer { color: var(--muted); font-size: 12px; margin-top: 18px; }
@media print { body { background: #fff; } section { break-inside: avoid; } details.raw { display: none; } }
)CSS";

QString version(const QJsonObject &data) {
    const QString application = QCoreApplication::applicationVersion();
    return application.isEmpty() ? data.value("version").toString() : application;
}
// No scripts may run and nothing may be fetched, even if escaping were ever missed.
QString page(const QString &title, const QString &body, const QJsonObject &raw) {
    const QString data = QString::fromUtf8(QJsonDocument(raw).toJson(QJsonDocument::Indented));
    return QStringLiteral(
        "<!doctype html>\n<html lang=\"%5\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; img-src data:\">\n"
        "<meta name=\"referrer\" content=\"no-referrer\">\n<title>%1</title>\n<style>%2</style>\n</head>\n<body>\n<main>\n%3"
        "<details class=\"raw\"><summary>%6</summary><pre>%4</pre></details>\n"
        "<footer>%7</footer>\n"
        "</main>\n</body>\n</html>\n").arg(esc(title), QString::fromUtf8(Style), body, esc(data), DevStatsLanguage::tag(),
            esc(T("原始数据（JSON）")), esc(T("由 DevStats 生成。报告包含应用名、PID 和资源指标，不含命令行、可执行文件路径和项目路径。")));
}
QString header(const QString &title, const QStringList &meta) {
    return QStringLiteral("<header><h1>%1</h1><p class=\"meta\">%2</p></header>\n").arg(esc(title), esc(meta.join(QStringLiteral(" · "))));
}
QString sampleTitle(const QJsonObject &sample) {
    const QMap<QString, QString> labels{{"onset", T("开始时")}, {"peakCPU", T("CPU 峰值")},
        {"peakMemoryPressure", T("内存峰值")}, {"peakIO", T("磁盘峰值")}, {"peakLatency", T("延迟峰值")},
        {"last", T("最后")}, {"oom", T("内存耗尽")}};
    QStringList marks;
    for (const auto &mark : sample.value("labels").toArray()) if (labels.contains(mark.toString())) marks << labels.value(mark.toString());
    const QString time = when(sample.value("timestamp"), QStringLiteral("HH:mm:ss"));
    return marks.isEmpty() ? time : time + QStringLiteral(" · ") + marks.join('/');
}
QStringList sampleCells(const QJsonObject &sample) {
    const auto cpuData = sample.value("cpu").toObject(), memory = sample.value("memory").toObject(), vm = sample.value("vm").toObject();
    const auto disk = sample.value("disk").toObject(), probe = sample.value("responsiveness").toObject(), thermal = sample.value("thermal").toObject();
    const double swapIn = n(vm.value("swapInBytesPerSecond")), swapOut = n(vm.value("swapOutBytesPerSecond"));
    const double swap = swapIn >= 0 || swapOut >= 0 ? std::max(0.0, swapIn) + std::max(0.0, swapOut) : -1;
    return {sampleTitle(sample), fraction(cpuData.value("usage")), size(memory.value("availableBytes")), formatBytes(swap, true),
            fraction(disk.value("busiestUtilization")), milliseconds(probe.value("schedulerMaxMs")), milliseconds(probe.value("displayMaxMs")),
            n(thermal.value("hottestCelsius")) < 0 ? None : QString::number(n(thermal.value("hottestCelsius")), 'f', 0) + QStringLiteral(" °C")};
}
QStringList sampleHeaders() {
    return {T("时刻"), QStringLiteral("CPU"), T("可用内存"), T("换页"), T("磁盘繁忙"), T("调度延迟"), T("显示延迟"), T("温度")};
}
const QVector<bool> SampleNumeric{false, true, true, true, true, true, true, true};
}

QString DevStatsReport::diagnosisHtml(const QJsonObject &report) {
    const auto diagnosis = report.value("diagnostics").toObject();
    const auto cpuData = report.value("cpu").toObject(), memory = report.value("memory").toObject(), vm = report.value("vm").toObject();
    const auto states = report.value("processStates").toObject(), disk = report.value("diskIO").toObject();
    const auto probe = report.value("responsiveness").toObject(), thermal = report.value("thermal").toObject(), pressure = report.value("pressure").toObject();
    QStringList meta{T("采集于 %1").arg(when(report.value("capturedAt")))};
    if (n(cpuData.value("coreCount")) > 0) meta << T("%1 核").arg(decimal(cpuData.value("coreCount"), 0));
    if (n(memory.value("totalBytes")) > 0) meta << T("内存 %1").arg(size(memory.value("totalBytes")));
    if (n(states.value("total")) >= 0) meta << Tn("%1 个进程", qint64(n(states.value("total")))).arg(decimal(states.value("total"), 0));
    if (!version(report).isEmpty()) meta << QStringLiteral("DevStats %1").arg(version(report));
    QString body = header(T("DevStats 诊断报告"), meta);

    const auto state = stateOf(diagnosis.value("state").toString());
    body += QStringLiteral("<section class=\"verdict %1\"><div class=\"verdict-head\">%2<h2>%3</h2></div><p>%4</p></section>\n")
        .arg(state.second, pill(state.first, state.second), esc(diagnosis.value("headline").toString(T("还没有结论"))),
             esc(diagnosis.value("summary").toString()));

    const auto meters = diagnosis.value("meters").toArray();
    if (!meters.isEmpty()) {
        QString grid = QStringLiteral("<div class=\"grid\">");
        for (const auto &value : meters) {
            const auto meter = value.toObject();
            grid += QStringLiteral("<div class=\"meter %1\"><div class=\"label\">%2</div><div class=\"value\">%3</div><div class=\"detail\">%4</div></div>")
                .arg(tone(meter.value("tone").toString()), esc(meter.value("label").toString()), esc(meter.value("value").toString(None)), esc(meter.value("detail").toString()));
        }
        body += section(T("关键指标"), grid + QStringLiteral("</div>"));
    }
    body += section(T("当前发现的问题"), findings(diagnosis.value("findings").toArray(), T("当时没有发现问题。")));

    const auto run = report.value("oneClickDiagnosis").toObject();
    const QString runState = run.value("state").toString();
    if (runState == "completed" || runState == "cancelled" || runState == "interrupted") {
        QString content = QStringLiteral("<p class=\"meta\">%1</p>").arg(esc(QStringList{
            T("开始 %1").arg(when(run.value("startedAt"))), T("用时 %1").arg(duration(run.value("elapsedSeconds"))),
            T("%1 次采样").arg(decimal(run.value("sampleCount"), 0))}.join(QStringLiteral(" · "))));
        if (!run.value("headline").toString().isEmpty())
            content += QStringLiteral("<p><strong>%1</strong> %2</p>").arg(esc(run.value("headline").toString()), esc(run.value("summary").toString()));
        QString checks;
        for (const auto &value : run.value("checks").toArray()) {
            const auto check = value.toObject();
            const QString status = check.value("status").toString();
            checks += pill(check.value("title").toString() + ' ' + (status == "pressure" ? T("有压力") : status == "ok" ? T("正常") : T("不可测")),
                           status == "pressure" ? QStringLiteral("warn") : status == "ok" ? QStringLiteral("ok") : QStringLiteral("neutral"));
        }
        if (!checks.isEmpty()) content += QStringLiteral("<div class=\"checks\">%1</div>").arg(checks);
        content += findings(run.value("findings").toArray(), T("观察期间没有发现问题。"));
        QList<QStringList> rows;
        for (const auto &value : run.value("suspects").toArray()) {
            const auto suspect = value.toObject();
            rows << QStringList{suspect.value("name").toString(None), owner(suspect), decimal(suspect.value("processCount"), 0), cpu(suspect.value("averageCpuPercent")),
                                cpu(suspect.value("peakCpuPercent")), size(suspect.value("peakMemoryBytes")), size(suspect.value("peakIOBytesPerSecond"), true)};
        }
        if (!rows.isEmpty())
            content += QStringLiteral("<h3>%1</h3>").arg(esc(T("观察期间的相关进程"))) + table({T("程序"), T("用户"), T("进程数"), T("平均 CPU"),
                T("峰值 CPU"), T("内存峰值"), T("读写峰值")}, {false, false, true, true, true, true, true}, rows);
        body += section(T("一键诊断（约 10 秒观察）"), content);
    }

    QList<QStringList> suspects;
    for (const auto &value : report.value("suspects").toArray()) {
        const auto suspect = value.toObject();
        suspects << QStringList{suspect.value("name").toString(None), owner(suspect), decimal(suspect.value("processCount"), 0), cpu(suspect.value("cpuPercent")),
                                size(suspect.value("memoryBytes")), size(suspect.value("ioBytesPerSecond"), true), suspect.value("reason").toString()};
    }
    body += section(T("相关进程"), suspects.isEmpty() ? T("<p class=\"empty\">没有进程数据。</p>")
        : T("<p class=\"meta\">CPU 100% 等于占满 1 个核心。排行表示当时相关，不等于确认是原因。</p>")
            + table({T("程序"), T("用户"), T("进程数"), QStringLiteral("CPU"), T("内存"), T("读写"), T("说明")},
                    {false, false, true, true, true, true, false}, suspects));

    const auto load = cpuData.value("load").toArray();
    QStringList loads;
    for (int i = 0; i < 3; ++i) loads << decimal(load.at(i), 2);
    const auto psi = [&](const char *resource, const char *type) {
        return decimal(pressure.value(resource).toObject().value(type).toObject().value("avg10"), 1) + (n(pressure.value(resource).toObject().value(type).toObject().value("avg10")) < 0 ? QString() : QStringLiteral("%"));
    };
    const QList<QStringList> details{
        {T("CPU 使用率 · I/O 等待 · 中断"), QStringLiteral("%1 · %2 · %3").arg(fraction(cpuData.value("usage")), fraction(cpuData.value("iowait")), fraction(cpuData.value("irq")))},
        {T("负载（1 / 5 / 15 分钟）"), loads.join(QStringLiteral(" / "))},
        {T("调度排队 · 每秒新建进程"), QStringLiteral("%1 · %2").arg(decimal(cpuData.value("runQueueWait"), 2), decimal(cpuData.value("forksPerSecond"), 0))},
        {T("可用内存 / 总内存"), QStringLiteral("%1 / %2").arg(size(memory.value("availableBytes")), size(memory.value("totalBytes")))},
        {T("换入 · 换出"), QStringLiteral("%1 · %2").arg(size(vm.value("swapInBytesPerSecond"), true), size(vm.value("swapOutBytesPerSecond"), true))},
        {T("直接回收 · 分配停顿"), T("%1 页/秒 · %2 次/秒").arg(decimal(vm.value("directReclaimPagesPerSecond"), 0), decimal(vm.value("allocStallsPerSecond"), 1))},
        {T("最忙的磁盘"), T("%1 · 繁忙 %2 · 平均等待 %3").arg(disk.value("busiestDevice").toString(None), fraction(disk.value("busiestUtilization")), milliseconds(disk.value("busiestAwaitMilliseconds")))},
        {T("磁盘读取 · 写入"), QStringLiteral("%1 · %2").arg(size(disk.value("readBytesPerSecond"), true), size(disk.value("writeBytesPerSecond"), true))},
        {T("调度延迟（最大 · P95）"), QStringLiteral("%1 · %2").arg(milliseconds(probe.value("schedulerMaxMs")), milliseconds(probe.value("schedulerP95Ms")))},
        {T("显示响应"), milliseconds(probe.value("displayMaxMs"))},
        {T("最高温度 · 频率"), T("%1 · 最高频率的 %2").arg(n(thermal.value("hottestCelsius")) < 0 ? None : QString::number(n(thermal.value("hottestCelsius")), 'f', 0) + QStringLiteral(" °C"), fraction(thermal.value("frequencyFraction")))},
        {T("PSI 停顿（CPU · 内存 · I/O，10 秒平均）"), QStringLiteral("%1 · %2 · %3").arg(psi("cpu", "some"), psi("memory", "some"), psi("io", "some"))},
        {T("进程（运行 · 等待磁盘 · 僵尸）"), QStringLiteral("%1 · %2 · %3").arg(decimal(states.value("running"), 0), decimal(states.value("blocked"), 0), decimal(states.value("zombie"), 0))}};
    body += section(T("详细数据"), table({T("项目"), T("数值")}, {false, false}, details));

    const auto history = report.value("history").toObject();
    const double start = n(report.value("historyStart")), end = n(report.value("historyEnd"));
    const QList<std::tuple<QString, QString, Unit>> plots{
        {"cpu", T("CPU 使用率"), Unit::Fraction}, {"memory", T("内存使用率"), Unit::Fraction},
        {"schedulerLatency", T("调度延迟"), Unit::Milliseconds}, {"diskBusy", T("磁盘繁忙"), Unit::Fraction},
        {"cpuIowait", T("CPU I/O 等待"), Unit::Fraction}, {"swapIn", T("换入速率"), Unit::Rate},
        {"swapOut", T("换出速率"), Unit::Rate}, {"memoryPressure", T("内存停顿（PSI）"), Unit::Fraction},
        {"ioPressure", T("I/O 停顿（PSI）"), Unit::Fraction}};
    QString charts = T("<p class=\"meta\">%1 至 %2，按分钟汇总；线是平均值，浅色区域是每分钟的最小到最大值。未运行或休眠的时间段没有数据。</p><div class=\"charts\">")
        .arg(esc(when(start)), esc(when(end)));
    for (const auto &plot : plots) charts += chart(std::get<1>(plot), history.value(std::get<0>(plot)).toArray(), start, end, std::get<2>(plot));
    body += section(T("历史曲线"), charts + QStringLiteral("</div>"));

    QStringList coverage = strings(diagnosis.value("coverage").toArray()) + strings(run.value("coverage").toArray());
    if (n(report.value("processSampleAgeSeconds")) >= 0)
        coverage << T("进程数据采集于报告前 %1 秒。").arg(decimal(report.value("processSampleAgeSeconds"), 0));
    if (!report.value("note").toString().isEmpty()) coverage << report.value("note").toString();
    coverage.removeDuplicates();
    body += section(T("数据来源与限制"), coverage.isEmpty() ? T("<p class=\"empty\">没有特别说明。</p>") : notes(coverage));

    return page(T("DevStats 诊断报告 · %1").arg(when(report.value("capturedAt"), QStringLiteral("yyyy-MM-dd HH:mm"))), body, report);
}

QString DevStatsReport::incidentHtml(const QJsonObject &incident) {
    const QMap<QString, QString> kinds{{"cpu", QStringLiteral("CPU")}, {"memory", T("内存")}, {"io", T("磁盘")},
        {"oom", T("内存耗尽")}, {"responsiveness", T("调度")}, {"display", T("显示")}};
    QStringList kindText;
    for (const auto &kind : incident.value("kinds").toArray()) if (kinds.contains(kind.toString())) kindText << kinds.value(kind.toString());
    const auto status = statusOf(incident.value("status").toString());
    QStringList meta{T("开始 %1").arg(when(incident.value("startTimestamp"))), T("持续 %1").arg(duration(incident.value("durationSeconds")))};
    if (incident.value("episodeCount").toInt() > 1) meta << T("合并 %1 段").arg(incident.value("episodeCount").toInt());
    if (!kindText.isEmpty()) meta << T("类型：%1").arg(kindText.join(T("、")));
    if (!version(incident).isEmpty()) meta << QStringLiteral("DevStats %1").arg(version(incident));
    QString body = header(T("DevStats 卡顿记录"), meta);

    const QString severity = incident.value("severity").toString();
    QString verdict = QStringLiteral("<div class=\"verdict-head\">%1<h2>%2</h2></div>").arg(pill(status.first, status.second),
        esc(incident.value("summary").toString(T("资源压力事件"))));
    if (!incident.value("headlineEvidence").toString().isEmpty()) verdict += QStringLiteral("<p>%1</p>").arg(esc(incident.value("headlineEvidence").toString()));
    if (!incident.value("interruptionReason").toString().isEmpty()) verdict += QStringLiteral("<p class=\"muted\">%1</p>").arg(esc(incident.value("interruptionReason").toString()));
    const auto strongest = incident.value("strongestApplication").toObject();
    if (!strongest.isEmpty()) {
        const QString basis = incident.value("strongestApplicationBasis").toString();
        const QString amount = basis == "cpu" ? cpu(strongest.value("cpuPercent")) : basis == "memory" ? size(strongest.value("memoryBytes"))
            : basis == "io" ? size(strongest.value("ioBytesPerSecond"), true) : None;
        verdict += T("<p>最忙的程序：<strong>%1</strong>%2</p>").arg(esc(strongest.value("name").toString(None)),
            amount == None ? QString() : esc(T("（%1）").arg(amount)));
    }
    body += QStringLiteral("<section class=\"verdict %1\">%2</section>\n").arg(severity == "critical" ? QStringLiteral("critical") : QStringLiteral("warn"), verdict);

    const auto witnesses = incident.value("witnesses").toArray();
    QList<QStringList> rows;
    for (const auto &value : witnesses) rows << sampleCells(value.toObject());
    body += section(T("关键时刻"), rows.isEmpty() ? T("<p class=\"empty\">这条记录没有保存关键时刻。</p>")
        : T("<p class=\"meta\">开始、各项峰值和最后一次采样时的系统状态。</p>") + table(sampleHeaders(), SampleNumeric, rows));

    QJsonArray unique;
    QSet<QString> seen;
    for (const auto *part : {"witnesses", "prelude"}) for (const auto &value : incident.value(part).toArray())
        for (const auto &entry : value.toObject().value("findings").toArray()) {
            const QString title = entry.toObject().value("title").toString();
            if (title.isEmpty() || seen.contains(title) || unique.size() >= 8) continue;
            seen.insert(title); unique.append(entry);
        }
    body += section(T("发现"), findings(unique, T("这条记录没有保存具体发现。")));

    // Who was busy: the last witness with process data, or a peak witness when there is one.
    QJsonObject peak;
    for (const auto &value : witnesses) if (!value.toObject().value("topProcesses").toArray().isEmpty()) peak = value.toObject();
    for (const auto &value : witnesses) {
        const auto sample = value.toObject();
        for (const auto &mark : sample.value("labels").toArray())
            if (mark.toString().startsWith("peak") && !sample.value("topProcesses").toArray().isEmpty()) peak = sample;
    }
    QList<QStringList> processes;
    for (const auto &value : peak.value("topProcesses").toArray()) {
        const auto process = value.toObject();
        const double read = n(process.value("ioReadBytesPerSecond")), write = n(process.value("ioWriteBytesPerSecond"));
        processes << QStringList{process.value("name").toString(None), owner(process), decimal(process.value("pid"), 0), cpu(process.value("cpuPercent")),
                                 size(process.value("memoryBytes")), read < 0 && write < 0 ? None : formatBytes(std::max(0.0, read) + std::max(0.0, write), true)};
    }
    body += section(T("当时最忙的进程"), processes.isEmpty() ? T("<p class=\"empty\">没有进程数据。</p>")
        : T("<p class=\"meta\">%1时刻。PID 是当时的记录，现在可能已被复用，不能据此结束进程。</p>").arg(esc(sampleTitle(peak)))
            + table({T("进程"), T("用户"), QStringLiteral("PID"), QStringLiteral("CPU"), T("内存"), T("读写")},
                    {false, false, true, true, true, true}, processes));

    QList<QStringList> before;
    for (const auto &value : incident.value("prelude").toArray()) before << sampleCells(value.toObject());
    if (!before.isEmpty()) body += section(T("发生前的状态"), table(sampleHeaders(), SampleNumeric, before));

    QStringList coverage;
    for (const auto &value : witnesses) coverage += strings(value.toObject().value("processCoverage").toObject().value("notes").toArray());
    coverage += strings(incident.value("coverage").toArray());
    coverage.removeDuplicates();
    body += section(T("数据来源与限制"), coverage.isEmpty() ? T("<p class=\"empty\">没有特别说明。</p>") : notes(coverage));

    return page(T("DevStats 卡顿记录 · %1").arg(when(incident.value("startTimestamp"), QStringLiteral("yyyy-MM-dd HH:mm"))), body, incident);
}

QString DevStatsReport::htmlPath(const QString &chosen) {
    return QFileInfo(chosen).suffix().isEmpty() ? chosen + QStringLiteral(".html") : chosen;
}

QString DevStatsReport::writePrivate(const QString &path, const QByteArray &data) {
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return file.errorString();
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        file.cancelWriting();
        return T("无法把文件设为仅自己可读写。");
    }
    if (file.write(data) != data.size()) {
        const QString error = file.errorString();
        file.cancelWriting();
        return error;
    }
    if (!file.commit()) return file.errorString();
    return {};
}
