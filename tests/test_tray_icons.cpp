#include "../src/tray_icon.h"
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QFontInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void check(bool value, const QString &message) {
    if (!value) { std::fprintf(stderr, "FAIL tray icons: %s\n", qPrintable(message)); std::exit(1); }
}
QJsonObject sample() {
    QJsonArray history;
    for (int i = 0; i < 24; ++i) history.append(QJsonObject{{"timestamp", 1792632000. + i * 2},
        {"value", .12 + .70 * (.5 + .5 * std::sin(i * .55))}});
    return {{"cpu", QJsonObject{{"usage", .427}}}, {"menuCPUHistory", history},
        {"memory", QJsonObject{{"fraction", .596}}},
        {"network", QJsonObject{{"uploadBytesPerSecond", 1.24 * 1024 * 1024}, {"downloadBytesPerSecond", 8.62 * 1024 * 1024}}},
        {"diskIO", QJsonObject{{"readBytesPerSecond", 32.5 * 1024 * 1024}, {"writeBytesPerSecond", 4.7 * 1024 * 1024}}},
        {"battery", QJsonObject{{"state", "charging"}, {"level", .65}}}};
}
QJsonObject changed(QJsonObject object, const QString &section, const QString &key, const QJsonValue &value) {
    QJsonObject part = object.value(section).toObject(); part[key] = value; object[section] = part; return object;
}
QByteArray digest(const QImage &image) {
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    return QCryptographicHash::hash(QByteArray(reinterpret_cast<const char *>(rgba.constBits()), int(rgba.sizeInBytes())), QCryptographicHash::Sha256).toHex();
}
QRect inkBounds(const QImage &image, const QRect &region) {
    QRect bounds;
    for (int y = region.top(); y <= region.bottom(); ++y) for (int x = region.left(); x <= region.right(); ++x)
        if (image.pixelColor(x,y).alpha() >= 128) bounds = bounds.united(QRect(x,y,1,1));
    return bounds;
}
QRect differences(const QImage &first, const QImage &second) {
    check(first.size() == second.size(), "difference inputs have equal dimensions");
    QRect bounds;
    for (int y = 0; y < first.height(); ++y) for (int x = 0; x < first.width(); ++x)
        if (first.pixelColor(x, y) != second.pixelColor(x, y)) bounds = bounds.united(QRect(x, y, 1, 1));
    return bounds;
}
void validate(const QImage &image, int scale, const QColor &ink) {
    check(!image.isNull() && image.size() == QSize(22 * scale, 22 * scale), "22x22 logical icon with exact integer-scale pixels");
    check(qFuzzyCompare(image.devicePixelRatio(), double(scale)), "device-pixel ratio matches representation");
    int colored = 0, transparent = 0;
    for (int y = 0; y < image.height(); ++y) for (int x = 0; x < image.width(); ++x) {
        const QColor color = image.pixelColor(x, y);
        if (color.alpha() == 0) ++transparent;
        else {
            ++colored;
            check(color.red() == ink.red() && color.green() == ink.green() && color.blue() == ink.blue(), "all nontransparent pixels use one panel ink");
        }
    }
    check(colored >= 4 * scale * scale, "known and unknown states draw visible content");
    check(transparent >= image.width() * image.height() / 4, "icon background remains transparent");
}
struct Fixture { QString name, metric, caption; QJsonObject data; };
}
int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
    QApplication application(argc, argv);
    const QString output = application.arguments().value(1);
    const QString before = application.arguments().value(2);
    check(!output.isEmpty() && QDir().mkpath(output), "evidence output directory available");
    check(DevStatsTray::IconSize == 22, "public logical-size contract");
    const QColor white(Qt::white), black(Qt::black);
    const QJsonObject base = sample();
    for (const QString &metric : {"cpu", "memory", "network", "battery", "disk"}) {
        const QString key = DevStatsTray::cacheKey(metric, base, white);
        check(!key.isEmpty(), metric + " cache key is not empty");
        QJsonObject irrelevant = base; irrelevant["timestamp"] = 123.; irrelevant["processes"] = QJsonArray{42};
        check(key == DevStatsTray::cacheKey(metric, irrelevant, white), metric + " cache ignores unrelated snapshot changes");
        check(key != DevStatsTray::cacheKey(metric, base, black), metric + " cache follows panel ink");
        check(key != DevStatsTray::cacheKey(metric, base, white, true), metric + " cache follows numeral mode");
        check(DevStatsTray::render(metric, base, white, 1) != DevStatsTray::render(metric, base, white, 1, true), metric + " large mode changes visible numerals");
        const QString tip = DevStatsTray::toolTip(metric, base);
        check(!tip.isEmpty() && !tip.contains("nan", Qt::CaseInsensitive) && !tip.contains("inf", Qt::CaseInsensitive), metric + " tooltip readable");
    }
    for (const QString &metric : {"cpu", "memory", "battery"})
        check(DevStatsTray::toolTip(metric, base).contains('%'), metric + " tooltip retains explicit percent unit");
    const auto nearRateA = changed(base,"network","uploadBytesPerSecond",2.1*1024*1024);
    const auto nearRateB = changed(base,"network","uploadBytesPerSecond",2.2*1024*1024);
    check(DevStatsTray::cacheKey("network",nearRateA,white,true)==DevStatsTray::cacheKey("network",nearRateB,white,true), "large rate cache follows rounded whole value");
    check(DevStatsTray::cacheKey("network",nearRateA,white)!=DevStatsTray::cacheKey("network",nearRateB,white), "standard rate cache retains visible decimal");
    check(DevStatsTray::cacheKey("network",changed(base,"network","uploadBytesPerSecond",100.*1024*1024),white,true).contains(":.1G/"), "large hundred-megabyte rate uses nonzero next binary unit");
    const QString networkTip = DevStatsTray::toolTip("network", base);
    check(networkTip.indexOf(QString::fromUtf8("上传")) >= 0 && networkTip.indexOf(QString::fromUtf8("上传")) < networkTip.indexOf(QString::fromUtf8("下载")), "network tooltip upload precedes download");
    QJsonObject shifted = base, interrupted = base;
    QJsonArray shiftedPoints, interruptedPoints = base.value("menuCPUHistory").toArray();
    for (const QJsonValue &entry : base.value("menuCPUHistory").toArray()) {
        QJsonObject point = entry.toObject(); point["timestamp"] = point.value("timestamp").toDouble() + 1000; shiftedPoints.append(point);
    }
    shifted["menuCPUHistory"] = shiftedPoints;
    check(DevStatsTray::cacheKey("cpu", base, white) == DevStatsTray::cacheKey("cpu", shifted, white), "CPU cache ignores absolute clock when visible cadence is unchanged");
    QJsonObject gap = interruptedPoints[12].toObject(); gap["timestamp"] = gap.value("timestamp").toDouble() + 10; interruptedPoints[12] = gap;
    interrupted["menuCPUHistory"] = interruptedPoints;
    check(DevStatsTray::cacheKey("cpu", base, white) != DevStatsTray::cacheKey("cpu", interrupted, white)
        && DevStatsTray::render("cpu", base, white) != DevStatsTray::render("cpu", interrupted, white), "real CPU gaps invalidate cache and remain visible");
    const QJsonObject splitCpu{{"cpu",QJsonObject{{"usage",.9}}},{"menuCPUHistory",QJsonArray{
        QJsonObject{{"timestamp",0.},{"value",.2}},QJsonObject{{"timestamp",8.},{"value",.9}},
        QJsonObject{{"timestamp",10.},{"value",.2}},QJsonObject{{"timestamp",12.},{"value",.9}}}}};
    for (bool largeText : {false,true}) for (int scale : {1,2,3}) {
        const QImage split = DevStatsTray::render("cpu",splitCpu,white,scale,largeText);
        check(split.pixelColor(14*scale,3*scale).alpha()==0, "isolated CPU sample does not fill the later open curve above its real samples");
    }
    for (const QString &metric : {"cpu", "memory"}) {
        const QString section = metric == "cpu" ? "cpu" : "memory", field = metric == "cpu" ? "usage" : "fraction";
        const QJsonObject low = changed(base, section, field, .1), high = changed(base, section, field, .9);
        check(DevStatsTray::cacheKey(metric, low, white) != DevStatsTray::cacheKey(metric, high, white), metric + " visible percentage invalidates cache");
        check(DevStatsTray::cacheKey(metric, changed(base, section, field, .4271), white)
            == DevStatsTray::cacheKey(metric, changed(base, section, field, .4272), white), metric + " same displayed percentage avoids redraw");
        const QJsonObject missing{{section, QJsonObject{}}};
        for (double invalid : {-1., 1.5, std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()}) {
            const QJsonObject bad{{section, QJsonObject{{field, invalid}}}};
            check(DevStatsTray::render(metric, bad, white) == DevStatsTray::render(metric, missing, white), metric + " invalid fraction displays unknown");
        }
        check(DevStatsTray::render(metric, changed({}, section, field, 0.), white) != DevStatsTray::render(metric, missing, white), metric + " measured zero differs from unknown");
    }
    for (bool largeText : {false,true}) for (int scale : {1,2,3}) {
        const QImage ram = DevStatsTray::render("memory", base, white, scale, largeText);
        const QRect header(0,0,22*scale,8*scale);
        for (double fraction : {-1.,0.,.6,1.}) {
            const QImage state = DevStatsTray::render("memory",changed(base,"memory","fraction",fraction),white,scale,largeText);
            check(ram.copy(header) == state.copy(header), "RAM identity stays recognizable at unknown, zero and full usage");
        }
        for (int x : {5,10,15})
            check(ram.pixelColor(x*scale,2*scale).alpha() >= 128, "DIMM shows three discrete memory chips");
        for (int x : {8,13})
            check(ram.pixelColor(x*scale,2*scale).alpha() == 0, "transparent gaps separate memory chips instead of a battery fill bar");
        for (int x : {4,6,8,13,15,17})
            check(ram.pixelColor(x*scale,6*scale).alpha() >= 128, "DIMM contact fingers stay visible at every raster scale");
        for (int x : {5,7,11,14,16})
            check(ram.pixelColor(x*scale,6*scale).alpha() == 0, "DIMM contact fingers remain separated");
        check(ram.pixelColor(11*scale,5*scale).alpha() == 0, "DIMM key notch breaks the lower board edge");
        check(ram.pixelColor(20*scale,3*scale).alpha() == 0, "RAM silhouette has no battery terminal");
    }
    for (double rate : {0., 1023., 1024., 9.99 * 1024, 999.5 * 1024, 1.24 * 1024 * 1024,
            98.7 * 1024 * 1024, 72.4 * 1024 * 1024 * 1024, 8.8 * std::pow(1024., 4), std::pow(1024., 5)}) {
        const QString compact = DevStatsTray::compactRate(rate);
        check(!compact.isEmpty() && compact.size() <= 5 && !compact.contains('e', Qt::CaseInsensitive), "high-rate token stays compact without exponent notation: " + compact);
    }
    check(DevStatsTray::compactRate(-1) == DevStatsTray::compactRate(std::numeric_limits<double>::quiet_NaN())
        && DevStatsTray::compactRate(-1) != DevStatsTray::compactRate(0), "unknown rates remain distinct from zero");
    for (bool largeText : {false,true}) for (int scale : {1, 2, 3}) {
        const QImage network = DevStatsTray::render("network", base, white, scale, largeText);
        const QRect upload = differences(network, DevStatsTray::render("network", changed(base, "network", "uploadBytesPerSecond", 250. * 1024), white, scale, largeText));
        const QRect download = differences(network, DevStatsTray::render("network", changed(base, "network", "downloadBytesPerSecond", 250. * 1024), white, scale, largeText));
        check(!upload.isEmpty() && upload.bottom() < 11 * scale, "only upper row changes for upload");
        check(!download.isEmpty() && download.top() >= 11 * scale, "only lower row changes for download");
        const QImage charging = DevStatsTray::render("battery", base, white, scale, largeText);
        const QImage discharging = DevStatsTray::render("battery", changed(base, "battery", "state", "discharging"), white, scale, largeText);
        const QRect stateChange = differences(charging, discharging);
        check(!stateChange.isEmpty() && stateChange.bottom() < 11 * scale, "charging indicator changes independently of lower percentage");
    }
    for (double rate : {0., .4, 99.49*1024, 99.5*1024, 999.49*1024, 999.5*1024,
            1024., 9.95*1024, 100.*1024*1024, std::numeric_limits<double>::max()}) {
        const auto data = changed(changed(base,"network","uploadBytesPerSecond",rate),"network","downloadBytesPerSecond",rate);
        const QImage icon = DevStatsTray::render("network",data,white,1,true);
        const auto key = DevStatsTray::cacheKey("network",data,white,true);
        check(key.section(':',-1).section('/',0,0).size() <= 3, "large rate token fits three glyphs including unit");
        check(icon.copy(0,0,4,22) == DevStatsTray::render("network",base,white,1,true).copy(0,0,4,22), "manual arrow gutter is independent of rate width");
    }
    const QJsonObject highRates = changed(changed(base, "network", "uploadBytesPerSecond", 999.5 * 1024 * 1024), "network", "downloadBytesPerSecond", 8.8 * std::pow(1024., 4));
    QJsonArray hundredModels; for (int i = 0; i < 100; ++i) hundredModels.append(QJsonObject{{"name",QString::number(i)}});
    const QList<Fixture> fixtures = {
        {"cpu-live", "cpu", "CPU 43% + history", base}, {"cpu-zero", "cpu", "CPU measured 0%", changed(base,"cpu","usage",0.)},
        {"cpu-full", "cpu", "CPU measured 100%", changed(base,"cpu","usage",1.)}, {"cpu-unknown", "cpu", "CPU unknown", {}},
        {"cpu-split", "cpu", "Real gap + isolated point", splitCpu},
        {"memory-live", "memory", "Memory 60%", base}, {"memory-zero", "memory", "Memory measured 0%", changed(base,"memory","fraction",0.)},
        {"memory-full", "memory", "Memory measured 100%", changed(base,"memory","fraction",1.)}, {"memory-unknown", "memory", "Memory unknown", {}},
        {"network-live", "network", "Upload / download", base}, {"network-high", "network", "High rates + rollover", highRates},
        {"network-zero", "network", "Network measured zero", QJsonObject{{"network",QJsonObject{{"uploadBytesPerSecond",0.},{"downloadBytesPerSecond",0.}}}}},
        {"network-unknown", "network", "Network unknown", {}},
        {"network-max", "network", "Maximum finite rate", changed(changed(base,"network","uploadBytesPerSecond",std::numeric_limits<double>::max()),"network","downloadBytesPerSecond",std::numeric_limits<double>::max())},
        {"battery-charging", "battery", "Battery 65%, charging", base},
        {"battery-discharging", "battery", "Battery 65%, discharge", changed(base,"battery","state","discharging")},
        {"battery-plugged", "battery", "Battery 65%, plugged", changed(base,"battery","state","pluggedIn")},
        {"battery-full", "battery", "Battery 100%, full", changed(changed(base,"battery","state","full"),"battery","level",1.)},
        {"battery-zero", "battery", "Battery measured 0%", changed(changed(base,"battery","state","discharging"),"battery","level",0.)},
        {"battery-unreadable", "battery", "Battery unreadable", QJsonObject{{"battery",QJsonObject{{"state","unknown"}}}}},
        {"battery-none", "battery", "No internal battery", QJsonObject{{"battery",QJsonObject{{"state","noBattery"}}}}},
        {"battery-unknown", "battery", "Battery unknown", {}},
        {"disk-live", "disk", "Disk read / write", base}, {"disk-unknown", "disk", "Disk unknown", {}}};
    QJsonArray manifest;
    for (const Fixture &fixture : fixtures) for (int theme = 0; theme < 2; ++theme) for (bool largeText : {false,true}) for (int scale : {1,2,3}) {
        const QColor ink = theme ? black : white;
        const QImage image = DevStatsTray::render(fixture.metric, fixture.data, ink, scale, largeText);
        validate(image, scale, ink);
        check(image == DevStatsTray::render(fixture.metric, fixture.data, ink, scale, largeText), "renderer is deterministic");
        const QRect glyphs = inkBounds(image,QRect(0,8*scale,22*scale,14*scale));
        if (scale == 1 && (fixture.name == "cpu-full" || fixture.name == "memory-full" || fixture.name == "battery-full")) {
            check(glyphs.height() >= (largeText ? 10 : 8), "100 keeps large actual numeral ink at 1x without 8px fallback");
            check(glyphs.top() > 8 && glyphs.bottom() < 21, "full percentage glyphs fit vertically without clipping");
        }
        const QRect topRate = inkBounds(image,QRect(5*scale,0,17*scale,11*scale));
        const QRect bottomRate = inkBounds(image,QRect(5*scale,11*scale,17*scale,11*scale));
        if (scale == 1 && fixture.name == "network-live") {
            check(topRate.height() >= (largeText ? 9 : 7) && bottomRate.height() >= (largeText ? 9 : 7), "both network rows keep readable numeral ink at 1x");
        }
        const QString name = fixture.name + (theme ? "-light-" : "-dark-") + (largeText ? "large-" : "standard-") + QString::number(scale) + "x.png";
        check(image.save(output + "/" + name), "PNG evidence saved");
        manifest.append(QJsonObject{{"file",name},{"width",image.width()},{"height",image.height()},{"dpr",image.devicePixelRatio()},{"mode",largeText ? "large" : "standard"},{"lowerInkHeight",glyphs.height()},{"lowerInkWidth",glyphs.width()},{"upperRateInkHeight",topRate.height()},{"lowerRateInkHeight",bottomRate.height()},{"rgbaSha256",QString::fromLatin1(digest(image))}});
    }
    const QStringList shown = {"cpu-full","memory-full","network-live","network-high","battery-charging","battery-none","cpu-unknown","network-unknown"};
    QImage contact(1040, 550, QImage::Format_ARGB32_Premultiplied); contact.fill(QColor("#e8ebef"));
    QPainter painter(&contact); painter.setRenderHint(QPainter::SmoothPixmapTransform,false);
    QFont font = application.font(); font.setPixelSize(13); painter.setFont(font);
    for (int theme = 0; theme < 2; ++theme) {
        const int left = theme * 520; const QColor ink = theme ? black : white;
        painter.fillRect(left,0,520,550,theme ? QColor("#f3f4f5") : QColor("#282c30")); painter.setPen(ink);
        painter.drawText(left+14,24,theme ? "Light panel · exact raster pixels" : "Dark panel · exact raster pixels");
        for (int column = 0; column < 3; ++column)
            painter.drawText(left+156+column*120,48,(column==0 ? "Before" : column==1 ? "Standard" : "Large")+QString(" 1x / 2x"));
        for (int row = 0; row < shown.size(); ++row) {
            const auto it = std::find_if(fixtures.begin(),fixtures.end(),[&](const Fixture &f){return f.name==shown[row];});
            const int top = 62 + row*60;
            painter.drawText(QRect(left+14,top,134,48),Qt::AlignVCenter|Qt::AlignLeft|Qt::TextWordWrap,it->caption);
            for (int column = 0; column < 3; ++column) for (int scale : {1,2}) {
                QImage image;
                if (column==0 && !before.isEmpty()) image.load(before+"/"+it->name+(theme ? "-light-" : "-dark-")+QString::number(scale)+"x.png");
                else if (column>0) image = DevStatsTray::render(it->metric,it->data,ink,scale,column==2);
                if (image.isNull()) continue;
                image.setDevicePixelRatio(1);
                painter.drawImage(QPoint(left+160+column*120+(scale==1?0:32),top+(48-image.height())/2),image);
            }
        }
    }
    painter.end();check(contact.save(output+"/tray-contact.png"),"bounded before/after 1x+2x contact saved");
    QJsonObject beforeMetrics;
    if (!before.isEmpty()) for (const QString &name : {"cpu-full","memory-full","battery-full","network-live"}) {
        const QImage image(before+"/"+name+"-dark-1x.png");
        check(!image.isNull() && image.size()==QSize(22,22), "before evidence has exact 1x icon");
        const QRect lower = inkBounds(image,QRect(0,10,22,12));
        beforeMetrics[name] = QJsonObject{{"lowerInkHeight",lower.height()},{"lowerInkWidth",lower.width()},
            {"upperRateInkHeight",inkBounds(image,QRect(5,0,17,11)).height()},
            {"lowerRateInkHeight",inkBounds(image,QRect(5,11,17,11)).height()}};
    }
    QFile file(output+"/render-matrix.json");check(file.open(QIODevice::WriteOnly),"matrix manifest opens");
    file.write(QJsonDocument(QJsonObject{{"result","PASS"},{"logicalSize",22},{"labelPixelSizes",QJsonArray{12,14}},{"ratePixelSizes",QJsonArray{11,13}},{"font",application.font().toString()},{"resolvedFont",QFontInfo(application.font()).family()},{"before1xInk",beforeMetrics},{"fixtureCount",fixtures.size()},{"images",manifest}}).toJson());
    std::printf("PASS tray icons:22x22 logical,1x/2x/3x,transparent monochrome,DIMM chips/contacts/key notch independent of usage,standard12px/large14px labels,11px/13px rate rows,unknown/zero/full/state/rate boundaries,upload-above-download,mode-aware cache/per-metric tooltips;%d fixtures/%d PNGs;bounded contact %s/tray-contact.png\n",fixtures.size(),manifest.size(),qPrintable(output));
}
