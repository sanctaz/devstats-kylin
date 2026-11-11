#include "application_actions.h"
#include "i18n.h"
#include "process_actions.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLibrary>
#include <QMap>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#include <QUuid>
#include <QTimer>
#include <algorithm>

#ifdef Q_OS_LINUX
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <unistd.h>
#include <atomic>
#endif

namespace {
constexpr int MaximumProcesses = 64;
constexpr int MaximumWindows = 32;
constexpr int MaximumClients = 4096;
constexpr qint64 SnapshotLifetimeMs = 120000;

QJsonObject failure(const QString &code, const QString &message) {
    return {{"ok", false}, {"canClose", false}, {"windowCount", 0},
            {"code", code}, {"message", message}, {"windows", QJsonArray()}};
}

QJsonObject validateIdentities(const QJsonArray &identities, QMap<qint64, QJsonObject> &processes) {
    if (identities.isEmpty() || identities.size() > MaximumProcesses)
        return failure("invalidSelection", T("请选择 1–64 个应用进程，再查看可正常关闭的窗口。"));
    const ProcessActions actions;
    for (const auto &value : identities) {
        if (!value.isObject()) return failure("invalidIdentity", T("进程身份无效，请刷新后重新选择。"));
        QJsonObject identity = actions.inspectIdentity(value.toObject());
        if (!identity.value("ok").toBool()) {
            identity["canClose"] = false;
            identity["windowCount"] = 0;
            identity["windows"] = QJsonArray();
            return identity;
        }
        const qint64 pid = qint64(identity.value("pid").toDouble());
        if (processes.contains(pid))
            return failure("duplicateIdentity", T("应用进程重复，请重新选择。"));
        processes.insert(pid, {{"pid", identity.value("pid")},
                               {"startTicks", identity.value("startTicks")},
                               {"executablePath", identity.value("executablePath")}});
    }
    return {{"ok", true}};
}

#ifdef Q_OS_LINUX
// Runtime already ships libX11.so.6; do not introduce QtX11Extras, wmctrl or
// xdotool dependencies. Xlib headers are used only for the protocol ABI.
struct XApi {
    QLibrary library{QStringLiteral("libX11.so.6")};
#define X_FUNCTION(name) decltype(&::name) name = nullptr;
    X_FUNCTION(XOpenDisplay) X_FUNCTION(XCloseDisplay) X_FUNCTION(XDefaultRootWindow)
    X_FUNCTION(XInternAtom) X_FUNCTION(XGetWindowProperty) X_FUNCTION(XFree)
    X_FUNCTION(XSelectInput) X_FUNCTION(XPending) X_FUNCTION(XNextEvent)
    X_FUNCTION(XSync) X_FUNCTION(XGrabServer) X_FUNCTION(XUngrabServer)
    X_FUNCTION(XSendEvent) X_FUNCTION(XSetErrorHandler) X_FUNCTION(XGetWindowAttributes)
#undef X_FUNCTION
    bool load() {
        if (!library.load()) return false;
#define LOAD_X(name) name = reinterpret_cast<decltype(name)>(library.resolve(#name)); if (!name) return false;
        LOAD_X(XOpenDisplay) LOAD_X(XCloseDisplay) LOAD_X(XDefaultRootWindow)
        LOAD_X(XInternAtom) LOAD_X(XGetWindowProperty) LOAD_X(XFree)
        LOAD_X(XSelectInput) LOAD_X(XPending) LOAD_X(XNextEvent)
        LOAD_X(XSync) LOAD_X(XGrabServer) LOAD_X(XUngrabServer)
        LOAD_X(XSendEvent) LOAD_X(XSetErrorHandler) LOAD_X(XGetWindowAttributes)
#undef LOAD_X
        return true;
    }
};

QMutex xMutex;
struct ErrorTrap;
thread_local ErrorTrap *activeTrap = nullptr;
std::atomic<XErrorHandler> previousHandler{nullptr};
struct ErrorTrap {
    XApi &x;
    Display *display;
    int error = 0;
    XErrorHandler previous;
    static int handler(Display *display, XErrorEvent *event) {
        if (activeTrap && activeTrap->display == display) {
            activeTrap->error = event->error_code;
            return 0;
        }
        const auto previous = previousHandler.load();
        return previous ? previous(display, event) : 0;
    }
    ErrorTrap(XApi &api, Display *d) : x(api), display(d) {
        previous = x.XSetErrorHandler(handler);
        previousHandler.store(previous);
        activeTrap = this;
    }
    ~ErrorTrap() {
        x.XSync(display, False);
        activeTrap = nullptr;
        x.XSetErrorHandler(previous);
    }
    bool synced() { x.XSync(display, False); return error == 0; }
};

struct ServerGrab {
    XApi &x;
    Display *display;
    ServerGrab(XApi &api, Display *d) : x(api), display(d) { x.XGrabServer(display); }
    ~ServerGrab() { x.XUngrabServer(display); x.XSync(display, False); }
};

struct Property {
    Atom type = None;
    int format = 0;
    QByteArray bytes;
    QList<quint32> words;
    bool present = false;
};

bool property(XApi &x, Display *display, Window window, Atom name, Property &result,
              unsigned long maximum = 4096) {
    Atom type = None;
    int format = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char *data = nullptr;
    const int status = x.XGetWindowProperty(display, window, name, 0, long(maximum), False,
                                           AnyPropertyType, &type, &format, &count, &remaining, &data);
    const auto release = [&] { if (data) x.XFree(data); };
    if (status != Success || remaining != 0 || count > maximum * 4) { release(); return false; }
    result.type = type;
    result.format = format;
    result.present = type != None;
    if (format == 8 && data) result.bytes = QByteArray(reinterpret_cast<char *>(data), int(count));
    else if (format == 32 && data) {
        const auto *values = reinterpret_cast<unsigned long *>(data);
        for (unsigned long i = 0; i < count; ++i) result.words.append(quint32(values[i]));
    } else if (result.present && format != 8 && format != 32) { release(); return false; }
    release();
    return true;
}

struct Atoms {
    Atom clients, pid, host, protocols, deleteWindow, utf8, netName, name, type, normal, dialog;
    Atoms(XApi &x, Display *display) {
        const auto a = [&](const char *name) { return x.XInternAtom(display, name, False); };
        clients = a("_NET_CLIENT_LIST"); pid = a("_NET_WM_PID"); host = a("WM_CLIENT_MACHINE");
        protocols = a("WM_PROTOCOLS"); deleteWindow = a("WM_DELETE_WINDOW"); utf8 = a("UTF8_STRING");
        netName = a("_NET_WM_NAME"); name = XA_WM_NAME; type = a("_NET_WM_WINDOW_TYPE");
        normal = a("_NET_WM_WINDOW_TYPE_NORMAL"); dialog = a("_NET_WM_WINDOW_TYPE_DIALOG");
    }
};

QString displayTitle(QString text) {
    // Titles are untrusted text. The caller must use Qt::PlainText; stripping
    // controls additionally prevents injected lines or invisible bidi controls.
    for (int i = 0; i < text.size(); ++i) {
        if (text[i].category() == QChar::Other_Control || text[i].category() == QChar::Other_Format)
            text[i] = QLatin1Char(' ');
    }
    text = text.simplified().left(240);
    return text.isEmpty() ? T("（无标题窗口）") : text;
}

QString digest(const QList<Property> &properties) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto &p : properties) {
        hash.addData(QByteArray::number(qulonglong(p.type)) + ':' + QByteArray::number(p.format) + ':');
        hash.addData(p.bytes.toBase64());
        for (quint32 value : p.words) hash.addData(':' + QByteArray::number(value));
        hash.addData(";");
    }
    return QString::fromLatin1(hash.result().toHex());
}

