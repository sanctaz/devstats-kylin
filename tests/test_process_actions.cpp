#include "../src/process_actions.cpp"

#include <QCoreApplication>
#include <QThread>
#include <cstdio>
#include <cstdlib>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/wait.h>

namespace {
void check(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

QJsonObject jsonIdentity(const Identity &identity) {
    return {{"pid", double(identity.pid)}, {"startTicks", double(identity.start)},
            {"executablePath", identity.executable}};
}

Process sample(pid_t pid, bool waitForSleep = false) {
    for (int retry = 0; retry < 200; ++retry) {
        Descriptor directory(openProcess(pid));
        Process process;
        if (directory.value >= 0 && readProcess(directory.value, process) &&
            (!waitForSleep || QFileInfo(process.identity.executable).fileName() == "sleep"))
            return process;
        QThread::msleep(5);
    }
    check(false, "read test process identity");
    return {};
}

bool alive(pid_t pid) {
    Descriptor directory(openProcess(pid));
    Process process;
    return directory.value >= 0 && readProcess(directory.value, process) &&
           process.state != 'Z' && process.state != 'X' && process.state != 'x';
}

pid_t sleeper(bool ignoreTerm = false) {
    const pid_t parent = ::getpid();
    const pid_t child = ::fork();
    check(child >= 0, "fork owned fixture");
    if (child == 0) {
        // Also end this fixture if a failing assertion exits its exact owner.
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != parent) ::_exit(126);
        if (ignoreTerm) ::signal(SIGTERM, SIG_IGN);
        ::execl("/bin/sleep", "sleep", "30", static_cast<char *>(nullptr));
        ::_exit(127);
    }
    return child;
}

pid_t stubborn = 0;

void cleanupOwnedChild(pid_t child) {
    int status = 0;
    pid_t result;
    do { result = ::waitpid(child, &status, WNOHANG); } while (result < 0 && errno == EINTR);
    if (result == 0) {
        // This supervisor is the sole reaper. Its unreaped child PID cannot be
        // reused, and cleanup never searches or signals any other user process.
        check(::kill(child, SIGKILL) == 0 || errno == ESRCH, "terminate owned fixture during cleanup");
        do { result = ::waitpid(child, &status, 0); } while (result < 0 && errno == EINTR);
    }
    check(result == child, "reap exact owned fixture");
}

void refused(const QJsonObject &result, const char *message, const char *code = nullptr) {
    check(!result.value("ok").toBool() && !result.value("canTerminate").toBool(), message);
    check(!result.value("message").toString().isEmpty(), "refusal has user-facing explanation");
    if (code) {
        if (result.value("code").toString() != QLatin1String(code))
            std::fprintf(stderr, "unexpected refusal code: %s\n", qPrintable(result.value("code").toString()));
        check(result.value("code").toString() == QLatin1String(code), message);
    }
}

void unsupportedKernel(const QJsonObject &identity) {
    const pid_t child = ::fork();
    check(child >= 0, "fork old-kernel simulation");
    if (child == 0) {
#if defined(SYS_pidfd_send_signal)
        // Only this short-lived test child sees ENOSYS. This exercises the real
        // public path without a production switch that could bypass checks.
        struct sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_pidfd_send_signal, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
        };
        struct sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])), filter};
        check(::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
              ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0,
              "install child-only old-kernel simulation");
#endif
        ProcessActions actions;
        refused(actions.inspect(identity), "old kernel inspect is unavailable", "unsupportedKernel");
        refused(actions.execute(identity, "terminate"), "old kernel refuses termination", "unsupportedKernel");
        ::_exit(0);
    }
    int status = 0;
    check(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "old-kernel child checks pass");
}

