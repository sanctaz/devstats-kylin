#!/usr/bin/env python3
"""A real upgrade from an earlier user package, in an isolated prefix under QEMU.

Everything a 0.7.x user has must survive: settings, old rules, the action log, the
minute history, recorded stalls, and even the data of the features 0.8 removed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import sqlite3
import subprocess
import time

parser = argparse.ArgumentParser(description=__doc__)
for option in ('old', 'new', 'qemu', 'sysroot', 'work'):
    parser.add_argument('--' + option, type=Path, required=True)
parser.add_argument('--incidents', type=Path, help='An incidents.sqlite3 recorded by an earlier version')
a = parser.parse_args()
assert os.geteuid() != 0
a.work.mkdir(parents=True)
work = a.work.resolve()
prefix = work / 'prefix 用户'
prefix.mkdir()
config, data, cache, runtime = prefix / 'config', prefix / 'share', prefix / 'cache', prefix / 'runtime'
runtime.mkdir(mode=0o700)
clean = {k: v for k, v in os.environ.items() if not k.startswith(('DEVSTATS_', 'XDG_')) and k not in ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH')}
checks = []


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def script(path, *arguments):
    result = subprocess.run(['bash', str(path), *map(str, arguments)], env=clean, text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, (path, arguments, result.stdout[-1500:], result.stderr[-1500:])
    return result.stdout


def target(bundle, *arguments):
    environment = dict(LD_LIBRARY_PATH=f'{bundle}/lib', QT_PLUGIN_PATH=f'{bundle}/plugins', QT_QPA_PLATFORM='offscreen',
                       DEVSTATS_LAUNCHER_PATH=f'{bundle}/devstats', DEVSTATS_DISABLE_PROBES='1', XDG_CONFIG_HOME=str(config),
                       XDG_DATA_HOME=str(data), XDG_CACHE_HOME=str(cache), XDG_RUNTIME_DIR=str(runtime))
    command = [str(a.qemu), '-L', str(a.sysroot)]
    for key, value in environment.items():
        command += ['-E', f'{key}={value}']
    return command + [f'{bundle}/libexec/devstats', *arguments]


def snapshot(bundle):
    result = subprocess.run(target(bundle, '--snapshot'), env=clean, text=True, capture_output=True, timeout=120)
    assert result.returncode == 0, (result.stdout[-800:], result.stderr[-800:])
    return json.loads(result.stdout.splitlines()[-1])


def autostart_entry(launcher):
    """The login entry exactly as the application writes it (AppController::setAutoStart)."""
    quoted = ''.join('%%' if c == '%' else ('\\' + c if c in '\\"`$' else c) for c in str(launcher))
    quoted = quoted.replace('\\', '\\\\')
    return ('[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- "' + quoted +
            '" --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n')


# A user of the previous version, with data from everything including removed features.
script(a.old / 'install-user.sh', '--prefix', prefix)
assert (prefix / 'bin/devmon').is_symlink(), 'the old version really had devmon'
before = snapshot(a.old)
assert 'ollama' in before and 'tasks' in before, 'the old version really had these'
rule = dict(id='old', enabled=False, match='path', value='/opt/not-installed/helper')
(config / 'devstats').mkdir(parents=True, exist_ok=True)
settings = config / 'devstats/settings.ini'
escaped = json.dumps([rule]).replace('\\', '\\\\').replace('"', '\\"')
settings.write_text('[General]\nollamaEndpoint=http://example.invalid:11434\ntrayMetrics=cpu, ollama\nappearance=2\n'
                    'automationEnabled=false\nautomationRules="@ByteArray(' + escaped + ')"\nunrelated=keep\n')
root = data / 'devstats'
kept = {root / 'Tasks/old-task.json': b'{"id":"old-task","status":"succeeded"}', root / 'Launchers/1.sh': b'#!/bin/sh\n',
        root / 'user notes.txt': b'keep'}
for path, value in kept.items():
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(value)
log = root / 'process-rules-log.json'
log.write_text(json.dumps([dict(timestamp=1770000000.0, ok=False, code='protectedProcess', message='old row', ruleValue='x', executablePath='/usr/bin/x', pid=1)]))
log.chmod(0o600)
stalls = 0
if a.incidents:
    (root / 'Incidents').mkdir(mode=0o700, exist_ok=True)
    shutil.copy2(a.incidents, root / 'Incidents/incidents.sqlite3')
    (root / 'Incidents/incidents.sqlite3').chmod(0o600)
    with sqlite3.connect(root / 'Incidents/incidents.sqlite3') as connection:
        stalls = connection.execute('SELECT count(*) FROM incidents').fetchone()[0]
history = root / 'History/metrics.sqlite3'
with sqlite3.connect(history) as connection:
    rows = connection.execute('SELECT count(*) FROM minute_metrics').fetchone()[0]
assert rows > 0
hashes = {path: sha(path) for path in [*kept, log]}
old_version = (a.old / 'VERSION').read_text().strip()
# Login startup turned on in the old version, written the way that version writes it.
autostart = config / 'autostart/devstats.desktop'
autostart.parent.mkdir(parents=True, exist_ok=True)
autostart.write_text(autostart_entry(prefix / f'opt/devstats-{old_version}/devstats')); autostart.chmod(0o600)

installed = script(a.new / 'install-user.sh', '--prefix', prefix)
version = (a.new / 'VERSION').read_text().strip()
assert (prefix / 'bin/devstats').resolve() == prefix / f'opt/devstats-{version}/devstats', 'the command now runs the new version'
assert not (prefix / 'bin/devmon').exists() and not (prefix / 'bin/devmon').is_symlink(), 'the devmon link is gone'
assert not (a.new / 'devmon').exists() and not (a.new / 'libexec/devmon').exists()
assert (prefix / f'opt/devstats-{(a.old / "VERSION").read_text().strip()}').is_dir(), 'the old version stays installed'
assert autostart.read_text() == autostart_entry(prefix / f'opt/devstats-{version}/devstats'), autostart.read_text()
assert f'Login startup now opens DevStats {version} instead of {old_version}' in installed, installed
checks.append('install switches the command and login startup to the new version, removes devmon; the old version stays')

after = snapshot(a.new)
assert after['version'] == version and 'ollama' not in after and 'tasks' not in after
assert 'systemProcesses' in after and 'thermal' in after and after['diagnostics']['state'] in ('normal', 'attention', 'critical', 'unknown')
with sqlite3.connect(history) as connection:
    now = connection.execute('SELECT count(*) FROM minute_metrics').fetchone()[0]
assert now >= rows, 'the existing history database keeps its rows'
checks.append('the new version runs on the old data directory and keeps writing the same history')

# The complete application starts with the old settings and leaves them intact.
application = subprocess.Popen(target(a.new, '--background'), env=clean, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
time.sleep(12)
assert application.poll() is None, application.stdout.read()[-1200:]
application.send_signal(signal.SIGTERM)
try:
    application.wait(timeout=30)
except subprocess.TimeoutExpired:
    application.kill(); application.wait()
    raise AssertionError('did not exit on SIGTERM')
assert application.returncode == 0, application.returncode
text = settings.read_text()
assert 'unrelated=keep' in text and 'not-installed' in text and 'appearance=2' in text, text
assert all(sha(path) == value for path, value in hashes.items()), 'data of removed features and the action log are untouched'
if a.incidents:
    with sqlite3.connect(root / 'Incidents/incidents.sqlite3') as connection:
        assert connection.execute('SELECT count(*) FROM incidents').fetchone()[0] == stalls, 'recorded stalls survive'
checks.append('the full application starts with old settings and rules; removed-feature data, action log and stalls are untouched')

# Uninstalling removes both versions and no data.
dry = script(a.new / 'uninstall-user.sh', '--prefix', prefix, '--dry-run')
assert 'Verified user installations to remove: 2' in dry, dry
assert f'Remove managed file/link: {autostart}' in dry, dry
script(a.new / 'uninstall-user.sh', '--prefix', prefix)
assert not list((prefix / 'opt').glob('devstats-*')) and not (prefix / 'bin/devstats').is_symlink() and not autostart.exists()
assert all(sha(path) == value for path, value in hashes.items()) and history.is_file() and settings.is_file()
checks.append('uninstall removes the old and new version and their login entry together, and keeps every data file')
print(json.dumps(dict(result='PASS', old=(a.old / 'VERSION').read_text().strip(), new=version, historyRows=rows, recordedStalls=stalls, checks=checks), ensure_ascii=False))
