#include "../src/process_network_sampler.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {
void check(bool success, const char *description) {
    if (!success) { std::fprintf(stderr, "FAIL: %s\n", description); std::exit(1); }
}
double clockSeconds(clockid_t id = CLOCK_MONOTONIC) {
    timespec ts {}; check(::clock_gettime(id, &ts) == 0, "read test clock");
    return double(ts.tv_sec) + double(ts.tv_nsec) / 1e9;
}
void writeFile(const QString &path, const QByteArray &bytes) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "make fixture directory");
    QFile file(path); check(file.open(QIODevice::WriteOnly | QIODevice::Truncate), "open fixture file");
    check(file.write(bytes) == bytes.size(), "write fixture file");
}
void link(const QString &target, const QString &path) {
    check(QDir().mkpath(QFileInfo(path).absolutePath()), "make link directory");
    QFile::remove(path);
    check(::symlink(QFile::encodeName(target).constData(), QFile::encodeName(path).constData()) == 0, "make fixture symlink");
}
QByteArray statText(int pid, int start) {
    QList<QByteArray> fields;
    for (int i = 3; i <= 24; ++i) fields.append("0");
    fields[0] = "S"; fields[19] = QByteArray::number(start);
    return QByteArray::number(pid) + " (test (worker)) " + fields.join(' ') + '\n';
}
QJsonObject process(const QString &proc, int pid, int start, const QString &app, const QString &exe,
                    const QVector<quint64> &inodes = {}) {
    const QString base = proc + '/' + QString::number(pid);
    writeFile(base + "/stat", statText(pid, start)); link(exe, base + "/exe");
    QDir().mkpath(base + "/fd");
    int fd = 3;
    for (quint64 inode : inodes) link("socket:[" + QString::number(inode) + ']', base + "/fd/" + QString::number(fd++));
    return {{"pid", pid}, {"startTicks", start}, {"applicationIdentity", app}, {"name", app + " worker"}, {"executablePath", exe}};
}
QJsonObject snapshot(const QJsonArray &processes) {
    return {{"processesFresh", true}, {"processes", processes},
            {"activityApplications", QJsonArray {QJsonObject {{"identity", "A"}, {"name", "Alpha"}},
                                                QJsonObject {{"identity", "B"}, {"name", "Beta"}}}}};
}
QJsonObject appByName(const QJsonObject &result, const QString &name) {
    for (const auto &value : result.value("applications").toArray()) if (value.toObject().value("name").toString() == name) return value.toObject();
    return {};
}
QJsonObject member(const QJsonObject &app, int pid) {
    for (const auto &value : app.value("processes").toArray()) if (value.toObject().value("pid").toInt() == pid) return value.toObject();
    return {};
}
QByteArray message(quint16 type, const QByteArray &payload, quint16 flags = NLM_F_MULTI, quint32 sequence = 91, quint32 port = 77) {
    nlmsghdr header {}; header.nlmsg_len = sizeof(header) + payload.size(); header.nlmsg_type = type;
    header.nlmsg_flags = flags; header.nlmsg_seq = sequence; header.nlmsg_pid = port;
    QByteArray data(reinterpret_cast<const char *>(&header), sizeof(header)); data += payload;
    data.resize(NLMSG_ALIGN(data.size())); return data;
}
QByteArray diagnostic(int family, const char *source, const char *destination, quint32 uid = quint32(::getuid()),
                      quint32 inode = 101, quint64 cookie = 202, quint64 rx = 400, quint64 tx = 600, int infoLength = 136) {
    inet_diag_msg diag {}; diag.idiag_family = family; diag.idiag_state = 1; diag.idiag_inode = inode; diag.idiag_uid = uid;
    diag.id.idiag_cookie[0] = quint32(cookie); diag.id.idiag_cookie[1] = quint32(cookie >> 32);
    check(::inet_pton(family, source, diag.id.idiag_src) == 1 && ::inet_pton(family, destination, diag.id.idiag_dst) == 1, "fixture IP address");
    QByteArray data(reinterpret_cast<const char *>(&diag), sizeof(diag));
    rtattr attr {}; attr.rta_type = INET_DIAG_INFO; attr.rta_len = sizeof(attr) + infoLength;
    data.append(reinterpret_cast<const char *>(&attr), sizeof(attr));
    QByteArray info(infoLength, 0);
    if (infoLength >= 136) { std::memcpy(info.data() + 120, &tx, sizeof(tx)); std::memcpy(info.data() + 128, &rx, sizeof(rx)); }
    data += info; data.resize(RTA_ALIGN(data.size())); return message(SOCK_DIAG_BY_FAMILY, data);
}
}

