#!/usr/bin/env bash
# Screenshots in the middle of a slowed-down zoom-in glide (from cell 24 to 48),
# taken from inside the game loop so nothing suspends rendering.
#   midshot.sh <label> <rate> "<frame offsets>"
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LABEL="$1"; RATE="${2:-2}"; OFFS="${3:-20 40 60}"
bash $T/recenter.sh >/dev/null
bash $T/dfr.sh "smoothpan zoom testcursor map" >/dev/null
bash $T/dflua.sh "
local g=require('gui')
dfhack.timeout(5,'frames',function() local v=dfhack.gui.getCurViewscreen() for i=1,3 do g.simulateInput(v,'ZOOM_OUT') end end)
" >/dev/null
sleep 2
bash $T/dfr.sh "smoothpan zoom rate $RATE" >/dev/null
bash $T/dflua.sh "
local g=require('gui')
dfhack.timeout(5,'frames',function() local v=dfhack.gui.getCurViewscreen() for i=1,3 do g.simulateInput(v,'ZOOM_IN') end end)
for o in ('$OFFS'):gmatch('%S+') do
  local n=tonumber(o)
  dfhack.timeout(5+n,'frames',function() dfhack.run_command('smoothpan','watch','shot','${LABEL}_'..o) end)
end
" >/dev/null
sleep 4
bash $T/dfr.sh "smoothpan zoom rate 16" >/dev/null
bash $T/dfr.sh "smoothpan zoom testcursor off" >/dev/null
ls -la "/home/jospeh/.local/share/Steam/steamapps/common/Dwarf Fortress/dfhack-config/smoothpan/" | grep "shot_${LABEL}_"
