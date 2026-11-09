#!/usr/bin/env python3
"""Build tests against the production objects and run each in its own environment.

Tests are plain programs: exit status 0 and a final PASS line mean success. Objects
from a qmake build directory are reused, so a test costs one compile and one link.
A test that #includes a production .cpp (to reach private helpers) replaces that object.

  --mode native  run directly (fast iteration; needs a native Qt build directory)
  --mode arm     cross-compiled objects under qemu-aarch64, as in the release check

Suites that need a real X server run only with --display, and the window-closing
ones only natively: under qemu their target's /proc/PID/exe is qemu itself.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import select
import shlex
import subprocess
import sys

parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('--mode', choices=['native', 'arm'], required=True)
parser.add_argument('--build-dir', type=Path, required=True, help='Directory holding the qmake Makefile and objects')
parser.add_argument('--work-dir', type=Path, required=True, help='Where test objects, logs and evidence go')
parser.add_argument('--sdk', type=Path, required=True)
parser.add_argument('--native-runtime', type=Path, help='Private native Qt libraries and plugins (native mode)')
parser.add_argument('--timeout', type=int, default=300)
parser.add_argument('--display', help='X display for suites that need a real X server; without it they are skipped')
parser.add_argument('suites', nargs='*', help='Default: every tests/test_*.cpp')
args = parser.parse_args()

source = Path(__file__).resolve().parents[1]
build = args.build_dir.resolve()
work = args.work_dir.resolve()
sdk = args.sdk.resolve()
work.mkdir(parents=True, exist_ok=True)
makefile = (build / 'Makefile').read_text()
variables = {name: re.search(r'^' + name + r'\s*=\s*(.*)$', makefile, re.M).group(1)
             for name in ['CXX', 'CXXFLAGS', 'DEFINES', 'INCPATH', 'LFLAGS', 'LIBS', 'LINK']}


def expand(text):
    for key, value in variables.items():
        text = text.replace('$(' + key + ')', value)
    return re.sub(r'\$\([^)]+\)', '', text)


if args.mode == 'arm':
    tool_env = dict(os.environ, PATH=str(sdk / 'host/usr/bin') + ':' + os.environ['PATH'], LD_LIBRARY_PATH=str(sdk / 'host-runtime'))
    headers = sdk / 'sysroot/usr/include/aarch64-linux-gnu/qt5'
    test_library = sdk / 'sysroot/usr/lib/aarch64-linux-gnu/libQt5Test.so'
else:
    tool_env = dict(os.environ, LD_LIBRARY_PATH=f'{args.native_runtime}:{sdk / "host-runtime"}')
    headers = sdk / 'host/usr/include/x86_64-linux-gnu/qt5'
    test_library = sdk / 'host/usr/lib/x86_64-linux-gnu/libQt5Test.so'

# These talk to a real X server and to fixtures started by their own harness.
needs_display = {'application_actions', 'relief_controller'}
# Emulation distorts real timing, and old qemu cannot forward pidfd_send_signal.
native_only = {'live_diagnosis', 'process_actions', 'process_rules'}
x11_library = sdk / 'sysroot/usr/lib/aarch64-linux-gnu/libX11.so' if args.mode == 'arm' else args.native_runtime / 'libX11.so.6'
objects = [p for p in sorted(build.glob('*.o')) if p.name != 'main.o']
assert objects, 'Build the production project first'
newest = max(p.stat().st_mtime for p in objects)
suites = args.suites or sorted(p.stem[len('test_'):] for p in (source / 'tests').glob('test_*.cpp'))
results = []
for suite in suites:
    cpp = source / 'tests' / f'test_{suite}.cpp'
    assert cpp.exists(), suite
    log = work / f'{suite}-build.log'
    binary, object_file = work / f'test_{suite}', work / f'{suite}.o'
    text = cpp.read_text()
    embedded = {m + '.o' for m in re.findall(r'#include "\.\./src/(\w+)\.cpp"', text)}
    if not binary.exists() or binary.stat().st_mtime < max(newest, cpp.stat().st_mtime):
        with log.open('w') as stream:
            compile_args = shlex.split(expand(variables['CXX'])) + shlex.split(expand(variables['CXXFLAGS'])) + shlex.split(expand(variables['INCPATH'])) + \
                [f'-I{source / "src"}', f'-I{headers / "QtTest"}', '-DQT_TESTLIB_LIB', '-Wno-deprecated-copy', '-c', str(cpp), '-o', str(object_file)]
            done = subprocess.run(compile_args, env=tool_env, cwd=build, stdout=stream, stderr=subprocess.STDOUT)
            assert done.returncode == 0, (suite, log.read_text()[-3000:])
            link = shlex.split(expand(variables['LINK'])) + shlex.split(expand(variables['LFLAGS'])) + ['-o', str(binary), str(object_file)] + \
                [str(p) for p in objects if p.name not in embedded and p.name != 'moc_predefs.o'] + shlex.split(expand(variables['LIBS'])) + [str(test_library), str(x11_library)]
            done = subprocess.run(link, env=tool_env, cwd=build, stdout=stream, stderr=subprocess.STDOUT)
            assert done.returncode == 0, (suite, log.read_text()[-3000:])
    if (suite in needs_display and (not args.display or args.mode == 'arm')) or (suite in native_only and args.mode == 'arm'):
        results.append(dict(suite=suite, result='SKIP', code=None, logSha256=None, last=['needs --display and native mode']))
        print(json.dumps(results[-1], ensure_ascii=False), flush=True)
        continue
    root = work / f'{suite}-run'
    if root.exists():
        subprocess.run(['rm', '-rf', str(root)], check=True)
    for name in ['runtime', 'config', 'data', 'cache', 'font-cache', 'evidence']:
        (root / name).mkdir(mode=0o700, parents=True)
    fonts = root / 'fonts.conf'
    fonts.write_text(f'<?xml version="1.0"?><fontconfig><dir>/usr/share/fonts</dir><dir>{sdk / "host/usr/share/fonts"}</dir><cachedir>{root / "font-cache"}</cachedir></fontconfig>\n')
    clean = {k: v for k, v in os.environ.items() if not k.startswith(('DEVSTATS_', 'XDG_')) and
             k not in ['LD_LIBRARY_PATH', 'LD_PRELOAD', 'QT_PLUGIN_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORMTHEME', 'QML2_IMPORT_PATH', 'DISPLAY']}
    target = dict(QT_QPA_PLATFORM='xcb' if suite in needs_display else 'offscreen', FONTCONFIG_FILE=str(fonts), XDG_RUNTIME_DIR=str(root / 'runtime'),
                  XDG_CONFIG_HOME=str(root / 'config'), XDG_DATA_HOME=str(root / 'data'), XDG_CACHE_HOME=str(root / 'cache'),
                  DEVSTATS_DATA_DIR=str(root / 'data/devstats'), DEVSTATS_DISABLE_PROBES='1', DEVSTATS_TEST_EVIDENCE=str(root / 'evidence'), TZ='Asia/Shanghai')
    if suite in needs_display:
        target['DISPLAY'] = args.display
    if args.mode == 'arm':
        library_dirs = f'{sdk / "sysroot/usr/lib/aarch64-linux-gnu"}:{sdk / "sysroot/lib/aarch64-linux-gnu"}'
        target.update(LD_LIBRARY_PATH=library_dirs, QT_PLUGIN_PATH=str(sdk / 'sysroot/usr/lib/aarch64-linux-gnu/qt5/plugins'))
        command = [str(sdk / 'host/usr/bin/qemu-aarch64-static'), '-L', str(sdk / 'sysroot')]
        for key, value in target.items():
            command += ['-E', f'{key}={value}']
        command += [str(binary)]
        environment = clean
    else:
        target.update(LD_LIBRARY_PATH=f'{args.native_runtime}:{sdk / "host-runtime"}', QT_PLUGIN_PATH=str(args.native_runtime / 'plugins'))
        command, environment = [str(binary)], dict(clean, **target)
    if suite in ('tray_icons', 'ui_preview'):
        command.append(str(root / 'evidence'))
    # The relief test closes real windows of a sibling process it did not start.
    fixture = None
    if suite == 'relief_controller':
        copy = root / 'target-fixture'
        copy.write_bytes((work / 'test_application_actions').read_bytes())
        copy.chmod(0o755)
        fixture = subprocess.Popen([str(copy), '--fixture'], env=environment, cwd=root, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
        assert select.select([fixture.stdout], [], [], 20)[0], 'window fixture did not start'
        assert fixture.stdout.readline().strip() == 'READY'
        fixture.stdin.write('add\n'); fixture.stdin.flush()  # A second window to close.
        assert fixture.stdout.readline().strip() == 'OK'
        environment = dict(environment, DEVSTATS_RELIEF_FIXTURE_PID=str(fixture.pid))
    run_log = root / 'run.log'
    with run_log.open('w') as stream:
        try:
            done = subprocess.run(command, env=environment, cwd=root, stdout=stream, stderr=subprocess.STDOUT, timeout=args.timeout)
            code = done.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    if fixture:
        fixture.kill()
        fixture.wait()
    lines = run_log.read_text(errors='replace').splitlines()
    results.append(dict(suite=suite, result='PASS' if code == 0 else 'FAIL', code=code, logSha256=hashlib.sha256(run_log.read_bytes()).hexdigest(), last=lines[-3:]))
    print(json.dumps(results[-1], ensure_ascii=False), flush=True)
failed = [r['suite'] for r in results if r['result'] == 'FAIL']
(work / 'summary.json').write_text(json.dumps(dict(result='FAIL' if failed else 'PASS', mode=args.mode, failed=failed, runs=results), ensure_ascii=False, indent=2) + '\n')
skipped = [r['suite'] for r in results if r['result'] == 'SKIP']
print(json.dumps(dict(result='FAIL' if failed else 'PASS', passed=len(results) - len(failed) - len(skipped), failed=failed, skipped=skipped), ensure_ascii=False))
sys.exit(1 if failed else 0)
