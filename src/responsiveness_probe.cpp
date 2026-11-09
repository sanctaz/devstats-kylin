#include "responsiveness_probe.h"

#include <QLibrary>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <poll.h>

namespace {
qint64 monotonicMicroseconds() {
    timespec now {};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return qint64(now.tv_sec) * 1000000 + now.tv_nsec / 1000;
}

class Worker final : public QThread {
public:
    explicit Worker(std::function<void()> body) : body_(std::move(body)) { setStackSize(256 * 1024); }
protected:
    void run() override { body_(); }
private:
    std::function<void()> body_;
};

// libxcb is already loaded by the xcb platform plugin, and unlike Xlib its error
// paths never call exit(). Everything here is resolved at run time so that a
// Wayland-only session simply reports the display as unavailable.
struct Xcb {
    struct Connection;
    struct Cookie { unsigned sequence; };
    QLibrary library{QStringLiteral("libxcb.so.1")};
    Connection *(*connect)(const char *, int *) = nullptr;
    int (*hasError)(Connection *) = nullptr;
    void (*disconnect)(Connection *) = nullptr;
    int (*flush)(Connection *) = nullptr;
    int (*fileDescriptor)(Connection *) = nullptr;
    Cookie (*inputFocus)(Connection *) = nullptr;
    int (*pollForReply)(Connection *, unsigned, void **, void **) = nullptr;
    bool load() {
        if (!library.load()) return false;
        connect = reinterpret_cast<decltype(connect)>(library.resolve("xcb_connect"));
        hasError = reinterpret_cast<decltype(hasError)>(library.resolve("xcb_connection_has_error"));
        disconnect = reinterpret_cast<decltype(disconnect)>(library.resolve("xcb_disconnect"));
        flush = reinterpret_cast<decltype(flush)>(library.resolve("xcb_flush"));
        fileDescriptor = reinterpret_cast<decltype(fileDescriptor)>(library.resolve("xcb_get_file_descriptor"));
        inputFocus = reinterpret_cast<decltype(inputFocus)>(library.resolve("xcb_get_input_focus"));
        pollForReply = reinterpret_cast<decltype(pollForReply)>(library.resolve("xcb_poll_for_reply"));
        return connect && hasError && disconnect && flush && fileDescriptor && inputFocus && pollForReply;
    }
};
}

struct ResponsivenessProbe::Impl {
    static constexpr int Buckets = 12;
    static constexpr double BucketLimits[Buckets] = {1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 1e12};
    std::atomic<bool> stopping{false}, fast{false}, displayAvailable{false};
    std::atomic<qint64> displayPendingSince{-1};
    QMutex mutex, pauseMutex;
    QWaitCondition stopRequested;
    double schedulerMax = 0, displayMax = 0;
    int histogram[Buckets] = {};
    int schedulerSamples = 0, displaySamples = 0;
    qint64 windowStart = 0;
    std::unique_ptr<Worker> schedulerThread, displayThread;

