#!/usr/bin/env bash
# No root, package manager or global environment changes; never turns on login startup.
set -euo pipefail
umask 022
fail() { printf 'DevStats install: %s\n' "$*" >&2; exit 1; }
[[ $(id -u) != 0 ]] || fail 'run as your ordinary desktop user, without sudo.'
prefix=
while (($#)); do
    case $1 in
        --prefix) (($# >= 2)) && [[ -n $2 ]] || fail '--prefix needs a nonempty absolute directory.'; prefix=$2; shift ;;
        --help|-h) printf '%s\n' 'Usage: install-user.sh [--prefix DIRECTORY]' 'Default: ~/.local/opt and ~/.local/bin, with current XDG data/config locations.' 'With --prefix: DIRECTORY/opt, bin, share and config; no HOME changes.'; exit 0 ;;
        *) fail "unknown option: $1" ;;
    esac
    shift
done
source_dir=$(cd -- "$(dirname -- "$(readlink -f -- "$0")")" && pwd -P)
version=$(cat "$source_dir/VERSION")
[[ $version =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || fail 'invalid package version.'
install_dir=${DEVSTATS_USER_INSTALL_DIR:-"$HOME/.local/opt/devstats-$version"}
bin_dir=${DEVSTATS_USER_BIN_DIR:-"$HOME/.local/bin"}
data_dir=${XDG_DATA_HOME:-"$HOME/.local/share"}
config_dir=${XDG_CONFIG_HOME:-"$HOME/.config"}
if [[ -n $prefix ]]; then
    [[ $prefix == /* && $prefix != / && $prefix != */ && $prefix != *//* && $prefix != *$'\n'* && $prefix != *$'\r'* && /$prefix/ != */../* && /$prefix/ != */./* ]] || fail 'prefix must be an unambiguous absolute directory.'
    IFS=/ read -r -a prefix_parts <<< "${prefix#/}"; prefix_walk=
    for prefix_part in "${prefix_parts[@]}"; do prefix_walk+="/$prefix_part"; [[ ! -L $prefix_walk ]] || fail "refusing symlink prefix: $prefix_walk"; done
    install_dir="$prefix/opt/devstats-$version"; bin_dir="$prefix/bin"; data_dir="$prefix/share"; config_dir="$prefix/config"
fi
for path in "$install_dir" "$bin_dir" "$data_dir" "$config_dir"; do
    [[ $path == /* && $path != *$'\n'* && $path != *$'\r'* ]] || fail 'install paths must be absolute and contain no newlines.'
done
[[ -f $source_dir/.devstats-user-package && -f $source_dir/SHA256SUMS ]] || fail 'incomplete package.'
(cd "$source_dir" && sha256sum --quiet -c SHA256SUMS) || fail 'package checksum verification failed.'
# A symlink is managed only when it resolves to a marked bundle owned by us.
managed_link() {
    [[ -L $1 ]] || return 1
    local target dir
    target=$(readlink -f -- "$1") || return 1
    case "$target" in
        */share/devstats.svg) dir=${target%/share/devstats.svg} ;;
        */devstats|*/devmon|*/uninstall-user.sh) dir=${target%/*} ;;
        *) return 1 ;;
    esac
    [[ -O $dir && -f $dir/.devstats-user-install && ! -L $dir/.devstats-user-install ]] &&
        [[ $(cat "$dir/.devstats-user-install") == 'DevStats user installation v1' ]]
}
app_file="$data_dir/applications/devstats.desktop"
icon_file="$data_dir/icons/hicolor/scalable/apps/devstats.svg"
for path in "$bin_dir/devstats" "$bin_dir/devstats-uninstall" "$icon_file"; do
    if [[ -e $path || -L $path ]]; then managed_link "$path" || fail "refusing unrelated target: $path"; fi
done
if [[ -e $app_file || -L $app_file ]]; then
    [[ -f $app_file && ! -L $app_file && -O $app_file ]] &&
        [[ $(head -n 1 "$app_file") == '# DevStats user installation v1' ]] || fail "refusing unrelated desktop entry: $app_file"
fi
if [[ -e $install_dir || -L $install_dir ]]; then
    [[ -d $install_dir && ! -L $install_dir && -O $install_dir && -f $install_dir/.devstats-user-install ]] || fail "refusing existing directory: $install_dir"
    [[ $(cat "$install_dir/.devstats-user-install") == 'DevStats user installation v1' ]] || fail 'unrecognized installation marker.'
    cmp -s "$source_dir/SHA256SUMS" "$install_dir/SHA256SUMS" || fail 'same-version contents differ; choose a new empty DEVSTATS_USER_INSTALL_DIR.'
    (cd "$install_dir" && sha256sum --quiet -c SHA256SUMS) || fail 'installed files changed; choose a new empty DEVSTATS_USER_INSTALL_DIR.'
else
    mkdir -p -- "$(dirname -- "$install_dir")"
    stage=$(mktemp -d "$(dirname -- "$install_dir")/.devstats-install-XXXXXXXX")
    cleanup() { [[ -n ${stage:-} && -d $stage && $stage == "$(dirname -- "$install_dir")"/.devstats-install-* ]] && rm -r -- "$stage"; }
    trap cleanup EXIT
    cp -a -- "$source_dir/." "$stage/"
    printf 'DevStats user installation v1\n' > "$stage/.devstats-user-install"
    chmod -R go-w -- "$stage"
    # -T rejects replacement of an unexpectedly created directory.
    mv -T --no-clobber -- "$stage" "$install_dir"
    [[ ! -d $stage ]] || fail "install target appeared during install: $install_dir"
    stage=
    trap - EXIT
fi
# Record paths without sourcing them; the uninstaller can follow custom XDG/bin locations.
[[ ! -e $install_dir/.devstats-user-paths || ( -f $install_dir/.devstats-user-paths && -O $install_dir/.devstats-user-paths ) ]] && [[ ! -L $install_dir/.devstats-user-paths ]] || fail 'unsafe installation path record.'
paths_stage=$(mktemp "$install_dir/.devstats-paths-XXXXXXXX")
printf 'DevStats user paths v1\n%s\n%s\n%s\n' "$bin_dir" "$data_dir" "$config_dir" > "$paths_stage"
chmod 0600 "$paths_stage"
mv -fT -- "$paths_stage" "$install_dir/.devstats-user-paths"
mkdir -p -- "$bin_dir" "$(dirname -- "$app_file")" "$(dirname -- "$icon_file")"
ln -sfnT -- "$install_dir/devstats" "$bin_dir/devstats"
# Versions before 0.8 shipped a devmon command; this one has none. Remove only a
# link verified as ours; a user's own "devmon" is none of our business.
if managed_link "$bin_dir/devmon"; then rm -f -- "$bin_dir/devmon"; fi
ln -sfnT -- "$install_dir/uninstall-user.sh" "$bin_dir/devstats-uninstall"
ln -sfnT -- "$install_dir/share/devstats.svg" "$icon_file"
# Desktop Entry string escaping and Exec argument escaping are separate passes.
desktop_value() {
    local value=$1 character index
    for ((index=0; index<${#value}; index++)); do
        character=${value:index:1}
        case "$character" in
            '\') printf '%s' '\\' ;;
            $'\t') printf '%s' '\t' ;;
            *) printf '%s' "$character" ;;
        esac
    done
}
desktop_exec() {
    local value=$1 character escaped= index
    for ((index=0; index<${#value}; index++)); do
        character=${value:index:1}
        case "$character" in
            '\'|'"'|'`'|'$') escaped+="\\$character" ;;
            '%') escaped+='%%' ;;
            *) escaped+="$character" ;;
        esac
    done
    desktop_value "\"$escaped\""
}
app_stage=$(mktemp "$(dirname -- "$app_file")/.devstats-desktop-XXXXXXXX")
{
    printf '# DevStats user installation v1\n[Desktop Entry]\nType=Application\nName=DevStats\nComment=System monitor and stall diagnosis\nExec=/usr/bin/env -- '
    desktop_exec "$install_dir/devstats"
    printf ' --show\nIcon='; desktop_value "$install_dir/share/devstats.svg"
    printf '\nTerminal=false\nCategories=System;Monitor;\nStartupNotify=false\n'
} > "$app_stage"
chmod 0644 "$app_stage"
mv -fT -- "$app_stage" "$app_file"
# Login startup is the user's choice and never created here. An entry DevStats wrote
# names the version that was running when it was turned on, so move it to this one;
# otherwise every later login would start the old version. Edited entries stay as they are.
autostart_content() {
    printf '[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- '
    desktop_exec "$1/devstats"
    printf ' --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n'
}
autostart_file="$config_dir/autostart/devstats.desktop"
if [[ -f $autostart_file && ! -L $autostart_file && -O $autostart_file ]]; then
    for previous in "${install_dir%/*}"/*/; do
        previous=${previous%/}
        [[ $previous != "$install_dir" && ! -L $previous && -O $previous && -f $previous/.devstats-user-install ]] || continue
        cmp -s -- "$autostart_file" <(autostart_content "$previous") || continue
        autostart_stage=$(mktemp "${autostart_file%/*}/.devstats-autostart-XXXXXXXX")
        autostart_content "$install_dir" > "$autostart_stage"
        chmod 0600 "$autostart_stage"
        mv -fT -- "$autostart_stage" "$autostart_file"
        printf 'Login startup now opens DevStats %s instead of %s.\n' "$version" "$(cat -- "$previous/VERSION" 2>/dev/null || printf '%s' "${previous##*/}")"
        break
    done
fi
printf 'Installed DevStats %s\nLauncher: %s/devstats\nDesktop entry: %s\nUninstaller: %s/devstats-uninstall\nPrevious version directories are retained on install. The uninstaller removes all discovered user versions and always retains data. This installer does not enable login startup.\n' "$version" "$bin_dir" "$app_file" "$bin_dir"
case ":$PATH:" in *":$bin_dir:"*) ;; *) printf 'Your current PATH does not include %s. You can use its full path.\n' "$bin_dir" ;; esac
# Opening DevStats while a copy is still running only brings that copy forward.
if command -v pgrep >/dev/null 2>&1 && pgrep -u "$(id -u)" -x devstats >/dev/null 2>&1; then
    printf 'DevStats is still running. Quit it from the tray menu, then open it again to use version %s.\n' "$version"
fi
