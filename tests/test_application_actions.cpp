#include "../src/application_actions.h"
#include "../src/process_actions.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QThread>
#include <QTemporaryDir>
#include <QVector>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xproto.h>
// Test-only raw Xlib request allocator, used to recreate the exact numeric XID.
extern "C" void *_XGetRequest(Display *, unsigned char, size_t);
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <poll.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#include <cstddef>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {
int assertions = 0;
void check(bool condition, const char *message) {
    ++assertions;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
void writeLine(int fd, const QByteArray &line) {
    const QByteArray bytes = line + '\n';
    int written = 0;
    while (written < bytes.size()) {
        const ssize_t count = ::write(fd, bytes.constData() + written, size_t(bytes.size() - written));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) ::_exit(126);
        written += int(count);
    }
}
QByteArray readLine(int fd) {
    QByteArray result;
    for (int i = 0; i < 2048; ++i) {
        pollfd descriptor{fd, POLLIN, 0};
        const int polled = ::poll(&descriptor, 1, 5000);
        if (polled <= 0) return {};
        char byte = 0;
        if (::read(fd, &byte, 1) != 1) return {};
        if (byte == '\n') return result;
        result.append(byte);
    }
    return {};
}

QVector<unsigned long> listed(Display *display) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    QVector<unsigned long> result;
    XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_CLIENT_LIST", False),
                       0, 4096, False, XA_WINDOW, &type, &format, &count, &remaining, &data);
    if (type == XA_WINDOW && format == 32 && data) {
        const auto *values = reinterpret_cast<unsigned long *>(data);
        for (unsigned long i = 0; i < count; ++i) result.append(values[i]);
    }
    if (data) XFree(data);
    return result;
}
void registerWindow(Display *display, Window window, bool add) {
    XGrabServer(display);
    auto clients = listed(display);
    clients.removeAll(window);
    if (add) clients.append(window);
    XChangeProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_CLIENT_LIST", False),
                    XA_WINDOW, 32, PropModeReplace, reinterpret_cast<unsigned char *>(clients.data()), clients.size());
    XUngrabServer(display);
    XSync(display, False);
}