    void scheduler() {
        while (!stopping.load(std::memory_order_relaxed)) {
            const qint64 periodUs = fast.load(std::memory_order_relaxed) ? 20000 : 200000;
            const qint64 before = monotonicMicroseconds();
            timespec request {};
            request.tv_sec = periodUs / 1000000; request.tv_nsec = (periodUs % 1000000) * 1000;
            // An interrupted sleep measures nothing about the scheduler: discard it.
            if (::clock_nanosleep(CLOCK_MONOTONIC, 0, &request, nullptr) != 0) continue;
            const double lateMs = std::max<qint64>(0, monotonicMicroseconds() - before - periodUs) / 1000.;
            QMutexLocker lock(&mutex);
            int bucket = 0;
            while (bucket < Buckets - 1 && lateMs >= BucketLimits[bucket]) ++bucket;
            ++histogram[bucket]; ++schedulerSamples;
            schedulerMax = std::max(schedulerMax, lateMs);
        }
    }
    // Waits without waking up in between, yet returns at once when stopped.
    void pause(int milliseconds) {
        QMutexLocker lock(&pauseMutex);
        if (!stopping.load()) stopRequested.wait(&pauseMutex, static_cast<unsigned long>(milliseconds));
    }
    void display() {
        if (qEnvironmentVariableIsEmpty("DISPLAY")) return;
        Xcb xcb;
        if (!xcb.load()) return;
        while (!stopping.load(std::memory_order_relaxed)) {
            int screen = 0;
            Xcb::Connection *connection = xcb.connect(nullptr, &screen);
            if (!connection || xcb.hasError(connection)) {
                if (connection) xcb.disconnect(connection);
                displayAvailable = false;
                pause(30000);
                continue;
            }
            displayAvailable = true;
            roundTrips(xcb, connection);
            xcb.disconnect(connection);
            displayPendingSince = -1;
            displayAvailable = false;
            pause(5000);
        }
    }
    void roundTrips(Xcb &xcb, Xcb::Connection *connection) {
        const int descriptor = xcb.fileDescriptor(connection);
        while (!stopping.load(std::memory_order_relaxed) && !xcb.hasError(connection)) {
            const Xcb::Cookie cookie = xcb.inputFocus(connection);
            xcb.flush(connection);
            const qint64 started = monotonicMicroseconds();
            displayPendingSince = started;
            bool answered = false;
            while (!answered && !stopping.load(std::memory_order_relaxed)) {
                void *reply = nullptr, *error = nullptr;
                if (xcb.pollForReply(connection, cookie.sequence, &reply, &error)) {
                    std::free(reply); std::free(error);
                    answered = true;
                    break;
                }
                if (xcb.hasError(connection)) return;
                // A server unresponsive this long gets a new connection attempt
                // rather than an unbounded wait; its age has already been reported.
                if (monotonicMicroseconds() - started > 60000000) return;
                pollfd wait{descriptor, POLLIN, 0};
                ::poll(&wait, 1, 100);
            }
            displayPendingSince = -1;
            if (!answered) return;
            const double elapsedMs = (monotonicMicroseconds() - started) / 1000.;
            {
                QMutexLocker lock(&mutex);
                ++displaySamples;
                displayMax = std::max(displayMax, elapsedMs);
            }
            pause(fast.load(std::memory_order_relaxed) ? 200 : 1000);
        }
    }
};
constexpr double ResponsivenessProbe::Impl::BucketLimits[];

ResponsivenessProbe::ResponsivenessProbe() : d(new Impl) {}
ResponsivenessProbe::~ResponsivenessProbe() { stop(); }

void ResponsivenessProbe::start() {
    if (d->schedulerThread) return;
    d->stopping = false;
    d->windowStart = monotonicMicroseconds();
    d->schedulerThread.reset(new Worker([this] { d->scheduler(); }));
    d->displayThread.reset(new Worker([this] { d->display(); }));
    d->schedulerThread->start();
    d->displayThread->start();
}

void ResponsivenessProbe::stop() {
    if (!d->schedulerThread) return;
    {
        QMutexLocker lock(&d->pauseMutex);
        d->stopping = true;
        d->stopRequested.wakeAll();
    }
    d->schedulerThread->wait();
    d->displayThread->wait();
    d->schedulerThread.reset();
    d->displayThread.reset();
}

void ResponsivenessProbe::setFast(bool fast) { d->fast = fast; }

QJsonObject ResponsivenessProbe::takeWindow() {
    QMutexLocker lock(&d->mutex);
    const qint64 now = monotonicMicroseconds();
    double p95 = 0;
    if (d->schedulerSamples) {
        const int rank = (d->schedulerSamples * 95 + 99) / 100;
        int seen = 0;
        for (int bucket = 0; bucket < Impl::Buckets; ++bucket) {
            seen += d->histogram[bucket];
            if (seen >= rank) { p95 = std::min(Impl::BucketLimits[bucket], d->schedulerMax); break; }
        }
    }
    const qint64 pendingSince = d->displayPendingSince.load();
    const double pendingMs = pendingSince >= 0 ? (now - pendingSince) / 1000. : 0;
    const bool available = d->displayAvailable.load();
    QJsonObject result{{"schedulerSamples", d->schedulerSamples},
        {"schedulerMaxMs", d->schedulerSamples ? QJsonValue(d->schedulerMax) : QJsonValue(QJsonValue::Null)},
        {"schedulerP95Ms", d->schedulerSamples ? QJsonValue(p95) : QJsonValue(QJsonValue::Null)},
        {"displayAvailable", available}, {"displaySamples", d->displaySamples},
        {"displayMaxMs", available && (d->displaySamples || pendingMs > 0) ? QJsonValue(std::max(d->displayMax, pendingMs)) : QJsonValue(QJsonValue::Null)},
        {"windowSeconds", (now - d->windowStart) / 1e6}};
    std::fill(std::begin(d->histogram), std::end(d->histogram), 0);
    d->schedulerSamples = d->displaySamples = 0;
    d->schedulerMax = d->displayMax = 0;
    d->windowStart = now;
    return result;
}