class ProcessNetworkSamplerTest {
    using Dump = ProcessNetworkSampler::Dump;
    using Connection = ProcessNetworkSampler::Connection;
    static QJsonObject sample(ProcessNetworkSampler &sampler, const QJsonObject &rows, const Dump &dump, double now, double budget = .2) {
        return sampler.sampleDump(rows, dump, now, 1770000000, clockSeconds() + budget);
    }
    static Connection connection(quint64 inode, quint64 cookie, quint64 received, quint64 acknowledged, bool counters = true) {
        Connection c; c.family = AF_INET; c.inode = inode; c.cookie = cookie;
        c.received = received; c.acknowledged = acknowledged; c.counters = counters; return c;
    }
public:
    static void parser() {
        Dump dump; bool done = false;
        auto parse = [&](const QByteArray &data) { return ProcessNetworkSampler::parseDatagram(data, 91, 77, quint32(::getuid()), dump, done); };
        check(parse(diagnostic(AF_INET, "10.0.0.1", "10.0.0.2")), "IPv4 diagnostic parses");
        check(dump.connections.size() == 1 && dump.connections[0].received == 400 && dump.connections[0].acknowledged == 600, "TCP_INFO byte offsets");
        check(parse(diagnostic(AF_INET6, "2001:db8::1", "2001:db8::2", quint32(::getuid()), 102)), "IPv6 diagnostic parses");
        check(parse(diagnostic(AF_INET, "127.3.2.1", "10.0.0.2")) && parse(diagnostic(AF_INET6, "::1", "2001:db8::2")) &&
              parse(diagnostic(AF_INET6, "2001:db8::1", "::ffff:127.0.0.2")), "all loopback forms parse and exclude");
        check(parse(diagnostic(AF_INET, "10.0.0.1", "10.0.0.2", quint32(::getuid()) + 1)), "other UID diagnostic parses and excludes");
        check(dump.connections.size() == 2, "loopback and other users excluded");
        check(parse(diagnostic(AF_INET, "10.0.0.1", "10.0.0.2", quint32(::getuid()), 103, 203, 0, 0, 128)), "older TCP_INFO parses");
        check(!dump.connections.last().counters, "short TCP_INFO counters unknown");
        check(parse(diagnostic(AF_INET, "10.0.0.1", "10.0.0.2", quint32(::getuid()), 104, ~quint64(0))), "missing cookie parses");
        check(!dump.connections.last().counters, "unavailable cookie cannot identify connection");
        QByteArray bad = diagnostic(AF_INET, "10.0.0.1", "10.0.0.2");
        check(!parse(bad.left(bad.size() - 1)), "truncated diagnostic rejected");
        bad = message(NLMSG_DONE, {} , NLM_F_MULTI, 92); check(!parse(bad), "wrong sequence rejected");
        bad = message(NLMSG_DONE, {} , NLM_F_MULTI, 91, 78); check(!parse(bad), "wrong recipient port rejected");
        qint32 error = -1;
        check(!parse(message(NLMSG_DONE, QByteArray(reinterpret_cast<const char *>(&error), sizeof(error)))), "DONE errors rejected");
        check(parse(message(NLMSG_NOOP, {}, NLM_F_MULTI | NLM_F_DUMP_INTR)) && !dump.complete, "interrupted dump marked incomplete");
        check(parse(message(NLMSG_DONE, {})) && done, "multipart completion required");
        check(!parse(message(NLMSG_DONE, {})), "data following completion rejected");
        done = false; nlmsgerr netlinkError {}; netlinkError.error = -13;
        check(!parse(message(NLMSG_ERROR, QByteArray(reinterpret_cast<const char *>(&netlinkError), sizeof(netlinkError)))), "netlink permission error rejected");
        bad = diagnostic(AF_INET, "10.0.0.1", "10.0.0.2");
        const quint16 invalidAttributeLength = 2;
        std::memcpy(bad.data() + sizeof(nlmsghdr) + sizeof(inet_diag_msg), &invalidAttributeLength, sizeof(invalidAttributeLength));
        check(!parse(bad), "invalid attribute length rejected");
        Dump full; full.connections.resize(32768); done = false;
        check(!ProcessNetworkSampler::parseDatagram(diagnostic(AF_INET, "10.0.0.1", "10.0.0.2"), 91, 77, quint32(::getuid()), full, done) && full.limited, "socket limit enforced");
        std::puts("PASS parser IPv4 IPv6 UID loopback TCP_INFO truncation sequence recipient DONE interrupt cookie limits");
    }
    static void accounting() {
        QTemporaryDir temp; check(temp.isValid(), "accounting fixture");
        const QString proc = temp.path() + "/proc", exe = temp.path() + "/private/path/app";
        writeFile(exe, "app");
        QJsonArray rows {process(proc, 101, 11, "A", exe, {1001, 1001}), process(proc, 102, 12, "A", exe, {1002}), process(proc, 103, 13, "B", exe, {1003})};
        ProcessNetworkSampler sampler(proc); Dump dump;
        dump.connections = {connection(1001, 501, 100, 200), connection(1002, 502, 300, 400), connection(1003, 503, 10, 20)};
        auto first = sample(sampler, snapshot(rows), dump, 10);
        check(first["state"] == "starting" && appByName(first, "Alpha")["downloadBytesPerSecond"].isNull(), "baseline is unknown");
        dump.connections = {connection(1001, 501, 500, 220), connection(1002, 502, 500, 440), connection(1003, 503, 30, 220)};
        auto second = sample(sampler, snapshot(rows), dump, 12);
        const auto alpha = appByName(second, "Alpha"), beta = appByName(second, "Beta");
        check(second["state"] == "ready" && alpha["downloadBytesPerSecond"].toDouble() == 300 && alpha["uploadBytesPerSecond"].toDouble() == 30, "multiprocess application rates summed once");
        check(alpha["connectionCount"].toInt() == 2 && alpha["processCount"].toInt() == 2, "duplicate FDs deduplicated");
        check(beta["downloadBytesPerSecond"].toDouble() == 10 && beta["uploadBytesPerSecond"].toDouble() == 100, "separate application directions");
        check(!QJsonDocument(second).toJson().contains(temp.path().toUtf8()), "output has no executable paths");
        auto unchanged = sample(sampler, snapshot(rows), dump, 14);
        check(appByName(unchanged, "Alpha")["downloadBytesPerSecond"].toDouble(-1) == 0, "measured idle connection is zero");
        dump.connections[0].cookie++;
        auto changed = sample(sampler, snapshot(rows), dump, 16);
        check(appByName(changed, "Alpha")["partial"].toBool() && member(appByName(changed, "Alpha"), 101)["downloadBytesPerSecond"].isNull(), "inode reuse with changed cookie resets baseline");
        dump.connections[0].received = 1;
        auto rollback = sample(sampler, snapshot(rows), dump, 18);
        check(member(appByName(rollback, "Alpha"), 101)["downloadBytesPerSecond"].isNull(), "counter rollback unknown");
        writeFile(proc + "/101/stat", statText(101, 99));
        auto reused = sample(sampler, snapshot(rows), dump, 20);
        check(member(appByName(reused, "Alpha"), 101)["downloadBytesPerSecond"].isNull() && reused["unattributedConnectionCount"].toInt() == 1, "stale PID identity discards FD attribution");
        writeFile(proc + "/101/stat", statText(101, 11));
        const QString replacement = temp.path() + "/replacement"; writeFile(replacement, "new"); link(replacement, proc + "/101/exe");
        auto exec = sample(sampler, snapshot(rows), dump, 22);
        check(exec["unattributedConnectionCount"].toInt() == 1, "stale executable identity rejected");
        auto changedExe = rows[0].toObject(); changedExe["executablePath"] = replacement; rows[0] = changedExe;
        auto validExec = sample(sampler, snapshot(rows), dump, 23);
        check(member(appByName(validExec, "Alpha"), 101)["downloadBytesPerSecond"].isNull(), "fresh executable after exec starts a new baseline");
        sampler.reset(); check(sample(sampler, snapshot(rows), dump, 24)["state"] == "starting", "reset loses rate baselines");
        auto stale = snapshot(rows); stale["processesFresh"] = false;
        check(sample(sampler, stale, dump, 26)["applications"].toArray().isEmpty(), "stale process scan not reused");
        Dump unavailable; unavailable.available = false;
        check(sample(sampler, snapshot(rows), unavailable, 28)["state"] == "unavailable", "unsupported diagnostics clearly unavailable");
        check(sampler.sample(snapshot(rows))["state"] == "unavailable", "custom proc never mixes host diagnostics");
        const QString deleted = temp.path() + "/deleted-app"; writeFile(deleted, "old executable");
        QJsonArray deletedRows {process(proc, 104, 14, "A", deleted)};
        check(QFile::remove(deleted), "delete only own fixture executable");
        link(deleted + " (deleted)", proc + "/104/exe");
        sampler.reset(); sample(sampler, snapshot(deletedRows), Dump(), 30);
        auto deletedExe = appByName(sample(sampler, snapshot(deletedRows), Dump(), 32), "Alpha");
        check(deletedExe["downloadBytesPerSecond"].toDouble(-1) == 0 && !deletedExe["partial"].toBool(), "running deleted executable retains verified identity");
        std::puts("PASS accounting grouping directions duplicate-FD idle cookie rollback PID exec reset unknown privacy");
    }
    static void sharingAndMissing() {
        QTemporaryDir temp; const QString proc = temp.path() + "/proc", exe = temp.path() + "/app"; writeFile(exe, "app");
        QJsonArray rows {process(proc, 201, 21, "A", exe, {2001}), process(proc, 202, 22, "A", exe, {2001})};
        Dump dump; dump.connections = {connection(2001, 601, 100, 200)};
        ProcessNetworkSampler sampler(proc); sample(sampler, snapshot(rows), dump, 10);
        dump.connections[0].received += 800; dump.connections[0].acknowledged += 200;
        auto shared = appByName(sample(sampler, snapshot(rows), dump, 12), "Alpha");
        check(shared["downloadBytesPerSecond"].toDouble() == 400 && shared["connectionCount"].toInt() == 1, "same app shared socket counts once");
        check(!shared["partial"].toBool(), "stable same-app sharing has a complete application total");
        check(member(shared, 201)["shared"].toBool() && member(shared, 201)["downloadBytesPerSecond"].isNull() && member(shared, 202)["uploadBytesPerSecond"].isNull(), "shared child process rate not duplicated");
        auto other = rows[1].toObject(); other["applicationIdentity"] = "B"; rows[1] = other;
        auto cross = sample(sampler, snapshot(rows), dump, 14);
        check(cross["unattributedConnectionCount"].toInt() == 1 && appByName(cross, "Alpha")["downloadBytesPerSecond"].isNull() && appByName(cross, "Beta")["downloadBytesPerSecond"].isNull(), "cross-app sharing never apportioned");
        rows = QJsonArray {process(proc, 203, 23, "A", exe)}; Dump empty;
        sampler.reset(); sample(sampler, snapshot(rows), empty, 20);
        auto idle = appByName(sample(sampler, snapshot(rows), empty, 22), "Alpha");
        check(idle["downloadBytesPerSecond"].toDouble(-1) == 0 && !idle["partial"].toBool(), "two complete empty scans are measured zero");
        rows.append(process(proc, 204, 24, "A", exe, {2002})); dump.connections = {connection(2002, 602, 100, 200)};
        auto fresh = appByName(sample(sampler, snapshot(rows), dump, 24), "Alpha");
        check(fresh["partial"].toBool() && fresh["downloadBytesPerSecond"].isNull(), "new member cannot make unknown traffic zero");
        dump.connections[0].received += 100;
        sample(sampler, snapshot(rows), dump, 26);
        rows.removeLast(); auto exited = appByName(sample(sampler, snapshot(rows), empty, 28), "Alpha");
        check(exited["partial"].toBool() && exited["downloadBytesPerSecond"].isNull(), "exited member final traffic unknown");
        std::puts("PASS sharing same-app cross-app new-member exited-member complete-zero");
    }
    static void limitsAndPerformance() {
        QTemporaryDir temp; const QString proc = temp.path() + "/proc", exe = temp.path() + "/app"; writeFile(exe, "app");
        QJsonArray rows;
        for (int i = 0; i < 50; ++i) {
            QVector<quint64> fds; for (int j = 0; j < 20; ++j) fds.append(quint64(3000 + i));
            rows.append(process(proc, 300 + i, 30 + i, QString::number(i), exe, fds));
        }
        Dump dump;
        for (int i = 0; i < 50; ++i) dump.connections.append(connection(3000 + i, 700 + i, 100, 200));
        ProcessNetworkSampler sampler(proc);
        double wall = clockSeconds(), cpu = clockSeconds(CLOCK_PROCESS_CPUTIME_ID);
        auto output = sample(sampler, snapshot(rows), dump, 10);
        wall = clockSeconds() - wall; cpu = clockSeconds(CLOCK_PROCESS_CPUTIME_ID) - cpu;
        check(output["applications"].toArray().size() == 30 && output["limited"].toBool(), "application output bounded at 30");
        rusage usage {}; ::getrusage(RUSAGE_SELF, &usage);
        std::printf("BENCH {\"fixture\":\"normal-50-process-1000-fd\",\"wallMs\":%.3f,\"cpuMs\":%.3f,\"maxRssKiB\":%ld}\n", wall * 1000, cpu * 1000, usage.ru_maxrss);
        // Alpha at row 50 has less download than >30 peers but the most upload.
        for (int i = 0; i < dump.connections.size(); ++i) {
            dump.connections[i].received += quint64(50 - i) * 100;
            dump.connections[i].acknowledged += i == 49 ? 100000 : quint64(50 - i);
        }
        output = sample(sampler, snapshot(rows), dump, 12);
        bool uploadLeaderPresent = false;
        for (const auto &value : output["applications"].toArray()) {
            const auto app = value.toObject();
            if (app["uploadBytesPerSecond"].toDouble() == 50000) uploadLeaderPresent = true;
        }
        check(uploadLeaderPresent && output["applications"].toArray().size() <= 60 && output["applications"].toArray().size() > 30,
              "candidate union retains upload leader below download top 30");
        QJsonArray crowded;
        for (int i = 0; i < 101; ++i) crowded.append(process(proc, 400 + i, 100 + i, "A", exe));
        sampler.reset(); output = sample(sampler, snapshot(crowded), Dump(), 10);
        check(appByName(output, "Alpha")["processes"].toArray().size() == 100 && output["limited"].toBool(), "process output bounded at 100 per app");
        QJsonArray tooMany;
        for (int i = 0; i < 4100; ++i) tooMany.append(QJsonObject {{"pid", 10000 + i}, {"startTicks", 1}, {"applicationIdentity", "A"}, {"executablePath", exe}, {"name", "A"}});
        sampler.reset(); output = sample(sampler, snapshot(tooMany), Dump(), 10, 5);
        check(output["scannedProcessCount"].toInt() == 4096 && output["limited"].toBool(), "PID candidate limit enforced");
        const int pid = 600;
        rows = QJsonArray {process(proc, pid, 600, "A", exe)};
        for (int i = 0; i < 33000; ++i) link("socket:[8001]", proc + "/600/fd/" + QString::number(i));
        dump.connections = {connection(8001, 8001, 1, 2)};
        wall = clockSeconds(); cpu = clockSeconds(CLOCK_PROCESS_CPUTIME_ID);
        sampler.reset(); output = sample(sampler, snapshot(rows), dump, 10);
        wall = clockSeconds() - wall; cpu = clockSeconds(CLOCK_PROCESS_CPUTIME_ID) - cpu;
        ::getrusage(RUSAGE_SELF, &usage);
        check(output["limited"].toBool() && appByName(output, "Alpha")["partial"].toBool(), "large FD/time limit incomplete");
        check(wall < 1, "large FD scan returns within bounded scheduling allowance");
        std::printf("BENCH {\"fixture\":\"large-1-process-33000-fd\",\"wallMs\":%.3f,\"cpuMs\":%.3f,\"maxRssKiB\":%ld}\n", wall * 1000, cpu * 1000, usage.ru_maxrss);
        output = sample(sampler, snapshot(rows), dump, 12, -.001);
        check(output["limited"].toBool() && output["scannedProcessCount"].toInt() == 0, "expired time budget stops before process IO");
        std::puts("PASS output PID FD time limits and bounded performance");
    }
};

