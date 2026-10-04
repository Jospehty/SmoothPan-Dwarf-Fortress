#!/usr/bin/env bash
# Run one or more DFHack commands against the live game.
#   dfr.sh "cmd one" "cmd two" ...
cd /home/jospeh/.local/share/Steam/steamapps/common/DFHack || exit 1
for c in "$@"; do
    # shellcheck disable=SC2086
    timeout 20 ./dfhack-run $c 2>&1 | sed 's/\x1b\[[0-9;]*m//g'
done
