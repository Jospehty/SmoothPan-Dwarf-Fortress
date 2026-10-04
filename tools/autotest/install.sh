#!/usr/bin/env bash
# Put the worktree build into DFHack's plugin dir (does not load or enable it).
D="/home/jospeh/.local/share/Steam/steamapps/common/DFHack"
SRC="/home/jospeh/SmoothPan-Dwarf-Fortress/.claude/worktrees/zoom-fps-fix/build-linux/smoothpan.plug.so"
cp -f "$SRC" "$D/hack/plugins/smoothpan.plug.so" && echo "installed $(md5sum "$SRC" | cut -c1-8)"
