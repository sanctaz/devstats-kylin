# DevStats for Kylin ARM64: the package that installs without an administrator

For Kylin Desktop V10 SP1 / 2303 on ARM64 (aarch64) with glibc 2.31 or newer.
The package installs into the user's own directories and carries private dynamic
runtime libraries. Starting DevStats needs no Qt, SQLite or development packages,
runs no apt or sudo, and does not change the system's Qt or any system directory.
It still depends on Linux / glibc, a graphical session, the system's fonts and a
driver that matches the graphics hardware, so it is not a fully static program
without any system dependencies.

中文说明见 [README-user.md](README-user.md)。

## Unpack and run

```sh
tar -xzf devstats-1.0.0-linux-arm64-user.tar.gz
cd devstats-1.0.0-linux-arm64-user
./devstats --show
```

A sample on the command line needs no desktop: `./devstats --snapshot`. Keep the
whole unpacked directory together, somewhere the current user owns. The launcher
finds its own directory, so the directory can be moved as a whole; do not run
`libexec/devstats` directly. Do not source the launcher script, and there is no
need to add the private libraries to `.bashrc` or the system environment.

## Install for the current user

```sh
./install-user.sh
```

The default install directory is `~/.local/opt/devstats-1.0.0`, the command is
`~/.local/bin/devstats`, and the menu entry and icon are under
`${XDG_DATA_HOME:-~/.local/share}/applications` and `icons/hicolor/scalable/apps`.
The installer refuses to run as root and does not turn on start at login for you.
Data goes to the current user's XDG data directory and settings to the XDG config
directory by default. Start at login can be turned on in the application's settings.

Installing the same version with the same content again is safe. A new version
uses a different version directory and switches this application's entries to it,
keeping the old version. Start at login, if it is on, is switched to the new
version too; otherwise the next login would still start the old one.
Before upgrading, quit DevStats from the tray menu: while the old version is
running, opening DevStats again still shows the old version, and the installer says so.
Existing files, directories, commands or menu entries that do not belong to
DevStats make the installation stop. When the same version has different or
modified content, use a new empty directory; the original files are not overwritten.
An example with custom locations:

```sh
DEVSTATS_USER_INSTALL_DIR="$HOME/Apps/DevStats 1.0.0" \
DEVSTATS_USER_BIN_DIR="$HOME/.local/bin" ./install-user.sh
```

Paths must be absolute and contain no line break. Directories may contain spaces,
Chinese characters and shell special characters. If `.local/bin` is not in PATH
yet, start DevStats by its full path. The installer does not change shell configuration.

## Language

The interface comes in Simplified Chinese and English. By default it follows the
system: Chinese when the system language is Chinese (`LC_ALL`, `LC_MESSAGES`,
`LANG`, and the first entry of `LANGUAGE`), English for any other language.
Open the settings (the gear at the top right of the window, or Ctrl+,) and choose
**Language**: Follow system, 中文 or English. The change applies at once.
Stall records and action-log entries saved before a change keep the words they
were saved with.

## Uninstall every recognized user version at once, keeping all data

Quit DevStats from the tray menu first. An ordinary Kylin user runs the
uninstaller directly, without `sudo`. It first inspects every candidate
installation and entry read-only and only then deletes. If any candidate is still
running, has modified files, contains unknown files, has a wrong marker or wrong
ownership, or holds an unsafe symbolic link, the whole operation stops and no
version is deleted. It never ends a process.

```sh
# Show every version and entry to be removed and the data paths that are kept; changes nothing
~/.local/bin/devstats-uninstall --dry-run
# Uninstall every user installation found, always keeping the data
~/.local/bin/devstats-uninstall
```

You can also use `uninstall-user.sh` inside the install directory, the script of
the same name in the unpacked package, or a copy of the script on its own; a
standalone script needs no `VERSION` file next to it. With a custom command
directory, use the `devstats-uninstall` in that directory. Found by default:

- Every version with a DevStats user-installation marker among the direct
  subdirectories of `~/.local/opt`.
- The installed directory the script is in, an installation named by
  `DEVSTATS_USER_INSTALL_DIR` or `--install-dir`, and the direct sibling versions
  in the directories of those installations.
- The installations that `devstats`, `devstats-uninstall` and the old `devmon`
  links point to in `~/.local/bin`, in `DEVSTATS_USER_BIN_DIR` and in the command
  directories recorded by the installations found, with their sibling versions.