int fixture() {
    Display *display = XOpenDisplay(nullptr);
    if (!display) return 124;
    char hostname[256]{};
    if (::gethostname(hostname, sizeof(hostname)) != 0) return 124;
    const Atom pidAtom = XInternAtom(display, "_NET_WM_PID", False);
    const Atom hostAtom = XInternAtom(display, "WM_CLIENT_MACHINE", False);
    const Atom protocols = XInternAtom(display, "WM_PROTOCOLS", False);
    const Atom deleteWindow = XInternAtom(display, "WM_DELETE_WINDOW", False);
    const Atom nameAtom = XInternAtom(display, "_NET_WM_NAME", False);
    const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
    const Atom typeAtom = XInternAtom(display, "_NET_WM_WINDOW_TYPE", False);
    const auto setPid = [&](Window window) {
        unsigned long pid = ::getpid();
        XChangeProperty(display, window, pidAtom, XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&pid), 1);
    };
    const auto setHost = [&](Window window, const QByteArray &host) {
        XChangeProperty(display, window, hostAtom, XA_STRING, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(host.constData()), host.size());
    };
    const auto setType = [&](Window window, const char *name) {
        unsigned long type = XInternAtom(display, name, False);
        XChangeProperty(display, window, typeAtom, XA_ATOM, 32, PropModeReplace,
                        reinterpret_cast<unsigned char *>(&type), 1);
    };
    const auto configure = [&](Window window) {
        setPid(window); setHost(window, hostname);
        Atom protocol = deleteWindow;
        XSetWMProtocols(display, window, &protocol, 1);
        const QByteArray title = "Fixture <b>plain</b> window";
        XChangeProperty(display, window, nameAtom, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(title.constData()), title.size());
        setType(window, "_NET_WM_WINDOW_TYPE_NORMAL");
    };
    const auto create = [&] {
        const Window window = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 80, 50, 0, 0, 0);
        configure(window);
        XMapWindow(display, window);
        registerWindow(display, window, true);
        return window;
    };
    Window mainWindow = create(), extraWindow = None;
    QVector<Window> manyWindows;
    int received = 0;
    const auto drain = [&] {
        XSync(display, False);
        while (XPending(display)) {
            XEvent event{};
            XNextEvent(display, &event);
            if (event.type == ClientMessage && event.xclient.message_type == protocols &&
                event.xclient.format == 32 && event.xclient.data.l[0] == long(deleteWindow)) ++received;
        }
    };
    writeLine(STDOUT_FILENO, "READY");
    for (;;) {
        pollfd descriptors[2]{{STDIN_FILENO, POLLIN, 0}, {ConnectionNumber(display), POLLIN, 0}};
        if (::poll(descriptors, 2, 5000) < 0 && errno != EINTR) break;
        drain();
        if (descriptors[0].revents & (POLLHUP | POLLERR)) break;
        if (!(descriptors[0].revents & POLLIN)) continue;
        const QByteArray command = readLine(STDIN_FILENO);
        if (command == "quit" || command.isEmpty()) break;
        if (command == "count") { drain(); writeLine(STDOUT_FILENO, QByteArray::number(received)); continue; }
        if (command == "missingPid") XDeleteProperty(display, mainWindow, pidAtom);
        else if (command == "pid") setPid(mainWindow);
        else if (command == "missingHost") XDeleteProperty(display, mainWindow, hostAtom);
        else if (command == "foreignHost") setHost(mainWindow, "definitely-not-this-machine.invalid");
        else if (command == "host") setHost(mainWindow, hostname);
        else if (command == "noProtocol") XDeleteProperty(display, mainWindow, protocols);
        else if (command == "protocol") { Atom protocol = deleteWindow; XSetWMProtocols(display, mainWindow, &protocol, 1); }
        else if (command == "dock") setType(mainWindow, "_NET_WM_WINDOW_TYPE_DOCK");
        else if (command == "normal") setType(mainWindow, "_NET_WM_WINDOW_TYPE_NORMAL");
        else if (command == "title") {
            const QByteArray title = "Changed title";
            XChangeProperty(display, mainWindow, nameAtom, utf8, 8, PropModeReplace,
                            reinterpret_cast<const unsigned char *>(title.constData()), title.size());
        } else if (command == "many") { for (int i = 0; i < 32; ++i) manyWindows.append(create()); }
        else if (command == "clearMany") {
            for (Window window : manyWindows) { registerWindow(display, window, false); XDestroyWindow(display, window); }
            manyWindows.clear();
        }
        else if (command == "add") { if (!extraWindow) extraWindow = create(); }
        else if (command == "removeExtra") {
            if (extraWindow) { registerWindow(display, extraWindow, false); XDestroyWindow(display, extraWindow); extraWindow = None; }
        } else if (command == "reuse") {
            // XCreateSimpleWindow allocates a new XID. Send the normal core
            // CreateWindow request with the now-free old ID to prove that
            // identical PID/title/properties/list membership cannot bypass the
            // monitor connection's retained DestroyNotify subscription.
            XDestroyWindow(display, mainWindow);
            auto *request = static_cast<xCreateWindowReq *>(_XGetRequest(display, X_CreateWindow, sizeof(xCreateWindowReq)));
            if (!request) return 123;
            request->depth = CopyFromParent;
            request->wid = CARD32(mainWindow); request->parent = CARD32(DefaultRootWindow(display));
            request->x = 0; request->y = 0; request->width = 80; request->height = 50;
            request->borderWidth = 0; request->c_class = InputOutput;
            request->visual = CopyFromParent; request->mask = 0;
            configure(mainWindow);
            XMapWindow(display, mainWindow);
        } else if (command == "replace") {
            registerWindow(display, mainWindow, false); XDestroyWindow(display, mainWindow); mainWindow = create();
        }
        XSync(display, False);
        writeLine(STDOUT_FILENO, "OK");
    }
    for (Window window : manyWindows) { registerWindow(display, window, false); XDestroyWindow(display, window); }
    if (extraWindow) { registerWindow(display, extraWindow, false); XDestroyWindow(display, extraWindow); }
    registerWindow(display, mainWindow, false); XDestroyWindow(display, mainWindow);
    XCloseDisplay(display);
    return 0;
}

