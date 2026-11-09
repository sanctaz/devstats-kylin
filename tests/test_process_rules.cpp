#include "../src/process_rules.h"
#include "../src/process_actions.cpp"

#include <QCoreApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/wait.h>

namespace {
int requestFd = -1, responseFd = -1;
void check(bool condition, const char *message) {
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

QJsonObject nativeIdentity(pid_t pid) {
    for (int retry = 0; retry < 200; ++retry) {
        Descriptor directory(openProcess(pid));
        Process process;
        if (directory.value >= 0 && readProcess(directory.value, process))
            return {{"pid", double(pid)}, {"startTicks", double(process.identity.start)},
                    {"executablePath", process.identity.executable}, {"name", "misleading argv name"}};
        QThread::msleep(5);
    }
    check(false, "read exact owned native identity"); return {};
}

bool alive(pid_t pid) {
    Descriptor directory(openProcess(pid));
    Process process;
    return directory.value >= 0 && readProcess(directory.value, process) &&
           process.state != 'Z' && process.state != 'X' && process.state != 'x';
}

pid_t sleeper(bool ignoreTerm = false) {
    const pid_t owner = ::getpid(), child = ::fork();
    check(child >= 0, "fork owned sleeper");
    if (child == 0) {
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != owner) ::_exit(126);
        // An ignored signal stays ignored across exec.
        if (ignoreTerm) ::signal(SIGTERM, SIG_IGN);
        ::execl("/bin/sleep", "arbitrary-display-name", "120", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    return child;
}

pid_t ownedSibling(bool ignoreTerm = false) {
    const char command = ignoreTerm ? 'i' : 's';
    check(::write(requestFd, &command, 1) == 1, "request independently supervised sibling");
    pid_t child = 0;
    check(::read(responseFd, &child, sizeof(child)) == ssize_t(sizeof(child)) && child > 1,
          "receive exact owned sibling PID");
    for (int retry = 0; retry < 200; ++retry) {
        if (QFileInfo(nativeIdentity(child).value("executablePath").toString()).fileName() == "sleep") return child;
        QThread::msleep(5);
    }
    check(false, "owned sibling execs sleep"); return 0;
}

void cleanup(pid_t child) {
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) {
        // Sole supervisor/reaper; the exact unreaped child PID cannot be reused.
        check(::kill(child, SIGKILL) == 0 || errno == ESRCH, "cleanup exact owned fixture");
        do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    }
    check(result == child, "reap exact owned fixture");
}

QJsonObject rule(const QString &value, const QString &match = "path", const QString &id = "rule-1", bool enabled = true) {
    return {{"id", id}, {"enabled", enabled}, {"match", match}, {"value", value}};
}

QJsonObject snapshot(const QJsonArray &processes, double sample) {
    return {{"processesFresh", true}, {"processMonotonicSeconds", sample},
            {"processSampleAgeSeconds", 0}, {"processes", processes}};
}

QJsonArray entries(const ProcessRules &engine) { return engine.state().value("log").toArray(); }

void validations(const QString &executable) {
    ProcessRules engine;
    const QJsonArray valid{rule(executable)};
    check(engine.configure(false, valid).value("ok").toBool(), "canonical executable configuration");
    const auto original = engine.state();
    const QStringList invalidNames{"", "slee", "sleep*", "sleep?", "^sleep$", "sleep|sh", "sleep 1",
                                    "../sleep", "/bin/sleep", "sleep\n", ".", "..", "sleep[0]"};
    for (const auto &name : invalidNames) {
        // A different complete basename is valid but never a substring match.
        if (name == "slee") continue;
        check(!engine.configure(true, {rule(name, "name")}).value("ok").toBool(), "invalid basename rejected");
        check(engine.state() == original, "invalid update preserves active configuration");
    }
    check(engine.configure(false, {rule("/definitely/missing/devstats-fixture")}).value("ok").toBool(),
          "clean missing path survives uninstall and reinstall");
    check(!engine.configure(true, {rule("/definitely/missing/../devstats-fixture")}).value("ok").toBool(),
          "noncanonical missing path rejected");
    check(!engine.configure(true, {rule(executable), rule("sleep", "name")}).value("ok").toBool(), "duplicate ids rejected");
    check(!engine.configure(true, {rule(executable), rule(executable, "path", "other")}).value("ok").toBool(), "duplicate targets rejected");
    auto missingEnabled = rule(executable); missingEnabled.remove("enabled");
    check(!engine.configure(true, {missingEnabled}).value("ok").toBool(), "missing enabled state rejected");
    QJsonArray tooMany;
    for (int i = 0; i < 33; ++i) tooMany.append(rule(QStringLiteral("owned-helper-%1").arg(i), "name", QString::number(i)));
    check(!engine.configure(true, tooMany).value("ok").toBool(), "rule count bounded");
    check(engine.configure(true, {rule("slee", "name")}).value("ok").toBool(), "complete other basename allowed");
}

void unsupported(const QJsonObject &target) {
    const pid_t child = ::fork();
    check(child >= 0, "fork isolated unsupported-kernel probe");
    if (child == 0) {
#if defined(SYS_pidfd_send_signal)
        struct sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
        };
        struct sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
        check(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
              ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0, "install local ENOSYS simulation");
#endif
        ProcessRules engine;
        check(engine.configure(true, {rule(target.value("executablePath").toString())}).value("ok").toBool(), "unsupported config valid");
        const auto input = snapshot({target}, 1);
        engine.observe(input, 0);
        const auto result = entries(engine).first().toObject();
        check(!result.value("ok").toBool() && result.value("code") == "unsupportedKernel" && !result.contains("signal"),
              "unsupported kernel logged as refusal, never numeric PID fallback or false success");
        ::_exit(0);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "unsupported-kernel checks pass");
}

QJsonObject withCpu(QJsonObject identity, const QJsonValue &cpu) { identity["cpuPercent"] = cpu; return identity; }
QJsonObject cpuRule(const QString &value, double limit, int sustain, bool force = false, int grace = 10) {
    auto result = rule(value);
    result["cpuAbovePercent"] = limit; result["sustainSeconds"] = sustain; result["forceKill"] = force; result["graceSeconds"] = grace;
    return result;
}
bool exitsWithin(pid_t pid, const QJsonObject &identity) {
    // /proc/PID/exe disappears slightly before the exiting task becomes a zombie.
    for (int i = 0; i < 400 && (alive(pid) || ProcessActions().isRunning(identity)); ++i) QThread::msleep(5);
    return !alive(pid) && !ProcessActions().isRunning(identity);
}

void thresholds(pid_t sentinel) {
    const QString executable = nativeIdentity(sentinel).value("executablePath").toString();
    ProcessRules parser;
    auto bad = cpuRule(executable, 0, 30);
    check(!parser.configure(true, {bad}).value("ok").toBool(), "zero CPU limit rejected");
    bad = cpuRule(executable, 80, 3601);
    check(!parser.configure(true, {bad}).value("ok").toBool(), "over-long sustain rejected");
    bad = cpuRule(executable, 80, -1);
    check(!parser.configure(true, {bad}).value("ok").toBool(), "negative sustain rejected");
    bad = cpuRule(executable, 80, 30); bad["cpuAbovePercent"] = "80";
    check(!parser.configure(true, {bad}).value("ok").toBool(), "non-numeric CPU limit rejected");
    bad = cpuRule(executable, 80, 30, true, 0);
    check(!parser.configure(true, {bad}).value("ok").toBool(), "zero grace rejected");
    bad = cpuRule(executable, 80, 30, true, 121);
    check(!parser.configure(true, {bad}).value("ok").toBool(), "over-long grace rejected");
    const auto stored = parser.configure(true, {cpuRule(executable, 80, 30, true, 15)});
    const auto normal = stored.value("rules").toArray().first().toObject();
    check(stored.value("ok").toBool() && normal.value("cpuAbovePercent").toDouble() == 80 && normal.value("sustainSeconds").toInt() == 30 &&
          normal.value("forceKill").toBool() && normal.value("graceSeconds").toInt() == 15, "CPU rule fields survive normalization");
    check(parser.configure(true, {rule(executable)}).value("rules").toArray().first().toObject().value("cpuAbovePercent").isUndefined(),
          "a rule without a limit stays unconditional, as saved by earlier versions");

    // Below the limit nothing happens, however long it runs.
    const pid_t quiet = ownedSibling();
    const auto quietId = nativeIdentity(quiet);
    ProcessRules engine;
    engine.configure(true, {cpuRule(executable, 80, 30)});
    engine.observe(snapshot({withCpu(quietId, 10)}, 1), 0);
    engine.observe(snapshot({withCpu(quietId, 79.9)}, 2), 10000);
    engine.observe(snapshot({withCpu(quietId, 79.9)}, 3), 100000);
    check(entries(engine).isEmpty() && alive(quiet) && !engine.wantsFreshProcesses(), "below the limit: no action and no extra sampling");
    // Unknown is not "below": a first sample has no rate and must not touch the clock.
    engine.observe(snapshot({withCpu(quietId, QJsonValue())}, 4), 110000);
    check(entries(engine).isEmpty() && alive(quiet), "unknown CPU neither triggers nor counts");

    // Over the limit, the countdown is real time. The reading covers the previous interval.
    engine.observe(snapshot({withCpu(quietId, 150)}, 5), 120000);
    check(entries(engine).isEmpty() && alive(quiet) && engine.wantsFreshProcesses(), "first reading over the limit arms the rule and asks for faster sampling");
    auto status = engine.state().value("status").toObject().value("rule-1").toObject();
    check(status.value("armed").toInt() == 1 && status.value("matched").toInt() == 1 && status.value("topCpuPercent").toDouble() == 150, "status exposes the running countdown");
    engine.observe(snapshot({withCpu(quietId, 150)}, 6), 122000);
    status = engine.state().value("status").toObject().value("rule-1").toObject();
    check(entries(engine).isEmpty() && status.value("armedSeconds").toDouble() >= 11 && status.value("averageCpuPercent").toDouble() >= 149, "countdown and average are reported");
    // A brief dip while the average stays above the limit does not restart the clock.
    engine.observe(snapshot({withCpu(quietId, 40)}, 7), 124000);
    check(engine.wantsFreshProcesses() && entries(engine).isEmpty(), "brief dip keeps the countdown running");
    // Falling below on average does.
    engine.observe(snapshot({withCpu(quietId, 0)}, 8), 138000);
    engine.observe(snapshot({withCpu(quietId, 0)}, 9), 140000);
    check(!engine.wantsFreshProcesses() && entries(engine).isEmpty() && alive(quiet), "sustained drop cancels the countdown");

    // Exactly at the sustain time the process receives SIGTERM, and the exit is verified.
    engine.observe(snapshot({withCpu(quietId, 200)}, 10), 150000); // Covers the previous 10 s.
    engine.observe(snapshot({withCpu(quietId, 200)}, 11), 165000);
    check(entries(engine).isEmpty() && alive(quiet), "not yet: 25 of 30 seconds");
    engine.observe(snapshot({withCpu(quietId, 200)}, 12), 170000);
    check(entries(engine).size() == 1, "sustain reached");
    auto row = entries(engine).first().toObject();
    if (row.value("code") == "unsupportedKernel") return;
    check(row.value("phase") == "requested" && row.value("signal").toInt() == SIGTERM && row.value("triggerCpuPercent").toDouble() >= 80 &&
          row.value("condition").toString().contains("80"), "log says what triggered it and that SIGTERM was sent");
    check(exitsWithin(quiet, quietId), "the matched process exits on SIGTERM");
    check(engine.wantsFreshProcesses(), "exit is being verified at the faster cadence");
    engine.observe(snapshot({}, 13), 172000);
    row = entries(engine).first().toObject();
    check(entries(engine).size() == 1 && row.value("phase") == "exited" && row.value("ok").toBool(), "the same log row now says it exited");
    check(!engine.wantsFreshProcesses(), "nothing left to verify");

    // A process ignoring SIGTERM: without escalation it is reported as still running.
    const pid_t stubborn = ownedSibling(true);
    const auto stubbornId = nativeIdentity(stubborn);
    ProcessRules gentle;
    gentle.configure(true, {cpuRule(executable, 80, 0, false, 5)});
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 1), 0);
    check(entries(gentle).first().toObject().value("phase") == "requested", "zero sustain acts on the first reading over the limit");
    QThread::msleep(50);
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 2), 4000);
    check(entries(gentle).first().toObject().value("phase") == "requested" && alive(stubborn), "still within the grace time");
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 3), 5000);
    check(entries(gentle).size() == 1 && entries(gentle).first().toObject().value("phase") == "survived" && alive(stubborn), "reported as not exited; never killed without permission");
    check(gentle.state().value("status").toObject().value("rule-1").toObject().value("unresponsive").toInt() == 1, "live status says it ignored the request");
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 4), 70000);
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 5), 200000);
    check(entries(gentle).size() == 1 && !gentle.wantsFreshProcesses(), "a request the process ignored is not repeated forever");
    check(gentle.state().value("status").toObject().value("rule-1").toObject().value("armed").toInt() == 0, "a process given up on is not shown as counting down");
    gentle.configure(true, {cpuRule(executable, 80, 0, false, 6)});
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 6), 260000);
    check(entries(gentle).size() == 2, "changing the rule retries");
    QThread::msleep(50);
    gentle.observe(snapshot({withCpu(stubbornId, 500)}, 7), 270000);

    // With escalation the same process receives SIGKILL once the grace time is over.
    ProcessRules forceful;
    forceful.configure(true, {cpuRule(executable, 80, 0, true, 5)});
    forceful.observe(snapshot({withCpu(stubbornId, 500)}, 1), 0);
    QThread::msleep(50);
    check(alive(stubborn), "SIGTERM alone does not stop this fixture");
    forceful.observe(snapshot({withCpu(stubbornId, 500)}, 2), 4999);
    check(alive(stubborn) && entries(forceful).first().toObject().value("phase") == "requested", "no SIGKILL before the grace time");
    forceful.observe(snapshot({withCpu(stubbornId, 500)}, 3), 5000);
    check(entries(forceful).first().toObject().value("phase") == "forcing" && entries(forceful).first().toObject().value("signal").toInt() == SIGKILL, "SIGKILL after the grace time");
    check(exitsWithin(stubborn, stubbornId), "the stubborn process is gone");
    forceful.observe(snapshot({}, 4), 7000);
    check(entries(forceful).size() == 1 && entries(forceful).first().toObject().value("phase") == "killed" && entries(forceful).first().toObject().value("ok").toBool(), "row records the forced exit");
    check(alive(sentinel), "only the exact identities were signalled");

    // Pausing never escalates a request already sent.
    const pid_t stubborn2 = ownedSibling(true);
    const auto stubborn2Id = nativeIdentity(stubborn2);
    ProcessRules paused;
    paused.configure(true, {cpuRule(executable, 80, 0, true, 5)});
    paused.observe(snapshot({withCpu(stubborn2Id, 500)}, 1), 0);
    paused.pause();
    QThread::msleep(50);
    paused.observe(snapshot({withCpu(stubborn2Id, 500)}, 2), 6000);
    check(alive(stubborn2) && entries(paused).first().toObject().value("phase") == "survived", "pause cancels the pending SIGKILL");
    check(!paused.wantsFreshProcesses(), "paused rules ask for no extra sampling");
}

