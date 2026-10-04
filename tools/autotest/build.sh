#!/usr/bin/env bash
# Build smoothpan against the installed DFHack, using the locally-extracted
# GCC 15 (DFHack rejects the system GCC 16).
set -uo pipefail
TC="$HOME/.cache/smoothpan-toolchain/usr/bin"
export CC="$TC/gcc-15"
export CXX="$TC/g++-15"
export PERL5LIB="$HOME/perl5/lib/perl5"
export DF_DIR="/home/jospeh/.local/share/Steam/steamapps/common/DFHack"
WT="/home/jospeh/SmoothPan-Dwarf-Fortress/.claude/worktrees/zoom-fps-fix"
cd "$WT" || exit 1

"$CXX" --version | head -1
./tools/linux/build.sh --no-install
echo "BUILD_EXIT=$?"