The path record of each installation found is also used to clean up its custom
command, XDG menu entry, icon and the start-at-login entry in the config
directory. Following these entries to further installations only looks at direct
sibling directories and keeps track of the paths visited; the home directory, the
disk and ordinary subdirectories are never searched recursively. Unpacked
packages without an installation marker, unrelated files, a system `.deb`
installation and source builds are kept. Only an install directory of the current
user whose marker, package manifest and file checksums all pass can be deleted.
Menu and start-at-login entries must match exactly what a recognized version
generated; unknown entries are reported and kept; a menu entry with an
installation marker that still points to a version being removed but was modified
stops the whole operation.

An old custom installation that no entry points to any more may not be found
automatically. Name its parent directory explicitly:

```sh
./uninstall-user.sh --search-dir "$HOME/Apps/DevStats old versions" --dry-run
./uninstall-user.sh --search-dir "$HOME/Apps/DevStats old versions"
```

`--search-dir` only looks at the direct subdirectories of the given directory;
`--install-dir "$HOME/Apps/devstats-0.7.2"` names one known installation as a
starting point. Both options can be repeated and widen what is found; the
uninstall still handles every recognized user version. Where an old 0.7.1 / 0.7.2
installation has no path record, custom entries need the original
`DEVSTATS_USER_BIN_DIR`, `XDG_DATA_HOME` and `XDG_CONFIG_HOME` to be cleaned up.
Directories and entries that are not recognized are never deleted on a guess.
Check the actual scope with `--dry-run` first.

All history, settings, tasks and caches are kept unconditionally; there is no
option to clear data:

- Every known `${XDG_CONFIG_HOME:-$HOME/.config}/devstats/settings.ini`.
- Every known `${XDG_DATA_HOME:-$HOME/.local/share}/devstats/History/metrics.sqlite3`
  with its `-wal` and `-shm`, and `Tasks/`, `Launchers/` and the other files in
  the same data directory.
- Custom data named by `DEVSTATS_DATA_DIR`, and every cache directory, known or not.

One data directory can be shared by several versions, and uninstalling does not
change it; data in other XDG directories is kept as well. If an install directory
overlaps a data directory that is to be kept, the uninstall stops. With nothing to
delete it does nothing, so with the unpacked package or a standalone script kept
you can check again. The uninstall script inside an install directory is deleted
with that installation.

## Private runtime and system requirements

The package's Qt 5.12.8, Qt SQLite driver, SQLite, C++ runtime and direct dynamic
dependencies come from Ubuntu 20.04 ARM64 packages. `share/runtime-manifest.json`
lists the hash of every file, the version and hash of the original package, where
its source is and the libraries left to the system; copyright notices are in
`share/licenses` and the full common licenses in `share/common-licenses`.
DevStats is under the MIT license. Qt and the other libraries stay dynamically
linked, so a user may replace the copies in the package with libraries that
comply with their licenses and have the same ABI. The source entries point to
each original version, whose source and distribution patches are available from
Ubuntu / Launchpad.

The package contains the X11/xcb, Wayland and offscreen platform plugins, the
Wayland shell plugins, input method plugins, image plugins and the SQLite plugin.
`libexec/qt.conf` points the plugins to this package, so the private Qt does not
pick up plugins of a newer Qt on the target system by mistake. The launcher sets
the library search path locally, and the application restores the original
environment once it has started, so terminals and other programs started from
the application keep the caller's LD_LIBRARY_PATH / Qt environment (including the
difference between unset and empty).

The system must provide the AArch64 glibc dynamic loader
`/lib/ld-linux-aarch64.so.1`, glibc 2.31 or newer and Linux `/proc` and `/sys`.
The graphical interface needs a working X11 or Wayland session with tray support,
and uses the target system's fonts. The GPU's DRI / EGL vendor modules, kernel
drivers, font files, the desktop tray and D-Bus services belong to the target
system; the private runtime brings neither graphics drivers nor fonts. The tray
and the hardware battery display have to be verified on a real Kylin computer;
verification under QEMU or offscreen cannot replace that.

## Building this package (maintainers)

On a Linux build machine, first build the ARM64 ELF against the glibc 2.31
baseline, then run:

```sh
scripts/package-linux-user.sh --binary build/linux-arm64/devstats \
  --sysroot /path/to/arm64-sysroot --overlay /path/to/runtime-overlay \
  --archives /path/to/original-arm64-debs --archives /path/to/overlay-debs \
  --output-dir ../../dist
```