struct Child { pid_t pid = -1; int command = -1, response = -1; };
Child spawn(const QByteArray &path, bool shell = false) {
    int command[2], response[2];
    check(::pipe(command) == 0 && ::pipe(response) == 0, "create fixture IPC");
    const pid_t parent = ::getpid(), child = ::fork();
    check(child >= 0, "fork fixture sibling");
    if (child == 0) {
        if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent) ::_exit(126);
        ::dup2(command[0], STDIN_FILENO); ::dup2(response[1], STDOUT_FILENO);
        ::close(command[0]); ::close(command[1]); ::close(response[0]); ::close(response[1]);
        const QByteArray nativeLibraries = qgetenv("DEVSTATS_NATIVE_LD_LIBRARY_PATH");
        if (!nativeLibraries.isEmpty()) ::setenv("LD_LIBRARY_PATH", nativeLibraries.constData(), 1);
        if (shell) ::execl("/bin/bash", "bash", "-c", "read answer", static_cast<char *>(nullptr));
        else ::execl(path.constData(), path.constData(), "--fixture", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    ::close(command[0]); ::close(response[1]);
    Child result{child, command[1], response[0]};
    if (!shell) check(readLine(result.response) == "READY", "real Xlib fixture ready");
    return result;
}
QByteArray command(const Child &child, const QByteArray &value) {
    writeLine(child.command, value);
    const QByteArray response = readLine(child.response);
    check(!response.isEmpty(), "fixture still alive and responds");
    return response;
}
QJsonObject identity(pid_t pid) {
    for (int retry = 0; retry < 100; ++retry) {
        // Match the production /proc-directory access. QEMU synthesizes a
        // direct /proc/self/stat with zero starttime; openat on a pinned native
        // proc directory exposes the real identity used by ProcessActions.
        const QByteArray path = "/proc/" + QByteArray::number(pid);
        const int directory = ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        QFile stat;
        const int descriptor = directory >= 0 ? ::openat(directory, "stat", O_RDONLY | O_CLOEXEC | O_NOFOLLOW) : -1;
        char executableBytes[16384];
        const ssize_t length = directory >= 0 ? ::readlinkat(directory, "exe", executableBytes, sizeof(executableBytes)) : -1;
        if (directory >= 0) ::close(directory);
        if (descriptor >= 0 && stat.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
            const QByteArray text = stat.readAll();
            const auto fields = text.mid(text.lastIndexOf(')') + 1).simplified().split(' ');
            const QString executable = length > 0 ? QFile::decodeName(QByteArray(executableBytes, int(length))) : QString();
            if (fields.size() >= 20 && fields[19].toULongLong() != 0 && !executable.isEmpty())
                return {{"pid", double(pid)}, {"startTicks", double(fields[19].toULongLong())}, {"executablePath", executable}};
        } else if (descriptor >= 0) ::close(descriptor);
        QThread::msleep(5);
    }
    check(false, "native proc identity readable");
    return {};
}
void refused(const QJsonObject &result, const char *message, const char *code = nullptr) {
    check(!result.value("ok").toBool() && !result.value("canClose").toBool(), message);
    check(!result.value("message").toString().isEmpty(), "refusal includes explanation");
    if (code && result.value("code").toString() != QLatin1String(code)) {
        std::fprintf(stderr, "expected=%s got=%s message=%s\n", code,
                     qPrintable(result.value("code").toString()), qPrintable(result.value("message").toString()));
        check(false, message);
    }
}
int tests(const Child &target, const Child &sentinel, const Child &critical) {
    check(::getuid() != 0 && ::getuid() == ::geteuid(), "ordinary UID required");
    // Simulate a 4.19 kernel for the existing signal operation. The identity
    // checker and normal window close must remain available without pidfds.
#if defined(SYS_pidfd_send_signal) && !defined(__aarch64__)
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
    };
    sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
    check(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
          ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "simulate Linux 4.19 pidfd unavailable");
#endif
    const QJsonArray selected{identity(target.pid)};
    check(ProcessActions().inspectIdentity(selected[0].toObject()).value("ok").toBool(),
          "shared identity validation requires no signal capability");
#if defined(SYS_pidfd_send_signal) && !defined(__aarch64__)
    check(ProcessActions().inspect(selected[0].toObject()).value("code").toString() == "unsupportedKernel",
          "existing SIGTERM operation still refuses unsupported kernel");
#endif
    ApplicationActions actions;
    refused(actions.inspect({}), "empty selection", "invalidSelection");
    const auto ownIdentity = identity(::getpid());
    const auto ownInspection = actions.inspect(QJsonArray{ownIdentity});
    if (ownInspection.value("code").toString() != "protectedProcess")
        std::fprintf(stderr, "own identity=%s\n", QJsonDocument(ownIdentity).toJson(QJsonDocument::Compact).constData());
    refused(ownInspection, "monitor is protected", "protectedProcess");
    refused(actions.inspect(QJsonArray{identity(::getppid())}), "monitor ancestry is protected", "protectedProcess");
    refused(actions.inspect(QJsonArray{identity(critical.pid)}), "real system bash critical process", "protectedProcess");
    refused(actions.inspect(QJsonArray{selected[0], selected[0]}), "duplicates refused", "duplicateIdentity");
    QJsonArray excessive;
    for (int i = 0; i < 65; ++i) excessive.append(selected[0]);
    refused(actions.inspect(excessive), "selection bound", "invalidSelection");
    auto stale = selected[0].toObject(); stale["startTicks"] = stale.value("startTicks").toDouble() + 1;
    refused(actions.inspect(QJsonArray{stale}), "stale start time", "staleIdentity");
    auto changed = selected[0].toObject(); changed["executablePath"] = "/definitely/a/different/executable";
    refused(actions.inspect(QJsonArray{changed}), "changed executable", "executableChanged");
    qputenv("XDG_SESSION_TYPE", "wayland");
    refused(actions.inspect(selected), "Wayland disabled even with DISPLAY", "unsupportedSession");
    qputenv("XDG_SESSION_TYPE", "x11");
    qputenv("WAYLAND_DISPLAY", "wayland-fixture");
    refused(actions.inspect(selected), "Wayland socket disables Xwayland association", "unsupportedSession");
    qunsetenv("WAYLAND_DISPLAY");
    const QByteArray localDisplay = qgetenv("DISPLAY"); qputenv("DISPLAY", "localhost:99");
    refused(actions.inspect(selected), "remote display disabled", "unsupportedSession");
    qputenv("DISPLAY", localDisplay);
    for (const auto &pair : QList<QPair<QByteArray, QByteArray>>{{"missingPid", "pid"}, {"missingHost", "host"},
                                                               {"foreignHost", "host"}, {"noProtocol", "protocol"}, {"dock", "normal"}}) {
        command(target, pair.first);
        refused(actions.inspect(selected), "missing identity/protocol/foreign/protected windows refused");
        check(command(target, "count") == "0" && command(sentinel, "count") == "0", "refusals send no close");
        command(target, pair.second);
    }
    command(target, "many");
    refused(actions.inspect(selected), "more than 32 windows refused", "tooManyWindows");
    check(command(target, "count") == "0", "window bound sends no requests");
    command(target, "clearMany");
    auto inspect = actions.inspect(selected);
    check(inspect.value("ok").toBool() && inspect.value("canClose").toBool() && inspect.value("windowCount").toInt() == 1,
          "native owned sibling window actionable");
    check(inspect.value("windows").toArray()[0].toObject().value("title").toString().contains("<b>plain</b>"),
          "titles remain plain text for caller to render safely");
    check(command(target, "count") == "0", "inspect never sends WM_DELETE");
    const auto originalToken = inspect.value("windows").toArray()[0].toObject();
    command(target, "reuse");
    refused(actions.close(selected, inspect.value("windows").toArray()),
            "actual identical numeric XID reuse is refused", "windowReplaced");
    inspect = actions.inspect(selected);
    const auto reusedToken = inspect.value("windows").toArray()[0].toObject();
    check(reusedToken.value("windowId") == originalToken.value("windowId") &&
          reusedToken.value("fingerprint") == originalToken.value("fingerprint"),
          "fixture really recreated the same XID with identical PID/hostname/protocol/title/type");
    check(command(target, "count") == "0", "numeric XID reuse refusal sends no close");
    const auto cancelledWindows = inspect.value("windows").toArray();
    actions.discard();
    refused(actions.close(selected, cancelledWindows), "cancelled confirmation releases snapshot", "confirmationExpired");
    inspect = actions.inspect(selected);
    auto modifiedTokens = inspect.value("windows").toArray();
    auto token = modifiedTokens[0].toObject(); token["windowId"] = "1"; modifiedTokens[0] = token;
    refused(actions.close(selected, modifiedTokens), "tampered confirmation refused", "confirmationExpired");
    refused(actions.close(selected, inspect.value("windows").toArray()), "confirmation is single use", "confirmationExpired");
    inspect = actions.inspect(selected); command(target, "title");
    refused(actions.close(selected, inspect.value("windows").toArray()), "changed title refused", "windowSetChanged");
    inspect = actions.inspect(selected); command(target, "add");
    refused(actions.close(selected, inspect.value("windows").toArray()), "new window requires new confirmation", "windowSetChanged");
    command(target, "removeExtra");
    inspect = actions.inspect(selected); command(target, "replace");
    refused(actions.close(selected, inspect.value("windows").toArray()), "destroyed/replaced XID refused", "windowReplaced");
    inspect = actions.inspect(selected);
    refused(actions.close(QJsonArray{stale}, inspect.value("windows").toArray()), "stale identity at close", "staleIdentity");
    inspect = actions.inspect(selected); command(target, "noProtocol");
    refused(actions.close(selected, inspect.value("windows").toArray()), "protocol removed at close", "closeUnsupported");
    command(target, "protocol");
    inspect = actions.inspect(selected); command(target, "foreignHost");
    refused(actions.close(selected, inspect.value("windows").toArray()), "hostname changed at close", "foreignWindow");
    command(target, "host");
    check(command(target, "count") == "0" && command(sentinel, "count") == "0", "all refusal paths leave target and sentinel untouched");
    const QJsonArray both{selected[0], identity(sentinel.pid)};
    inspect = actions.inspect(both);
    check(inspect.value("windowCount").toInt() == 2, "multiple selected processes are inspected");
    auto staleSentinel = both[1].toObject();
    staleSentinel["startTicks"] = staleSentinel.value("startTicks").toDouble() + 1;
    refused(actions.close(QJsonArray{selected[0], staleSentinel}, inspect.value("windows").toArray()),
            "one stale process rejects the entire batch", "staleIdentity");
    check(command(target, "count") == "0" && command(sentinel, "count") == "0", "batch refusal sends nothing to valid member");
    inspect = actions.inspect(selected);
    refused(actions.close(QJsonArray{both[1]}, inspect.value("windows").toArray()),
            "changed selected process refused", "selectionChanged");
    command(target, "add");
    inspect = actions.inspect(selected);
    check(inspect.value("windowCount").toInt() == 2, "all selected process windows appear in confirmation");
    command(sentinel, "add"); // Like DevStats' own confirmation dialog: unselected.
    const auto result = actions.close(selected, inspect.value("windows").toArray());
    if (!result.value("ok").toBool()) std::fprintf(stderr, "%s\n", QJsonDocument(result).toJson().constData());
    check(result.value("ok").toBool() && result.value("requestedCount").toInt() == 2, "only confirmed windows receive WM_DELETE");
    check(command(target, "count") == "2", "native fixture records exactly two ICCCM close messages");
    check(command(sentinel, "count") == "0", "unselected sentinel receives no close messages");
    refused(actions.close(selected, inspect.value("windows").toArray()), "cannot replay successful confirmation", "confirmationExpired");
    check(command(target, "count") == "2", "close request does not force fixture exit or additional messages");
    inspect = actions.inspect(selected);
    command(sentinel, "removeExtra");
    const auto afterUnselectedRemoval = actions.close(selected, inspect.value("windows").toArray());
    check(afterUnselectedRemoval.value("ok").toBool() && afterUnselectedRemoval.value("requestedCount").toInt() == 2,
          "unselected window removal during confirmation does not block close");
    check(command(target, "count") == "4", "unselected removal scenario sends exactly two further target requests");
    check(command(sentinel, "count") == "0", "both unselected window-change scenarios leave sentinel untouched");
    QJsonObject evidence{{"ok", true}, {"uid", double(::getuid())}, {"assertions", assertions},
                         {"selectedWindowRequests", 4}, {"successfulCloseScenarios", 2}, {"targetRequestsPerScenario", 2},
                         {"unselectedWindowRequests", 0},
                         {"targetRemainedRunning", true}, {"protocol", "WM_DELETE_WINDOW"}};
    std::puts(QJsonDocument(evidence).toJson(QJsonDocument::Compact).constData());
    return 0;
}
void cleanup(const Child &child, bool shell = false) {
    if (shell) writeLine(child.command, "finish"); else writeLine(child.command, "quit");
    ::close(child.command); ::close(child.response);
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child.pid, &status, 0); } while (result < 0 && errno == EINTR);
    check(result == child.pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "reap exact owned fixture normally");
}
}