namespace {
QJsonObject nativeProcess(pid_t pid, const QString &identity, const QString &name) {
    const QString base = "/proc/" + QString::number(pid);
    QFile file(base + "/stat"); check(file.open(QIODevice::ReadOnly), "native proc stat readable");
    const QByteArray stat = file.readAll(); const auto fields = stat.mid(stat.lastIndexOf(')') + 1).simplified().split(' ');
    return {{"pid", pid}, {"startTicks", double(fields[19].toULongLong())}, {"applicationIdentity", identity}, {"name", name},
            {"executablePath", QFileInfo(QFileInfo(base + "/exe").symLinkTarget()).canonicalFilePath()}};
}
void nativeTransfer() {
    check(::getuid() == 1000, "native capability verification runs as UID 1000");
    ifaddrs *addresses = nullptr; check(::getifaddrs(&addresses) == 0, "enumerate own local interface");
    in_addr own {};
    for (ifaddrs *a = addresses; a; a = a->ifa_next) if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET) {
        const auto *ip = reinterpret_cast<const sockaddr_in *>(a->ifa_addr);
        if ((ntohl(ip->sin_addr.s_addr) >> 24) != 127) { own = ip->sin_addr; break; }
    }
    ::freeifaddrs(addresses); check(own.s_addr != 0, "non-loopback local address available");
    int server = ::socket(AF_INET, SOCK_STREAM, 0); check(server >= 0, "create private test listener");
    sockaddr_in local {}; local.sin_family = AF_INET; local.sin_addr = own;
    check(::bind(server, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0 && ::listen(server, 4) == 0, "bind own-interface test listener");
    socklen_t length = sizeof(local); check(::getsockname(server, reinterpret_cast<sockaddr *>(&local), &length) == 0, "test listener address");
    int clients[3], accepted[3], command[3][2], status[3][2]; pid_t children[3];
    const int sizes[3] = {512 * 1024, 256 * 1024, 64 * 1024};
    for (int i = 0; i < 3; ++i) {
        clients[i] = ::socket(AF_INET, SOCK_STREAM, 0); check(clients[i] >= 0, "private client socket");
        check(::connect(clients[i], reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0, "connect only to own interface");
        accepted[i] = ::accept(server, nullptr, nullptr); check(accepted[i] >= 0, "accept private test client");
        check(::pipe(command[i]) == 0 && ::pipe(status[i]) == 0, "private test synchronization");
    }
    for (int i = 0; i < 3; ++i) {
        children[i] = ::fork(); check(children[i] >= 0, "fork own receiver");
        if (!children[i]) {
            ::alarm(15); // Bound own children even if an assertion terminates the parent.
            ::close(server);
            for (int j = 0; j < 3; ++j) { ::close(clients[j]); if (j != i) ::close(accepted[j]); }
            char instruction; if (::read(command[i][0], &instruction, 1) != 1) _exit(2);
            int left = sizes[i]; char bytes[65536];
            while (left > 0) { const ssize_t got = ::read(accepted[i], bytes, qMin(left, int(sizeof(bytes)))); if (got <= 0) _exit(3); left -= int(got); }
            if (::write(status[i][1], "K", 1) != 1) _exit(4);
            if (::read(command[i][0], &instruction, 1) != 1) _exit(5);
            _exit(0);
        }
    }
    ::close(server); for (int fd : accepted) ::close(fd);
    QJsonArray processes {nativeProcess(::getpid(), "U", "Uploader"), nativeProcess(children[0], "A", "Alpha worker 1"),
                         nativeProcess(children[1], "A", "Alpha worker 2"), nativeProcess(children[2], "B", "Beta worker")};
    auto rows = snapshot(processes);
    ProcessNetworkSampler sampler;
    const auto before = sampler.sample(rows);
    check(before["state"] == "starting", "rootless native socket dump available");
    QByteArray payload(65536, 'N');
    for (int i = 0; i < 3; ++i) {
        check(::write(command[i][1], "R", 1) == 1, "start own receiver");
        int left = sizes[i];
        while (left) { const ssize_t sent = ::send(clients[i], payload.constData(), qMin(left, payload.size()), MSG_NOSIGNAL); check(sent > 0, "local payload sent"); left -= int(sent); }
        char reply; check(::read(status[i][0], &reply, 1) == 1 && reply == 'K', "local payload completely received");
    }
    ::usleep(100000);
    const auto after = sampler.sample(rows);
    const auto alpha = appByName(after, "Alpha"), beta = appByName(after, "Beta"), uploader = appByName(after, "Uploader");
    check(!alpha.isEmpty() && !beta.isEmpty() && !uploader.isEmpty(), "native applications attributed");
    const double interval = after["intervalSeconds"].toDouble();
    const double alphaBytes = alpha["downloadBytesPerSecond"].toDouble(-1) * interval;
    const double betaBytes = beta["downloadBytesPerSecond"].toDouble(-1) * interval;
    const double uploadBytes = uploader["uploadBytesPerSecond"].toDouble(-1) * interval;
    check(std::abs(alphaBytes - (sizes[0] + sizes[1])) < 1, "native multiprocess grouped download matches sent bytes");
    check(std::abs(betaBytes - sizes[2]) < 1 && alphaBytes > betaBytes, "native download ranking identifies heavier application");
    check(std::abs(uploadBytes - (sizes[0] + sizes[1] + sizes[2])) < 1, "native acknowledged upload exact");
    check(alpha["processCount"].toInt() == 2 && alpha["connectionCount"].toInt() == 2, "native two-process group");
    std::printf("NATIVE {\"uid\":%d,\"alphaDownloadBytes\":%.0f,\"betaDownloadBytes\":%.0f,\"acknowledgedUploadBytes\":%.0f,\"intervalSeconds\":%.6f,\"alphaProcesses\":2}\n", int(::getuid()), alphaBytes, betaBytes, uploadBytes, interval);
    for (int i = 0; i < 3; ++i) { check(::write(command[i][1], "Q", 1) == 1, "release own receiver"); int state; check(::waitpid(children[i], &state, 0) == children[i] && WIFEXITED(state) && WEXITSTATUS(state) == 0, "own receiver exits cleanly"); ::close(clients[i]); }
    std::puts("PASS UID1000 rootless real local TCP upload download ranking multiprocess grouping");
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    if (application.arguments().contains("--probe-netlink")) {
        ProcessNetworkSampler sampler;
        const auto result = sampler.sample(snapshot(QJsonArray {nativeProcess(::getpid(), "probe", "Network probe")}));
        std::printf("PROBE %s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        return 0;
    }
    ProcessNetworkSamplerTest::parser(); ProcessNetworkSamplerTest::accounting();
    ProcessNetworkSamplerTest::sharingAndMissing(); ProcessNetworkSamplerTest::limitsAndPerformance();
    if (application.arguments().contains("--native")) nativeTransfer();
    else std::puts("SKIP real netlink: fixture-only run; invoke --native on native UID1000 Linux");
    return 0;
}