QJsonObject enumerate(XApi &x, Display *display, const Atoms &a, const QByteArray &hostname,
                      const QMap<qint64, QJsonObject> &processes, const QString &snapshotId,
                      QJsonArray &windows, QList<quint32> *recordClients = nullptr,
                      const QList<quint32> *expectedClients = nullptr,
                      const QSet<quint32> *limitTo = nullptr) {
    Property clients;
    if (!property(x, display, x.XDefaultRootWindow(display), a.clients, clients, MaximumClients) ||
        !clients.present || clients.type != XA_WINDOW || clients.format != 32 ||
        clients.words.size() > MaximumClients)
        return failure("windowListUnavailable", T("桌面未提供可信的应用窗口列表，无法正常关窗。"));
    QSet<quint32> unique;
    QSet<qint64> represented;
    QList<quint32> sorted = clients.words;
    std::sort(sorted.begin(), sorted.end());
    if (expectedClients && sorted != *expectedClients)
        return failure("windowSetChanged", T("桌面窗口列表已变化，请重新查看并确认，未发送关窗请求。"));
    if (recordClients) *recordClients = sorted;
    for (quint32 id : sorted) {
        if (id == 0 || unique.contains(id))
            return failure("windowListChanged", T("桌面窗口列表异常，请刷新后重试。"));
        unique.insert(id);
        if (limitTo && !limitTo->contains(id)) continue;
        Property pid;
        if (!property(x, display, Window(id), a.pid, pid))
            return failure("windowChanged", T("桌面窗口已变化，请重新确认。"));
        // Unrelated or legacy windows without PID cannot be associated with a
        // selected process. Never guess from titles, classes or process names.
        if (pid.type != XA_CARDINAL || pid.format != 32 || pid.words.size() != 1 ||
            !processes.contains(qint64(pid.words[0]))) continue;
        const qint64 processId = qint64(pid.words[0]);
        Property host, protocols, title, legacyTitle, type;
        if (!property(x, display, Window(id), a.host, host, 256) ||
            !property(x, display, Window(id), a.protocols, protocols, 256) ||
            !property(x, display, Window(id), a.netName, title, 1024) ||
            !property(x, display, Window(id), a.name, legacyTitle, 1024) ||
            !property(x, display, Window(id), a.type, type, 64))
            return failure("windowChanged", T("无法完整核对应用窗口，请刷新后重试。"));
        if (host.type != XA_STRING || host.format != 8 || host.bytes != hostname)
            return failure("foreignWindow", T("应用窗口未声明本机主机名，无法安全关联本机进程。"));
        if (protocols.type != XA_ATOM || protocols.format != 32 ||
            !protocols.words.contains(quint32(a.deleteWindow)))
            return failure("closeUnsupported", T("所选应用有窗口不支持正常关闭协议，请在应用内操作。"));
        // Do not close desktop/dock/menu/splash/notification windows, even if a
        // client labels one with a selected PID. Legacy type-less windows are
        // ordinary ICCCM windows; explicitly typed windows must be normal/dialog.
        if (type.present && (type.type != XA_ATOM || type.format != 32 ||
            type.words.isEmpty() || (type.words[0] != a.normal && type.words[0] != a.dialog)))
            return failure("protectedWindow", T("桌面、面板及非普通应用窗口受到保护。"));
        XWindowAttributes attributes{};
        if (!x.XGetWindowAttributes(display, Window(id), &attributes) ||
            attributes.c_class != InputOutput || attributes.override_redirect)
            return failure("protectedWindow", T("此窗口不是可正常关闭的普通应用窗口。"));
        if (windows.size() >= MaximumWindows)
            return failure("tooManyWindows", T("一次最多关闭 32 个窗口，请缩小应用选择范围。"));
        const auto identity = processes.value(processId);
        QString text;
        if (title.type == a.utf8 && title.format == 8) text = QString::fromUtf8(title.bytes);
        else if (legacyTitle.type == XA_STRING && legacyTitle.format == 8)
            text = QString::fromLocal8Bit(legacyTitle.bytes);
        windows.append(QJsonObject{{"windowId", QString::number(id)}, {"pid", double(processId)},
                         {"startTicks", identity.value("startTicks")},
                         {"executablePath", identity.value("executablePath")},
                         {"title", displayTitle(text)}, {"host", QString::fromLocal8Bit(hostname)},
                         {"fingerprint", digest({pid, host, protocols, title, legacyTitle, type})},
                         {"snapshotId", snapshotId}});
        represented.insert(processId);
    }
    if (windows.isEmpty() || represented.size() != processes.size())
        return failure("noApplicationWindows", T("部分所选进程没有可核对的应用窗口；后台服务或窗口缺少身份信息时无法正常关窗。"));
    return {{"ok", true}};
}

