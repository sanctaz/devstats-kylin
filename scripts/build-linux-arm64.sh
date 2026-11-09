#!/usr/bin/env bash
set -euo pipefail

project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
build_dir=${DEVSTATS_BUILD_DIR:-"$project_dir/build/linux-arm64"}
qmake_bin=${QMAKE:-qmake}
sysroot=${DEVSTATS_SYSROOT:-}
host_prefix=${DEVSTATS_QT_HOST_PREFIX:-}
cross_prefix=${CROSS_COMPILE:-aarch64-linux-gnu-}
project="$project_dir/devstats.pro"
jobs=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf 2)}
usage() {
    cat <<'EOF'
Usage: scripts/build-linux-arm64.sh [options]
  --build-dir PATH       Out-of-source build directory
  --qmake PATH           Qt 5 qmake executable
  --sysroot PATH         ARM64 development root; enables cross compilation
  --qt-host-prefix PATH  Prefix containing host lib/qt5/bin tools
  --cross-prefix PREFIX  Cross compiler prefix (default aarch64-linux-gnu-)
  --project PATH         Alternate qmake project, for a test suite
  --jobs N               Parallel make jobs
Without --sysroot, run on ARM64 Linux with Qt 5 development packages.
EOF
}
while (($#)); do
    case "$1" in
        --help|-h) usage; exit 0 ;;
        --build-dir|--qmake|--sysroot|--qt-host-prefix|--cross-prefix|--project|--jobs)
            (($# >= 2)) || { printf 'Missing value: %s\n' "$1" >&2; exit 2; }
            case "$1" in
                --build-dir) build_dir=$2 ;; --qmake) qmake_bin=$2 ;;
                --sysroot) sysroot=$2 ;; --qt-host-prefix) host_prefix=$2 ;;
                --cross-prefix) cross_prefix=$2 ;; --project) project=$2 ;; --jobs) jobs=$2 ;;
            esac
            shift 2 ;;
        *) printf 'Unknown option: %s\n' "$1" >&2; exit 2 ;;
    esac
done
[[ $(uname -s) == Linux ]] || { printf 'Build on ARM64 Linux or a Linux cross SDK host.\n' >&2; exit 1; }
[[ $jobs =~ ^[1-9][0-9]*$ ]] || { printf 'Invalid job count.\n' >&2; exit 2; }
command -v "$qmake_bin" >/dev/null
command -v make >/dev/null
[[ -f $project ]] || { printf 'Project missing: %s\n' "$project" >&2; exit 1; }
qt_version=$("$qmake_bin" -query QT_VERSION)
[[ $qt_version == 5.* && $(printf '%s\n' 5.12 "$qt_version" | sort -V | head -n 1) == 5.12 ]] || {
    printf 'Qt 5.12 or later Qt 5 required; found %s\n' "$qt_version" >&2; exit 1;
}
mkdir -p "$build_dir"
build_dir=$(cd "$build_dir" && pwd)
args=("$project" 'CONFIG+=release' 'CONFIG-=debug')
if [[ -n $sysroot ]]; then
    sysroot=$(cd "$sysroot" && pwd)
    [[ -n $host_prefix ]] || { printf '--qt-host-prefix required with --sysroot.\n' >&2; exit 2; }
    host_prefix=$(cd "$host_prefix" && pwd)
    command -v "${cross_prefix}g++" >/dev/null
    export PKG_CONFIG_SYSROOT_DIR="$sysroot"
    export PKG_CONFIG_LIBDIR="$sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:$sysroot/usr/lib/pkgconfig:$sysroot/usr/share/pkgconfig"
    export PKG_CONFIG_PATH=
    cat > "$build_dir/qt-cross.conf" <<EOF
[Paths]
Prefix=$sysroot/usr
ArchData=lib/aarch64-linux-gnu/qt5
Data=share/qt5
Headers=include/aarch64-linux-gnu/qt5
Libraries=lib/aarch64-linux-gnu
LibraryExecutables=lib/aarch64-linux-gnu/qt5/libexec
Binaries=lib/qt5/bin
Plugins=lib/aarch64-linux-gnu/qt5/plugins
Imports=lib/aarch64-linux-gnu/qt5/imports
Qml2Imports=lib/aarch64-linux-gnu/qt5/qml
Translations=share/qt5/translations
HostPrefix=$host_prefix
HostBinaries=lib/qt5/bin
HostData=$sysroot/usr/lib/aarch64-linux-gnu/qt5
Sysroot=$sysroot
TargetSpec=linux-aarch64-gnu-g++
HostSpec=linux-g++
EOF
    args=(-qtconf "$build_dir/qt-cross.conf" -spec linux-aarch64-gnu-g++ "${args[@]}"
        "QMAKE_CC=${cross_prefix}gcc" "QMAKE_CXX=${cross_prefix}g++" "QMAKE_LINK=${cross_prefix}g++"
        "QMAKE_AR=${cross_prefix}ar cqs" "QMAKE_STRIP=${cross_prefix}strip"
        "QMAKE_CFLAGS+=--sysroot=$sysroot" "QMAKE_CXXFLAGS+=--sysroot=$sysroot" "QMAKE_LFLAGS+=--sysroot=$sysroot"
        "QMAKE_LFLAGS+=-L$sysroot/usr/lib/aarch64-linux-gnu"
        "QMAKE_LFLAGS+=-Wl,-rpath-link,$sysroot/usr/lib/aarch64-linux-gnu"
        "QMAKE_LFLAGS+=-Wl,-rpath-link,$sysroot/lib/aarch64-linux-gnu"
        'QMAKE_LIBS_OPENGL=-lGL' "QMAKE_LIBDIR_OPENGL=$sysroot/usr/lib/aarch64-linux-gnu")
else
    case $(uname -m) in aarch64|arm64) ;; *) printf 'Use --sysroot to cross compile on this CPU.\n' >&2; exit 1 ;; esac
fi
cd "$build_dir"
"$qmake_bin" "${args[@]}"
make -j "$jobs"
if [[ $project == "$project_dir/devstats.pro" ]]; then
    [[ -x $build_dir/devstats ]] || { printf 'devstats output missing.\n' >&2; exit 1; }
fi
printf 'Build complete: %s (Qt %s)\n' "$build_dir" "$qt_version"
