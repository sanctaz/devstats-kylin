#include "process_network_sampler.h"
#include "i18n.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <limits>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef Q_OS_LINUX
#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <poll.h>
#include <sys/socket.h>
#endif

namespace {
constexpr int MaxProcesses = 4096, MaxFds = 32768, MaxConnections = 32768;
constexpr int MaxMessages = 65536, MaxDumpBytes = 8 * 1024 * 1024;
constexpr double BudgetSeconds = .200;
double monotonic() {
    struct timespec ts {};
    return ::clock_gettime(CLOCK_MONOTONIC, &ts) == 0 ? double(ts.tv_sec) + double(ts.tv_nsec) / 1e9 : 0;
}
QJsonValue unknown() { return QJsonValue(QJsonValue::Null); }
QString opaqueIdentity(const QString &identity) {
    return QStringLiteral("app:") + QString::fromLatin1(QCryptographicHash::hash(identity.toUtf8(), QCryptographicHash::Sha256).toHex());
}
QString displayName(QString name) {
    for (int i = 0; i < name.size(); ++i) if (name[i].category() == QChar::Other_Control) name[i] = QLatin1Char(' ');
    return name.left(128);
}
QByteArray readBounded(const QString &path, qint64 limit) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(limit) : QByteArray();
}
QString executable(const QString &base) {
    // LinuxSampler uses canonical executable paths. Re-resolve every time;
    // no path or process ownership assumptions survive between scans.
    char target[16384];
    const ssize_t length = ::readlink(QFile::encodeName(base + QStringLiteral("/exe")).constData(), target, sizeof(target));
    if (length <= 0 || size_t(length) == sizeof(target)) return {};
    QString path = QFile::decodeName(QByteArray::fromRawData(target, int(length)));
    if (path.endsWith(QStringLiteral(" (deleted)"))) path.chop(10);
    if (!QDir::isAbsolutePath(path)) return {};
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? path : canonical);
}
bool verify(const QString &base, qint64 pid, quint64 start, const QString &exe) {
    struct stat owner {};
    if (::stat(QFile::encodeName(base).constData(), &owner) || owner.st_uid != ::getuid()) return false;
    const QByteArray stat = readBounded(base + QStringLiteral("/stat"), 16384);
    const int first = stat.indexOf('('), last = stat.lastIndexOf(')');
    if (first < 1 || last <= first) return false;
    bool ok = false;
    if (stat.left(first).trimmed().toLongLong(&ok) != pid || !ok) return false;
    const QList<QByteArray> fields = stat.mid(last + 1).simplified().split(' ');
    if (fields.size() <= 19 || fields[19].toULongLong(&ok) != start || !ok) return false;
    return !exe.isEmpty() && executable(base) == exe;
}
QString socketKey(int family, quint64 inode, quint64 cookie) {
    return QString::number(family) + ':' + QString::number(inode) + ':' + QString::number(cookie);
}
#ifdef Q_OS_LINUX
bool loopback(int family, const void *address) {
    const auto *p = static_cast<const unsigned char *>(address);
    if (family == AF_INET) return p[0] == 127;
    static const unsigned char v6[16] = {0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1};
    static const unsigned char mapped[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    return std::memcmp(p, v6, 16) == 0 || (std::memcmp(p, mapped, 12) == 0 && p[12] == 127);
}
#endif
}

ProcessNetworkSampler::ProcessNetworkSampler(const QString &procRoot) : procRoot_(procRoot) {}

void ProcessNetworkSampler::reset() {
    previous_.clear(); processes_.clear(); previousTime_ = -1;
}

