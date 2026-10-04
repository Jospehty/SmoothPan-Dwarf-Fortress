#!/usr/bin/env bash
# Record telemetry across virtual pans (and optional wheel notches), all
# triggered from INSIDE the game loop by dfhack.timeout (no console stalls).
#   panrec.sh <label> <frames> "<step> ..."
# steps: pan<dx>,<dy>,<ms>  (e.g. pan1,0,1200) | out<N> | in<N> | w<ticks>
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/recenter.sh >/dev/null   # known mid-map view: tests must not walk the camera off the map
LABEL="$1"; N="$2"; STEPS="$3"
bash $T/dfr.sh "smoothpan zoom testcursor map" >/dev/null
bash $T/dfr.sh "smoothpan watch record $N $LABEL ${PANREC_NOPIX:-}" >/dev/null
bash $T/dflua.sh "
local g=require('gui')
local at=10
for s in ('$STEPS'):gmatch('%S+') do
  if s:sub(1,1)=='w' then at=at+(tonumber(s:sub(2)) or 1)
  elseif s:sub(1,3)=='pan' then
    local dx,dy,ms=s:match('pan(%-?%d+),(%-?%d+),(%d+)')
    dfhack.timeout(at,'frames',function() dfhack.run_command('smoothpan','testpan',dx,dy,ms) end)
  else
    local k=tonumber(s:match('%d+')) or 1
    local key=(s:sub(1,2)=='in') and 'ZOOM_IN' or 'ZOOM_OUT'
    dfhack.timeout(at,'frames',function()
      local v=dfhack.gui.getCurViewscreen(); for i=1,k do g.simulateInput(v,key) end end)
  end
end
" >/dev/null
sleep "$(awk "BEGIN{print $N/120 + 1.5}")"
for i in $(seq 1 20); do
    bash $T/dfr.sh "smoothpan watch" | grep -q 'recording=0' && break
    sleep 1
done
bash $T/dfr.sh "smoothpan zoom testcursor off" >/dev/null
python3 $T/panstats.py "$LABEL"