Building needs Python 3.9+, readelf and dpkg-deb; installing and starting need no
Python. The builder collects dependencies along each ELF's DT_NEEDED, verifies
that every ELF shipped is AArch64 and has no build RPATH, and that the GLIBC
symbols required do not exceed 2.31. The original .deb files are read without
being changed, to produce source information that can be checked. Only the glibc
family and the loader are left out; the other direct dependencies of the platform
plugins are shipped. Deployment follows what Qt recommends, an application-local
launcher script and qt.conf:
https://doc.qt.io/qt-6/linux-deployment.html ,
https://doc.qt.io/qt-6/deployment-plugins.html .

## An explicit install scope

To put the program and its entries into one given directory, run
`./install-user.sh --prefix /absolute/path/DevStats`; the version directory is
`opt/devstats-1.0.0` and the command `bin/devstats`. Uninstall with
`./uninstall-user.sh --prefix /absolute/path/DevStats`, which only inspects
verified installations under that prefix and does not scan the default HOME
directories; settings, history, tasks and caches are still kept. The prefix must
be a canonical absolute path without symbolic links, and neither empty nor the
root directory.

## Stall diagnosis

Open DevStats → **Diagnosis** in the sidebar, or right-click the tray icon →
**One-click diagnosis**.

- **Live**: the banner at the top gives the conclusion and its evidence. Below it
  are six indicators (processor, memory, disk busyness, response delay,
  temperature, processes waiting for the disk) and the related processes. Other
  users' processes and kernel threads are listed too.
- **One-click diagnosis**: observes for about 10 seconds, tells sustained
  problems from occasional ones, and then ranks the related processes by average
  CPU, peak memory or peak reads and writes. It does not close any program.
- **Stall records**: on sustained pressure or a short freeze the evidence of that
  moment is saved automatically and kept for 31 days. Records can be searched and
  exported as an HTML report; automatic recording can be turned off at the top
  right of the page.
- **Pressure history**: long-term charts of scheduling delay, disk busyness, I/O
  wait, swap-out rate and PSI stalls.

Exporting a report or the selected record produces a single HTML file that opens
and prints in a browser without a network connection; the raw data is at the end
of the page. A report contains application names, PIDs and resource figures, but
no command lines and no program paths, and only you can read and write the file.

The diagnosis only reads `/proc` and `/sys` and needs no root. Where the kernel
has no PSI it judges by scheduling wait, direct reclaim and disk delay instead.
A process's place in the ranking means it was involved at the time; it does not
confirm the cause.

## Auto-kill

Open DevStats → **Auto-kill** in the sidebar, or right-click the tray icon →
**Auto-kill…**. It is off by default and has no preset rules.

1. Choose **Add rule…** and select a running process, or enter the full path of
   a program or its exact name. Right-clicking a process under **Diagnosis** or
   in a process list adds it directly.
2. Choose when: **CPU above X% for Y seconds** (100% is one core in full, judged
   per process), or whenever it runs.
3. Choose how: SIGTERM first, and optionally SIGKILL if the process is still
   running after N seconds. SIGKILL gives a program no chance to save; turn it
   off if it is not needed.
4. Turn on the main switch at the top and confirm once. From then on the rules
   act by themselves, without asking each time.

Each rule shows its live state (for example “Over the limit 18 / 30 s · average
92%”). The **action log** confirms whether the program really exited and keeps
the latest 100 entries (`process-rules-log.json` in your data directory, mode
0600); neither an upgrade nor an uninstall deletes it. Turning the main switch
off takes effect at once. Rules apply only while DevStats runs; to have them
apply from startup, turn on start at login in the settings.

Only processes of the current ordinary user that are not elevated can be ended;
the desktop, the session, terminals and DevStats itself are protected. Root is
not used. Before each signal the identity of the process is checked again, and
the signal is sent through a stable process handle; where the kernel does not
support this the reason is recorded, and DevStats does not fall back to a signal
by PID number. The kernel `5.4.96-24-kr9a0` meets the basic requirement
(available since Linux 5.1); the actual permission is decided by a check at run time.

After an upgrade from 0.7.x, old rules stay as they were: no CPU condition, no
forced end.

A prefix only controls the scope of installing and uninstalling. At run time the
data locations still follow `XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME`
and `DEVSTATS_DATA_DIR`; to run fully isolated, set these variables explicitly.
HOME does not need to be changed.