bool ProcessNetworkSampler::parseDatagram(const QByteArray &data, quint32 sequence, quint32 port,
                                         quint32 uid, Dump &dump, bool &done) {
#ifdef Q_OS_LINUX
    int offset = 0, messages = 0;
    while (offset < data.size()) {
        if (++messages > MaxMessages || data.size() - offset < int(sizeof(nlmsghdr))) return false;
        nlmsghdr header {};
        std::memcpy(&header, data.constData() + offset, sizeof(header));
        if (header.nlmsg_len < sizeof(header) || header.nlmsg_len > quint32(data.size() - offset) ||
            header.nlmsg_seq != sequence || header.nlmsg_pid != port || done) return false;
        const char *payload = data.constData() + offset + sizeof(header);
        const int length = int(header.nlmsg_len - sizeof(header));
        if (header.nlmsg_flags & NLM_F_DUMP_INTR) { dump.complete = false; dump.issues << QStringLiteral("TCP socket dump interrupted"); }
        if (header.nlmsg_type == NLMSG_DONE) {
            if (length > 0) {
                if (length < int(sizeof(qint32))) return false;
                qint32 error = 0; std::memcpy(&error, payload, sizeof(error));
                if (error) return false;
            }
            done = true;
        } else if (header.nlmsg_type == NLMSG_ERROR) {
            if (length < int(sizeof(nlmsgerr))) return false;
            nlmsgerr error {}; std::memcpy(&error, payload, sizeof(error));
            if (error.error) return false;
        } else if (header.nlmsg_type == SOCK_DIAG_BY_FAMILY) {
            if (length < int(sizeof(inet_diag_msg))) return false;
            inet_diag_msg message {}; std::memcpy(&message, payload, sizeof(message));
            if (message.idiag_family != AF_INET && message.idiag_family != AF_INET6) return false;
            const bool relevant = message.idiag_uid == uid && message.idiag_inode && message.idiag_state != 10 &&
                !loopback(message.idiag_family, message.id.idiag_src) && !loopback(message.idiag_family, message.id.idiag_dst);
            Connection connection;
            connection.family = message.idiag_family; connection.inode = message.idiag_inode;
            connection.cookie = quint64(message.id.idiag_cookie[0]) | (quint64(message.id.idiag_cookie[1]) << 32);
            // TCP_INFO is an append-only Linux UAPI. Both counters have these
            // byte offsets in Linux 4.19; require the actual returned length.
            // Avoid depending on the build machine's newer struct tcp_info.
            int at = NLMSG_ALIGN(sizeof(message));
            bool infoSeen = false;
            while (at < length) {
                if (length - at < int(sizeof(rtattr))) return false;
                rtattr attribute {}; std::memcpy(&attribute, payload + at, sizeof(attribute));
                if (attribute.rta_len < sizeof(attribute) || attribute.rta_len > length - at) return false;
                if ((attribute.rta_type & 0x3fff) == INET_DIAG_INFO) {
                    if (infoSeen) return false;
                    infoSeen = true;
                    if (attribute.rta_len >= sizeof(attribute) + 136) {
                        std::memcpy(&connection.acknowledged, payload + at + sizeof(attribute) + 120, sizeof(quint64));
                        std::memcpy(&connection.received, payload + at + sizeof(attribute) + 128, sizeof(quint64));
                        connection.counters = connection.cookie != std::numeric_limits<quint64>::max();
                    }
                }
                const int aligned = RTA_ALIGN(attribute.rta_len);
                if (aligned > length - at && attribute.rta_len != length - at) return false;
                at += aligned;
            }
            if (relevant) {
                if (dump.connections.size() >= MaxConnections) {
                    dump.complete = false; dump.limited = true;
                    dump.issues << QStringLiteral("TCP connection limit reached");
                    return false;
                }
                dump.connections.append(connection);
            }
        } else if (header.nlmsg_type != NLMSG_NOOP) return false;
        const int aligned = NLMSG_ALIGN(header.nlmsg_len);
        if (aligned > data.size() - offset && int(header.nlmsg_len) != data.size() - offset) return false;
        offset += aligned;
    }
    return true;
#else
    Q_UNUSED(data) Q_UNUSED(sequence) Q_UNUSED(port) Q_UNUSED(uid) Q_UNUSED(dump) Q_UNUSED(done)
    return false;
#endif
}

