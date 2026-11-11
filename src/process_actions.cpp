#include "process_actions.h"
#include "i18n.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QStringList>
#include <cerrno>
#include <climits>
#include <cmath>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

namespace {
struct Descriptor {
    int value = -1;
    explicit Descriptor(int fd = -1) : value(fd) {}
    ~Descriptor() { if (value >= 0) ::close(value); }
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
};

struct Identity {
    pid_t pid = 0;
    quint64 start = 0;
    QString executable;
};

struct Process {
    Identity identity;
    pid_t parent = 0;
    uid_t realUid = uid_t(-1), effectiveUid = uid_t(-1), savedUid = uid_t(-1), fsUid = uid_t(-1);
    uid_t directoryUid = uid_t(-1);
    bool executableTrusted = false;
    char state = 0;
};

QJsonObject failure(const QString &code, const QString &message) {
    return {{"ok", false}, {"code", code}, {"message", message}, {"canTerminate", false}};
}

bool jsonInteger(const QJsonValue &value, double minimum, double maximum, quint64 &result) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    if (!std::isfinite(number) || number < minimum || number > maximum || std::floor(number) != number)
        return false;
    result = quint64(number);
    return true;
}

bool parseIdentity(const QJsonObject &json, Identity &identity) {
    quint64 pid = 0;
    if (!jsonInteger(json.value("pid"), 2, INT_MAX, pid) ||
        !jsonInteger(json.value("startTicks"), 1, 9007199254740991.0, identity.start)) return false;
    identity.pid = pid_t(pid);
    identity.executable = json.value("executablePath").toString();
    return !identity.executable.isEmpty() && QDir::isAbsolutePath(identity.executable) &&
           !identity.executable.contains(QChar::Null) &&
           identity.executable == QDir::cleanPath(identity.executable) &&
           !identity.executable.endsWith(QStringLiteral(" (deleted)"));
}