bool invalidatedEvents(XApi &x, Display *display) {
    // A confirmation can stay open while windows move. Bound the event drain
    // so a busy or misbehaving client cannot keep the X server grabbed.
    for (int count = 0; x.XPending(display); ++count) {
        if (count >= 4096) return true;
        XEvent event{};
        x.XNextEvent(display, &event);
        if (event.type == DestroyNotify) return true;
    }
    return false;
}

bool localEnvironment() {
    const QByteArray display = qgetenv("DISPLAY");
    return (display.startsWith(':') || display.startsWith("unix:")) &&
           qgetenv("XDG_SESSION_TYPE").toLower() != "wayland" &&
           qgetenv("WAYLAND_DISPLAY").isEmpty() &&
           !qgetenv("QT_QPA_PLATFORM").toLower().startsWith("wayland");
}
#endif
}

struct ApplicationActions::Snapshot {
#ifdef Q_OS_LINUX
    XApi x;
    Display *display = nullptr;
    QMap<qint64, QJsonObject> processes;
    QJsonArray windows;
    QList<quint32> clientIds;
    QByteArray hostname;
    QByteArray displayName;
    QString id;
    QElapsedTimer age;
    ~Snapshot() { if (display) x.XCloseDisplay(display); }
#endif
};

ApplicationActions::ApplicationActions() : cleanupContext_(new QObject) {}
ApplicationActions::~ApplicationActions() = default;

