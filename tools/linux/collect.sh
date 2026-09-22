#!/usr/bin/env bash
# Collect everything needed to debug SmoothPan on this machine into one folder
# (+ .tar.gz): system/graphics info, versions, plugin compatibility, DF/DFHack
# logs, and all SmoothPan logs/captures/screenshots.  If Dwarf Fortress is
# running, it also asks the live plugin for `smoothpan diag`.
#
#   tools/linux/collect.sh                 # -> ~/smoothpan-debug-<timestamp>/
#   tools/linux/collect.sh --out DIR
#   tools/linux/collect.sh --no-live       # do not talk to the running game
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

OUT=""
LIVE=1
while [ $# -gt 0 ]; do
    case "$1" in
        --out) OUT="$2"; shift ;;
        --no-live) LIVE=0 ;;
        -h|--help) sed -n '2,10p' "$0"; exit 0 ;;
        *) sp_die "unknown argument: $1" ;;
    esac
    shift
done

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT="${OUT:-$HOME/smoothpan-debug-$STAMP}"
mkdir -p "$OUT"
DF="$(sp_find_df || true)"
S="$OUT/summary.txt"

{
    echo "# SmoothPan debug bundle $STAMP"
    echo "df_dir: ${DF:-NOT FOUND}"
    echo "dfhack_installed: $( [ -n "$DF" ] && sp_dfhack_describe "$DF" || echo '?')"
    if [ -n "$DF" ] && [ -f "$DF/hack/plugins/smoothpan.plug.so" ]; then
        P="$DF/hack/plugins/smoothpan.plug.so"
        echo "plugin_installed: SmoothPan $(sp_plugin_build_version "$P") built for DFHack $(sp_plugin_dfhack_tag "$P")"
        echo "plugin_sha256: $(sha256sum "$P" | cut -d' ' -f1)"
        command -v objdump >/dev/null && echo "plugin_glibc_needed: $(objdump -T "$P" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -n1)"
    else
        echo "plugin_installed: NO"
    fi
    echo "glibc: $(ldd --version 2>/dev/null | head -n1)"
    echo "kernel: $(uname -srm)"
    [ -f /etc/os-release ] && echo "os: $(. /etc/os-release; echo "$PRETTY_NAME")"
    echo "session: XDG_SESSION_TYPE=${XDG_SESSION_TYPE:-} WAYLAND_DISPLAY=${WAYLAND_DISPLAY:-} DISPLAY=${DISPLAY:-} DESKTOP=${XDG_CURRENT_DESKTOP:-}"
    if command -v glxinfo >/dev/null; then glxinfo -B 2>/dev/null | grep -E 'renderer string|version string|vendor string' ; fi
    command -v nvidia-smi >/dev/null && nvidia-smi --query-gpu=name,driver_version --format=csv,noheader 2>/dev/null
    command -v xrandr >/dev/null && xrandr --current 2>/dev/null | grep -E ' connected|\*' | head -6
    echo "df_running: $(pgrep -xa dwarfort | head -n2 | tr '\n' ' ')"
} > "$S" 2>&1

if [ -n "$DF" ]; then
    # Which SDL/libs DF loads (from the DF folder's point of view).
    {
        echo "== ls DF =="; ls -la "$DF" | head -60
        echo "== ls hack/plugins (smoothpan) =="; ls -la "$DF/hack/plugins" | grep -i smooth
        for b in "$DF/dwarfort" "$DF/libg_src_lib.so"; do
            [ -f "$b" ] || continue
            echo "== ldd $(basename "$b") =="; (cd "$DF" && LD_LIBRARY_PATH="$DF:$DF/hack" ldd "$b" 2>&1)
            if command -v readelf >/dev/null; then
                echo "== SDL relocations in $(basename "$b") =="
                readelf -rW "$b" 2>/dev/null | grep -E 'SDL_(RenderCopy|RenderPresent|SetRenderTarget|RenderSetClipRect|RenderFillRect|GetMouseState)' | head -20
            fi
        done
        if [ -f "$DF/hack/plugins/smoothpan.plug.so" ]; then
            echo "== ldd smoothpan.plug.so =="; LD_LIBRARY_PATH="$DF:$DF/hack" ldd "$DF/hack/plugins/smoothpan.plug.so" 2>&1
        fi
        PID="$(pgrep -x dwarfort | head -n1)"
        if [ -n "$PID" ] && [ -r "/proc/$PID/maps" ]; then
            echo "== libraries mapped in running DF (pid $PID) =="
            awk '{print $6}' "/proc/$PID/maps" | grep '\.so' | sort -u
        fi
    } > "$OUT/binaries.txt" 2>&1

    # Live diag via dfhack-run (DFHack's remote console).
    if [ "$LIVE" = 1 ] && [ -x "$DF/dfhack-run" ] && pgrep -x dwarfort >/dev/null; then
        (cd "$DF" && timeout 20 ./dfhack-run smoothpan diag) > "$OUT/live_diag.txt" 2>&1 \
            || echo "(dfhack-run failed: is the plugin enabled / DFHack remote server running?)" >> "$OUT/live_diag.txt"
    fi

    # Crash backtraces (systemd-coredump), newest DF crash only.
    if command -v coredumpctl >/dev/null; then
        coredumpctl --no-pager list dwarfort 2>/dev/null | tail -n5 > "$OUT/crashes.txt"
        coredumpctl --no-pager info dwarfort 2>/dev/null | head -n 150 >> "$OUT/crashes.txt"
    fi

    # DF / DFHack logs.
    for f in stderr.log stdout.log dfhack.history; do
        [ -f "$DF/$f" ] && tail -c 400000 "$DF/$f" > "$OUT/df_$f"
    done
    # SmoothPan output folder (telemetry, logs, screenshots).
    SPD="$DF/dfhack-config/smoothpan"
    if [ -d "$SPD" ]; then
        mkdir -p "$OUT/smoothpan"
        find "$SPD" -maxdepth 1 -type f \( -name '*.txt' -o -name '*.png' \) -mmin -1440 -exec cp {} "$OUT/smoothpan/" \;
        # BMP captures are huge: keep only the newest 12, converted to PNG when possible.
        ls -t "$SPD"/*.bmp 2>/dev/null | head -n12 | while read -r b; do
            base="$(basename "${b%.bmp}")"
            if command -v convert >/dev/null; then convert "$b" "$OUT/smoothpan/$base.png" 2>/dev/null || cp "$b" "$OUT/smoothpan/"
            else cp "$b" "$OUT/smoothpan/"; fi
        done
        # Large text logs: keep the tail.
        for t in "$OUT"/smoothpan/*.txt; do
            [ -f "$t" ] || continue
            if [ "$(stat -c %s "$t")" -gt 8000000 ]; then tail -c 8000000 "$t" > "$t.tail" && mv "$t.tail" "$t"; fi
        done
    fi
fi

{
    echo
    echo "== key lines =="
    grep -hE '^(CHECK|OK|WARN|FAIL|INFO RESULT)' "$OUT"/live_diag.txt "$OUT"/smoothpan/smoothpan_selftest.txt "$OUT"/smoothpan/smoothpan_diag.txt 2>/dev/null
    grep -hE 'DISABLED|compositor active' "$OUT"/smoothpan/smoothpan_compositor.txt 2>/dev/null | tail -3
} >> "$S"

tar -C "$(dirname "$OUT")" -czf "$OUT.tar.gz" "$(basename "$OUT")"
sp_log "bundle: $OUT  (archive: $OUT.tar.gz, $(du -h "$OUT.tar.gz" | cut -f1))"
cat "$S"