void fixtures(const Process &target) {
    Process malformed;
    check(!parseStat("bad stat", malformed), "malformed stat fixture refused");
    check(!parseStatus("Uid:\t1000 1000\nTgid:\t1\n", malformed), "malformed ownership fixture refused");
    Identity parsed;
    auto invalid = jsonIdentity(target.identity);
    invalid["startTicks"] = 1.25;
    check(!parseIdentity(invalid, parsed), "fractional ticks fixture refused");
    invalid = jsonIdentity(target.identity);
    invalid["startTicks"] = 9007199254740992.0;
    check(!parseIdentity(invalid, parsed), "inexact ticks fixture refused");

    Process fixture = target;
    fixture.realUid = ::getuid() + 1;
    refused(validate(fixture.identity, fixture), "different real owner fixture refused", "notOwned");
    fixture = target;
    fixture.savedUid = 0;
    refused(validate(fixture.identity, fixture), "saved root UID fixture refused", "notOwned");
    fixture = target;
    fixture.directoryUid = ::getuid() + 1;
    refused(validate(fixture.identity, fixture), "proc ownership fixture refused", "notOwned");
    fixture = target;
    fixture.identity.executable = "/usr/bin/bash";
    fixture.executableTrusted = true;
    refused(validate(fixture.identity, fixture), "trusted shell fixture protected", "protectedProcess");
    fixture.identity.executable = "/usr/lib/ukui/ukui-session-manager";
    refused(validate(fixture.identity, fixture), "trusted session fixture protected", "protectedProcess");
    fixture.identity.executable = "/usr/bin/dbus-daemon";
    refused(validate(fixture.identity, fixture), "trusted DBus fixture protected", "protectedProcess");
    fixture.identity.executable = "/home/example/bash";
    check(!importantSession(fixture), "basename alone does not classify system process");
    fixture.identity.executable = "/usr/bin/bash";
    fixture.executableTrusted = false;
    check(!importantSession(fixture), "untrusted executable not classified by path alone");
    fixture = target;
    fixture.state = 'Z';
    refused(validate(fixture.identity, fixture), "zombie fixture refused", "exited");
}