void ApplicationActions::discard() const {
#ifdef Q_OS_LINUX
    QMutexLocker locker(&xMutex);
#endif
    snapshot_.reset();
}

QJsonObject ApplicationActions::inspect(const QJsonArray &identities) const {
#ifndef Q_OS_LINUX
    Q_UNUSED(identities)
    return failure("unsupportedPlatform", T("正常关窗仅适用于本机 Linux X11 桌面。"));
#else
    QMutexLocker locker(&xMutex);
    snapshot_.reset();
    if (!localEnvironment())
        return failure("unsupportedSession", T("正常关窗需要本机 X11 桌面；Wayland 或远程显示会话暂不支持。"));
    auto snapshot = std::make_unique<Snapshot>();
    QJsonObject result = validateIdentities(identities, snapshot->processes);
    if (!result.value("ok").toBool()) return result;
    if (!snapshot->x.load() || !(snapshot->display = snapshot->x.XOpenDisplay(nullptr)))
        return failure("x11Unavailable", T("无法连接本机 X11 桌面，请在应用内关闭窗口。"));
    char hostname[256]{};
    if (::gethostname(hostname, sizeof(hostname)) != 0 || !hostname[0] || hostname[255] != '\0')
        return failure("hostnameUnavailable", T("无法确认本机主机名，正常关窗已禁用。"));
    snapshot->hostname = QByteArray(hostname);
    snapshot->displayName = qgetenv("DISPLAY");
    snapshot->id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ErrorTrap errors(snapshot->x, snapshot->display);
    const Atoms atoms(snapshot->x, snapshot->display);
    result = enumerate(snapshot->x, snapshot->display, atoms, snapshot->hostname,
                       snapshot->processes, snapshot->id, snapshot->windows, &snapshot->clientIds);
    if (!result.value("ok").toBool()) return result;
    // The connection stays alive through confirmation. DestroyNotify catches
    // XID reuse even when the replacement has identical PID/title/properties.
    for (const auto &value : snapshot->windows)
        snapshot->x.XSelectInput(snapshot->display,
            Window(value.toObject().value("windowId").toString().toULongLong()), StructureNotifyMask);
    if (!errors.synced())
        return failure("windowChanged", T("应用窗口已变化，请重新确认。"));
    QJsonArray checked;
    result = enumerate(snapshot->x, snapshot->display, atoms, snapshot->hostname,
                       snapshot->processes, snapshot->id, checked, nullptr, &snapshot->clientIds);
    if (!result.value("ok").toBool() || checked != snapshot->windows || !errors.synced())
        return failure("windowChanged", T("应用窗口已变化，请重新确认。"));
    snapshot->age.start();
    result = {{"ok", true}, {"canClose", true}, {"code", "ready"},
              {"windowCount", snapshot->windows.size()}, {"windows", snapshot->windows},
              {"message", T("可请求正常关闭这些应用窗口。应用可提示保存或拒绝关闭；关窗不保证程序退出或立即释放内存。")}};
    snapshot_ = std::move(snapshot);
    const QString id = snapshot_->id;
    // Cancelled dialogs must not retain a live event subscription indefinitely.
    // This timer only releases a snapshot; it never scans or acts on windows.
    QTimer::singleShot(int(SnapshotLifetimeMs), cleanupContext_.get(), [this, id] {
        QMutexLocker locker(&xMutex);
        if (snapshot_ && snapshot_->id == id) snapshot_.reset();
    });
    return result;
#endif
}