int openProcess(pid_t pid) {
    const QByteArray path = "/proc/" + QByteArray::number(pid);
    return ::open(path.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

QByteArray readAt(int directory, const char *name) {
    Descriptor file(::openat(directory, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (file.value < 0) return {};
    QByteArray result;
    char buffer[4096];
    while (result.size() < 65536) {
        const ssize_t size = ::read(file.value, buffer, sizeof(buffer));
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) return size == 0 ? result : QByteArray();
        result.append(buffer, int(size));
    }
    return {};
}

bool parseStat(const QByteArray &stat, Process &process) {
    const int first = stat.indexOf('('), last = stat.lastIndexOf(')');
    if (first < 1 || last <= first) return false;
    bool valid = false;
    const qint64 pid = stat.left(first).trimmed().toLongLong(&valid);
    if (!valid || pid <= 0 || pid > INT_MAX) return false;
    const auto fields = stat.mid(last + 1).simplified().split(' ');
    if (fields.size() < 20 || fields[0].size() != 1) return false;
    const qint64 parent = fields[1].toLongLong(&valid);
    if (!valid || parent < 0 || parent > INT_MAX) return false;
    const quint64 start = fields[19].toULongLong(&valid);
    if (!valid || start == 0) return false;
    process.identity.pid = pid_t(pid);
    process.identity.start = start;
    process.parent = pid_t(parent);
    process.state = fields[0][0];
    return true;
}

bool parseStatus(const QByteArray &status, Process &process) {
    bool hasUid = false, hasTgid = false;
    for (const auto &line : status.split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.value(0) == "Uid:") {
            if (fields.size() != 5 || hasUid) return false;
            uid_t *uids[] = {&process.realUid, &process.effectiveUid, &process.savedUid, &process.fsUid};
            for (int i = 0; i < 4; ++i) {
                bool valid = false;
                const qulonglong uid = fields[i + 1].toULongLong(&valid);
                if (!valid || uid > UINT_MAX) return false;
                *uids[i] = uid_t(uid);
            }
            hasUid = true;
        } else if (fields.value(0) == "Tgid:") {
            bool valid = false;
            const qint64 tgid = fields.value(1).toLongLong(&valid);
            if (fields.size() != 2 || hasTgid || !valid || tgid != process.identity.pid) return false;
            hasTgid = true;
        }
    }
    return hasUid && hasTgid;
}

bool readProcess(int directory, Process &process) {
    struct stat directoryStat{}, executableStat{};
    if (::fstat(directory, &directoryStat) != 0 || !S_ISDIR(directoryStat.st_mode) ||
        !parseStat(readAt(directory, "stat"), process) ||
        !parseStatus(readAt(directory, "status"), process)) return false;
    process.directoryUid = directoryStat.st_uid;
    char path[16384];
    const ssize_t size = ::readlinkat(directory, "exe", path, sizeof(path));
    if (size <= 0 || size >= ssize_t(sizeof(path))) return false;
    process.identity.executable = QFile::decodeName(QByteArray(path, int(size)));
    if (!QDir::isAbsolutePath(process.identity.executable) ||
        process.identity.executable.endsWith(QStringLiteral(" (deleted)"))) return false;
    process.identity.executable = QDir::cleanPath(process.identity.executable);
    if (::fstatat(directory, "exe", &executableStat, 0) != 0 || !S_ISREG(executableStat.st_mode))
        return false;
    process.executableTrusted = executableStat.st_uid == 0 &&
                               !(executableStat.st_mode & (S_IWGRP | S_IWOTH));
    Process after;
    return parseStat(readAt(directory, "stat"), after) &&
           after.identity.pid == process.identity.pid && after.identity.start == process.identity.start &&
           after.parent == process.parent;
}

bool systemExecutable(const Process &process) {
    if (!process.executableTrusted) return false;
    const QString path = process.identity.executable;
    const QStringList roots{QStringLiteral("/bin/"), QStringLiteral("/sbin/"),
                            QStringLiteral("/usr/bin/"), QStringLiteral("/usr/sbin/"),
                            QStringLiteral("/usr/lib/"), QStringLiteral("/usr/libexec/"),
                            QStringLiteral("/lib/"), QStringLiteral("/libexec/")};
    for (const auto &root : roots) if (path.startsWith(root)) return true;
    return false;
}

bool importantSession(const Process &process) {
    if (!systemExecutable(process)) return false;
    const QString name = QFileInfo(process.identity.executable).fileName();
    static const QSet<QString> names{
        "sh", "bash", "dash", "zsh", "fish", "ksh", "mksh", "csh", "tcsh", "login", "sshd",
        "systemd", "init", "dbus-daemon", "dbus-broker", "dbus-broker-launch", "dbus-launch",
        "Xorg", "Xwayland", "weston", "gnome-shell", "gnome-session", "gnome-session-binary",
        "gnome-settings-daemon", "gnome-keyring-daemon", "gnome-screensaver", "mate-screensaver",
        "mate-session", "xfce4-session", "xfwm4", "xfce4-panel", "xfsettingsd", "xfconfd", "xfce4-power-manager",
        "startplasma-x11", "startplasma-wayland", "plasmashell", "kwin_x11", "kwin_wayland",
        "ukui-session", "ukui-session-manager", "ukui-panel", "ukui-menu", "ukui-settings-daemon",
        "ukui-power-manager", "ukui-window-switch", "ukwm", "marco", "peony", "caja", "nautilus", "dconf-service",
        "ibus-daemon", "fcitx", "fcitx5", "nm-applet", "NetworkManager", "pulseaudio", "pipewire",
        "wireplumber", "ssh-agent", "gpg-agent", "ksmserver", "kded5", "kdeinit5", "klauncher",
        "kscreenlocker_greet", "at-spi-bus-launcher", "at-spi2-registryd", "polkitd", "polkit-gnome-authentication-agent-1",
        "ukui-polkit", "mate-polkit", "lightdm", "sddm", "gdm", "gdm3"
    };
    return names.contains(name) || name.startsWith(QStringLiteral("gsd-")) ||
           name.startsWith(QStringLiteral("ukui-"));
}

bool ownedBy(const Process &process, uid_t uid) {
    return process.directoryUid == uid && process.realUid == uid && process.effectiveUid == uid &&
           process.savedUid == uid && process.fsUid == uid;
}

// A failed or cyclic ancestry check is refused instead of guessing about safety.
bool ancestry(pid_t pid, QSet<pid_t> &result) {
    for (int depth = 0; pid > 1 && depth < 1024; ++depth) {
        if (result.contains(pid)) return false;
        result.insert(pid);
        Descriptor directory(openProcess(pid));
        Process process;
        if (directory.value < 0 || !parseStat(readAt(directory.value, "stat"), process) ||
            process.identity.pid != pid || process.parent == 0) return false;
        pid = process.parent;
    }
    return pid == 1;
}

QJsonObject validate(const Identity &expected, const Process &process) {
    if (expected.pid != process.identity.pid || expected.start != process.identity.start)
        return failure("staleIdentity", T("进程已退出或 PID 已被复用，请刷新后重试。"));
    if (expected.executable != process.identity.executable)
        return failure("executableChanged", T("进程程序已变化，请刷新后重新选择。"));
    if (process.state == 'Z' || process.state == 'X' || process.state == 'x')
        return failure("exited", T("进程已退出，无需请求结束。"));
    if (!ownedBy(process, ::getuid()))
        return failure("notOwned", T("只能操作当前普通用户拥有且未提权的进程。"));
    const pid_t self = ::getpid();
    QSet<pid_t> ownAncestors, targetAncestors;
    if (!ancestry(self, ownAncestors) || !ancestry(process.identity.pid, targetAncestors))
        return failure("ancestryUnavailable", T("无法确认进程关系，为保护会话已禁用此操作。"));
    if (ownAncestors.contains(process.identity.pid) || targetAncestors.contains(self))
        return failure("protectedProcess", T("监控程序、其辅助进程及上级会话进程受到保护。"));
    Descriptor ownDirectory(openProcess(self));
    Process own;
    if (ownDirectory.value < 0 || !readProcess(ownDirectory.value, own))
        return failure("identityUnavailable", T("无法确认监控程序身份，已禁用此操作。"));
    const QString name = QFileInfo(process.identity.executable).fileName();
    const bool monitorSibling = QFileInfo(process.identity.executable).absolutePath() ==
                                QFileInfo(own.identity.executable).absolutePath() &&
                                name == "devstats";
    if (process.identity.executable == own.identity.executable || monitorSibling || importantSession(process))
        return failure("protectedProcess", T("监控、桌面、终端或会话基础进程受到保护，请使用系统监视器查看。"));
    return {{"ok", true}, {"canTerminate", false}, {"pid", double(expected.pid)},
            {"startTicks", double(expected.start)}, {"executablePath", expected.executable}};
}

int signalDescriptor(int descriptor, int signal) {
#if defined(SYS_pidfd_send_signal)
    return int(::syscall(SYS_pidfd_send_signal, descriptor, signal, nullptr, 0U));
#elif defined(__NR_pidfd_send_signal)
    return int(::syscall(__NR_pidfd_send_signal, descriptor, signal, nullptr, 0U));
#else
    Q_UNUSED(descriptor)
    Q_UNUSED(signal)
    errno = ENOSYS;
    return -1;
#endif
}

QJsonObject signalFailure(int error) {
    if (error == ENOSYS || error == EINVAL || error == EBADF || error == ENODEV)
        return failure("unsupportedKernel", T("当前内核不支持安全的进程句柄操作，无法请求结束。请使用系统监视器。"));
    if (error == EPERM || error == EACCES)
        return failure("permissionDenied", T("系统策略不允许操作此进程，请使用系统监视器查看。"));
    if (error == ESRCH || error == ENOENT)
        return failure("exited", T("进程已退出，请刷新列表。"));
    return failure("operationFailed", T("无法安全操作此进程（系统错误 %1）。").arg(error));
}

// signalNumber 0 runs every check and the capability probe without signalling.
QJsonObject operate(const QJsonObject &json, int signalNumber) {
    Identity expected;
    if (!parseIdentity(json, expected))
        return failure("invalidIdentity", T("缺少有效的进程身份，请刷新后重新选择。"));
    if (::getuid() == 0 || ::geteuid() != ::getuid())
        return failure("privilegedMonitor", T("此操作仅供未提权的普通用户使用，请以普通用户运行 DevStats。"));
    // One proc directory pins the identity for every openat/readlinkat and the
    // final signal. pidfd_send_signal(2) explicitly supports /proc/PID fds:
    // https://man7.org/linux/man-pages/man2/pidfd_send_signal.2.html
    // Unlike kill(PID), it cannot signal a different process after PID reuse.
    // Never replace this with a numeric-PID fallback on pre-5.1 kernels.
    Descriptor directory(openProcess(expected.pid));
    if (directory.value < 0) return signalFailure(errno);
    Process process;
    if (!readProcess(directory.value, process))
        return failure("identityUnavailable", T("进程已退出或身份不可读取，请刷新列表。"));
    QJsonObject result = validate(expected, process);
    if (!result.value("ok").toBool()) return result;
    if (signalDescriptor(directory.value, 0) != 0) return signalFailure(errno);
    // Refresh ownership, executable and lineage after the capability probe and
    // directly before sending the one user-requested signal to the same fd.
    Process fresh;
    if (!readProcess(directory.value, fresh))
        return failure("identityUnavailable", T("进程身份已变化，请刷新后重新选择。"));
    result = validate(expected, fresh);
    if (!result.value("ok").toBool()) return result;
    if (signalNumber && signalDescriptor(directory.value, signalNumber) != 0) return signalFailure(errno);
    result["canTerminate"] = true;
    result["code"] = signalNumber == SIGKILL ? "killed" : signalNumber ? "requested" : "ready";
    result["message"] = signalNumber == SIGKILL ? T("已强制结束所选进程（SIGKILL）。未保存的内容已丢失。")
                        : signalNumber ? T("已向所选进程发送结束请求（SIGTERM）。程序可能需要时间退出，也可能拒绝请求。")
                                       : T("可结束此进程；未保存的数据和正在运行的任务可能丢失。");
    if (signalNumber) result["signal"] = signalNumber;
    return result;
}
}

QJsonObject ProcessActions::inspectIdentity(const QJsonObject &identity) const {
    Identity expected;
    if (!parseIdentity(identity, expected))
        return failure("invalidIdentity", T("缺少有效的进程身份，请刷新后重新选择。"));
    if (::getuid() == 0 || ::geteuid() != ::getuid())
        return failure("privilegedMonitor", T("此操作仅供未提权的普通用户使用，请以普通用户运行 DevStats。"));
    Descriptor directory(openProcess(expected.pid));
    Process process;
    if (directory.value < 0 || !readProcess(directory.value, process))
        return failure("identityUnavailable", T("进程已退出或身份不可读取，请刷新列表。"));
    QJsonObject result = validate(expected, process);
    if (result.value("ok").toBool()) {
        result["code"] = "identityReady";
        result["message"] = T("已核对当前普通用户进程身份及会话保护规则。");
    }
    return result;
}

QJsonObject ProcessActions::inspect(const QJsonObject &identity) const {
    return operate(identity, 0);
}

QJsonObject ProcessActions::execute(const QJsonObject &identity, const QString &action) const {
    if (action == QStringLiteral("terminate")) return operate(identity, SIGTERM);
    if (action == QStringLiteral("kill")) return operate(identity, SIGKILL);
    return failure("unsupportedAction", T("不支持此进程操作。"));
}

bool ProcessActions::isRunning(const QJsonObject &identity) const {
    Identity expected;
    if (!parseIdentity(identity, expected)) return false;
    Descriptor directory(openProcess(expected.pid));
    Process process;
    if (directory.value < 0 || !parseStat(readAt(directory.value, "stat"), process)) return false;
    return process.identity.start == expected.start && process.state != 'Z' && process.state != 'X' && process.state != 'x';
}
