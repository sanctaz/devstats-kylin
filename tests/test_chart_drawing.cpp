// A chart draws each unbroken stretch in vertical strips, because one long jagged
// path costs Qt's rasterizer several times more than its pixels need. The strips
// must leave every pixel exactly as a single pass would.
#include "../src/charts.h"
#include "../src/history_feed.h"

#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
int checks = 0;
void check(bool ok, const char *what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL chart drawing: %s\n", what); std::exit(1); }
}
struct Shape {
    int width, height, points;
    double noise, scale;
    bool spread;
};
ChartRun runFor(const Shape &shape, const QRectF &plot) {
    ChartRun run;
    unsigned seed = 20261111u + unsigned(shape.points) * 7919u + unsigned(shape.width);
    const auto random = [&] { seed = seed * 1103515245u + 12345u; return ((seed >> 16) & 0x7fff) / 32767.0; };
    for (int i = 0; i < shape.points; ++i) {
        const double value = std::clamp(.45 + .25 * std::sin(i * 15.0 / shape.points) + shape.noise * .3 * (random() - .5), .02, .98);
        const double band = shape.spread ? .08 * random() : 0;
        // Past both edges of the plot, and sometimes two points in the same place, as real data has.
        const double x = plot.left() - 9 + (plot.width() + 18) * (i - (i % 11 == 5 ? 1 : 0)) / std::max(1, shape.points - 1);
        run.mean.append(QPointF(x, plot.bottom() - value * plot.height()));
        run.low.append(QPointF(x, plot.bottom() - std::max(0., value - band) * plot.height()));
        run.high.append(QPointF(x, plot.bottom() - std::min(1., value + band) * plot.height()));
    }
    // A chart repeats the first and last point of a stretch.
    run.mean.prepend(run.mean.first()); run.low.prepend(run.low.first()); run.high.prepend(run.high.first());
    run.mean.append(run.mean.last()); run.low.append(run.low.last()); run.high.append(run.high.last());
    return run;
}
QImage canvas(const Shape &shape) {
    QImage image(int(std::ceil(shape.width * shape.scale)), int(std::ceil(shape.height * shape.scale)), QImage::Format_ARGB32_Premultiplied);
    image.fill(QColor(250, 250, 252));
    return image;
}

