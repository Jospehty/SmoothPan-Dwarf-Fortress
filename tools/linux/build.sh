#!/usr/bin/env bash
# Build smoothpan.plug.so from source against the exact DFHack version that is
# installed in your Dwarf Fortress folder, and (optionally) install it.
#
#   tools/linux/build.sh                 # detect DF + DFHack, build, install
#   tools/linux/build.sh --no-install    # build only
#   tools/linux/build.sh --install-deps  # apt/dnf/pacman the build deps first (uses sudo)
#   DF_DIR=/path/to/df tools/linux/build.sh
#   tools/linux/build.sh --dfhack 53.16-r1.1   # force a DFHack tag
#
# Build tree: ${SMOOTHPAN_BUILD_DIR:-~/.cache/smoothpan-build}/dfhack-<tag>
# (first build compiles DFHack's core library: a few minutes; later builds only
# recompile the plugin).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

INSTALL=1
INSTALL_DEPS=0
TAG=""
JOBS="$(nproc 2>/dev/null || echo 4)"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-install) INSTALL=0 ;;
        --install-deps) INSTALL_DEPS=1 ;;
        --dfhack) TAG="$2"; shift ;;
        --jobs|-j) JOBS="$2"; shift ;;
        -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
        *) sp_die "unknown argument: $1" ;;
    esac
    shift
done

DF="$(sp_find_df || true)"
if [ -z "$TAG" ]; then
    [ -n "$DF" ] || sp_die "Dwarf Fortress with DFHack not found. Set DF_DIR=/path/to/'Dwarf Fortress' or pass --dfhack <tag>."
    TAG="$(sp_dfhack_tag "$DF" || true)"
    [ -n "$TAG" ] || sp_die "could not read the DFHack version from $DF/hack/libdfhack.so"
fi
sp_log "Dwarf Fortress: ${DF:-<not found>}"
sp_log "DFHack tag:     $TAG"

install_deps() {
    local id=""
    [ -f /etc/os-release ] && id="$(. /etc/os-release; echo "${ID} ${ID_LIKE:-}")"
    case "$id" in
        *debian*|*ubuntu*)
            sudo apt-get update
            sudo apt-get install -y git cmake ninja-build g++ perl libxml-libxml-perl libxml-libxslt-perl zlib1g-dev ;;
        *fedora*|*rhel*)
            sudo dnf install -y git cmake ninja-build gcc-c++ perl perl-XML-LibXML perl-XML-LibXSLT zlib-devel ;;
        *arch*)
            sudo pacman -S --needed --noconfirm git cmake ninja gcc perl perl-xml-libxml perl-xml-libxslt zlib ;;
        *suse*)
            sudo zypper install -y git cmake ninja gcc-c++ perl perl-XML-LibXML perl-XML-LibXSLT zlib-devel ;;
        *) sp_die "unknown distro ($id); install: git cmake ninja g++ perl XML::LibXML XML::LibXSLT zlib headers" ;;
    esac
}

missing=()
for t in git cmake g++ perl; do command -v "$t" >/dev/null || missing+=("$t"); done
command -v ninja >/dev/null || command -v ninja-build >/dev/null || missing+=("ninja")
perl -MXML::LibXML -e1 2>/dev/null || missing+=("perl XML::LibXML")
perl -MXML::LibXSLT -e1 2>/dev/null || missing+=("perl XML::LibXSLT")
if [ ${#missing[@]} -gt 0 ]; then
    if [ "$INSTALL_DEPS" = 1 ]; then
        install_deps
    else
        sp_die "missing build dependencies: ${missing[*]}  (re-run with --install-deps, or install them yourself)"
    fi
fi

BUILD_ROOT="${SMOOTHPAN_BUILD_DIR:-$HOME/.cache/smoothpan-build}"
SRC="$BUILD_ROOT/dfhack-$TAG"
mkdir -p "$BUILD_ROOT"
if [ ! -f "$SRC/CMakeLists.txt" ]; then
    sp_log "Cloning DFHack $TAG (with submodules) into $SRC"
    rm -rf "$SRC"
    git clone --quiet --depth 1 --branch "$TAG" --recurse-submodules --shallow-submodules \
        https://github.com/DFHack/dfhack "$SRC"
fi

mkdir -p "$SRC/plugins/external"
ln -sfn "$SP_REPO_ROOT/src" "$SRC/plugins/external/smoothpan"
printf 'add_subdirectory(smoothpan)\n' > "$SRC/plugins/external/CMakeLists.txt"

GEN="-G Ninja"
command -v ninja >/dev/null || GEN="-G Ninja -DCMAKE_MAKE_PROGRAM=$(command -v ninja-build)"
if [ ! -f "$SRC/build/build.ninja" ]; then
    sp_log "Configuring DFHack"
    # shellcheck disable=SC2086
    cmake -S "$SRC" -B "$SRC/build" $GEN \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_LIBRARY=ON -DBUILD_PLUGINS=ON -DBUILD_TESTS=OFF -DBUILD_DOCS=OFF \
        -DINSTALL_SCRIPTS=OFF -DINSTALL_DATA_FILES=OFF \
        -DCMAKE_INSTALL_PREFIX="$SRC/build/image" >"$BUILD_ROOT/configure-$TAG.log" 2>&1 \
        || { tail -40 "$BUILD_ROOT/configure-$TAG.log"; sp_die "configure failed (log: $BUILD_ROOT/configure-$TAG.log)"; }
fi

sp_log "Building smoothpan (jobs=$JOBS)"
cmake --build "$SRC/build" --target smoothpan -- -j"$JOBS"
SO="$SRC/build/plugins/external/smoothpan/smoothpan.plug.so"
[ -f "$SO" ] || sp_die "build finished but $SO is missing"

OUT="$SP_REPO_ROOT/build-linux"
mkdir -p "$OUT"
cp -f "$SO" "$OUT/smoothpan.plug.so"
sp_log "Built $(sp_plugin_build_version "$OUT/smoothpan.plug.so") for DFHack $(sp_plugin_dfhack_tag "$OUT/smoothpan.plug.so") -> $OUT/smoothpan.plug.so"

if [ "$INSTALL" = 1 ]; then
    [ -n "$DF" ] || sp_die "cannot install: DF folder not found (set DF_DIR)"
    "$SP_REPO_ROOT/tools/linux/install.sh" --so "$OUT/smoothpan.plug.so"
fi