int runTests(pid_t target, pid_t sentinel) {
    check(::getuid() != 0 && ::getuid() == ::geteuid(), "tests run as ordinary non-elevated user");
    ProcessActions actions;
    const Process process = sample(target, true);
    const auto identity = jsonIdentity(process.identity);
    fixtures(process);
    refused(actions.execute(identity, "lowerPriority"), "priority adjustment is not offered", "unsupportedAction");
    check(actions.isRunning(identity), "exact live identity is running");
    auto otherStart = identity; otherStart["startTicks"] = identity.value("startTicks").toDouble() + 1;
    check(!actions.isRunning(otherStart), "same PID with another start time is not this process");
    refused(actions.execute({}, "terminate"), "missing identity refused", "invalidIdentity");
    auto wrong = identity;
    wrong["pid"] = "123";
    refused(actions.execute(wrong, "terminate"), "string PID refused", "invalidIdentity");
    wrong = identity;
    wrong["pid"] = 1;
    refused(actions.execute(wrong, "terminate"), "PID one refused", "invalidIdentity");
    wrong["pid"] = -target;
    refused(actions.execute(wrong, "terminate"), "process-group PID refused", "invalidIdentity");
    wrong = identity;
    wrong["startTicks"] = identity.value("startTicks").toDouble() + 1;
    refused(actions.execute(wrong, "terminate"), "PID reuse identity refused", "staleIdentity");
    wrong = identity;
    wrong["executablePath"] = "/usr/bin/definitely-not-the-sampled-executable";
    refused(actions.execute(wrong, "terminate"), "executable mismatch refused", "executableChanged");
    refused(actions.execute(jsonIdentity(sample(::getpid()).identity), "terminate"), "self protected", "protectedProcess");
    refused(actions.execute(jsonIdentity(sample(::getppid()).identity), "terminate"), "ancestor protected", "protectedProcess");

    const pid_t descendant = sleeper();
    const auto descendantIdentity = jsonIdentity(sample(descendant, true).identity);
    refused(actions.execute(descendantIdentity, "terminate"), "monitor helper descendant protected", "protectedProcess");
    check(alive(descendant), "protected descendant survives refusal");
    cleanupOwnedChild(descendant);
    refused(actions.execute(descendantIdentity, "terminate"), "disappeared identity refused");

    const auto sentinelIdentity = jsonIdentity(sample(sentinel, true).identity);
    unsupportedKernel(sentinelIdentity);
    check(alive(sentinel) && alive(target), "refusals and old-kernel simulation leave fixtures alive");

    const auto inspection = actions.inspect(identity);
    if (inspection.value("code").toString() == "unsupportedKernel") {
        refused(actions.execute(identity, "terminate"), "actual old kernel refuses signal", "unsupportedKernel");
        check(alive(target), "unsupported kernel leaves target alive");
        std::puts("PASS: refusal/policy/old-kernel checks; live signal unavailable on this kernel");
        return 0;
    }
    check(inspection.value("ok").toBool() && inspection.value("canTerminate").toBool(), "owned sibling is actionable");
    check(alive(target), "inspection sends no termination");
    const auto result = actions.execute(identity, "terminate");
    check(result.value("ok").toBool() && result.value("signal").toInt() == SIGTERM &&
          result.value("code").toString() == "requested", "only SIGTERM is requested");
    for (int retry = 0; retry < 200 && alive(target); ++retry) QThread::msleep(5);
    check(!alive(target), "selected owned test sibling exits after SIGTERM");
    refused(actions.execute(identity, "terminate"), "exited identity refused");
    check(alive(sentinel), "unselected sentinel survives successful termination");
    const auto stubbornIdentity = jsonIdentity(sample(stubborn, true).identity);
    check(actions.execute(stubbornIdentity, "terminate").value("code") == "requested", "SIGTERM is delivered to the stubborn fixture");
    QThread::msleep(100);
    check(alive(stubborn) && actions.isRunning(stubbornIdentity), "a program can ignore SIGTERM");
    const auto killed = actions.execute(stubbornIdentity, "kill");
    check(killed.value("ok").toBool() && killed.value("code") == "killed" && killed.value("signal").toInt() == SIGKILL, "SIGKILL uses the same verified handle");
    // /proc/PID/exe disappears slightly before the exiting task becomes a zombie.
    for (int retry = 0; retry < 400 && (alive(stubborn) || actions.isRunning(stubbornIdentity)); ++retry) QThread::msleep(5);
    check(!alive(stubborn) && !actions.isRunning(stubbornIdentity), "SIGKILL ends it and isRunning agrees");
    refused(actions.execute(stubbornIdentity, "kill"), "exited identity refused for SIGKILL too");
    refused(actions.execute(jsonIdentity(sample(::getpid()).identity), "kill"), "self protected from SIGKILL", "protectedProcess");
    check(alive(sentinel), "unselected sentinel survives SIGKILL");
    std::puts("PASS: native identity/ownership/ancestry/protection/refusal/old-kernel/SIGTERM/SIGKILL checks");
    return 0;
}
}

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    const pid_t target = sleeper(), sentinel = sleeper();
    stubborn = sleeper(true);
    const pid_t tester = ::fork();
    check(tester >= 0, "fork test runner with sibling fixtures");
    if (tester == 0) {
        const int result = runTests(target, sentinel);
        std::fflush(stdout);
        ::_exit(result);
    }
    int status = 0;
    pid_t result;
    do { result = ::waitpid(tester, &status, 0); } while (result < 0 && errno == EINTR);
    cleanupOwnedChild(target);
    cleanupOwnedChild(sentinel);
    cleanupOwnedChild(stubborn);
    check(result == tester && WIFEXITED(status), "test runner exited normally");
    if (WEXITSTATUS(status) != 0) return WEXITSTATUS(status);
    std::puts("PASS: all exact test-owned child PIDs reaped; no fixture processes remain");
    return 0;
}
