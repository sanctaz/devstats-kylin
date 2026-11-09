#!/usr/bin/env python3
"""Run as an ordinary Linux user; all writes stay in a temporary directory."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parent.parent

def run(path, env, success=True):
    result = subprocess.run([str(path)], env=env, text=True, capture_output=True)
    if (result.returncode == 0) != success:
        raise AssertionError((path.name, result.returncode, result.stdout, result.stderr))
    return result

def sums(bundle):
    (bundle / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(bundle)) + '\n'
        for p in sorted(bundle.rglob('*')) if p.is_file() and not p.is_symlink() and p.name != 'SHA256SUMS'))

def autostart_entry(launcher):
    """The login entry exactly as the application writes it (AppController::setAutoStart)."""
    quoted = ''.join('%%' if c == '%' else ('\\' + c if c in '\\"`$' else c) for c in str(launcher))
    quoted = quoted.replace('\\', '\\\\')
    return ('[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- "' + quoted +
            '" --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n')

def main():
    assert os.geteuid() != 0, 'Run installer tests as a non-root Linux user'
    with tempfile.TemporaryDirectory(prefix='devstats-user-test-') as temporary:
        root = Path(temporary)
        # Includes both shell and Desktop Entry special characters, not only spaces.
        home = root / '家目录 spaces $`"\\%'
        home.mkdir()
        package = root / '解压 package'
        (package / 'libexec').mkdir(parents=True)
        (package / 'share').mkdir()
        shutil.copy2(PROJECT / 'packaging/user/install-user.sh', package)
        shutil.copy2(PROJECT / 'packaging/user/uninstall-user.sh', package)
        shutil.copy2(PROJECT / 'packaging/user/devstats', package / 'devstats')
        (package / 'VERSION').write_text('0.7.1\n')
        (package / '.devstats-user-package').write_text('DevStats private Qt runtime v1\n')
        (package / 'share/devstats.svg').write_text('<svg/>')
        (package / 'libexec/devstats').write_text('#!/usr/bin/env python3\nimport json,os,sys\nprint(json.dumps(dict(args=sys.argv[1:],env={k:v for k,v in os.environ.items() if k.startswith("DEVSTATS_ORIGINAL_") or k in ["LD_LIBRARY_PATH","QT_PLUGIN_PATH","QT_QPA_PLATFORM_PLUGIN_PATH","QT_QPA_PLATFORMTHEME","QML2_IMPORT_PATH","DEVSTATS_LAUNCHER_PATH"]})))\n')
        (package / 'libexec/devstats').chmod(0o755)
        sums(package)
        env = dict(os.environ, HOME=str(home), XDG_DATA_HOME=str(home / 'XDG data'),
                   XDG_CONFIG_HOME=str(home / 'XDG config'), XDG_CACHE_HOME=str(home / 'XDG cache'),
                   DEVSTATS_USER_BIN_DIR=str(home / '命令 bin'))
        for key in ('DEVSTATS_USER_INSTALL_DIR', 'LD_LIBRARY_PATH', 'QT_PLUGIN_PATH',
                    'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORMTHEME', 'QML2_IMPORT_PATH'):
            env.pop(key, None)
        # Exercise the root guard without sudo or any privileged execution.
        guard = root / 'mock-id'; guard.mkdir()
        (guard / 'id').write_text('#!/bin/sh\nprintf \'0\\n\'\n')
        (guard / 'id').chmod(0o755)
        result = run(package / 'install-user.sh', dict(env, PATH=str(guard) + ':' + env['PATH']), success=False)
        assert 'without sudo' in result.stderr
        assert not (home / '.local/opt').exists()
        # First install and idempotent install, exact user-local targets.
        run(package / 'install-user.sh', env)
        installed = home / '.local/opt/devstats-0.7.1'
        commands = Path(env['DEVSTATS_USER_BIN_DIR'])
        assert installed.is_dir()
        assert (commands / 'devstats').resolve() == installed / 'devstats'
        assert (commands / 'devstats-uninstall').resolve() == installed / 'uninstall-user.sh'
        desktop = Path(env['XDG_DATA_HOME']) / 'applications/devstats.desktop'
        before = desktop.read_bytes()
        run(package / 'install-user.sh', env)
        assert desktop.read_bytes() == before
        # Freedesktop's first string unescape produces doubled backslashes for Exec.
        exec_value = next(x[5:] for x in desktop.read_text().splitlines() if x.startswith('Exec='))
        assert '\\$' in exec_value and '%%' in exec_value and '\\"' in exec_value
        assert not (Path(env['XDG_CONFIG_HOME']) / 'autostart').exists()
        # GLib parses and launches the actual Desktop Entry, exercising both escape passes.
        if shutil.which('gio'):
            launched = subprocess.run(['gio', 'launch', str(desktop)], env=env, text=True, capture_output=True, timeout=15)
            assert launched.returncode == 0, (launched.stderr, desktop.read_text(), str(installed / 'devstats'), (installed / 'devstats').is_file())
            assert json.loads(launched.stdout.splitlines()[-1])['args'] == ['--show']
        # Wrapper forwards arguments literally, preserves unset vs empty, isolates itself.
        env['LD_LIBRARY_PATH'] = ''
        env['QT_PLUGIN_PATH'] = '/caller plugins'
        special = ['--name', '含 空格 $`"\\%', 'argument']
        probe = subprocess.run([str(commands / 'devstats'), *special], env=env, text=True, capture_output=True, check=True)
        data = json.loads(probe.stdout)
        assert data['args'] == special
        assert data['env']['DEVSTATS_ORIGINAL_LD_LIBRARY_PATH_SET'] == '1'
        assert data['env']['DEVSTATS_ORIGINAL_LD_LIBRARY_PATH'] == ''
        assert data['env']['DEVSTATS_ORIGINAL_QT_PLUGIN_PATH'] == '/caller plugins'
        assert data['env']['DEVSTATS_ORIGINAL_QML2_IMPORT_PATH_SET'] == '0'
        assert data['env']['LD_LIBRARY_PATH'] == str(installed / 'lib')
        assert data['env']['DEVSTATS_LAUNCHER_PATH'] == str(installed / 'devstats')
        assert 'QML2_IMPORT_PATH' not in data['env'] and 'QT_QPA_PLATFORMTHEME' not in data['env']
        assert env['LD_LIBRARY_PATH'] == ''  # Parent environment was untouched.
        # Source or installed-file changes are never silently overwritten.
        (installed / 'share/devstats.svg').write_text('modified')
        run(package / 'install-user.sh', env, success=False)
        assert (installed / 'share/devstats.svg').read_text() == 'modified'
        # Upgrade preserves old directory and only switches recognized managed links.
        # Login startup turned on in the old version follows the upgrade; otherwise the
        # next login would start the old version.
        autostart = Path(env['XDG_CONFIG_HOME']) / 'autostart/devstats.desktop'
        autostart.parent.mkdir(parents=True)
        autostart.write_text(autostart_entry(installed / 'devstats')); autostart.chmod(0o600)
        (package / 'VERSION').write_text('0.7.2\n'); sums(package)
        upgrade = run(package / 'install-user.sh', env)
        upgraded = home / '.local/opt/devstats-0.7.2'
        assert installed.is_dir() and upgraded.is_dir()
        assert (commands / 'devstats').resolve() == upgraded / 'devstats'
        assert autostart.read_text() == autostart_entry(upgraded / 'devstats'), autostart.read_text()
        assert autostart.stat().st_mode & 0o777 == 0o600
        assert 'Login startup now opens DevStats 0.7.2 instead of 0.7.1' in upgrade.stdout, upgrade.stdout
        # An entry the user edited is theirs and stays as it is.
        edited = autostart_entry(installed / 'devstats') + 'Hidden=true\n'
        autostart.write_text(edited)
        run(package / 'install-user.sh', env)
        assert autostart.read_text() == edited
        autostart.unlink()
        # An unrelated CLI, icon, desktop entry or installation directory is protected.
        protected = root / 'protected'; protected.mkdir()
        for kind in ('command', 'icon', 'desktop', 'directory'):
            case = protected / kind; case.mkdir()
            isolated = dict(env, DEVSTATS_USER_BIN_DIR=str(case / 'bin'), XDG_DATA_HOME=str(case / 'data'),
                            DEVSTATS_USER_INSTALL_DIR=str(case / 'install'))
            target = {'command': case / 'bin/devstats', 'icon': case / 'data/icons/hicolor/scalable/apps/devstats.svg',
                      'desktop': case / 'data/applications/devstats.desktop', 'directory': case / 'install'}[kind]
            target.parent.mkdir(parents=True, exist_ok=True)
            if kind == 'directory': target.mkdir(); (target / 'unrelated').write_text('keep')
            else: target.write_text('unrelated')
            run(package / 'install-user.sh', isolated, success=False)
            assert (target / 'unrelated').read_text() == 'keep' if kind == 'directory' else target.read_text() == 'unrelated'
        # 0.8 has no devmon: an old managed link goes, and a user's own command stays.
        legacy = root / 'legacy devmon'; legacy.mkdir()
        (legacy / 'devmon').symlink_to(installed / 'devstats')
        run(package / 'install-user.sh', dict(env, DEVSTATS_USER_BIN_DIR=str(legacy)))
        assert not (legacy / 'devmon').is_symlink()
        foreign = root / 'foreign devmon'; foreign.mkdir()
        (foreign / 'devmon').write_text('my own tool')
        run(package / 'install-user.sh', dict(env, DEVSTATS_USER_BIN_DIR=str(foreign)))
        assert (foreign / 'devmon').read_text() == 'my own tool'
        # Broken or unrelated symbolic links must also survive unchanged.
        broken_dir = root / 'broken'; broken_dir.mkdir()
        (broken_dir / 'devstats').symlink_to('/nonexistent/unrelated')
        run(package / 'install-user.sh', dict(env, DEVSTATS_USER_BIN_DIR=str(broken_dir)), success=False)
        assert os.readlink(broken_dir / 'devstats') == '/nonexistent/unrelated'
        # Relative paths and corrupt source payload fail before creating an install.
        run(package / 'install-user.sh', dict(env, DEVSTATS_USER_INSTALL_DIR='relative'), success=False)
        (package / 'share/devstats.svg').write_text('corrupt source')
        run(package / 'install-user.sh', dict(env, DEVSTATS_USER_INSTALL_DIR=str(root / 'corrupt')), success=False)
        assert not (root / 'corrupt').exists()
        print('PASS: user install/idempotence/upgrade/old-version retention, Unicode+spaces+shell escaping, unrelated target protection, checksum rejection, wrapper argument+environment isolation; login startup follows an upgrade but is never created; no privileged writes.')

if __name__ == '__main__':
    main()
