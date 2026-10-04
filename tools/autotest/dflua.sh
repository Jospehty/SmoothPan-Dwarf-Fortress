#!/usr/bin/env bash
# Run ONE lua chunk in the live game, passed as a single argument (dfr.sh
# word-splits, which silently runs only the first statement of a chunk).
cd /home/jospeh/.local/share/Steam/steamapps/common/DFHack || exit 1
timeout 30 ./dfhack-run lua "$1" 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
