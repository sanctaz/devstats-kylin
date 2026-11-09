#!/usr/bin/env python3
"""QEMU checks for a fully extracted user bundle; never adds SDK Qt paths."""
import argparse
import json
import os
from pathlib import Path
import re
import sqlite3
import subprocess

KEYS = ('LD_LIBRARY_PATH', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH',
        'QT_QPA_PLATFORMTHEME', 'QML2_IMPORT_PATH')

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', required=True, type=Path)
    parser.add_argument('--qemu', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--work', required=True, type=Path)
    args = parser.parse_args()
    bundle = args.bundle.resolve(strict=True)
    args.work.mkdir(parents=True, exist_ok=True)
    work = args.work.resolve()
    for directory in ('runtime', 'data', 'config', 'cache'):
        (work / directory).mkdir(mode=0o700, exist_ok=True)
    env = dict(os.environ)
    for key in KEYS:
        env.pop(key, None)
    loader = args.sysroot / 'lib/ld-linux-aarch64.so.1'
    qemu = [str(args.qemu), '-L', str(args.sysroot)]
    manifest = json.loads((bundle / 'share/runtime-manifest.json').read_text())
    allowed = set(manifest['system_libraries'])
    elfs = [bundle / 'libexec/devstats'] + [bundle / row['path'] for row in manifest['files']]
    loads, foreign = 0, []
    loader_log = work / 'loader-paths.log'
    with loader_log.open('w') as log:
        for executable in elfs:
            result = subprocess.run(qemu + [str(loader), '--library-path', str(bundle / 'lib'), '--list', str(executable)],
                                    env=env, text=True, capture_output=True)
            log.write(str(executable.relative_to(bundle)) + '\n' + result.stdout + result.stderr + '\n')
            assert result.returncode == 0 and 'not found' not in result.stdout, (executable, result.stderr)
            for name, path in re.findall(r'^\s*(\S+) => (\S+)', result.stdout, re.M):
                loads += 1
                if Path(path).parent != bundle / 'lib' and Path(name).name not in allowed:
                    foreign.append((str(executable.relative_to(bundle)), name, path))
    assert not foreign, foreign
    assert not (bundle / 'lib/libssl.so.1.1').exists(), 'nothing in the application uses TLS'
    # Simulate wrapper env exactly; qemu receives no SDK Qt/plugin library path.
    private = dict(LD_LIBRARY_PATH=str(bundle / 'lib'), QT_PLUGIN_PATH=str(bundle / 'plugins'),
                   QT_QPA_PLATFORM_PLUGIN_PATH=str(bundle / 'plugins/platforms'),
                   DEVSTATS_LAUNCHER_PATH=str(bundle / 'devstats'),
                   QT_QPA_PLATFORM='offscreen', QT_DEBUG_PLUGINS='1',
                   DEVSTATS_DATA_DIR=str(work / 'data'), XDG_CONFIG_HOME=str(work / 'config'),
                   XDG_DATA_HOME=str(work / 'data'), XDG_CACHE_HOME=str(work / 'cache'),
                   XDG_RUNTIME_DIR=str(work / 'runtime'))
    for key in KEYS:
        private['DEVSTATS_ORIGINAL_' + key] = ''
        private['DEVSTATS_ORIGINAL_' + key + '_SET'] = '0'
    run_args = qemu[:]
    for name, value in private.items():
        run_args += ['-E', name + '=' + value]
    def execute(name, arguments):
        result = subprocess.run(run_args + [str(bundle / 'libexec/devstats'), *arguments],
                                env=env, text=True, capture_output=True, timeout=60)
        (work / (name + '.log')).write_text(result.stdout + result.stderr)
        assert result.returncode == 0, (name, result.returncode, result.stdout[-1000:], result.stderr[-1000:])
        return result.stdout
    snapshot = json.loads(execute('snapshot', ['--snapshot']).splitlines()[-1])
    assert snapshot['platform'] == 'linux'
    assert snapshot['version'] == (bundle / 'VERSION').read_text().strip()
    database = work / 'data/History/metrics.sqlite3'
    assert database.is_file()
    with sqlite3.connect('file:' + str(database) + '?mode=ro', uri=True) as connection:
        history_rows = connection.execute('SELECT count(*) FROM minute_metrics').fetchone()[0]
        assert history_rows > 0, 'Snapshot did not persist any SQLite history metrics'
    assert str(bundle / 'plugins/sqldrivers/libqsqlite.so') in (work / 'snapshot.log').read_text()
    execute('smoke', ['--smoke-test'])
    print(json.dumps(dict(result='PASS', runtime_elfs=len(elfs), dependency_resolutions=loads,
                          foreign_non_glibc_libraries=0, sqlite_history_rows=history_rows, snapshot_version=snapshot['version'],
                          loader_log=str(loader_log), checks=['snapshot+SQLite', 'offscreen smoke', 'all ELF loader paths private-or-glibc'])))

if __name__ == '__main__':
    main()
