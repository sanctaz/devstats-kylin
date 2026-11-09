#!/usr/bin/env python3
"""Bundle the actual ARM64 DT_NEEDED closure, not a system Qt dependency."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import tempfile

PROJECT = Path(__file__).resolve().parent.parent
BASE = {'libc.so.6', 'libpthread.so.0', 'libdl.so.2', 'libm.so.6', 'librt.so.1',
        'libresolv.so.2', 'libutil.so.1', 'libanl.so.1', 'ld-linux-aarch64.so.1'}
PLUGIN_GROUPS = ('platforms', 'xcbglintegrations', 'wayland-graphics-integration-client',
                 'wayland-shell-integration', 'wayland-decoration-client',
                 'platforminputcontexts', 'imageformats', 'sqldrivers')
PLATFORMS = {'libqxcb.so', 'libqoffscreen.so', 'libqminimal.so',
             'libqwayland-generic.so', 'libqwayland-egl.so'}

def command(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)

def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest() if hasattr(hashlib, 'file_digest') else hashlib.sha256(stream.read()).hexdigest()

def elf(path):
    head = command('readelf', '-h', str(path))
    if not re.search(r'Machine:\s+AArch64', head):
        raise RuntimeError('Non-AArch64 ELF: ' + str(path))
    dynamic = command('readelf', '-d', str(path))
    if re.search(r'\((?:RPATH|RUNPATH)\)', dynamic):
        raise RuntimeError('ELF has RPATH/RUNPATH: ' + str(path))
    versions = re.findall(r'\bGLIBC_(\d+(?:\.\d+)+)\b', command('readelf', '--version-info', str(path)))
    if versions and max(tuple(map(int, x.split('.'))) for x in versions) > (2, 31):
        raise RuntimeError('ELF needs glibc newer than 2.31: ' + str(path))
    return re.findall(r'\(NEEDED\).*?\[(.*?)\]', dynamic)

def archive_index(archives, required):
    """Inspect only archive members, with no extraction or SDK modification."""
    index = {}
    for directory in archives:
        for archive in sorted(directory.glob('*.deb')):
            if '_arm64.deb' not in archive.name and '_all.deb' not in archive.name:
                continue
            # Development packages do not own any shipped runtime files.
            if re.search(r'(?:-dev|-dbgsym|-dbg)_', archive.name):
                continue
            matches = []
            for line in command('dpkg-deb', '--contents', str(archive)).splitlines():
                parts = line.split(None, 5)
                if len(parts) != 6:
                    continue
                member = parts[5].split(' -> ', 1)[0].removeprefix('./')
                if member in required:
                    matches.append(member)
            if matches:
                fields = command('dpkg-deb', '-f', str(archive), 'Package', 'Version', 'Architecture', 'Source').splitlines()
                values = dict(line.split(': ', 1) for line in fields if ': ' in line)
                package = values['Package']
                source = values.get('Source', package)
                match = re.match(r'([^ ]+)(?: \(([^)]+)\))?', source)
                source_package, source_version = match[1], match[2] or values['Version']
                if (package.startswith('libqt5') or package == 'qtwayland5') and not values['Version'].startswith('5.12.8'):
                    raise RuntimeError('Expected consistent Qt 5.12.8 runtime/plugin package: ' + package)
                record = dict(package=package, version=values['Version'], architecture=values['Architecture'],
                              source_package=source_package, source_version=source_version,
                              archive=archive.name, archive_sha256=digest(archive),
                              source_url='https://launchpad.net/ubuntu/+source/' + source_package + '/' + source_version)
                for member in matches:
                    if member in index and index[member]['archive_sha256'] != record['archive_sha256']:
                        raise RuntimeError('Ambiguous source package: ' + member)
                    index[member] = record
    missing = required - set(index)
    if missing:
        raise RuntimeError('No package provenance for: ' + ', '.join(sorted(missing)))
    return index

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True, type=Path)
    parser.add_argument('--sysroot', required=True, type=Path)
    parser.add_argument('--overlay', type=Path, action='append', default=[])
    parser.add_argument('--archives', type=Path, action='append', required=True,
                        help='read-only directory of original arm64/all .deb archives; repeatable')
    parser.add_argument('--output-dir', type=Path, default=PROJECT.parent.parent / 'dist')
    args = parser.parse_args()
    version = (PROJECT / 'VERSION').read_text().strip()
    if not re.fullmatch(r'[0-9][0-9A-Za-z.+~-]*', version):
        raise RuntimeError('Invalid version')
    binary = args.binary.resolve(strict=True)
    roots = [p.resolve(strict=True) for p in args.overlay] + [args.sysroot.resolve(strict=True)]
    # Search root filesystem layouts only, never the build host's library paths.
    lib_dirs = [root / sub for root in roots for sub in
                ('usr/lib/aarch64-linux-gnu', 'lib/aarch64-linux-gnu', 'usr/lib', 'lib')]
    def locate(name):
        for directory in lib_dirs:
            path = directory / name
            if path.is_file():
                resolved = path.resolve(strict=True)
                if not any(resolved.is_relative_to(root) for root in roots):
                    raise RuntimeError('Library symlink escapes supplied runtime roots: ' + str(path))
                return path
        raise RuntimeError('Unresolved dependency: ' + name)
    args.output_dir.mkdir(parents=True, exist_ok=True)
    package_name = 'devstats-' + version + '-linux-arm64-user'
    with tempfile.TemporaryDirectory(prefix='.devstats-user-', dir=args.output_dir) as temp:
        stage = Path(temp) / package_name
        for sub in ('lib', 'libexec', 'plugins', 'share/licenses', 'share/common-licenses'):
            (stage / sub).mkdir(parents=True, exist_ok=True)
        shutil.copy2(binary, stage / 'libexec/devstats')
        os.chmod(stage / 'libexec/devstats', 0o755)
        for name in ('devstats', 'install-user.sh', 'uninstall-user.sh'):
            shutil.copy2(PROJECT / 'packaging/user' / name, stage / name)
        shutil.copy2(PROJECT / 'VERSION', stage / 'VERSION')
        shutil.copy2(PROJECT / 'packaging/devstats.svg', stage / 'share/devstats.svg')
        shutil.copy2(PROJECT / 'packaging/LICENSE', stage / 'share/LICENSE.DevStats')
        shutil.copy2(PROJECT / 'packaging/README-user.md', stage / 'README-user.md')
        (stage / '.devstats-user-package').write_text('DevStats private Qt runtime v1\n')
        (stage / 'libexec/qt.conf').write_text('[Paths]\nPrefix=..\nLibraries=lib\nPlugins=plugins\n')
        queue = [(binary, stage / 'libexec/devstats')]
        for group in PLUGIN_GROUPS:
            selected = {}
            for root in roots:
                directory = root / 'usr/lib/aarch64-linux-gnu/qt5/plugins' / group
                for path in sorted(directory.glob('*.so')):
                    if group == 'platforms' and path.name not in PLATFORMS:
                        continue
                    if group == 'sqldrivers' and path.name != 'libqsqlite.so':
                        continue
                    selected.setdefault(path.name, path)
            for name, path in selected.items():
                output = stage / 'plugins' / group / name
                output.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(path, output)
                queue.append((path, output))
        for required in ('platforms/libqxcb.so', 'platforms/libqwayland-generic.so', 'sqldrivers/libqsqlite.so'):
            if not (stage / 'plugins' / required).exists():
                raise RuntimeError('Required plugin absent: ' + required)
        shipped = []
        seen = set()
        external = set()
        while queue:
            source, output = queue.pop(0)
            if output in seen:
                continue
            seen.add(output)
            needs = elf(source)
            if source != binary:
                root = next(root for root in roots if source.resolve().is_relative_to(root))
                shipped.append(dict(path=str(output.relative_to(stage)), original=str(source.resolve().relative_to(root)),
                                    sha256=digest(output), needed=needs, root=str(root)))
            for name in needs:
                if name in BASE:
                    external.add(name)
                    continue
                output = stage / 'lib' / name
                if output not in seen and not output.exists():
                    source_library = locate(name)
                    shutil.copy2(source_library, output)
                    queue.append((source_library, output))
        index = archive_index(args.archives, {r['original'] for r in shipped})
        packages = {}
        for record in shipped:
            provenance = index[record['original']]
            record.update(provenance)
            root = Path(record.pop('root'))
            package = record['package']
            packages[package] = provenance
            license_source = root / 'usr/share/doc' / package / 'copyright'
            if not license_source.is_file():
                # A shared copyright may live in another supplied root.
                license_source = next((p / 'usr/share/doc' / package / 'copyright' for p in roots
                                       if (p / 'usr/share/doc' / package / 'copyright').is_file()), None)
            if license_source is None:
                raise RuntimeError('Missing copyright: ' + package)
            shutil.copy2(license_source, stage / 'share/licenses' / (package + '.copyright'))
        # Preserve full license texts referenced by Debian copyright documents.
        common = Path('/usr/share/common-licenses')
        if not common.is_dir():
            raise RuntimeError('Build host must provide Debian common-license texts')
        for path in common.iterdir():
            if path.is_file():
                shutil.copy2(path, stage / 'share/common-licenses' / path.name)
        manifest = dict(format=1, application='DevStats', version=version, architecture='arm64',
                        qt_runtime='5.12.8', baseline='GNU/Linux AArch64 glibc >= 2.31',
                        system_libraries=sorted(external), files=sorted(shipped, key=lambda x: x['path']),
                        packages=sorted(packages.values(), key=lambda x: x['package']))
        (stage / 'share/runtime-manifest.json').write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n')
        with (stage / 'SHA256SUMS').open('w') as sums:
            for path in sorted(stage.rglob('*')):
                if path.is_file() and not path.is_symlink() and path.name != 'SHA256SUMS':
                    sums.write(digest(path) + '  ' + str(path.relative_to(stage)) + '\n')
        destination = args.output_dir / (package_name + '.tar.gz')
        def metadata(member):
            member.uid = member.gid = 0
            member.uname = member.gname = 'root'
            member.mtime = 0
            member.mode &= ~0o022
            return member
        with tarfile.open(destination, 'w:gz', compresslevel=9) as archive:
            archive.add(stage, arcname=package_name, filter=metadata)
        print(json.dumps(dict(package=str(destination.resolve()), sha256=digest(destination),
                              bytes=destination.stat().st_size, runtime_elfs=len(shipped), packages=len(packages),
                              system_libraries=sorted(external))))

if __name__ == '__main__':
    main()