int main(int argc, char **argv) {
    if (argc > 1 && QByteArray(argv[1]) == "--fixture") return fixture();
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    check(directory.isValid(), "create isolated fixture directory");
    QString source = QString::fromLocal8Bit(qgetenv("DEVSTATS_NATIVE_FIXTURE"));
    if (source.isEmpty()) source = QFileInfo("/proc/self/exe").symLinkTarget();
    const QString fixturePath = directory.path() + "/owned-xlib-fixture";
    check(QFile::copy(source, fixturePath), "copy separate executable for safe sibling fixture");
    check(QFile::setPermissions(fixturePath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner), "fixture executable permissions");
    const Child target = spawn(QFile::encodeName(fixturePath));
    const Child sentinel = spawn(QFile::encodeName(fixturePath));
    const Child critical = spawn({}, true);
    // A sibling test runner is essential: a monitor's own descendants must be
    // protected in production. All fixture PIDs remain unreaped until cleanup.
    const pid_t tester = ::fork();
    check(tester >= 0, "fork test runner sibling");
    if (tester == 0) {
        const int result = tests(target, sentinel, critical);
        std::fflush(stdout); ::_exit(result);
    }
    int status = 0;
    pid_t result;
    do { result = ::waitpid(tester, &status, 0); } while (result < 0 && errno == EINTR);
    cleanup(target); cleanup(sentinel); cleanup(critical, true);
    check(result == tester && WIFEXITED(status), "tester exited normally");
    if (WEXITSTATUS(status) != 0) return WEXITSTATUS(status);
    std::puts("PASS: all owned native X11 fixture processes exited normally and were reaped");
    return 0;
}
