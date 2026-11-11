#!/usr/bin/env python3
"""End to end: the real application, real /proc, real processes burning real CPU.

Usage: test_cpu_limit_e2e.py [RUNNER...] DEVSTATS_BINARY

Every target is a private copy of a small system program at a unique path, so the
rules cannot match anything else on the machine. Nothing but those copies is signalled.
"""
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import time

command = sys.argv[1:]
if not command:
    raise SystemExit(__doc__)
assert os.geteuid() != 0, 'Use an ordinary account'


def qt_ini(value):
    return '"@ByteArray(' + value.replace('\\', '\\\\').replace('"', '\\"') + ')"'


def until(predicate, seconds, message):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = predicate()
        if result:
            return result
        time.sleep(.2)
    raise AssertionError(message)


def alive(process):
    """Not exited, and not merely a zombie waiting for this script to reap it."""
    if process.poll() is not None:
        return False
    try:
        state = Path(f'/proc/{process.pid}/stat').read_text().rsplit(')', 1)[1].split()[0]
    except OSError:
        return False
    return state not in ('Z', 'X')


with tempfile.TemporaryDirectory(prefix='devstats-cpu-limit-') as temporary:
    root = Path(temporary)
    for name in ('config/devstats', 'data', 'cache', 'runtime', 'bin'):
        (root / name).mkdir(parents=True, mode=0o700)
    binaries = {}
    for name, source in (('burn-term', '/usr/bin/yes'), ('burn-stubborn', '/usr/bin/yes'), ('burn-kept', '/usr/bin/yes'),
                         ('burn-quiet', '/usr/bin/yes'), ('idle-always', '/bin/sleep'), ('idle-limited', '/bin/sleep')):
        binaries[name] = root / 'bin' / name
        shutil.copy2(source, binaries[name])

    def rule(name, limit=None, sustain=0, force=False, grace=4, enabled=True):
        result = dict(id=name, enabled=enabled, match='path', value=str(binaries[name]), forceKill=force, graceSeconds=grace)
        if limit is not None:
            result.update(cpuAbovePercent=limit, sustainSeconds=sustain)
        return result
    rules = [rule('burn-term', 50, 8), rule('burn-stubborn', 50, 8, force=True), rule('burn-kept', 50, 8),
             # A limit of 90 cores can never be reached by one single-threaded process.
             rule('burn-quiet', 9000, 0), rule('idle-always'), rule('idle-limited', 50, 0)]
    (root / 'config/devstats/settings.ini').write_text(
        '[General]\nautomationEnabled=true\nautomationRules=' + qt_ini(json.dumps(rules)) + '\n')

    environment = {k: v for k, v in os.environ.items() if not k.startswith(('DEVSTATS_', 'XDG_')) and k != 'DISPLAY'}
    # The log is worded in the interface language, and the checks below read the Chinese wording.
    environment.update(LANG='zh_CN.UTF-8', LANGUAGE='zh_CN:zh')
    environment.pop('LC_ALL', None); environment.pop('LC_MESSAGES', None)
    environment.update(QT_QPA_PLATFORM='offscreen', XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'data'),
                       XDG_CACHE_HOME=str(root / 'cache'), XDG_RUNTIME_DIR=str(root / 'runtime'), DEVSTATS_DATA_DIR=str(root / 'data/devstats'))

    def burn(name, ignore_term=False):
        def prepare():
            os.setsid()
            if ignore_term:
                signal.signal(signal.SIGTERM, signal.SIG_IGN)
        return subprocess.Popen([str(binaries[name])], stdout=subprocess.DEVNULL, preexec_fn=prepare)

    def idle(name):
        return subprocess.Popen([str(binaries[name]), '300'], preexec_fn=os.setsid)
    started = time.monotonic()
    targets = {'burn-term': burn('burn-term'), 'burn-stubborn': burn('burn-stubborn', True), 'burn-kept': burn('burn-kept', True),
               'burn-quiet': burn('burn-quiet'), 'idle-always': idle('idle-always'), 'idle-limited': idle('idle-limited')}
    application = subprocess.Popen(command + ['--background'], env=environment, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log_path = root / 'data/devstats/process-rules-log.json'

    def log():
        try:
            return {row['ruleId']: row for row in json.loads(log_path.read_text())}
        except (OSError, ValueError):
            return {}
    try:
        # An unconditional rule acts on the first scans.
        row = until(lambda: log().get('idle-always'), 30, 'unconditional rule never acted')
        assert row['condition'] == '运行时立即结束', row
        until(lambda: not alive(targets['idle-always']), 10, 'unconditional target did not exit')
        unconditional = time.monotonic() - started

        # The default SIGTERM ends a normal process once its limit held long enough.
        row = until(lambda: log().get('burn-term'), 45, 'CPU rule never acted')
        assert row['triggerCpuPercent'] >= 50 and '50%' in row['condition'] and '8 秒' in row['condition'], row
        fired = time.monotonic() - started
        assert fired >= 8, ('fired before the sustain time', fired)
        until(lambda: not alive(targets['burn-term']), 10, 'SIGTERM did not end the normal burner')
        row = until(lambda: log().get('burn-term') if log().get('burn-term', {}).get('phase') == 'exited' else None, 15, 'exit was not recorded')
        assert row['ok'] is True, row

        # A program ignoring SIGTERM is killed only where the rule allows it.
        row = until(lambda: log().get('burn-stubborn') if log().get('burn-stubborn', {}).get('phase') == 'killed' else None, 45, 'escalation never finished')
        assert row['signal'] == signal.SIGKILL, row
        assert not alive(targets['burn-stubborn'])
        row = until(lambda: log().get('burn-kept') if log().get('burn-kept', {}).get('phase') == 'survived' else None, 45, 'unforced survivor was not reported')
        assert alive(targets['burn-kept']), 'a rule without force must not kill'

        # Below the limit nothing happens, whatever the process is doing.
        assert alive(targets['burn-quiet']), 'a burner under its limit was ended'
        assert alive(targets['idle-limited']), 'an idle process with a CPU rule was ended'
        assert 'burn-quiet' not in log() and 'idle-limited' not in log(), log()
        assert application.poll() is None, 'the application stayed up'

        # The log is private and survives a restart of the application.
        assert log_path.stat().st_mode & 0o777 == 0o600
        print(json.dumps(dict(result='PASS', unconditionalActedAfterSeconds=round(unconditional, 1), cpuRuleFiredAfterSeconds=round(fired, 1),
                              ended=['idle-always', 'burn-term', 'burn-stubborn (SIGKILL)'], keptRunning=['burn-kept (no force)', 'burn-quiet (under limit)', 'idle-limited (idle)']), ensure_ascii=False))
    finally:
        application.send_signal(signal.SIGTERM)
        try:
            application.wait(timeout=15)
        except subprocess.TimeoutExpired:
            application.kill()
            application.wait()
        for process in targets.values():
            if process.poll() is None:
                process.kill()
            process.wait()
