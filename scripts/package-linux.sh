#!/usr/bin/env bash
set -euo pipefail
project_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
binary=${1:-"$project_dir/build/linux-arm64/devstats"}
output_dir=${2:-"$project_dir/../../dist"}
version=$(tr -d '\r\n' < "$project_dir/VERSION")
[[ $version =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || { printf 'Invalid Debian version.\n' >&2; exit 2; }
[[ -f $binary ]] || { printf 'Build output missing: %s\n' "$binary" >&2; exit 1; }
for tool in dpkg-deb readelf; do command -v "$tool" >/dev/null; done
readelf -h "$binary" | grep -q 'Machine:.*AArch64' || { printf 'Expected an AArch64 ELF.\n' >&2; exit 1; }
readelf -l "$binary" | grep -q '/lib/ld-linux-aarch64.so.1' || { printf 'Unexpected ELF interpreter.\n' >&2; exit 1; }
if readelf -d "$binary" | grep -Eq '\((RPATH|RUNPATH)\)'; then
    printf 'Remove build-directory RPATH/RUNPATH before packaging.\n' >&2; exit 1
fi
glibc_max=$(readelf --version-info "$binary" | grep -o 'GLIBC_[0-9.]*' | sort -Vu | tail -n 1)
[[ -n $glibc_max && $(printf '%s\n' 2.31 "${glibc_max#GLIBC_}" | sort -V | tail -n 1) == 2.31 ]] || {
    printf 'The executable requires %s; target maximum is GLIBC_2.31.\n' "$glibc_max" >&2; exit 1;
}
mkdir -p "$output_dir"
output_dir=$(cd "$output_dir" && pwd)
stage=$(mktemp -d "$output_dir/.devstats-package-XXXXXXXX")
cleanup() { [[ -n ${stage:-} && $stage == "$output_dir"/.devstats-package-* && -d $stage ]] && rm -r -- "$stage"; }
trap cleanup EXIT
chmod 0755 "$stage"
install -d "$stage/DEBIAN" "$stage/usr/bin" "$stage/usr/share/applications" \
    "$stage/usr/share/icons/hicolor/scalable/apps" "$stage/usr/share/doc/devstats"
install -m 0755 "$binary" "$stage/usr/bin/devstats"
install -m 0644 "$project_dir/packaging/devstats.desktop" "$stage/usr/share/applications/devstats.desktop"
install -m 0644 "$project_dir/packaging/devstats.svg" "$stage/usr/share/icons/hicolor/scalable/apps/devstats.svg"
install -m 0644 "$project_dir/packaging/copyright" "$stage/usr/share/doc/devstats/copyright"
install -m 0644 "$project_dir/packaging/LICENSE" "$stage/usr/share/doc/devstats/LICENSE"
install -m 0644 "$project_dir/README.md" "$stage/usr/share/doc/devstats/README.md"
install -m 0644 "$project_dir/TARGET-ENVIRONMENT.md" "$stage/usr/share/doc/devstats/TARGET-ENVIRONMENT.md"
installed_size=$(du -sk "$stage/usr" | awk '{print $1}')
sed -e "s/@VERSION@/$version/g" -e "s/@INSTALLED_SIZE@/$installed_size/g" "$project_dir/packaging/control.in" > "$stage/DEBIAN/control"
(
    cd "$stage"
    find usr -type f -print0 | sort -z | xargs -0 md5sum > DEBIAN/md5sums
)
deb="$output_dir/devstats_${version}_arm64.deb"
dpkg-deb --root-owner-group -Zxz --build "$stage" "$deb"
dpkg-deb --info "$deb"
printf 'Package: %s\n' "$deb"