int runTests() {
    check(::getuid() != 0 && ::geteuid() == ::getuid(), "ordinary native test user");
    QTemporaryDir temporary;
    check(temporary.isValid(), "private temporary log directory");
    const QString logPath = temporary.path() + "/process-rules-log.json";
    const pid_t target = ownedSibling(), sentinel = ownedSibling();
    const auto targetIdentity = nativeIdentity(target), sentinelIdentity = nativeIdentity(sentinel);
    const QString executable = targetIdentity.value("executablePath").toString();
    validations(executable);
    ProcessRules pausable;
    const QJsonArray pauseRules{rule(executable)};
    check(pausable.configure(true, pauseRules).value("ok").toBool(), "valid rules explicitly activate automation");
    check(!pausable.configure(false, {rule("sleep*", "name")}).value("ok").toBool() &&
          pausable.state().value("enabled").toBool(), "invalid configuration remains atomic even when draft asks for disable");
    const auto beforePause = pausable.state();
    pausable.pause();
    check(!pausable.state().value("enabled").toBool() &&
          pausable.state().value("rules") == beforePause.value("rules") &&
          pausable.state().value("log") == beforePause.value("log"), "unconditional pause preserves canonical rules and reports");
    pausable.observe(snapshot({targetIdentity}, 1), 0);
    check(entries(pausable).isEmpty() && alive(target), "paused engine never acts on a fresh eligible identity");
    check(!pausable.configure(true, {rule("sleep*", "name")}).value("ok").toBool() &&
          !pausable.state().value("enabled").toBool(), "invalid draft cannot reactivate paused engine");
    check(pausable.configure(true, pauseRules).value("ok").toBool() && pausable.state().value("enabled").toBool(),
          "explicit valid configuration can reactivate paused engine");
    ProcessRules engine(temporary.path());
    auto input = snapshot({targetIdentity, sentinelIdentity}, 1);
    engine.observe(input, 0);
    check(!engine.state().value("enabled").toBool() && entries(engine).isEmpty() && !QFile::exists(logPath),
          "default off produces no actions or log writes");
    check(engine.configure(false, {rule(executable)}).value("ok").toBool(), "disabled configuration valid");
    engine.observe(input, 0);
    check(entries(engine).isEmpty() && !QFile::exists(logPath), "global disabled has no log file");
    engine.configure(true, {rule("slee", "name")});
    engine.observe(input, 0);
    check(entries(engine).isEmpty() && !QFile::exists(logPath), "exact name never matches substring or argv");
    engine.configure(true, {rule("sleep", "name", "sleep-name", false)});
    engine.observe(snapshot({targetIdentity}, 2), 10000);
    check(entries(engine).isEmpty() && !QFile::exists(logPath), "per-rule disabled has no actions");
    engine.configure(true, {rule(executable)});
    auto stale = input; stale["processesFresh"] = false;
    engine.observe(stale, 10000);
    stale = input; stale["processSampleAgeSeconds"] = 6;
    engine.observe(stale, 10000);
    stale = input; stale.remove("processMonotonicSeconds"); engine.observe(stale, 10000);
    check(entries(engine).isEmpty() && alive(target), "cached, old and unmarked snapshots refused");
    qputenv("DEVSTATS_PROC_ROOT", "/tmp/devstats-test-not-native");
    engine.observe(snapshot({targetIdentity}, 2.5), 10000);
    check(entries(engine).isEmpty(), "injected proc root cannot cause action");
    qunsetenv("DEVSTATS_PROC_ROOT");

    ProcessRules protectedEngine;
    const auto self = nativeIdentity(::getpid());
    protectedEngine.configure(true, {rule(self.value("executablePath").toString())});
    const auto protectedInput = snapshot({self}, 1);
    protectedEngine.observe(protectedInput, 0);
    check(entries(protectedEngine).size() == 1 && entries(protectedEngine).first().toObject().value("code") == "protectedProcess",
          "protected monitor logged as refusal");
    protectedEngine.observe(snapshot({self}, 2), 120000);
    check(entries(protectedEngine).size() == 1, "one protected process is reported once, not on every scan");
    check(protectedEngine.state().value("status").toObject().value("rule-1").toObject().value("protected").toInt() == 1,
          "live status shows why the rule cannot act");
    const pid_t descendant = sleeper();
    for (int i = 0; i < 200 && QFileInfo(nativeIdentity(descendant).value("executablePath").toString()).fileName() != "sleep"; ++i) QThread::msleep(5);
    ProcessRules descendants;
    descendants.configure(true, {rule(executable)});
    descendants.observe(snapshot({nativeIdentity(descendant)}, 1), 0);
    check(entries(descendants).first().toObject().value("code") == "protectedProcess" && alive(descendant), "monitor helper descendant protected");
    cleanup(descendant);
    unsupported(sentinelIdentity);
    check(alive(target) && alive(sentinel), "protection and old kernel preserve fixtures");

    auto changedIdentity = targetIdentity;
    changedIdentity["startTicks"] = targetIdentity.value("startTicks").toDouble() + 1;
    ProcessRules wrongIdentity;
    wrongIdentity.configure(true, {rule(executable)});
    wrongIdentity.observe(snapshot({changedIdentity}, 1), 0);
    check(entries(wrongIdentity).first().toObject().value("code") == "staleIdentity" && alive(target), "native PID identity revalidated");

    // Reset only this test engine's sample continuity by supplying a newer fresh
    // sample; configure itself preserves all retry/backoff state.
    engine.observe(snapshot({targetIdentity}, 3), 10000);
    check(entries(engine).size() == 1, "matching fresh sample logs one attempt");
    const auto first = entries(engine).first().toObject();
    if (first.value("code") == "unsupportedKernel") {
        check(alive(target), "real old kernel leaves target running");
        std::puts("PASS: native policy/preview/exact rules/old-kernel checks; live SIGTERM unavailable"); return 0;
    }
    check(first.value("ok").toBool() && first.value("code") == "requested" && first.value("signal").toInt() == SIGTERM,
          "normal descriptor SIGTERM logged as request only");
    for (int i = 0; i < 200 && alive(target); ++i) QThread::msleep(5);
    check(!alive(target) && alive(sentinel), "only selected exact owned sibling terminates");
    struct stat fileInfo{};
    check(::stat(QFile::encodeName(logPath).constData(), &fileInfo) == 0 && (fileInfo.st_mode & 0777) == 0600, "persistent log mode 0600");
    ProcessRules restored(temporary.path()); restored.configure(false, {});
    check(entries(restored) == entries(engine), "latest reports survive restart");
    engine.configure(true, {rule(executable)});
    engine.observe(snapshot({targetIdentity}, 4), 20000);
    check(entries(engine).first().toObject().value("phase") == "exited", "the next scan verifies the exit and updates the same row");
    check(::stat(QFile::encodeName(logPath).constData(), &fileInfo) == 0, "verified exit is saved");
    engine.observe(snapshot({targetIdentity}, 5), 69000);
    check(entries(engine).size() == 1, "same identity cooldown60s survives unchanged configure");
    struct stat unchangedInfo{};
    check(::stat(QFile::encodeName(logPath).constData(), &unchangedInfo) == 0 && unchangedInfo.st_ino == fileInfo.st_ino,
          "cooldown samples do not rewrite persistent log");
    engine.observe(snapshot({targetIdentity}, 6), 70000);
    check(entries(engine).size() == 2 && !entries(engine).first().toObject().value("ok").toBool(), "retry after60s rechecks exited native identity");
    engine.observe(snapshot({sentinelIdentity}, 6), 100000);
    engine.observe(snapshot({sentinelIdentity}, 5), 100000);
    check(entries(engine).size() == 2 && alive(sentinel), "duplicate and backwards sample markers never act");
    engine.observe(snapshot({sentinelIdentity}, 7), 69999);
    check(entries(engine).size() == 2 && alive(sentinel), "backwards monotonic clock cannot bypass cooldown");

    // Genuine new supervised siblings are spawned after the prior SIGTERM.
    ProcessRules respawns;
    respawns.configure(true, {rule(executable)});
    const pid_t generation1 = ownedSibling();
    respawns.observe(snapshot({nativeIdentity(generation1)}, 1), 0);
    const pid_t generation2 = ownedSibling();
    const auto generation2Id = nativeIdentity(generation2);
    respawns.observe(snapshot({generation2Id}, 2), 29999);
    check(entries(respawns).size() == 1 && alive(generation2), "first respawn waits30s");
    respawns.observe(snapshot({generation2Id}, 3), 30000);
    check(entries(respawns).size() == 2, "first backoff expires30s");
    const pid_t generation3 = ownedSibling();
    const auto generation3Id = nativeIdentity(generation3);
    respawns.observe(snapshot({generation3Id}, 4), 89999);
    check(entries(respawns).size() == 2 && alive(generation3), "repeated respawn expands backoff60s");
    respawns.observe(snapshot({generation3Id}, 5), 90000);
    check(entries(respawns).size() == 3, "second backoff expires60s");
    const pid_t generation4 = ownedSibling();
    const auto generation4Id = nativeIdentity(generation4);
    respawns.observe(snapshot({generation4Id}, 6), 209999);
    check(entries(respawns).size() == 3 && alive(generation4), "third generation waits120s");
    respawns.observe(snapshot({generation4Id}, 7), 210000);
    check(entries(respawns).size() == 4, "third backoff expires120s");
    const pid_t generation5 = ownedSibling();
    respawns.observe(snapshot({nativeIdentity(generation5)}, 8), 450000);
    check(entries(respawns).size() == 5, "fourth backoff expires240s");
    const pid_t generation6 = ownedSibling();
    const auto generation6Id = nativeIdentity(generation6);
    respawns.observe(snapshot({generation6Id}, 9), 749999);
    check(entries(respawns).size() == 5 && alive(generation6), "respawn backoff reaches300s");
    respawns.observe(snapshot({generation6Id}, 10), 750000);
    check(entries(respawns).size() == 6, "backoff expires at300s");
    const pid_t generation7 = ownedSibling();
    respawns.observe(snapshot({nativeIdentity(generation7)}, 11), 1050000);
    check(entries(respawns).size() == 7, "continued respawn backoff is capped at300s");

    ProcessRules simultaneous;
    simultaneous.configure(true, {rule(executable)});
    const pid_t member1 = ownedSibling(), member2 = ownedSibling();
    simultaneous.observe(snapshot({nativeIdentity(member1), nativeIdentity(member2), self}, 1), 0);
    check(entries(simultaneous).size() == 2 && entries(simultaneous).first().toObject().value("ok").toBool() &&
          entries(simultaneous).last().toObject().value("ok").toBool(), "one executable wave includes both concurrent eligible members");
    for (int i = 0; i < 200 && (alive(member1) || alive(member2)); ++i) QThread::msleep(5);
    check(!alive(member1) && !alive(member2) && alive(sentinel), "both exact selected siblings terminate, unmatched monitor and unselected sentinel survive");

    ProcessRules budget;
    check(budget.configure(true, {rule("sleep", "name")}).value("ok").toBool(), "bounded rule valid");
    QJsonArray budgetProcesses;
    for (int i = 0; i < 9; ++i) budgetProcesses.append(nativeIdentity(ownedSibling()));
    budget.observe(snapshot(budgetProcesses, 1), 0);
    check(entries(budget).size() == 8 && alive(sentinel), "one fresh scan signals at most 8 processes");
    budget.observe(snapshot(budgetProcesses, 2), 30000);
    check(entries(budget).size() == 9 && alive(sentinel), "a later scan reaches the remaining process");

    // Resume starts a new sampler's relative clock while retaining retry limits.
    ProcessRules resumed;
    resumed.configure(true, {rule(executable)});
    resumed.observe(snapshot({nativeIdentity(ownedSibling())}, 5), 0);
    check(entries(resumed).size() == 1, "fixture acts before the reset");
    resumed.resetObservation();
    const pid_t afterReset = ownedSibling();
    resumed.observe(snapshot({nativeIdentity(afterReset)}, 1), 40000);
    check(entries(resumed).size() == 2 && entries(resumed).first().toObject().value("pid").toInt() == afterReset,
          "sampler reset accepts the new clock epoch");

    ProcessRules candidateBound;
    candidateBound.configure(true, {rule(executable)});
    QJsonArray oversized;
    for (int i = 0; i < 4096; ++i) oversized.append(QJsonObject{});
    oversized.append(sentinelIdentity);
    candidateBound.observe(snapshot(oversized, 1), 0);
    check(entries(candidateBound).isEmpty() && alive(sentinel), "candidate loop is bounded independently of snapshot size");

    // Rejections are also bounded and retained newest-first; no helper receives
    // a signal because this is the protected monitor's exact native identity.
    ProcessRules bounded(temporary.path());
    bounded.configure(true, {rule(self.value("executablePath").toString())});
    for (int i = 0; i < 106; ++i) {
        // Each stale identity is a distinct refusal; the same one would be reported only once.
        auto stale = self; stale["startTicks"] = self.value("startTicks").toDouble() + i + 1;
        bounded.observe(snapshot({stale}, i + 1), qint64(i) * 60000);
    }
    check(entries(bounded).size() == 100, "in-memory log bounded to latest100");
    ProcessRules reloaded(temporary.path()); reloaded.configure(false, {});
    check(entries(reloaded).size() == 100, "persistent log bounded to latest100");
    QTemporaryDir unsafeLog;
    check(unsafeLog.isValid(), "owned symlink refusal fixture directory");
    const QString preservedPath = unsafeLog.path() + "/preserved-owned-data";
    QFile preserved(preservedPath);
    check(preserved.open(QIODevice::WriteOnly) && preserved.write("preserve-this-owned-file") == 24, "write exact owned preservation marker");
    preserved.close();
    const QString unsafePath = unsafeLog.path() + "/process-rules-log.json";
    check(::symlink(QFile::encodeName(preservedPath).constData(), QFile::encodeName(unsafePath).constData()) == 0,
          "create exact owned symlink log fixture");
    ProcessRules unsafe(unsafeLog.path()); unsafe.configure(true, {rule(self.value("executablePath").toString())});
    unsafe.observe(snapshot({self}, 1), 0);
    check(!unsafe.state().value("storageError").toString().isEmpty() && entries(unsafe).size() == 1,
          "unsafe log path reports persistence failure while retaining truthful in-memory refusal");
    check(preserved.open(QIODevice::ReadOnly) && preserved.readAll() == "preserve-this-owned-file",
          "persistence never overwrites symlink target");
    thresholds(sentinel);
    check(alive(sentinel), "unselected sentinel survives full run");
    std::puts("PASS: exact rules/default off/CPU limits and sustain/exit verification/SIGKILL escalation/native ownership/session/PID safety/unsupported kernel/SIGTERM/concurrent wave/respawn backoff/cooldown/scan and log bounds/private persistence/symlink refusal");
    return 0;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    int requests[2], responses[2];
    check(::pipe(requests) == 0 && ::pipe(responses) == 0, "owned supervisor pipes");
    const pid_t tester = ::fork();
    check(tester >= 0, "fork sibling test runner");
    if (tester == 0) {
        ::close(requests[0]); ::close(responses[1]);
        requestFd = requests[1]; responseFd = responses[0];
        const int result = runTests(); std::fflush(stdout); ::_exit(result);
    }
    ::close(requests[1]); ::close(responses[0]);
    QVector<pid_t> owned;
    char command = 0;
    while (::read(requests[0], &command, 1) == 1) {
        check(command == 's' || command == 'i', "bounded supervisor command");
        const pid_t child = sleeper(command == 'i'); owned.append(child);
        check(::write(responses[1], &child, sizeof(child)) == ssize_t(sizeof(child)), "return exact helper pid");
    }
    ::close(requests[0]); ::close(responses[1]);
    int status = 0;
    check(::waitpid(tester, &status, 0) == tester, "reap test runner");
    for (const pid_t child : owned) cleanup(child);
    check(WIFEXITED(status), "test runner exits normally");
    if (WEXITSTATUS(status)) return WEXITSTATUS(status);
    std::puts("PASS: supervisor reaped every exact owned helper; no unrelated process enumerated or signalled");
    return 0;
}
