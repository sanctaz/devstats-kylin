#!/usr/bin/env python3
"""All-version keep-data uninstall tests; only disposable non-root Linux fixtures."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

PROJECT = Path(__file__).resolve().parent.parent


def run(path, env, *args, success=True):
    result = subprocess.run([str(path), *args], env=env, text=True, capture_output=True, timeout=30)
    assert (result.returncode == 0) == success, (path, args, result.returncode, result.stdout, result.stderr)
    return result


def sums(bundle):
    (bundle / 'SHA256SUMS').write_text(''.join(
        hashlib.sha256(p.read_bytes()).hexdigest() + '  ' + str(p.relative_to(bundle)) + '\n'
        for p in sorted(bundle.rglob('*')) if p.is_file() and not p.is_symlink() and p.name != 'SHA256SUMS'))


def desktop_exec(value):
    escaped = ''.join('%%' if c == '%' else '\\' + c if c in '\\"`$' else c for c in str(value))
    return ('"' + escaped + '"').replace('\\', '\\\\').replace('\t', '\\t')


def autostart(bundle):
    return ('[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- '
            + desktop_exec(bundle / 'devstats')
            + ' --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n')


def tree_snapshot(root):
    return {str(p.relative_to(root)): ('link', os.readlink(p)) if p.is_symlink()
            else ('dir',) if p.is_dir() else ('file', p.read_bytes())
            for p in root.rglob('*')}


class Fixture:
    def __init__(self, root):
        self.root = root
        self.home = root / '家目录 spaces $`"\\%'
        self.home.mkdir(parents=True)
        self.env = {k: v for k, v in os.environ.items() if not k.startswith(('DEVSTATS_', 'XDG_'))}
        self.env['HOME'] = str(self.home)
        # Standalone means there is intentionally no VERSION next to this script.
        self.uninstaller = root / 'standalone-uninstall.sh'
        shutil.copy2(PROJECT / 'packaging/user/uninstall-user.sh', self.uninstaller)
        self.installs = []
        self.records = []
        self.retained = {}

    def install(self, version, parent=None, bin_dir=None, data=None, config=None, legacy=False, devmon=True):
        package = self.root / ('解压 archive ' + version)
        (package / 'libexec').mkdir(parents=True)
        (package / 'share').mkdir()
        for name in ('devstats', 'install-user.sh', 'uninstall-user.sh'):
            shutil.copy2(PROJECT / 'packaging/user' / name, package / name)
        # Installs from before 0.8 carry a devmon launcher and command link.
        if devmon: (package / 'devmon').symlink_to('devstats')
        (package / 'VERSION').write_text(version + '\n')
        (package / '.devstats-user-package').write_text('DevStats private Qt runtime v1\n')
        (package / 'share/devstats.svg').write_text('<svg/>')
        shutil.copy2('/bin/sleep', package / 'libexec/devstats')
        if devmon: os.link(package / 'libexec/devstats', package / 'libexec/devmon')
        sums(package)
        target = (parent or self.home / '.local/opt') / ('devstats-' + version)
        commands = bin_dir or self.home / '.local/bin'
        data = data or self.home / '.local/share'
        config = config or self.home / '.config'
        env = dict(self.env, DEVSTATS_USER_INSTALL_DIR=str(target), DEVSTATS_USER_BIN_DIR=str(commands),
                   XDG_DATA_HOME=str(data), XDG_CONFIG_HOME=str(config))
        run(package / 'install-user.sh', env)
        if devmon: (commands / 'devmon').symlink_to(target / 'devmon')
        if legacy:
            (target / '.devstats-user-paths').unlink()
            (target / 'uninstall-user.sh').unlink()
            manifest = target / 'SHA256SUMS'
            manifest.write_text(''.join(line for line in manifest.read_text().splitlines(keepends=True)
                                        if not line.endswith('  uninstall-user.sh\n')))
            (commands / 'devstats-uninstall').unlink()
        self.installs.append(target)
        self.records.append((target, commands, data, config))
        for base, relative in [(data, 'devstats/History/metrics.sqlite3'),
                               (data, 'devstats/History/metrics.sqlite3-wal'),
                               (data, 'devstats/History/metrics.sqlite3-shm'),
                               (data, 'devstats/Tasks/01234567-89ab-cdef-0123-456789abcdef.json'),
                               (data, 'devstats/Launchers/1720000000000.sh'),
                               (data, 'devstats/user notes.txt'), (config, 'devstats/settings.ini')]:
            path = base / relative; path.parent.mkdir(parents=True, exist_ok=True)
            if not path.exists(): path.write_bytes((version + ':' + relative).encode())
            self.retained[path] = path.read_bytes()
        return target

    def assert_data(self):
        assert all(p.read_bytes() == value for p, value in self.retained.items())

    def uninstall(self, *args, env=None, success=True, script=None):
        return run(script or self.uninstaller, env or self.env, *args, success=success)

    def assert_removed(self):
        assert all(not p.exists() for p in self.installs)
        self.assert_data()
        for _, commands, data, config in self.records:
            assert not any((commands / n).is_symlink() for n in ('devstats', 'devmon', 'devstats-uninstall'))
            assert not (data / 'applications/devstats.desktop').exists()
            assert not (data / 'icons/hicolor/scalable/apps/devstats.svg').is_symlink()
            assert not (config / 'autostart/devstats.desktop').exists()

    def standard_versions(self):
        for version in ('0.7.1', '0.7.2', '0.7.3', '0.7.4'):
            self.install(version, legacy=version in ('0.7.1', '0.7.2'))
        return self.installs


def main():
    assert os.geteuid() != 0 and Path('/proc/self/exe').exists(), 'Use an ordinary Linux account'
    count = 0
    with tempfile.TemporaryDirectory(prefix='devstats-uninstall-all-fixtures-') as temporary:
        root = Path(temporary)
        def fixture(name):
            nonlocal count
            count += 1
            return Fixture(root / name)

        f = fixture('all-standard-and-legacy')
        f.standard_versions()
        default_config = f.home / '.config'
        auto = default_config / 'autostart/devstats.desktop'; auto.parent.mkdir(parents=True)
        auto.write_text(autostart(f.installs[1]))  # old autostart, new menu and links.
        cache = f.home / '.cache/devstats/cache'; cache.parent.mkdir(parents=True); cache.write_bytes(b'cache')
        custom = f.root / 'custom data outside XDG'; custom.mkdir(); (custom / 'keep').write_bytes(b'custom')
        retained_link = f.home / '.local/share/devstats/linked history'; retained_link.symlink_to(custom)
        env = dict(f.env, DEVSTATS_DATA_DIR=str(custom))
        before = tree_snapshot(f.root)
        result = f.uninstall('--dry-run', env=env)
        assert 'Verified user installations to remove: 4' in result.stdout
        assert all(str(p) in result.stdout for p in f.installs)
        assert 'Retain all user data/settings' in result.stdout and str(custom) in result.stdout
        assert tree_snapshot(f.root) == before
        f.uninstall(env=env); f.assert_removed()
        assert cache.read_bytes() == b'cache' and (custom / 'keep').read_bytes() == b'custom'
        assert os.readlink(retained_link) == str(custom)
        f.uninstall(); f.uninstall('--dry-run')  # standalone idempotence, no VERSION required.

        f = fixture('active-custom-links-and-recorded-bin-cycle')
        old = f.install('0.7.1', legacy=True)
        parent_a = f.home / '应用 custom parent A $`"\\%'
        parent_b = f.home / '应用 custom parent B'
        bin_a = f.home / '命令 bin A $`"\\%'
        data_a = f.home / 'XDG 数据 A'; config_a = f.home / 'XDG 配置 A'
        second = f.install('0.7.2', parent_a, bin_a, data_a, config_a, legacy=True)
        third = f.install('0.7.3', parent_a, bin_a, data_a, config_a)
        fourth = f.install('0.7.4', parent_b, data=f.home / 'XDG 数据 B', config=f.home / 'XDG 配置 B')
        default_bin = f.home / '.local/bin'
        # Each ordinary managed command can anchor a parent; metadata finds other bins.
        (default_bin / 'devmon').unlink(); (default_bin / 'devmon').symlink_to(third / 'devmon')
        for bundle, _, _, config in f.records:
            if not (config / 'autostart/devstats.desktop').exists():
                (config / 'autostart').mkdir(parents=True, exist_ok=True)
                (config / 'autostart/devstats.desktop').write_text(autostart(bundle))
        before = tree_snapshot(f.root)
        f.uninstall('--dry-run'); assert tree_snapshot(f.root) == before
        # Installed entry supplies its own anchor and recorded roots without environment overrides.
        f.uninstall(script=default_bin / 'devstats-uninstall')
        f.assert_removed()

        f = fixture('bounded-search-and-explicit-anchors')
        visible = f.install('0.7.4')
        hidden_parent = f.home / 'inactive custom versions'
        unknown = f.install('0.7.1', hidden_parent, f.home / 'unknown inactive bin', f.home / 'unknown legacy data', f.home / 'unknown legacy config', legacy=True)
        # Unknown legacy paths have no metadata; original environment names its extra entries.
        hidden_env = dict(f.env, DEVSTATS_USER_BIN_DIR=str(f.home / 'unknown inactive bin'),
                          XDG_DATA_HOME=str(f.home / 'unknown legacy data'), XDG_CONFIG_HOME=str(f.home / 'unknown legacy config'))
        # Keep it undiscoverable from default command paths for the first bounded pass.
        for name in ('devstats', 'devmon'):
            link = f.home / 'unknown inactive bin' / name; link.unlink()
        f.uninstall()
        assert not visible.exists() and unknown.is_dir(); f.assert_data()
        f.uninstall('--search-dir', str(hidden_parent), env=hidden_env)
        assert not unknown.exists(); f.assert_data()
        f.uninstall('--install-dir', str(unknown), '--search-dir', str(hidden_parent))

        f = fixture('repeat-install-dir-and-search-dir')
        a = f.install('0.7.1', f.home / 'first parent', f.home / 'first bin')
        b = f.install('0.7.4', f.home / 'second parent', f.home / 'second bin')
        f.uninstall('--install-dir', str(a), '--install-dir', str(b), '--search-dir', str(a.parent), '--search-dir', str(b.parent))
        f.assert_removed()

        for kind in ('modified-later', 'extra-later', 'devmon-file-later', 'devmon-directory-later', 'marker-later', 'marker-symlink', 'bundle-symlink',
                     'manifest-escape', 'path-record', 'record-symlink', 'ancestor-symlink', 'managed-menu',
                     'autostart-symlink', 'data-overlap', 'data-root-overlap', 'unowned-explicit', 'install-parent-readonly', 'entry-parent-readonly'):
            f = fixture('all-or-nothing-' + kind)
            f.standard_versions(); late = f.installs[-1]
            args = []
            if kind == 'modified-later': (late / 'share/devstats.svg').write_text('changed by user')
            elif kind == 'extra-later': (late / 'user document.txt').write_text('keep')
            elif kind == 'devmon-file-later':
                (late / 'devmon').unlink(); (late / 'devmon').write_text('user content must survive')
            elif kind == 'devmon-directory-later':
                (late / 'devmon').unlink(); (late / 'devmon').mkdir()
            elif kind == 'marker-later': (late / '.devstats-user-install').write_text('unrecognized\n')
            elif kind == 'marker-symlink':
                (late / '.devstats-user-install').unlink(); (late / '.devstats-user-install').symlink_to(late / '.devstats-user-package')
            elif kind == 'bundle-symlink':
                (late / 'share/devstats.svg').unlink(); (late / 'share/devstats.svg').symlink_to(f.root / 'unrelated')
            elif kind == 'manifest-escape':
                with (late / 'SHA256SUMS').open('a') as stream: stream.write('0' * 64 + '  ../../outside.txt\n')
            elif kind == 'path-record': (late / '.devstats-user-paths').write_text('unrecognized\n')
            elif kind == 'record-symlink':
                (late / '.devstats-user-paths').unlink(); (late / '.devstats-user-paths').symlink_to(f.root / 'absent')
            elif kind == 'ancestor-symlink':
                alias = f.root / 'alias'; alias.symlink_to(f.home, target_is_directory=True)
                args = ['--install-dir', str(alias / '.local/opt' / late.name)]
            elif kind == 'managed-menu':
                with (f.home / '.local/share/applications/devstats.desktop').open('a') as stream: stream.write('# user change\n')
            elif kind == 'autostart-symlink':
                auto = f.home / '.config/autostart/devstats.desktop'; auto.parent.mkdir(parents=True)
                auto.symlink_to(f.home / '.local/share/applications/devstats.desktop')
            elif kind == 'data-overlap': args = []; f.env['DEVSTATS_DATA_DIR'] = str(late / 'retained data')
            elif kind == 'data-root-overlap': f.env['DEVSTATS_DATA_DIR'] = '/'
            elif kind == 'unowned-explicit': args = ['--install-dir', '/usr']
            elif kind == 'install-parent-readonly': late.parent.chmod(0o555)
            elif kind == 'entry-parent-readonly': (f.home / '.local/share/applications').chmod(0o555)
            before = tree_snapshot(f.root)
            f.uninstall(*args, success=False)
            assert tree_snapshot(f.root) == before
            assert all(p.is_dir() for p in f.installs); f.assert_data()
            if kind == 'install-parent-readonly': late.parent.chmod(0o755)
            elif kind == 'entry-parent-readonly': (f.home / '.local/share/applications').chmod(0o755)

        f = fixture('running-later-candidate')
        f.standard_versions()
        process = subprocess.Popen([str(f.installs[-1] / 'libexec/devmon'), '60'])
        try:
            before = tree_snapshot(f.root)
            result = f.uninstall(success=False)
            assert 'is running' in result.stderr and process.poll() is None
            assert tree_snapshot(f.root) == before
        finally:
            process.terminate(); process.wait(timeout=10)
        f.uninstall(); f.assert_removed()

        f = fixture('unmarked-archives-and-unrelated-entries')
        f.standard_versions()
        archive = f.home / '.local/opt/extracted-but-not-installed'
        shutil.copytree(f.root / '解压 archive 0.7.4', archive, symlinks=True)
        unknown = f.home / '.local/opt/unrelated directory'; unknown.mkdir(); (unknown / 'keep').write_bytes(b'keep')
        alias = f.home / '.local/opt/unrelated symlink'; alias.symlink_to(unknown)
        # These are unrelated to every selected version and must remain intact.
        commands = f.home / '.local/bin'
        (commands / 'devmon').unlink(); (commands / 'devmon').write_bytes(b'other CLI')
        (commands / 'devstats-uninstall').unlink(); (commands / 'devstats-uninstall').symlink_to('/nonexistent/other-uninstaller')
        auto = f.home / '.config/autostart/devstats.desktop'; auto.parent.mkdir(parents=True)
        auto.write_text('[Desktop Entry]\nName=DevStats\nExec=/usr/bin/devstats\n')
        retained = {p: tree_snapshot(p) for p in (archive, unknown)}
        f.uninstall()
        assert all(not p.exists() for p in f.installs); f.assert_data()
        assert all(tree_snapshot(p) == value for p, value in retained.items())
        assert alias.is_symlink() and (commands / 'devmon').read_bytes() == b'other CLI'
        assert os.readlink(commands / 'devstats-uninstall') == '/nonexistent/other-uninstaller'
        assert 'Exec=/usr/bin/devstats' in auto.read_text()

        f = fixture('symlink-marked-sibling-refusal')
        target = f.install('0.7.4')
        (target.parent / 'alias-to-marked-install').symlink_to(target)
        before = tree_snapshot(f.root)
        f.uninstall(success=False); assert tree_snapshot(f.root) == before

        f = fixture('empty-root-and-old-options')
        stale_menu = f.home / '.local/share/applications/devstats.desktop'
        stale_menu.parent.mkdir(parents=True)
        stale_menu.write_text('# DevStats user installation v1\n[Desktop Entry]\nName=DevStats\nExec=/absent/devstats\n')
        before = tree_snapshot(f.root)
        result = f.uninstall(); assert 'installations to remove: 0' in result.stdout
        f.uninstall('--dry-run'); f.uninstall('--purge', success=False); f.uninstall('--yes', success=False)
        assert tree_snapshot(f.root) == before

        f = fixture('root-guard')
        f.standard_versions()
        mock = f.root / 'mock-id'; mock.mkdir()
        (mock / 'id').write_text('#!/bin/sh\nprintf "0\\n"\n'); (mock / 'id').chmod(0o755)
        before = tree_snapshot(f.root)
        result = f.uninstall(env=dict(f.env, PATH=str(mock) + ':' + f.env['PATH']), success=False)
        assert 'without sudo' in result.stderr and tree_snapshot(f.root) == before

        f = fixture('other-system-or-source-process-kept')
        f.standard_versions()
        executable = f.root / 'devstats'; shutil.copy2('/bin/sleep', executable)
        process = subprocess.Popen([str(executable), '60'])
        try:
            f.uninstall(); f.assert_removed(); assert process.poll() is None
        finally:
            process.terminate(); process.wait(timeout=10)

        # A 0.8 install has no devmon anywhere, and an older one's menu text differs.
        f = fixture('without devmon')
        f.install('0.7.12'); current = f.install('0.8.0', devmon=False)
        assert not (current / 'devmon').exists() and not (current / 'libexec/devmon').exists()
        assert not (f.home / '.local/bin/devmon').exists(), 'installing 0.8 removes the managed devmon link'
        f.uninstall('--dry-run'); assert all(p.exists() for p in f.installs)
        f.uninstall(); f.assert_removed()
        old_menu = fixture('older menu text')
        old_menu.install('0.7.12', devmon=False)
        entry = old_menu.home / '.local/share/applications/devstats.desktop'
        entry.write_text(entry.read_text().replace('Comment=System monitor and stall diagnosis', 'Comment=System and development monitor'))
        old_menu.uninstall(); old_menu.assert_removed()

        print(f'PASS: {count} isolated all-version uninstall fixtures; 0.7.1/2 legacy and 0.7.3/4; standalone/installed discovery, custom links/metadata/sibling bounds/cycles, explicit roots; all data/settings retained bytewise; dry-run/idempotence; all-or-nothing malformed/modified/unknown/symlink/unowned/running refusal; archives/system/unrelated entries retained; no purge or process termination.')


if __name__ == '__main__':
    main()
