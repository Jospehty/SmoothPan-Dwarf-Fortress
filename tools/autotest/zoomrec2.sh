#!/usr/bin/env bash
# Record telemetry across wheel notches delivered from INSIDE the game loop
# (dfhack.timeout), queued in one call that returns immediately.  Every
# dfhack-run pauses rendering while it runs, so nothing else talks to the game
# until the recording should be over.
#   zoomrec2.sh <label> <frames> "<steps>" [nopix] [vanilla]
# steps: out<N>|in<N> (N notches in one tick), w<ticks> (gap in game frames)
T="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
bash $T/recenter.sh >/dev/null   # known mid-map view: tests must not walk the camera off the map
LABEL="$1"; N="$2"; STEPS="$3"; MODE4="${4:-}"; MODE5="${5:-}"
NOPIX=""; [ "$MODE4" = "nopix" ] && NOPIX="nopix"
VAN=0; { [ "$MODE4" = "vanilla" ] || [ "$MODE5" = "vanilla" ]; } && VAN=1
[ $VAN = 1 ] && bash $T/dfr.sh "smoothpan zoom off" >/dev/null
bash $T/dfr.sh "smoothpan zoom testcursor map" >/dev/null
bash $T/dfr.sh "smoothpan watch record $N $LABEL $NOPIX" >/dev/null
bash $T/dflua.sh "
local g=require('gui')
local steps={}
for s in ('$STEPS'):gmatch('%S+') do steps[#steps+1]=s end
local at=10
for _,s in ipairs(steps) do
  local k=tonumber(s:match('%d+')) or 1
  if s:sub(1,1)=='w' then at=at+k
  else
    local key=(s:sub(1,2)=='in') and 'ZOOM_IN' or 'ZOOM_OUT'
    dfhack.timeout(at,'frames',function()
      local v=dfhack.gui.getCurViewscreen()
      for i=1,k do g.simulateInput(v,key) end
    end)
  end
end
" >/dev/null
# Let the recording run untouched (frames at >= ~120 fps, plus margin).
sleep "$(awk "BEGIN{print $N/120 + 1.5}")"
for i in $(seq 1 20); do
    bash $T/dfr.sh "smoothpan watch" | grep -q 'recording=0' && break
    sleep 1
done
bash $T/dfr.sh "smoothpan zoom testcursor off" >/dev/null
[ $VAN = 1 ] && bash $T/dfr.sh "smoothpan zoom on" >/dev/null
python3 $T/watchstats.py "$LABEL"
python3 - "$LABEL" <<'PY'
import subprocess, sys
out = subprocess.run(["python3", "$T/watchstats.py", sys.argv[1], "--frames"],
                     capture_output=True, text=True).stdout
rows = [dict(kv.split("=", 1) for kv in l.split()) for l in out.splitlines() if l.startswith("f=")]
slow = []
for i, r in enumerate(rows):
    if float(r["ms"]) > 20:
        prev = rows[i - 1]["cell"] if i else "?"
        slow.append(f"f={r['f']}:{r['ms']}ms(cell {prev}->{r['cell']})")
print("slow    " + (" ".join(slow) if slow else "none"))
PY
