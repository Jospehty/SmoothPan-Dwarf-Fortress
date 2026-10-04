#!/usr/bin/env bash
# Put the view back to a known, mid-map state before a test: zoom cell 48
# (vanilla zoom factor 192, stepped with smooth zoom off) and the camera
# centred on a citizen.  Tests anchor zoom off-centre and pan in one direction,
# so without this the camera ratchets to the map edge, where it clamps and the
# measurements stop meaning anything.
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/dfr.sh "smoothpan zoom off" >/dev/null
# DF takes one zoom step per frame, so step once per call, across frames.
for i in 1 2 3 4 5 6 7 8; do
bash $T/dflua.sh "
local z=df.global.gps.viewport_zoom_factor
if z~=192 then require('gui').simulateInput(dfhack.gui.getCurViewscreen(), z<192 and 'ZOOM_IN' or 'ZOOM_OUT') end
" >/dev/null
sleep 0.25
done
bash $T/dfr.sh "smoothpan zoom on" >/dev/null
bash $T/dflua.sh "
local u
for _,x in ipairs(df.global.world.units.active) do if dfhack.units.isCitizen(x) then u=x break end end
local p = u and u.pos or xyz2pos(df.global.world.map.x_count//2, df.global.world.map.y_count//2, df.global.window_z)
dfhack.gui.revealInDwarfmodeMap(p, true, true)
" >/dev/null
sleep 0.5
bash $T/dflua.sh "print(('view: window=(%d,%d,%d) cell=%d'):format(df.global.window_x, df.global.window_y, df.global.window_z, df.global.gps.viewport_zoom_factor//4))"
