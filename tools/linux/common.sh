#!/usr/bin/env bash
# Shared helpers for the SmoothPan Linux scripts.  Source, do not run.

sp_log()  { printf '\033[1;36m[smoothpan]\033[0m %s\n' "$*" >&2; }
sp_warn() { printf '\033[1;33m[smoothpan] WARNING:\033[0m %s\n' "$*" >&2; }
sp_die()  { printf '\033[1;31m[smoothpan] ERROR:\033[0m %s\n' "$*" >&2; exit 1; }

SP_REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# Print candidate Steam library roots, one per line.
sp_steam_libraries() {
    local roots=(
        "$HOME/.local/share/Steam"
        "$HOME/.steam/steam"
        "$HOME/.steam/root"
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        "$HOME/snap/steam/common/.local/share/Steam"
    )
    local r vdf
    for r in "${roots[@]}"; do
        [ -d "$r" ] || continue
        echo "$r"
        vdf="$r/steamapps/libraryfolders.vdf"
        if [ -f "$vdf" ]; then
            grep -oE '"path"[[:space:]]+"[^"]+"' "$vdf" | sed -E 's/.*"path"[[:space:]]+"([^"]+)"/\1/'
        fi
    done | awk '!seen[$0]++'
}

# Locate the Dwarf Fortress install that has DFHack.  Honors $DF_DIR.
sp_find_df() {
    if [ -n "${DF_DIR:-}" ]; then
        [ -d "$DF_DIR" ] || sp_die "DF_DIR=$DF_DIR does not exist"
        echo "$DF_DIR"; return 0
    fi
    local lib cand
    while IFS= read -r lib; do
        cand="$lib/steamapps/common/Dwarf Fortress"
        if [ -f "$cand/hack/libdfhack.so" ]; then echo "$cand"; return 0; fi
    done < <(sp_steam_libraries)
    for cand in "$HOME/df" "$HOME/Dwarf Fortress" "$HOME/Games/Dwarf Fortress" /opt/dwarf-fortress; do
        if [ -f "$cand/hack/libdfhack.so" ]; then echo "$cand"; return 0; fi
    done
    return 1
}

# DFHack git description of the installed libdfhack.so, e.g. 53.16-r1.1-0-gb638b59d
sp_dfhack_describe() {
    local so="$1/hack/libdfhack.so"
    [ -f "$so" ] || return 1
    grep -aoE '[0-9]{2}\.[0-9]{2}-r[0-9]+(\.[0-9]+)?(rc[0-9]+)?-[0-9]+-g[0-9a-f]{7,}' "$so" | head -n1
}

# DFHack release tag, e.g. 53.16-r1.1
sp_dfhack_tag() {
    local d
    d="$(sp_dfhack_describe "$1")" || return 1
    [ -n "$d" ] || return 1
    echo "$d" | sed -E 's/-[0-9]+-g[0-9a-f]+$//'
}

# DFHack version a built plugin expects (its plugin_version string).
sp_plugin_dfhack_tag() {
    grep -aoE '[0-9]{2}\.[0-9]{2}-r[0-9]+(\.[0-9]+)?(rc[0-9]+)?-[0-9]+-g[0-9a-f]{7,}' "$1" | head -n1 | sed -E 's/-[0-9]+-g[0-9a-f]+$//'
}

sp_plugin_build_version() {
    grep -aoE 'SMOOTHPAN_BUILD=[0-9.]+' "$1" | head -n1 | cut -d= -f2
}