QJsonObject ApplicationActions::close(const QJsonArray &identities, const QJsonArray &expectedWindows) const {
#ifndef Q_OS_LINUX
    Q_UNUSED(identities)
    Q_UNUSED(expectedWindows)
    return failure("unsupportedPlatform", T("正常关窗仅适用于本机 Linux X11 桌面。"));
#else
    QMutexLocker locker(&xMutex);
    auto snapshot = std::move(snapshot_); // Every attempt consumes confirmation.
    if (!snapshot || !snapshot->age.isValid() || snapshot->age.elapsed() > SnapshotLifetimeMs ||
        expectedWindows.isEmpty() || expectedWindows != snapshot->windows)
        return failure("confirmationExpired", T("关窗确认已过期或窗口选择已变化，请重新查看并确认。"));
    if (!localEnvironment() || qgetenv("DISPLAY") != snapshot->displayName)
        return failure("unsupportedSession", T("桌面会话已变化，请重新查看应用窗口。"));
    QMap<qint64, QJsonObject> freshProcesses;
    QJsonObject result = validateIdentities(identities, freshProcesses);
    if (!result.value("ok").toBool()) return result;
    if (freshProcesses != snapshot->processes)
        return failure("selectionChanged", T("进程选择或身份已变化，请重新查看并确认。"));
    XApi &x = snapshot->x;
    Display *display = snapshot->display;
    ErrorTrap errors(x, display);
    const Atoms atoms(x, display);
    x.XSync(display, False);
    if (invalidatedEvents(x, display))
        return failure("windowReplaced", T("确认期间窗口已销毁或变化过多，请重新确认窗口。"));
    // Confirmation UI and unrelated applications may create or remove windows.
    // Enumerate outside the grab and compare only selected process windows to
    // the confirmed tokens. Capture this fresh root list for the short atomic
    // validation below; never add newly discovered windows to the close batch.
    QJsonArray preflightWindows;
    QList<quint32> preflightClients;
    result = enumerate(x, display, atoms, snapshot->hostname, freshProcesses, snapshot->id,
                       preflightWindows, &preflightClients);
    if (!result.value("ok").toBool()) return result;
    if (preflightWindows != expectedWindows || !errors.synced())
        return failure("windowSetChanged", T("所选应用的窗口或标题已变化，未发送关窗请求，请重新确认。"));
    // Keep the grab limited to bounded X requests. No /proc reads, confirmation,
    // waits, DNS, file I/O or application response handling happen while held.
    QSet<quint32> confirmedIds;
    for (const auto &value : expectedWindows)
        confirmedIds.insert(value.toObject().value("windowId").toString().toUInt());
    ServerGrab grab(x, display);
    x.XSync(display, False);
    if (invalidatedEvents(x, display))
        return failure("windowReplaced", T("确认期间窗口已销毁或变化过多，请重新确认窗口。"));
    QJsonArray freshWindows;
    // Only the <=32 confirmed windows are re-read while grabbed. The root list
    // must still match the close preflight, closing the preflight-to-send race.
    result = enumerate(x, display, atoms, snapshot->hostname, freshProcesses, snapshot->id, freshWindows,
                       nullptr, &preflightClients, &confirmedIds);
    if (!result.value("ok").toBool()) return result;
    if (freshWindows != expectedWindows || !errors.synced())
        return failure("windowSetChanged", T("应用窗口或标题已变化，未发送关窗请求，请重新确认。"));
    // ICCCM 4.2.8.1: a WM_PROTOCOLS ClientMessage containing WM_DELETE_WINDOW.
    // Never XKillClient/XDestroyWindow, numeric-PID kill, or a signal fallback.
    // EWMH PID + WM_CLIENT_MACHINE are cooperative desktop hints, not an X11
    // security boundary. A hostile same-session X client can forge properties.
    int sent = 0;
    for (const auto &value : expectedWindows) {
        XEvent event{};
        event.xclient.type = ClientMessage;
        event.xclient.display = display;
        event.xclient.window = Window(value.toObject().value("windowId").toString().toULongLong());
        event.xclient.message_type = atoms.protocols;
        event.xclient.format = 32;
        event.xclient.data.l[0] = long(atoms.deleteWindow);
        event.xclient.data.l[1] = CurrentTime;
        if (!x.XSendEvent(display, event.xclient.window, False, NoEventMask, &event)) break;
        ++sent;
    }
    const bool delivered = errors.synced();
    if (!delivered || sent != expectedWindows.size()) {
        result = failure("deliveryFailed", T("部分关窗请求可能未送达，请在应用内检查窗口状态。"));
        result["requestedCount"] = sent;
        return result;
    }
    return {{"ok", true}, {"canClose", false}, {"code", "requested"},
            {"requestedCount", sent}, {"windowCount", sent},
            {"message", T("已请求正常关闭 %1 个窗口。请处理应用的保存提示；应用可能继续运行，内存释放情况以刷新后的采样为准。").arg(sent)}};
#endif
}
