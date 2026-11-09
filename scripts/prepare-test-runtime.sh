#!/usr/bin/env bash
set -euo pipefail
# Download and extract only missing test runtime packages. No system installation.
sdk=${1:?Usage: prepare-test-runtime.sh SDK WORKSPACE}
workspace=${2:?Usage: prepare-test-runtime.sh SDK WORKSPACE}
sdk=$(cd "$sdk" && pwd)
workspace=$(cd "$workspace" && pwd)
[[ -x $sdk/host/usr/bin/qemu-aarch64-static && -d $sdk/sysroot ]] || { printf 'Incomplete SDK.\n' >&2; exit 1; }
apt_root="$workspace/apt-runtime"
overlay="$workspace/runtime-overlay"
mkdir -p "$apt_root/etc/apt/sources.list.d" "$apt_root/etc/apt/preferences.d" \
    "$apt_root/var/lib/apt/lists/partial" "$apt_root/var/cache/apt/archives/partial" \
    "$apt_root/var/lib/dpkg" "$overlay" "$workspace/runtime" "$workspace/test-home" \
    "$workspace/test-config" "$workspace/test-data" "$workspace/test-cache"
chmod 0700 "$workspace/runtime" "$workspace/test-home" "$workspace/test-config" "$workspace/test-data" "$workspace/test-cache"
touch "$apt_root/var/lib/dpkg/status"
keyring=/usr/share/keyrings/ubuntu-archive-keyring.gpg
[[ -r $keyring ]] || { printf 'Ubuntu archive signing keyring missing.\n' >&2; exit 1; }
printf '%s\n' "deb [arch=arm64 signed-by=$keyring] https://ports.ubuntu.com/ubuntu-ports focal main universe" > "$apt_root/etc/apt/sources.list"
apt_options=(-o "Dir=$apt_root" -o APT::Architecture=arm64 -o APT::Architectures::=arm64)
apt-get "${apt_options[@]}" update > "$workspace/runtime-download.log" 2>&1
(
    cd "$apt_root/var/cache/apt/archives"
    apt-get "${apt_options[@]}" download libqt5sql5-sqlite:arm64 libsqlite3-0:arm64 fonts-wqy-microhei >> "$workspace/runtime-download.log" 2>&1
)
printf 'package\tversion\tarchitecture\tsha256\n' > "$workspace/runtime-packages.tsv"
for package in "$apt_root/var/cache/apt/archives/"*.deb; do
    architecture=$(dpkg-deb -f "$package" Architecture)
    [[ $architecture == arm64 || $architecture == all ]] || { printf 'Wrong runtime package architecture.\n' >&2; exit 1; }
    dpkg-deb -x "$package" "$overlay"
    printf '%s\t%s\t%s\t%s\n' "$(dpkg-deb -f "$package" Package)" "$(dpkg-deb -f "$package" Version)" \
        "$(dpkg-deb -f "$package" Architecture)" "$(sha256sum "$package" | cut -d ' ' -f 1)" >> "$workspace/runtime-packages.tsv"
done
python3 - "$workspace" <<'PY'
from pathlib import Path
from xml.sax.saxutils import escape
import sys
root = Path(sys.argv[1])
font_dir = escape(str(root / 'runtime-overlay/usr/share/fonts'))
cache_dir = escape(str(root / 'test-cache/fontconfig'))
(root / 'fonts.conf').write_text('<?xml version="1.0"?><!DOCTYPE fontconfig SYSTEM "fonts.dtd">\n'
    '<fontconfig><dir>/usr/share/fonts</dir><dir>' + font_dir + '</dir><cachedir>' + cache_dir + '</cachedir></fontconfig>\n')
PY
# Paths are embedded as shell-escaped literals, so even a workspace with spaces is safe.
{
    printf '#!/usr/bin/env bash\nset -euo pipefail\nulimit -c 0\n'
    printf 'sdk=%q\nworkspace=%q\n' "$sdk" "$workspace"
    cat <<'EOF'
exec env -u LD_LIBRARY_PATH "$sdk/host/usr/bin/qemu-aarch64-static" -L "$sdk/sysroot" \
    -E "LD_LIBRARY_PATH=$workspace/runtime-overlay/usr/lib/aarch64-linux-gnu:$sdk/sysroot/usr/lib/aarch64-linux-gnu:$sdk/sysroot/lib/aarch64-linux-gnu" \
    -E "QT_QPA_PLATFORM=${DEVSTATS_QPA_PLATFORM:-offscreen}" \
    -E "QT_PLUGIN_PATH=$workspace/runtime-overlay/usr/lib/aarch64-linux-gnu/qt5/plugins:$sdk/sysroot/usr/lib/aarch64-linux-gnu/qt5/plugins" \
    -E "FONTCONFIG_FILE=$workspace/fonts.conf" \
    -E "XDG_RUNTIME_DIR=${DEVSTATS_TEST_RUNTIME_DIR:-$workspace/runtime}" \
    -E "DEVSTATS_DATA_DIR=${DEVSTATS_DATA_DIR:-$workspace/test-data/devstats}" \
    -E "XDG_CONFIG_HOME=$workspace/test-config" -E "XDG_DATA_HOME=$workspace/test-data" \
    -E "XDG_CACHE_HOME=$workspace/test-cache" "$@"
EOF
} > "$workspace/run-arm64.sh"
chmod 0755 "$workspace/run-arm64.sh"
printf 'Isolated runtime: %s/run-arm64.sh EXECUTABLE [ARGUMENTS...]\n' "$workspace"
