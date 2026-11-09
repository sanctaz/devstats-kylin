#include "../src/responsiveness_probe.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <cstdio>
#include <cstdlib>

namespace {
void check(bool ok, const char *what) { if (!ok) { std::fprintf(stderr, "FAIL responsiveness probe: %s\n", what); std::exit(1); } }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    {
        ResponsivenessProbe probe;
        auto empty = probe.takeWindow();
        check(empty.value("schedulerMaxMs").isNull() && empty.value("schedulerSamples").toInt() == 0, "no samples before start is unknown, not zero");
        probe.start();
        probe.start(); // Starting twice is harmless.
        QThread::msleep(1200);
        auto slow = probe.takeWindow();
        const int background = slow.value("schedulerSamples").toInt();
        check(background >= 3 && background <= 7, "background rate is about 5 Hz");
        check(slow.value("schedulerMaxMs").isDouble() && slow.value("schedulerMaxMs").toDouble() >= 0, "lateness is measured");
        check(slow.value("schedulerP95Ms").toDouble() <= slow.value("schedulerMaxMs").toDouble() + .001, "P95 never exceeds the maximum");
        check(slow.value("windowSeconds").toDouble() >= 1, "window length is reported");

        probe.setFast(true);
        QThread::msleep(250); // The current slow sleep must finish first.
        probe.takeWindow();
        QThread::msleep(1000);
        const int fast = probe.takeWindow().value("schedulerSamples").toInt();
        check(fast >= 25 && fast <= 55, "diagnosis raises the rate to about 50 Hz");
        probe.setFast(false);

        // A window resets the statistics.
        QThread::msleep(250);
        probe.takeWindow();
        auto next = probe.takeWindow();
        check(next.value("schedulerSamples").toInt() <= 1, "taking a window clears it");

        // A busy thread on every core still leaves the probe a number to report.
        const bool hasDisplay = !qEnvironmentVariableIsEmpty("DISPLAY");
        QThread::msleep(hasDisplay ? 2500 : 300);
        auto display = probe.takeWindow();
        if (hasDisplay) {
            check(display.value("displayAvailable").toBool() && display.value("displaySamples").toInt() >= 1, "round trips reach the X server");
            check(display.value("displayMaxMs").isDouble() && display.value("displayMaxMs").toDouble() < 2000, "a healthy X server answers quickly");
        } else {
            check(!display.value("displayAvailable").toBool() && display.value("displayMaxMs").isNull(), "without a display the reading is unknown, not zero");
        }
        QElapsedTimer stopping; stopping.start();
        probe.stop();
        check(stopping.elapsed() < 1500, "stopping is prompt");
        probe.stop(); // Stopping twice is harmless.
        probe.start();
        QThread::msleep(450);
        check(probe.takeWindow().value("schedulerSamples").toInt() >= 1, "restart after stop, as when resuming from sleep");
    }
    std::puts("PASS responsiveness probe: rates, reset, unknown before start, display round trips, prompt stop and restart");
    return 0;
}
