#!/usr/bin/env bash
# Remove all discovered, verified user installations. Always retain user data.
set -euo pipefail
fail() { printf 'DevStats uninstall: %s\n' "$*" >&2; exit 1; }
uid=$(id -u)
[[ $uid != 0 ]] || fail 'run as your ordinary desktop user, without sudo.'
dry_run=0; anchors=(); extra_parents=(); prefix=
[[ -z ${DEVSTATS_USER_INSTALL_DIR:-} ]] || anchors+=("$DEVSTATS_USER_INSTALL_DIR")
while (($#)); do
    case $1 in
        --dry-run) dry_run=1 ;;
        --prefix) (($# >= 2)) && [[ -n $2 ]] || fail '--prefix needs a nonempty absolute directory.'; prefix=$2; shift ;;
        --install-dir) (($# >= 2)) || fail '--install-dir needs an absolute directory.'; anchors+=("$2"); shift ;;
        --search-dir) (($# >= 2)) || fail '--search-dir needs an absolute directory.'; extra_parents+=("$2"); shift ;;
        --help|-h) printf '%s\n' 'Usage: uninstall-user.sh [--dry-run] [--prefix DIRECTORY] [--install-dir DIRECTORY] [--search-dir DIRECTORY]' 'Remove ALL verified current-user DevStats installations discovered in bounded locations.' 'Default search: ~/.local/opt, script/explicit installations, managed links and recorded paths.' 'With --prefix: only DIRECTORY/opt, bin, share and config; reject paths outside DIRECTORY.' 'Repeat --search-dir for another directory of installations, or --install-dir for an anchor.' 'Settings, history, tasks, custom data and caches are ALWAYS retained. No purge option.' 'No process is stopped. Exit the discovered DevStats/devmon instances before uninstalling.'; exit 0 ;;
        *) fail "unknown option: $1 (settings and history are always retained)" ;;
    esac
    shift
done
# Destructive paths must have no symlink component, dot component or ambiguous slash.
safe_path() {
    local path=$1 part walk= remainder
    [[ $path == /* && $path != / && $path != */ && $path != *$'\n'* && $path != *$'\r'* ]] || fail "unsafe absolute path: $path"
    remainder=${path#/}
    while [[ -n $remainder ]]; do
        part=${remainder%%/*}
        [[ -n $part && $part != . && $part != .. ]] || fail "non-canonical path: $path"
        walk+="/$part"
        [[ ! -L $walk ]] || fail "refusing symlink path: $walk"
        [[ $remainder == */* ]] || break
        remainder=${remainder#*/}
    done
}
if [[ -n $prefix ]]; then safe_path "$prefix"; fi
within_prefix() {
    [[ -z $prefix || $1 == "$prefix" || $1 == "$prefix/"* ]] || fail "path outside explicit prefix: $1"
}
parents=(); bins=(); data_roots=(); config_roots=(); installs=(); remove_files=()
declare -A parent_seen=() bin_seen=() data_seen=() config_seen=() install_seen=() file_seen=()
add_location() {
    local kind=$1 path=$2
    safe_path "$path"
    within_prefix "$path"
    [[ ! -e $path || ( -d $path && -O $path ) ]] || fail "not an owned user directory: $path"
    case $kind in
        bin) [[ -z ${bin_seen["$path"]:-} ]] || return 0; bin_seen["$path"]=1; bins+=("$path") ;;
        data) [[ -z ${data_seen["$path"]:-} ]] || return 0; data_seen["$path"]=1; data_roots+=("$path") ;;
        config) [[ -z ${config_seen["$path"]:-} ]] || return 0; config_seen["$path"]=1; config_roots+=("$path") ;;
    esac
}
add_parent() {
    local path=$1
    safe_path "$path"
    within_prefix "$path"
    [[ -z ${parent_seen["$path"]:-} ]] || return 0
    [[ ! -e $path || ( -d $path && -r $path && -x $path ) ]] || fail "unreadable search directory: $path"
    parent_seen["$path"]=1; parents+=("$path")
}
add_install() {
    local directory=$1 paths_file
    safe_path "$directory"
    within_prefix "$directory"
    [[ -z ${install_seen["$directory"]:-} ]] || return 0
    [[ -d $directory && -O $directory ]] || fail "not an owned installation: $directory"
    [[ -f $directory/.devstats-user-install && ! -L $directory/.devstats-user-install && -O $directory/.devstats-user-install ]] &&
        [[ $(cat -- "$directory/.devstats-user-install") == 'DevStats user installation v1' ]] || fail "unrecognized installation marker: $directory"
    install_seen["$directory"]=1; installs+=("$directory")
    add_parent "${directory%/*}"
    # This record is data, never sourced as shell code.
    paths_file="$directory/.devstats-user-paths"
    if [[ -e $paths_file || -L $paths_file ]]; then
        [[ -f $paths_file && ! -L $paths_file && -O $paths_file ]] || fail "unsafe installation path record: $paths_file"
        local -a paths=()
        mapfile -t paths < "$paths_file"
        [[ ${#paths[@]} == 4 && ${paths[0]} == 'DevStats user paths v1' ]] || fail "invalid installation path record: $paths_file"
        add_location bin "${paths[1]}"; add_location data "${paths[2]}"; add_location config "${paths[3]}"
    fi
}
if [[ -n $prefix ]]; then
    add_location bin "$prefix/bin"; add_location data "$prefix/share"; add_location config "$prefix/config"; add_parent "$prefix/opt"
else
    add_location bin "$HOME/.local/bin"; add_location data "$HOME/.local/share"; add_location config "$HOME/.config"
    [[ -z ${DEVSTATS_USER_BIN_DIR:-} ]] || add_location bin "$DEVSTATS_USER_BIN_DIR"
    [[ -z ${XDG_DATA_HOME:-} ]] || add_location data "$XDG_DATA_HOME"
    [[ -z ${XDG_CONFIG_HOME:-} ]] || add_location config "$XDG_CONFIG_HOME"
    add_parent "$HOME/.local/opt"
fi
script=$(readlink -f -- "$0") || fail 'cannot resolve the uninstaller.'
source_dir=${script%/*}
if [[ -e $source_dir/.devstats-user-install || -L $source_dir/.devstats-user-install ]]; then add_install "$source_dir"; fi
for directory in "${anchors[@]}"; do
    safe_path "$directory"
    if [[ -e $directory || -L $directory ]]; then add_install "$directory"
    else printf 'Absent installation anchor: %s\n' "$directory"; add_parent "${directory%/*}"; fi
done
for directory in "${extra_parents[@]}"; do add_parent "$directory"; done
# A queue only visits direct children and known command links; seen sets bound cycles.
parent_index=0; bin_index=0
while ((parent_index < ${#parents[@]} || bin_index < ${#bins[@]})); do
    while ((parent_index < ${#parents[@]})); do
        parent=${parents[parent_index]}; parent_index=$((parent_index + 1))
        [[ -d $parent ]] || continue
        while IFS= read -r -d '' directory; do
            if [[ -e $directory/.devstats-user-install || -L $directory/.devstats-user-install ]]; then add_install "$directory"
            elif [[ -f $directory/.devstats-user-package ]]; then printf 'Retain uninstalled archive: %s\n' "$directory"; fi
        done < <(find "$parent" -mindepth 1 -maxdepth 1 -print0)
    done
    while ((bin_index < ${#bins[@]})); do
        bin=${bins[bin_index]}; bin_index=$((bin_index + 1))
        for name in devstats devmon devstats-uninstall; do
            path="$bin/$name"
            [[ -L $path ]] || continue
            target=$(readlink -- "$path") || fail "cannot read command link: $path"
            suffix=$name; [[ $name != devstats-uninstall ]] || suffix=uninstall-user.sh
            if [[ $target == /*/"$suffix" ]]; then
                directory=${target%/"$suffix"}
                if [[ -e $directory/.devstats-user-install || -L $directory/.devstats-user-install ]]; then add_install "$directory"; fi
            fi
        done
    done
done
verify_install() {
    local directory=$1 line relative entry version
    [[ -f $directory/SHA256SUMS && ! -L $directory/SHA256SUMS ]] || fail "missing regular checksum manifest: $directory"
    local -A allowed=([SHA256SUMS]=1 [.devstats-user-install]=1 [.devstats-user-paths]=1 [devmon]=1)
    # Versions before 0.8 ship this launcher as a symlink, which SHA256SUMS does
    # not cover. When present, verify its exact type, target and ownership.
    if [[ -e $directory/devmon || -L $directory/devmon ]]; then
        [[ -L $directory/devmon && $(readlink -- "$directory/devmon") == devstats && $(stat -c %u -- "$directory/devmon") == "$uid" ]] || fail "modified devmon launcher: $directory/devmon"
    fi
    while IFS= read -r line || [[ -n $line ]]; do
        [[ $line =~ ^[0-9a-f]{64}\ \ (.+)$ ]] || fail "invalid checksum manifest: $directory"
        relative=${BASH_REMATCH[1]}
        [[ $relative != /* && $relative != *'\'* && $relative != *$'\r'* && /$relative/ != *'/../'* && /$relative/ != *'/./'* && $relative != *'//'* ]] || fail "unsafe checksum manifest path: $directory"
        [[ $relative != .devstats-user-install && $relative != .devstats-user-paths && $relative != SHA256SUMS ]] || fail "reserved checksum manifest path: $directory"
        [[ -f $directory/$relative && ! -L $directory/$relative ]] || fail "not a regular package file: $directory/$relative"
        allowed["$relative"]=1
        while [[ $relative == */* ]]; do relative=${relative%/*}; allowed["$relative"]=1; done
    done < "$directory/SHA256SUMS"
    for relative in .devstats-user-package VERSION devstats libexec/devstats share/devstats.svg; do
        [[ -n ${allowed["$relative"]:-} ]] || fail "incomplete package manifest: $directory/$relative"
    done
    [[ $(cat -- "$directory/.devstats-user-package") == 'DevStats private Qt runtime v1' ]] || fail "unrecognized package marker: $directory"
    version=$(cat -- "$directory/VERSION")
    [[ $version =~ ^[0-9][0-9A-Za-z.+~-]*$ ]] || fail "invalid package version: $directory"
    [[ -r $directory && -w $directory && -x $directory && -w ${directory%/*} && -x ${directory%/*} ]] || fail "inaccessible installation directory or parent: $directory"
    while IFS= read -r -d '' entry; do
        relative=${entry#"$directory/"}
        [[ -O $entry && -n ${allowed["$relative"]:-} ]] || fail "unrecognized or unowned installation entry: $entry"
        if [[ -L $entry ]]; then
            [[ $relative == devmon && $(readlink -- "$entry") == devstats && $(stat -c %u -- "$entry") == "$uid" ]] || fail "unexpected installation symlink: $entry"
        else
            [[ -f $entry || -d $entry ]] || fail "special installation entry: $entry"
            [[ ! -d $entry || ( -r $entry && -w $entry && -x $entry ) ]] || fail "inaccessible installation directory: $entry"
        fi
    done < <(find "$directory" -mindepth 1 -print0)
    (cd -- "$directory" && sha256sum --quiet -c SHA256SUMS) || fail "installed files changed; inspect before uninstalling: $directory"
}
# Full preflight across every candidate completes before any deletion.
protected=()
for directory in "${data_roots[@]}"; do protected+=("$directory/devstats"); done
for directory in "${config_roots[@]}"; do protected+=("$directory/devstats"); done
if [[ -n ${DEVSTATS_DATA_DIR:-} ]]; then
    custom_data=$(readlink -m -- "$DEVSTATS_DATA_DIR") || fail 'cannot resolve retained custom data path.'
    protected+=("$custom_data")
fi
for directory in "${installs[@]}"; do
    for retained in "${protected[@]}"; do
        [[ $retained != / && $retained != "$directory" && $retained != "$directory/"* && $directory != "$retained/"* ]] || fail "installation overlaps retained user data: $directory ($retained)"
    done
    verify_install "$directory"
done
for proc in /proc/[0-9]*/exe; do
    [[ -O ${proc%/exe} && -e $proc ]] || continue
    running=$(readlink -- "$proc" 2>/dev/null) || continue
    running_path=${running%' (deleted)'}
    for directory in "${installs[@]}"; do
        if [[ $proc -ef $directory/libexec/devstats || $proc -ef $directory/libexec/devmon || $running_path == "$directory/libexec/devstats" || $running_path == "$directory/libexec/devmon" ]]; then
            pid=${proc%/exe}
            fail "DevStats/devmon is running (PID ${pid##*/}; $running). Exit it safely, then retry; nothing was removed."
        fi
    done
done
queue_file() {
    local path=$1
    [[ -w ${path%/*} && -x ${path%/*} ]] || fail "managed entry parent is not writable: $path"
    [[ -n ${file_seen["$path"]:-} ]] || { file_seen["$path"]=1; remove_files+=("$path"); }
}
plan_link() {
    local path=$1 suffix=$2 target directory
    safe_path "${path%/*}"
    [[ -e $path || -L $path ]] || return 0
    if [[ -L $path ]]; then
        target=$(readlink -- "$path") || fail "cannot read link: $path"
        for directory in "${installs[@]}"; do
            if [[ $target == "$directory/$suffix" ]]; then
                [[ $(stat -c %u -- "$path") == "$uid" ]] || fail "unowned managed link: $path"
                queue_file "$path"; return 0
            fi
        done
    fi
    printf 'Retain unrelated entry: %s\n' "$path"
}
for bin in "${bins[@]}"; do
    plan_link "$bin/devstats" devstats; plan_link "$bin/devmon" devmon; plan_link "$bin/devstats-uninstall" uninstall-user.sh
done
# Compare byte-exact Desktop Entry strings; never execute or source their contents.
desktop_value() {
    local value=$1 character index
    for ((index=0; index<${#value}; index++)); do
        character=${value:index:1}
        case $character in '\') printf '%s' '\\' ;; $'\t') printf '%s' '\t' ;; *) printf '%s' "$character" ;; esac
    done
}
desktop_exec() {
    local value=$1 character escaped= index
    for ((index=0; index<${#value}; index++)); do
        character=${value:index:1}
        case $character in '\'|'"'|'`'|'$') escaped+="\\$character" ;; '%') escaped+='%%' ;; *) escaped+="$character" ;; esac
    done
    desktop_value "\"$escaped\""
}
menu_content() {
    # $2: the description line, which differs between generations of the installer.
    printf '# DevStats user installation v1\n[Desktop Entry]\nType=Application\nName=DevStats\nComment=%s\nExec=/usr/bin/env -- ' "${2:-System monitor and stall diagnosis}"
    desktop_exec "$1/devstats"
    printf ' --show\nIcon='; desktop_value "$1/share/devstats.svg"
    printf '\nTerminal=false\nCategories=System;Monitor;\nStartupNotify=false\n'
}
autostart_content() {
    printf '[Desktop Entry]\nType=Application\nName=DevStats\nExec=/usr/bin/env -- '
    desktop_exec "$1/devstats"
    printf ' --background\nIcon=devstats\nTerminal=false\nX-GNOME-Autostart-enabled=true\n'
}
plan_desktop() {
    local path=$1 kind=$2 directory expected_exec line selected_exec=0
    safe_path "${path%/*}"
    [[ -e $path || -L $path ]] || return 0
    [[ -f $path && ! -L $path && -O $path ]] || fail "unsafe desktop entry: $path"
    for directory in "${installs[@]}"; do
        if cmp -s -- "$path" <("${kind}_content" "$directory"); then queue_file "$path"; return 0; fi
        if [[ $kind == menu ]] && cmp -s -- "$path" <(menu_content "$directory" 'System and development monitor'); then queue_file "$path"; return 0; fi
        if [[ $kind == menu ]]; then
            expected_exec=$(printf 'Exec=/usr/bin/env -- '; desktop_exec "$directory/devstats"; printf ' --show')
            while IFS= read -r line || [[ -n $line ]]; do [[ $line != "$expected_exec" ]] || selected_exec=1; done < "$path"
        fi
    done
    if [[ $kind == menu && $selected_exec == 1 && $(head -n 1 -- "$path") == '# DevStats user installation v1' ]]; then fail "modified managed desktop entry: $path"; fi
    printf 'Retain unrecognized desktop entry: %s\n' "$path"
}
for data in "${data_roots[@]}"; do
    plan_link "$data/icons/hicolor/scalable/apps/devstats.svg" share/devstats.svg
    plan_desktop "$data/applications/devstats.desktop" menu
done
for config in "${config_roots[@]}"; do plan_desktop "$config/autostart/devstats.desktop" autostart; done
printf 'Verified user installations to remove: %s\n' "${#installs[@]}"
for directory in "${installs[@]}"; do printf 'Remove installation: %s (version %s)\n' "$directory" "$(cat -- "$directory/VERSION")"; done
for path in "${remove_files[@]}"; do printf 'Remove managed file/link: %s\n' "$path"; done
for path in "${protected[@]}"; do printf 'Retain all user data/settings: %s\n' "$path"; done
printf 'All history, task records, settings, custom data and caches are retained.\n'
if [[ $dry_run == 1 ]]; then printf 'Dry run: nothing changed.\n'; exit 0; fi
for path in "${remove_files[@]}"; do rm -- "$path"; done
for directory in "${installs[@]}"; do rm -r -- "$directory"; done
printf 'Uninstalled %s verified DevStats user installation(s). User data was retained.\n' "${#installs[@]}"
