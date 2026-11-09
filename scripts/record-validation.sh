#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:?Usage: record-validation.sh BINARY DEB OUTPUT_DIRECTORY}
deb=${2:?Usage: record-validation.sh BINARY DEB OUTPUT_DIRECTORY}
output=${3:?Usage: record-validation.sh BINARY DEB OUTPUT_DIRECTORY}
binary=$(cd "$(dirname "$binary")" && pwd)/$(basename "$binary")
deb=$(cd "$(dirname "$deb")" && pwd)/$(basename "$deb")
mkdir -p "$output"
output=$(cd "$output" && pwd)
version=$(tr -d '\r\n' < "$project_dir/VERSION")
[[ $(dpkg-deb -f "$deb" Package) == devstats && $(dpkg-deb -f "$deb" Version) == "$version" && $(dpkg-deb -f "$deb" Architecture) == arm64 ]]
stage=$(mktemp -d "$output/.devstats-verify-XXXXXXXX")
cleanup() { [[ -n ${stage:-} && $stage == "$output"/.devstats-verify-* && -d $stage ]] && rm -r -- "$stage"; }
trap cleanup EXIT
dpkg-deb -x "$deb" "$stage"
cmp "$binary" "$stage/usr/bin/devstats"
[[ -f $stage/usr/share/applications/devstats.desktop && -f $stage/usr/share/icons/hicolor/scalable/apps/devstats.svg ]]
(cd "$stage" && md5sum --quiet -c <(dpkg-deb --ctrl-tarfile "$deb" | tar -xOf - ./md5sums))
readelf -h "$binary" | grep -q 'Machine:.*AArch64'
readelf -l "$binary" | grep -q '/lib/ld-linux-aarch64.so.1'
if readelf -d "$binary" | grep -Eq '\((RPATH|RUNPATH)\)'; then printf 'Unexpected ELF RPATH.\n' >&2; exit 1; fi
glibc_max=$(readelf --version-info "$binary" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n 1)
[[ -n $glibc_max && $(printf '%s\n' 2.31 "${glibc_max#GLIBC_}" | sort -V | tail -n 1) == 2.31 ]]
{
    printf 'DevStats %s ARM64 package validation\n' "$version"
    printf 'UTC: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
    file "$binary"
    printf 'Interpreter: /lib/ld-linux-aarch64.so.1\nMaximum glibc requirement: %s\nRPATH/RUNPATH: absent\n' "$glibc_max"
    printf 'Packaged ELF: byte-identical to build output\nPackage file checksums: passed\n'
    readelf -d "$binary" | awk '/NEEDED/ {print}'
    dpkg-deb --info "$deb"
    dpkg-deb --contents "$deb"
    sha256sum "$deb" "$binary"
} > "$output/package-validation.log"
(
    cd "$project_dir"
    find . \( -path './build' -o -path './dist' -o -path './validation' -o -type d -name '__pycache__' \) -prune -o -type f -print0 | sort -z | xargs -0 sha256sum
) > "$output/source-files.sha256"
if [[ -n ${DEVSTATS_SDK:-} ]]; then
    printf 'package\tversion\tarchitecture\tsha256\n' > "$output/build-packages.tsv"
    for archive in "$DEVSTATS_SDK"/apt-target/var/cache/apt/archives/*.deb "$DEVSTATS_SDK"/apt-host/var/cache/apt/archives/*.deb; do
        [[ -f $archive ]] || continue
        printf '%s\t%s\t%s\t%s\n' "$(dpkg-deb -f "$archive" Package)" "$(dpkg-deb -f "$archive" Version)" \
            "$(dpkg-deb -f "$archive" Architecture)" "$(sha256sum "$archive" | cut -d ' ' -f 1)" >> "$output/build-packages.tsv"
    done
fi
if [[ -n ${DEVSTATS_RUNTIME_WORKSPACE:-} && -f $DEVSTATS_RUNTIME_WORKSPACE/runtime-packages.tsv ]]; then
    cp "$DEVSTATS_RUNTIME_WORKSPACE/runtime-packages.tsv" "$output/runtime-packages.tsv"
fi
printf 'Package validated; evidence: %s\n' "$output"
