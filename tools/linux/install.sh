#!/usr/bin/env bash
# Install smoothpan.plug.so into Dwarf Fortress/hack/plugins after checking it
# was built for the DFHack version you have.
#
#   tools/linux/install.sh                      # uses ./smoothpan.plug.so or build-linux/
#   tools/linux/install.sh --so path/to/smoothpan.plug.so
#   tools/linux/install.sh --autoenable         # also add 'enable smoothpan' to dfhack init
#   DF_DIR=/path/to/df tools/linux/install.sh
#
# If the versions differ it tells you to run tools/linux/build.sh, which builds
# the plugin against your exact DFHack.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

SO=""
AUTOENABLE=0
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --so) SO="$2"; shift ;;
        --autoenable) AUTOENABLE=1 ;;
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) sp_die "unknown argument: $1" ;;
    esac
    shift
done

if [ -z "$SO" ]; then
    for c in "$PWD/smoothpan.plug.so" "$SP_REPO_ROOT/smoothpan.plug.so" "$SP_REPO_ROOT/build-linux/smoothpan.plug.so"; do
        [ -f "$c" ] && { SO="$c"; break; }
    done
fi
[ -n "$SO" ] && [ -f "$SO" ] || sp_die "no smoothpan.plug.so found (pass --so, or run tools/linux/build.sh)"

DF="$(sp_find_df)" || sp_die "Dwarf Fortress with DFHack not found. Set DF_DIR=/path/to/'Dwarf Fortress'."
HAVE="$(sp_dfhack_tag "$DF" || true)"
WANT="$(sp_plugin_dfhack_tag "$SO" || true)"
sp_log "Dwarf Fortress:        $DF"
sp_log "installed DFHack:      ${HAVE:-unknown}"
sp_log "plugin built for:      ${WANT:-unknown} (SmoothPan $(sp_plugin_build_version "$SO"))"

if [ -n "$HAVE" ] && [ -n "$WANT" ] && [ "$HAVE" != "$WANT" ] && [ "$FORCE" = 0 ]; then
    sp_warn "version mismatch: DFHack refuses plugins built for another version."
    sp_die "run:  $SP_REPO_ROOT/tools/linux/build.sh   (builds against $HAVE and installs)"
fi

# glibc check: the plugin must not need a newer glibc than this system has.
if command -v objdump >/dev/null && command -v ldd >/dev/null; then
    need="$(objdump -T "$SO" 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sed 's/GLIBC_//' | sort -Vu | tail -n1)"
    have="$(ldd --version 2>/dev/null | head -n1 | grep -oE '[0-9]+\.[0-9]+$')"
    if [ -n "$need" ] && [ -n "$have" ] && [ "$(printf '%s\n%s\n' "$need" "$have" | sort -V | tail -n1)" != "$have" ]; then
        sp_die "plugin needs glibc $need but this system has $have — run tools/linux/build.sh"
    fi
fi

mkdir -p "$DF/hack/plugins"
DEST="$DF/hack/plugins/smoothpan.plug.so"
if [ -f "$DEST" ]; then cp -f "$DEST" "$DEST.bak"; sp_log "previous plugin saved as $DEST.bak"; fi
cp -f "$SO" "$DEST"
sp_log "installed -> $DEST"

if [ "$AUTOENABLE" = 1 ]; then
    INIT="$DF/dfhack-config/init/dfhack.init"
    mkdir -p "$(dirname "$INIT")"
    if ! grep -qs '^enable smoothpan' "$INIT"; then
        printf '\n# SmoothPan (smooth pan + zoom)\nenable smoothpan\n' >> "$INIT"
        sp_log "added 'enable smoothpan' to $INIT"
    fi
fi

cat >&2 <<EOF

Next:
  1. Start Dwarf Fortress (Steam) and load a fortress.
  2. In the DFHack console (or: cd "$DF" && ./dfhack-run enable smoothpan):
       enable smoothpan
     Expect: "SmoothPan $(sp_plugin_build_version "$SO") enabled (linux, DFHack ${HAVE:-?})".
  3. Testing guide: $SP_REPO_ROOT/docs/LINUX_TESTING.md
EOF