ProcessNetworkSampler::Dump ProcessNetworkSampler::collect(double deadline) {
    Dump dump;
#ifdef Q_OS_LINUX
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_SOCK_DIAG);
    if (fd < 0) { dump.available = dump.complete = false; dump.issues << QStringLiteral("TCP socket diagnostics unavailable or permission denied"); return dump; }
    sockaddr_nl local {}; local.nl_family = AF_NETLINK;
    socklen_t localLength = sizeof(local);
    if (::bind(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) ||
        ::getsockname(fd, reinterpret_cast<sockaddr *>(&local), &localLength)) {
        ::close(fd); dump.available = dump.complete = false; dump.issues << QStringLiteral("Cannot bind TCP socket diagnostics"); return dump;
    }
    const quint32 sequence = quint32(QDateTime::currentMSecsSinceEpoch()) ^ quint32(::getpid());
    int totalBytes = 0, datagrams = 0, completed = 0;
    for (int family : {AF_INET, AF_INET6}) {
        if (monotonic() >= deadline) { dump.complete = false; dump.limited = true; break; }
        struct Request { nlmsghdr header; inet_diag_req_v2 request; } request {};
        request.header.nlmsg_len = sizeof(request); request.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
        request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
        request.header.nlmsg_seq = sequence + quint32(family);
        request.header.nlmsg_pid = local.nl_pid;
        request.request.sdiag_family = family; request.request.sdiag_protocol = IPPROTO_TCP;
        request.request.idiag_ext = 1 << (INET_DIAG_INFO - 1);
        request.request.idiag_states = ~quint32(1 << 10); // listeners carry no application payload
        request.request.id.idiag_cookie[0] = request.request.id.idiag_cookie[1] = ~quint32(0);
        sockaddr_nl kernel {}; kernel.nl_family = AF_NETLINK;
        bool done = false, failed = false;
        if (::sendto(fd, &request, sizeof(request), MSG_DONTWAIT, reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) != sizeof(request)) failed = true;
        while (!failed && !done) {
            const double remaining = deadline - monotonic();
            if (remaining <= 0) { dump.limited = true; failed = true; break; }
            pollfd wait {fd, POLLIN, 0};
            const int ready = ::poll(&wait, 1, qMax(1, int(std::ceil(remaining * 1000))));
            if (ready < 0 && errno == EINTR) continue;
            if (ready <= 0 || (wait.revents & (POLLERR | POLLHUP | POLLNVAL))) { failed = true; if (!ready) dump.limited = true; break; }
            char buffer[65536]; sockaddr_nl sender {}; iovec part {buffer, sizeof(buffer)};
            msghdr message {}; message.msg_name = &sender; message.msg_namelen = sizeof(sender);
            message.msg_iov = &part; message.msg_iovlen = 1;
            const ssize_t received = ::recvmsg(fd, &message, MSG_DONTWAIT);
            if (received < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (received <= 0 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
                message.msg_namelen != sizeof(sender) || sender.nl_family != AF_NETLINK || sender.nl_pid != 0 || sender.nl_groups != 0) { failed = true; break; }
            totalBytes += int(received);
            if (++datagrams > MaxMessages || totalBytes > MaxDumpBytes) { dump.limited = true; failed = true; break; }
            if (!parseDatagram(QByteArray(buffer, int(received)), request.header.nlmsg_seq, local.nl_pid, quint32(::getuid()), dump, done)) failed = true;
        }
        if (failed || !done) {
            dump.complete = false;
            dump.issues << QStringLiteral("TCP IPv%1 socket dump incomplete, unsupported, or restricted").arg(family == AF_INET ? 4 : 6);
            // A failed multipart dump cannot be followed on the same socket:
            // queued messages would belong to the preceding request.
            break;
        }
        ++completed;
    }
    ::close(fd);
    if (!completed && dump.connections.isEmpty()) dump.available = false;
    if (dump.limited) dump.issues << QStringLiteral("Sampling time or socket limit reached");
#else
    Q_UNUSED(deadline)
    dump.available = dump.complete = false;
    dump.issues << QStringLiteral("TCP socket diagnostics require Linux");
#endif
    return dump;
}

QJsonObject ProcessNetworkSampler::sample(const QJsonObject &freshSnapshot) {
    const double now = monotonic(), deadline = now + BudgetSeconds;
    Dump dump;
    if (procRoot_ == QStringLiteral("/proc")) dump = collect(deadline);
    else {
        dump.available = dump.complete = false;
        dump.issues << T("自定义进程目录不与当前主机的 TCP 连接混合采样。");
    }
    return sampleDump(freshSnapshot, dump, now, QDateTime::currentSecsSinceEpoch(), deadline);
}