void stripsLeaveEveryPixel() {
    const Shape shapes[] = {{371, 150, 300, 1, 1, false}, {371, 150, 300, .15, 1, false}, {371, 150, 600, 1, 1, true}, {371, 150, 40, 1, 1, true},
        {170, 44, 300, 1, 1, false}, {760, 170, 600, 1, 1, true}, {371, 150, 300, 1, 1.5, true}, {371, 150, 300, 1, 1.25, false},
        {760, 170, 300, 1, 2, false}, {371, 150, 2, 1, 1, true}, {371, 150, 1, 1, 1.25, true}, {90, 60, 600, 1, 1.75, true}};
    const QColor accent(40, 120, 255), bandColor(40, 120, 255, 33);
    const QPen pen(accent, 1.7, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
    int painted = 0, mismatches = 0;
    for (const auto &shape : shapes) {
        const QRectF plot(4, shape.height < 60 ? 3 : 22, shape.width - 8, shape.height - (shape.height < 60 ? 5 : 44));
        const QRectF clip = plot.adjusted(-3, -3, 3, 3);
        const ChartRun run = runFor(shape, plot);
        const int count = run.mean.size();
        QLinearGradient gradient(plot.topLeft(), plot.bottomLeft());
        gradient.setColorAt(0, QColor(40, 120, 255, 56)); gradient.setColorAt(1, QColor(40, 120, 255, 6));
        // The plain way: one path each for the area, the band and the line.
        QImage whole = canvas(shape);
        {
            QPainter painter(&whole);
            painter.scale(shape.scale, shape.scale);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(clip);
            QPainterPath line;
            line.moveTo(run.mean[0]);
            for (int i = 1; i < count; ++i) line.lineTo(run.mean[i]);
            QPainterPath area = line;
            area.lineTo(QPointF(run.mean[count - 1].x(), plot.bottom()));
            area.lineTo(QPointF(run.mean[0].x(), plot.bottom()));
            area.closeSubpath();
            painter.fillPath(area, gradient);
            QPainterPath band;
            band.moveTo(run.low[0]);
            for (int i = 1; i < count; ++i) band.lineTo(run.low[i]);
            for (int i = count - 1; i >= 0; --i) band.lineTo(run.high[i]);
            band.closeSubpath();
            painter.fillPath(band, bandColor);
            painter.setPen(pen);
            painter.drawPath(line);
            painter.fillRect(QRectF(clip.left(), clip.top(), 2, 2), accent);
        }
        if (whole != canvas(shape)) ++painted;
        for (const int stripWidth : {1, 7, 24, 64, 100000}) {
            QImage strips = canvas(shape);
            QPainter painter(&strips);
            painter.scale(shape.scale, shape.scale);
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setClipRect(clip);
            drawChartRun(painter, run, clip, plot.bottom(), gradient, bandColor, pen, stripWidth);
            // Afterwards the clip is the whole plot again: this mark lies outside the last strip.
            painter.fillRect(QRectF(clip.left(), clip.top(), 2, 2), accent);
            painter.end();
            if (strips != whole) {
                int pixels = 0, worst = 0, firstX = -1, lastX = -1;
                for (int y = 0; y < whole.height(); ++y) for (int x = 0; x < whole.width(); ++x) {
                    const QRgb a = whole.pixel(x, y), b = strips.pixel(x, y);
                    const int delta = std::max({std::abs(qRed(a) - qRed(b)), std::abs(qGreen(a) - qGreen(b)), std::abs(qBlue(a) - qBlue(b))});
                    if (delta) { ++pixels; worst = std::max(worst, delta); if (firstX < 0 || x < firstX) firstX = x; lastX = std::max(lastX, x); }
                }
                std::fprintf(stderr, "differs: %dx%d scale %.2f, %d points, noise %.2f, spread %d, strips of %d: %d pixels, worst %d/255, device x %d..%d of %d\n",
                             shape.width, shape.height, shape.scale, shape.points, shape.noise, int(shape.spread), stripWidth, pixels, worst, firstX, lastX, whole.width());
                ++mismatches;
            }
        }
    }
    check(mismatches == 0, "strips draw exactly the pixels of a single pass");
    check(painted == int(sizeof shapes / sizeof shapes[0]), "every shape drew something to compare");
}

// A lone sample ends with a filled dot. The stretch after it must still be a line
// with its shaded area below, not a solid shape between the line and its ends.
void loneSampleDoesNotFillTheNextStretch() {
    const qint64 t0 = 1770000000;
    QJsonArray points{QJsonObject{{"timestamp", double(t0 - 100)}, {"value", .9}, {"count", 1}, {"resolutionSeconds", 2}}};
    for (qint64 at = t0 - 60; at <= t0; at += 2)
        points.append(QJsonObject{{"timestamp", double(at)}, {"value", .2 + .6 * std::abs(double(at - (t0 - 30)) / 30)}, {"count", 1}, {"resolutionSeconds", 2}});
    HistoryChart chart(true);
    chart.resize(400, 200);
    chart.setHistory(HistoryFeed(QJsonObject{{"cpu", points}}, t0 - 120, t0), "cpu");
    const QImage image = chart.grab().toImage();
    // The plot is 392 x 156 at (4, 22). Both pixels are at 60%: one above the dip of
    // the second stretch, the other in the empty time between the two stretches.
    const QColor aboveDip = image.pixelColor(298, 84), empty = image.pixelColor(135, 84);
    check(aboveDip == empty, "nothing is painted above the line of a stretch that follows a lone sample");
    const QColor underLine = image.pixelColor(298, 170);
    check(underLine != empty, "the area under that line is still shaded");
}
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    stripsLeaveEveryPixel();
    loneSampleDoesNotFillTheNextStretch();
    std::printf("PASS chart drawing: %d checks; strips equal a single pass at every scale, lone samples\n", checks);
    return 0;
}