QJsonObject ProcessNetworkSampler::sampleDump(const QJsonObject &snapshot, const Dump &dump,
                                              double now, qint64 sampledAt, double deadline) {
    QStringList coverage {
        T("当前用户的 IPv4/IPv6 TCP：下载为已接收的 TCP 载荷，上传为 TCP 已确认字节，均非网卡线速。"),
        T("不含 UDP/QUIC、回环地址和其他用户；代理/VPN 流量可能归属于代理应用。"),
        T("仅比较两次采样都存在的连接；短连接、退出连接或退出进程可能漏计，不能与接口总量对齐。"),
        T("同应用共享连接仅计一次；共享进程或跨应用共享连接不强行分摊。")
    };
    coverage.append(dump.issues);
    const double interval = previousTime_ >= 0 && now > previousTime_ ? now - previousTime_ : 0;
    QJsonObject result {{"state", "starting"}, {"sampledAt", double(sampledAt)}, {"monotonicSeconds", now},
        {"intervalSeconds", interval > 0 ? QJsonValue(interval) : unknown()}, {"applications", QJsonArray()},
        {"coverage", QJsonArray::fromStringList(coverage)}, {"scannedProcessCount", 0},
        {"unattributedConnectionCount", 0}, {"limited", dump.limited}};
    if (!dump.available) { reset(); result["state"] = "unavailable"; return result; }
    if (snapshot.contains("processesFresh") && !snapshot.value("processesFresh").toBool()) {
        reset(); result["state"] = "partial";
        coverage << T("本轮进程快照未刷新，无法安全归属连接。");
        result["coverage"] = QJsonArray::fromStringList(coverage); return result;
    }
    struct Process {
        qint64 pid = 0; quint64 start = 0;
        QString key, application, name, exe;
        QSet<quint64> inodes;
        QSet<QString> sockets;
        bool complete = true, partial = false, shared = false, measured = false;
        double download = 0, upload = 0;
        int connections = 0;
    };
    struct Application {
        QString name; QVector<int> processes;
        QSet<QString> sockets;
        bool partial = false, measured = false;
        double download = 0, upload = 0;
    };
    QVector<Process> processes;
    QHash<QString, Application> apps;
    QHash<QString, QString> names;
    int metadataCount = 0;
    for (const auto &value : snapshot.value("activityApplications").toArray()) {
        if (++metadataCount > MaxProcesses) break;
        const auto app = value.toObject();
        names.insert(app.value("identity").toString(), displayName(app.value("name").toString()));
    }
    QHash<quint64, QVector<int>> owners;
    QSet<qint64> seenPids;
    bool limited = dump.limited, scanComplete = true, unreadable = false;
    int fdCount = 0, candidates = 0, scanned = 0;
    const QJsonArray input = snapshot.value("processes").toArray();
    for (const auto &value : input) {
        if (++candidates > MaxProcesses || monotonic() >= deadline) { limited = true; scanComplete = false; break; }
        const auto row = value.toObject();
        Process process;
        process.pid = qint64(row.value("pid").toDouble()); process.start = quint64(row.value("startTicks").toDouble());
        process.application = row.value("applicationIdentity").toString();
        process.exe = row.value("executablePath").toString(); process.name = displayName(row.value("name").toString());
        if (process.pid <= 0 || !process.start || process.application.isEmpty() || seenPids.contains(process.pid)) continue;
        seenPids.insert(process.pid);
        process.key = QString::number(process.pid) + ':' + QString::number(process.start);
        const QString base = procRoot_ + '/' + QString::number(process.pid);
        auto &app = apps[process.application];
        if (app.name.isEmpty()) app.name = names.value(process.application, process.name);
        ++scanned;
        if (!verify(base, process.pid, process.start, process.exe)) process.complete = false;
        else {
            DIR *directory = ::opendir(QFile::encodeName(base + QStringLiteral("/fd")).constData());
            if (!directory) process.complete = false;
            else {
                errno = 0;
                while (dirent *entry = ::readdir(directory)) {
                    if (entry->d_name[0] == '.') continue;
                    if (fdCount >= MaxFds || monotonic() >= deadline) { process.complete = false; limited = true; scanComplete = false; break; }
                    ++fdCount;
                    char link[128];
                    const ssize_t length = ::readlinkat(::dirfd(directory), entry->d_name, link, sizeof(link));
                    if (length < 0) { process.complete = false; errno = 0; continue; }
                    const QByteArray target(link, int(length));
                    if (!target.startsWith("socket:[") || !target.endsWith(']')) continue;
                    bool valid = false;
                    const quint64 inode = target.mid(8, target.size() - 9).toULongLong(&valid);
                    if (valid && inode) process.inodes.insert(inode);
                    errno = 0;
                }
                if (errno) process.complete = false;
                ::closedir(directory);
            }
            // Discard every FD if the process exited, exec'd, or the PID was reused.
            if (!verify(base, process.pid, process.start, process.exe)) { process.inodes.clear(); process.complete = false; }
        }
        const int index = processes.size();
        for (quint64 inode : process.inodes) owners[inode].append(index);
        app.processes.append(index);
        if (!process.complete) { app.partial = true; unreadable = true; }
        processes.append(process);
        if (fdCount >= MaxFds) { limited = true; scanComplete = false; break; }
    }
    QHash<QString, Previous> current;
    int unattributed = 0;
    bool anyPartial = !dump.complete || !scanComplete;
    QSet<QString> seenConnections;
    for (const Connection &connection : dump.connections) {
        const QString key = socketKey(connection.family, connection.inode, connection.cookie);
        if (seenConnections.contains(key)) continue;
        seenConnections.insert(key);
        const auto owning = owners.value(connection.inode);
        QSet<QString> applications, processKeys;
        for (int index : owning) { applications.insert(processes[index].application); processKeys.insert(processes[index].key); }
        if (owning.isEmpty()) { ++unattributed; anyPartial = true; continue; }
        for (int index : owning) {
            Process &process = processes[index];
            ++process.connections; process.sockets.insert(key);
            if (owning.size() > 1) process.shared = true;
        }
        if (applications.size() != 1) {
            ++unattributed; anyPartial = true;
            for (int index : owning) processes[index].partial = true;
            for (const QString &identity : applications) { apps[identity].partial = true; apps[identity].sockets.insert(key); }
            continue;
        }
        const QString identity = *applications.cbegin();
        Application &app = apps[identity]; app.sockets.insert(key);
        const auto old = previous_.constFind(key);
        bool stableProcesses = true;
        for (int index : owning) {
            const Process &process = processes[index];
            const auto prior = processes_.constFind(process.key);
            if (prior == processes_.cend() || prior->application != identity || prior->executable != process.exe) stableProcesses = false;
        }
        const bool measured = interval > 0 && connection.counters && old != previous_.cend() &&
            stableProcesses && old->application == identity && old->counters.counters &&
            connection.received >= old->counters.received && connection.acknowledged >= old->counters.acknowledged;
        if (!measured) {
            app.partial = true;
            for (int index : owning) processes[index].partial = true;
        }
        else {
            const double download = double(connection.received - old->counters.received) / interval;
            const double upload = double(connection.acknowledged - old->counters.acknowledged) / interval;
            app.download += download; app.upload += upload; app.measured = true;
            // Stable ownership is needed even for a connection that survives an exec.
            if (old->owners != processKeys) app.partial = true;
            for (int index : owning) {
                Process &process = processes[index];
                if (owning.size() == 1 && old->owners.size() == 1 && old->owners.contains(process.key)) {
                    process.download += download; process.upload += upload; process.measured = true;
                } else process.partial = true;
            }
        }
        current.insert(key, {connection, identity, processKeys});
    }
    QHash<QString, ProcessBaseline> processBaselines;
    for (Process &process : processes) {
        const auto old = processes_.constFind(process.key);
        const bool stable = interval > 0 && old != processes_.cend() && old->application == process.application && old->executable == process.exe;
        if (!stable || !old->complete || !process.complete || (stable && old->sockets != process.sockets)) {
            apps[process.application].partial = true; process.partial = true;
        }
        // A process without TCP sockets is measured zero only after two complete scans.
        if (stable && old->complete && process.complete && old->sockets.isEmpty() && process.sockets.isEmpty() && dump.complete && scanComplete) process.measured = true;
        if (process.shared) process.partial = true;
        processBaselines.insert(process.key, {process.application, process.exe, process.sockets, process.complete && dump.complete && scanComplete});
    }
    // An exited member or socket cannot contribute its final counter delta.
    for (auto it = processes_.cbegin(); it != processes_.cend(); ++it)
        if (!processBaselines.contains(it.key()) && apps.contains(it->application)) apps[it->application].partial = true;
    for (auto it = previous_.cbegin(); it != previous_.cend(); ++it)
        if (!current.contains(it.key()) && apps.contains(it->application)) apps[it->application].partial = true;

    QVector<QJsonObject> output;
    for (auto it = apps.begin(); it != apps.end(); ++it) {
        Application &app = it.value();
        if (!dump.complete || !scanComplete) app.partial = true;
        bool allZero = !app.processes.isEmpty();
        QJsonArray members;
        for (int index : app.processes) {
            const Process &process = processes[index];
            if (!process.measured || !process.sockets.isEmpty()) allZero = false;
            if (members.size() >= 100) { app.partial = true; limited = true; continue; }
            members.append(QJsonObject {{"pid", double(process.pid)}, {"startTicks", double(process.start)}, {"name", process.name},
                {"downloadBytesPerSecond", process.measured ? QJsonValue(process.download) : unknown()},
                {"uploadBytesPerSecond", process.measured ? QJsonValue(process.upload) : unknown()},
                {"connectionCount", process.connections}, {"shared", process.shared},
                {"partial", process.partial || !process.complete || !process.measured || !dump.complete || !scanComplete}});
        }
        if (allZero && !app.partial) app.measured = true;
        if (app.partial) anyPartial = true;
        output.append(QJsonObject {{"identity", opaqueIdentity(it.key())}, {"name", app.name},
            {"processCount", app.processes.size()}, {"connectionCount", app.sockets.size()},
            {"downloadBytesPerSecond", app.measured ? QJsonValue(app.download) : unknown()},
            {"uploadBytesPerSecond", app.measured ? QJsonValue(app.upload) : unknown()},
            {"partial", app.partial}, {"processes", members}});
    }
    const auto ranked = [](const QString &first, const QString &second) {
      return [first, second](const QJsonObject &a, const QJsonObject &b) {
        for (const QString &key : {first, second}) {
            const double left = a.value(key).toDouble(-1), right = b.value(key).toDouble(-1);
            if (left != right) return left > right;
        }
        if ((a.value("connectionCount").toInt() > 0) != (b.value("connectionCount").toInt() > 0))
            return a.value("connectionCount").toInt() > 0;
        return a.value("identity").toString() < b.value("identity").toString();
      };
    };
    std::sort(output.begin(), output.end(), ranked(QStringLiteral("downloadBytesPerSecond"), QStringLiteral("uploadBytesPerSecond")));
    QJsonArray applications;
    QSet<QString> selected;
    const auto appendLeaders = [&]() {
        for (int i = 0; i < qMin(30, output.size()); ++i) {
            const auto &app = output[i]; const QString identity = app.value("identity").toString();
            if (selected.contains(identity)) continue;
            selected.insert(identity); applications.append(app);
        }
    };
    appendLeaders();
    std::sort(output.begin(), output.end(), ranked(QStringLiteral("uploadBytesPerSecond"), QStringLiteral("downloadBytesPerSecond")));
    appendLeaders(); // Union of both directions' leaders; at most 60 rows.
    if (applications.size() < output.size()) { limited = true; anyPartial = true; }
    if (limited) coverage << T("已达到本轮时间、进程、文件描述符或展示数量上限，结果仅覆盖已测部分。");
    if (unreadable) coverage << T("部分进程已退出、身份改变或权限受限，无法完整读取其连接归属。");
    if (unattributed) coverage << T("部分当前用户 TCP 连接无法安全归属到可读取的应用。");
    result["state"] = (!dump.complete || !scanComplete || limited) ? "partial" :
        (interval <= 0 ? "starting" : (anyPartial ? "partial" : "ready"));
    result["applications"] = applications; result["coverage"] = QJsonArray::fromStringList(coverage);
    result["scannedProcessCount"] = scanned; result["unattributedConnectionCount"] = unattributed; result["limited"] = limited;
    previous_ = std::move(current); processes_ = std::move(processBaselines); previousTime_ = now;
    return result;
}

QJsonObject ProcessNetworkSampler::retainedObservation(const QJsonObject &current, const QJsonObject &previous) {
    const QString state = current.value("state").toString();
    bool measured = state == "ready"; // Measured empty clears an older ranking.
    if (state == "partial") for (const auto &value : current.value("applications").toArray()) {
        const auto app = value.toObject();
        for (const auto &key : {"downloadBytesPerSecond", "uploadBytesPerSecond"}) {
            const auto rate = app.value(key);
            measured |= rate.isDouble() && std::isfinite(rate.toDouble()) && rate.toDouble() >= 0;
        }
    }
    const auto at = current.value("sampledAt");
    if (!measured || !at.isDouble() || !std::isfinite(at.toDouble()) || at.toDouble() <= 0) return previous;
    auto result = current;
    // The retained value is one bare observation, never a published cache chain.
    result.remove("previous"); result.remove("networkApplicationsPrevious");
    return result;
}
